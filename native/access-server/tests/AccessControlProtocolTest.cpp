#include "runtime/AccessControlProtocol.h"
#include "runtime/AccessProcessArguments.h"

#include <gtest/gtest.h>

#include <array>
#include <optional>
#include <string>
#include <utility>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <fiber/async/Spawn.h>
#include <fiber/async/Yield.h>

namespace fiber::access_server {
namespace {

TEST(AccessControlProtocolTest, RoundTripsEscapedTextAndTypedState) {
    AccessControlMessage message{
            .pid = 123,
            .active_pid = 456,
            .prewarm_timeout_millis = 9000,
            .request_id = "request",
            .command = "STATUS",
            .state = "prepared",
            .compatibility = "quoted\"line\n",
            .upgrade_error = "PREWARM_TIMEOUT",
            .server_path = "/opt/access-server/bin/access-server",
            .ready = true,
            .fixed_ports = true,
            .background = true,
    };
    auto encoded = encode_access_control(message);
    ASSERT_TRUE(encoded);
    auto decoded = decode_access_control(*encoded);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->protocol_version, kAccessControlProtocolVersion);
    EXPECT_EQ(decoded->compatibility, message.compatibility);
    EXPECT_EQ(decoded->server_path, message.server_path);
    EXPECT_EQ(decoded->upgrade_error, "PREWARM_TIMEOUT");
    EXPECT_EQ(decoded->pid, 123);
    EXPECT_EQ(decoded->active_pid, 456);
    EXPECT_EQ(decoded->prewarm_timeout_millis, 9000);
    EXPECT_TRUE(decoded->ready);
    EXPECT_TRUE(decoded->fixed_ports);
    EXPECT_TRUE(decoded->background);
    EXPECT_FALSE(decoded->http3);
}

TEST(AccessControlProtocolTest, RejectsMalformedDuplicateUnboundedAndUnsupportedMessages) {
    for (auto input: {
                 R"({"protocolVersion":2,"requestId":"x","requestId":"y"})",
                 R"({"protocolVersion":1,"requestId":"x"})",
                 R"({"protocolVersion":2,"requestId":"x","unknown":true})",
                 R"({"protocolVersion":2,"requestId":"x","ready":"true"})",
                 R"({"protocolVersion":2,"requestId":"x","pid":-1})",
                 R"({"protocolVersion":2,"requestId":"x","serverPath":"/run/\u0000.sock"})",
                 R"({"protocolVersion":2,"requestId":"x"} {})",
                 R"({"protocolVersion":2,"requestId":""})",
                 R"({"requestId":"x"})",
                 R"([])",
         }) {
        EXPECT_FALSE(decode_access_control(input)) << input;
    }
    EXPECT_FALSE(decode_access_control(std::string(kAccessControlMaxFrame + 1, ' ')));
    AccessControlMessage large{.request_id = "x", .compatibility = std::string(kAccessControlMaxFrame, 'x')};
    EXPECT_FALSE(encode_access_control(large));
    EXPECT_FALSE(validate_access_control_peer(-1));
}

TEST(AccessControlProtocolTest, AcceptsSameUserPeerAndReassemblesFragmentedFrames) {
    std::array<int, 2> pair{-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, pair.data()), 0);
    EXPECT_TRUE(validate_access_control_peer(pair[0]));
    event::EventLoop loop;
    net::UnixStream writer(loop, pair[0]);
    net::UnixStream reader(loop, pair[1]);
    std::optional<common::IoResult<AccessControlMessage>> received;
    bool finished = false;
    async::spawn(loop, [&]() -> async::DetachedTask {
        // Read concurrently: every one-byte send holds a socket buffer slot
        // until the peer drains it.
        async::spawn(loop, [&]() -> async::DetachedTask { received = co_await read_access_control(reader); });
        AccessControlMessage request{.request_id = "split", .command = "STATUS"};
        auto encoded = encode_access_control(request);
        EXPECT_TRUE(encoded);
        if (encoded) {
            const auto size = ::htonl(static_cast<std::uint32_t>(encoded->size()));
            std::string frame(reinterpret_cast<const char *>(&size), sizeof(size));
            frame += *encoded;
            for (const char byte: frame) {
                auto written = co_await writer.write(&byte, 1, kAccessControlTimeout);
                EXPECT_TRUE(written);
                co_await async::yield();
            }
            while (!received) {
                co_await async::yield();
            }
            EXPECT_TRUE(*received);
            if (*received) {
                EXPECT_EQ((*received)->request_id, "split");
            }
        }
        const auto oversized = ::htonl(static_cast<std::uint32_t>(kAccessControlMaxFrame + 1));
        EXPECT_TRUE(co_await writer.write(&oversized, sizeof(oversized), kAccessControlTimeout));
        EXPECT_FALSE(co_await read_access_control(reader));
        writer.close();
        reader.close();
        finished = true;
        loop.stop();
    });
    loop.run();
    EXPECT_TRUE(finished);
}

TEST(AccessProcessArgumentsTest, AcceptsStandaloneAndBossChannelOnly) {
    auto standalone = parse_access_process_arguments(std::array<std::string_view, 1>{"settings.env"});
    ASSERT_TRUE(standalone);
    EXPECT_EQ(standalone->config_path, "settings.env");
    EXPECT_EQ(standalone->boss_fd, -1);

    auto supervised = parse_access_process_arguments(std::array<std::string_view, 2>{"settings.env", "--boss-fd=3"});
    ASSERT_TRUE(supervised);
    EXPECT_EQ(supervised->boss_fd, 3);

    auto defaults = parse_access_process_arguments(std::span<const std::string_view>{});
    ASSERT_TRUE(defaults);
    EXPECT_EQ(defaults->config_path, "access-server.env");

    for (auto arguments: {
                 std::array<std::string_view, 2>{"a.env", "b.env"},
                 std::array<std::string_view, 2>{"--boss-fd=3", "--boss-fd=4"},
                 std::array<std::string_view, 2>{"a.env", "--boss-fd=2"},
                 std::array<std::string_view, 2>{"a.env", "--boss-fd=x"},
                 std::array<std::string_view, 2>{"a.env", "--start-mode=prewarm"},
                 std::array<std::string_view, 2>{"a.env", "--control-dir=/run/access"},
         }) {
        EXPECT_FALSE(parse_access_process_arguments(arguments)) << arguments[1];
    }
}

TEST(AccessProcessArgumentsTest, RequiresAbsoluteBossPathsAndBoundedPrewarmDeadline) {
    auto parsed = parse_access_boss_arguments(
            std::array<std::string_view, 4>{"--socket=/run/access/boss.sock", "--server=/opt/access/access-server",
                                            "--config=/etc/access/a.env", "--prewarm-timeout-millis=9000"});
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed->socket_path, "/run/access/boss.sock");
    EXPECT_EQ(parsed->server_path, "/opt/access/access-server");
    EXPECT_EQ(parsed->config_path, "/etc/access/a.env");
    EXPECT_EQ(parsed->prewarm_timeout, std::chrono::milliseconds(9000));

    auto defaults = parse_access_boss_arguments(
            std::array<std::string_view, 3>{"--socket=/run/b.sock", "--server=/opt/server", "--config=/etc/a.env"});
    ASSERT_TRUE(defaults);
    EXPECT_EQ(defaults->prewarm_timeout, std::chrono::milliseconds(120000));

    const std::string long_socket = "--socket=/" + std::string(200, 's');
    for (auto arguments: {
                 std::array<std::string_view, 3>{"--socket=run/b.sock", "--server=/opt/server", "--config=/etc/a.env"},
                 std::array<std::string_view, 3>{"--socket=/run/b.sock", "--server=server", "--config=/etc/a.env"},
                 std::array<std::string_view, 3>{"--socket=/run/b.sock", "--server=/opt/server", "--config=a.env"},
                 std::array<std::string_view, 3>{"--socket=/run/b.sock", "--server=/opt/server", "--server=/opt/x"},
                 std::array<std::string_view, 3>{"--socket=/run/b.sock", "--server=/opt/server",
                                                 "--prewarm-timeout-millis=0"},
                 std::array<std::string_view, 3>{std::string_view(long_socket), "--server=/opt/server",
                                                 "--config=/etc/a.env"},
                 std::array<std::string_view, 3>{"--socket=/run/b.sock", "--server=/opt/server", "/etc/a.env"},
         }) {
        EXPECT_FALSE(parse_access_boss_arguments(arguments)) << arguments[2];
    }
    EXPECT_FALSE(parse_access_boss_arguments(
            std::array<std::string_view, 4>{"--socket=/run/b.sock", "--server=/opt/server", "--config=/etc/a.env",
                                            "--prewarm-timeout-millis=3600001"}));
}

} // namespace
} // namespace fiber::access_server
