#include "runtime/AccessProcessArguments.h"
#include "runtime/AccessProcessControl.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fiber/async/Yield.h>
#include <fiber/net/UnixAddress.h>

namespace fiber::access_server {
namespace {

class Directory {
public:
    Directory() {
        std::array<char, 40> pattern{};
        constexpr std::string_view prefix = "/tmp/access-control-XXXXXX";
        std::copy(prefix.begin(), prefix.end(), pattern.begin());
        if (auto *created = ::mkdtemp(pattern.data())) {
            path = created;
        }
    }
    ~Directory() {
        std::error_code error;
        if (!path.empty()) {
            std::filesystem::remove_all(path, error);
        }
    }
    std::string path;
};

struct Runtime {
    AccessServerRuntimeState state = AccessServerRuntimeState::Prepared;
    bool ready = true;
    bool fail_activation = false;
    unsigned releases = 0;
    unsigned activations = 0;
    Runtime *old = nullptr;

    AccessProcessActions actions() noexcept {
        return {
                .context = this,
                .state = [](void *p) noexcept { return static_cast<Runtime *>(p)->state; },
                .ready = [](void *p) noexcept { return static_cast<Runtime *>(p)->ready; },
                .registration = [](void *) noexcept -> std::string_view { return "retrying"; },
                .activate = &activate,
                .release = &release,
        };
    }
    static async::Task<common::IoResult<void>> activate(void *context) noexcept {
        auto &self = *static_cast<Runtime *>(context);
        EXPECT_TRUE(self.old);
        if (self.old) {
            EXPECT_EQ(self.old->state, AccessServerRuntimeState::Draining);
            EXPECT_EQ(self.old->releases, 1U);
        }
        ++self.activations;
        self.state = self.fail_activation ? AccessServerRuntimeState::Stopped : AccessServerRuntimeState::Running;
        if (self.fail_activation) {
            co_return std::unexpected(common::IoErr::AddrInUse);
        }
        co_return common::IoResult<void>{};
    }
    static async::Task<common::IoResult<void>> release(void *context) noexcept {
        auto &self = *static_cast<Runtime *>(context);
        ++self.releases;
        self.state = AccessServerRuntimeState::Draining;
        co_return common::IoResult<void>{};
    }
};

struct Pair {
    event::EventLoop loop;
    Directory old_directory;
    Directory new_directory;
    Runtime old{.state = AccessServerRuntimeState::Running};
    Runtime fresh{.old = &old};
    AccessProcessControl old_control;
    AccessProcessControl new_control;
    Pair(std::string old_compatibility = "same", bool old_http3 = false, bool shared_log = false) :
        old_control(loop, old.actions(),
                    {.directory = old_directory.path,
                     .compatibility = std::move(old_compatibility),
                     .log_identity = shared_log ? "same-log" : "",
                     .http3 = old_http3}),
        new_control(loop, fresh.actions(),
                    {.directory = new_directory.path,
                     .compatibility = "same",
                     .log_identity = shared_log ? "same-log" : ""}) {}


    template<class Function>
    void run(Function function) {
        ASSERT_FALSE(old_directory.path.empty());
        ASSERT_FALSE(new_directory.path.empty());
        ASSERT_TRUE(old_control.initialize());
        ASSERT_TRUE(new_control.initialize());
        async::spawn(loop, [&]() -> async::DetachedTask {
            old_control.start();
            new_control.start();
            co_await function();
            co_await new_control.quiesce();
            co_await old_control.quiesce();
            co_await new_control.shutdown();
            co_await old_control.shutdown();
            loop.stop();
        });
        loop.run();
    }

    AccessControlMessage takeover() const {
        return {
                .request_id = "takeover",
                .command = "TAKEOVER",
                .expected_boot_id = new_control.status().boot_id,
                .upgrade_id = "upgrade-1",
                .old_socket = old_control.path(),
                .old_boot_id = old_control.status().boot_id,
        };
    }

    async::Task<void> wait_job() {
        for (unsigned i = 0; i < 30000; ++i) {
            auto status = new_control.status();
            if (status.phase == "serving" || status.phase == "failed") {
                co_return;
            }
            co_await async::yield();
        }
        ADD_FAILURE() << "upgrade task did not finish";
    }
};

TEST(AccessControlProtocolTest, RoundTripsEscapedTextAndTypedState) {
    AccessControlMessage message{
            .pid = 123,
            .request_id = "request",
            .command = "STATUS",
            .boot_id = "boot",
            .state = "prepared",
            .compatibility = "quoted\"line\n",
            .ready = true,
    };
    auto encoded = encode_access_control(message);
    ASSERT_TRUE(encoded);
    auto decoded = decode_access_control(*encoded);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->compatibility, message.compatibility);
    EXPECT_EQ(decoded->pid, 123);
    EXPECT_TRUE(decoded->ready);
}

TEST(AccessControlProtocolTest, RejectsMalformedDuplicateUnboundedAndUnsupportedMessages) {
    for (auto input: {
                 R"({"protocolVersion":1,"requestId":"x","requestId":"y"})",
                 R"({"protocolVersion":2,"requestId":"x"})",
                 R"({"protocolVersion":1,"requestId":"x","unknown":true})",
                 R"({"protocolVersion":1,"requestId":"x","ready":"true"})",
                 R"({"protocolVersion":1,"requestId":"x","oldSocket":"/run/\u0000.sock"})",
                 R"({"protocolVersion":1,"requestId":"x"} {})",
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

TEST(AccessProcessArgumentsTest, ValidatesModeDirectoryAndBoundedPrewarmDeadline) {
    const std::array<std::string_view, 4> arguments{"settings.env", "--start-mode=prewarm",
                                                    "--control-dir=/run/access-test", "--prewarm-timeout-millis=9000"};
    auto result = parse_access_process_arguments(arguments);
    ASSERT_TRUE(result);
    EXPECT_TRUE(result->prewarm);
    EXPECT_EQ(result->config_path, "settings.env");
    EXPECT_EQ(result->prewarm_timeout, std::chrono::milliseconds(9000));
    EXPECT_FALSE(parse_access_process_arguments(std::array<std::string_view, 1>{"--start-mode=prewarm"}));
    EXPECT_FALSE(parse_access_process_arguments(std::array<std::string_view, 1>{"--control-dir=relative"}));
    EXPECT_FALSE(parse_access_process_arguments(std::array<std::string_view, 1>{"--prewarm-timeout-millis=0"}));
    EXPECT_FALSE(parse_access_process_arguments(std::array<std::string_view, 2>{"a.env", "b.env"}));
    EXPECT_FALSE(parse_access_process_arguments(
            std::array<std::string_view, 2>{"--start-mode=normal", "--start-mode=prewarm"}));
}

TEST(AccessProcessControlTest, UsesPidSocketProtectsLiveEndpointsAndCleansOnlyOwnInode) {
    event::EventLoop loop;
    Directory directory;
    ASSERT_FALSE(directory.path.empty());
    Runtime instance;
    std::string path;
    {
        AccessProcessControl control(loop, instance.actions(), {.directory = directory.path});
        ASSERT_TRUE(control.initialize());
        path = control.path();
        EXPECT_EQ(path, directory.path + "/" + std::to_string(::getpid()) + ".sock");
        struct stat st{};
        ASSERT_EQ(::lstat(path.c_str(), &st), 0);
        EXPECT_EQ(st.st_mode & 0777U, 0600U);
        AccessProcessControl duplicate(loop, instance.actions(), {.directory = directory.path});
        EXPECT_FALSE(duplicate.initialize());
        EXPECT_TRUE(std::filesystem::exists(path));
        // Replacing our path must not make the destructor delete the new file.
        ASSERT_EQ(::unlink(path.c_str()), 0);
        const int replacement = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
        ASSERT_GE(replacement, 0);
        ::close(replacement);
    }
    EXPECT_TRUE(std::filesystem::is_regular_file(path));
}

TEST(AccessProcessControlTest, RefusesPublicDirectoryAndNeverUnlinksRegularFile) {
    event::EventLoop loop;
    Directory directory;
    Runtime instance;
    ASSERT_FALSE(directory.path.empty());
    ASSERT_EQ(::chmod(directory.path.c_str(), 0755), 0);
    {
        AccessProcessControl control(loop, instance.actions(), {.directory = directory.path});
        EXPECT_FALSE(control.initialize());
    }
    ASSERT_EQ(::chmod(directory.path.c_str(), 0700), 0);
    const auto path = directory.path + "/" + std::to_string(::getpid()) + ".sock";
    const int fd = ::open(path.c_str(), O_CREAT | O_WRONLY | O_EXCL, 0600);
    ASSERT_GE(fd, 0);
    ::close(fd);
    {
        AccessProcessControl control(loop, instance.actions(), {.directory = directory.path});
        EXPECT_FALSE(control.initialize());
    }
    EXPECT_TRUE(std::filesystem::is_regular_file(path));
}

TEST(AccessProcessControlTest, PrewarmFailureAndBootMismatchLeaveOldListenerUntouched) {
    Pair pair;
    pair.run([&]() -> async::Task<void> {
        pair.fresh.ready = false;
        auto denied = co_await request_access_control(pair.loop, pair.new_control.path(), pair.takeover());
        EXPECT_TRUE(denied);
        if (denied) {
            EXPECT_EQ(denied->error, "NOT_PREPARED");
        }
        pair.fresh.ready = true;
        auto wrong = pair.takeover();
        wrong.expected_boot_id = "reused-pid";
        denied = co_await request_access_control(pair.loop, pair.new_control.path(), std::move(wrong));
        EXPECT_TRUE(denied);
        if (denied) {
            EXPECT_EQ(denied->error, "BOOT_ID_MISMATCH");
        }
        EXPECT_EQ(pair.old.releases, 0U);
        EXPECT_EQ(pair.old.state, AccessServerRuntimeState::Running);
    });
}

TEST(AccessProcessControlTest, AbortRepliesBeforeRequestingPreparedProcessExit) {
    Pair pair;
    pair.run([&]() -> async::Task<void> {
        AccessControlMessage abort{
                .request_id = "abort", .command = "ABORT", .expected_boot_id = pair.new_control.status().boot_id};
        auto response = co_await request_access_control(pair.loop, pair.new_control.path(), std::move(abort));
        EXPECT_TRUE(response);
        if (response) {
            EXPECT_TRUE(response->error.empty());
        }
        co_await pair.new_control.wait_exit();
        EXPECT_EQ(pair.old.releases, 0U);
        EXPECT_EQ(pair.fresh.activations, 0U);
    });
}

TEST(AccessProcessControlTest, ReleasesBeforeActivationAndKeepsDrainingGenerationQueryable) {
    Pair pair;
    pair.run([&]() -> async::Task<void> {
        auto accepted = co_await request_access_control(pair.loop, pair.new_control.path(), pair.takeover());
        EXPECT_TRUE(accepted);
        if (accepted) {
            EXPECT_TRUE(accepted->error.empty());
        }
        co_await pair.wait_job();
        EXPECT_EQ(pair.new_control.status().phase, "serving");
        EXPECT_EQ(pair.new_control.status().registration, "retrying");
        EXPECT_EQ(pair.old.state, AccessServerRuntimeState::Draining);
        EXPECT_EQ(pair.old.releases, 1U);
        EXPECT_EQ(pair.fresh.activations, 1U);
        co_await pair.old_control.wait_exit();
        AccessControlMessage status{.request_id = "old-status", .command = "STATUS"};
        auto old = co_await request_access_control(pair.loop, pair.old_control.path(), status);
        EXPECT_TRUE(old);
        if (old) {
            EXPECT_EQ(old->phase, "released");
            EXPECT_EQ(old->request_id, "old-status");
        }
        // Repeated accepted command returns the same task, with no new release.
        auto repeated = co_await request_access_control(pair.loop, pair.new_control.path(), pair.takeover());
        EXPECT_TRUE(repeated);
        EXPECT_EQ(pair.old.releases, 1U);
        EXPECT_EQ(pair.fresh.activations, 1U);
        auto conflict = pair.takeover();
        conflict.upgrade_id = "another-upgrade";
        auto rejected = co_await request_access_control(pair.loop, pair.new_control.path(), std::move(conflict));
        EXPECT_TRUE(rejected);
        if (rejected) {
            EXPECT_EQ(rejected->error, "UPGRADE_CONFLICT");
        }
    });
}

TEST(AccessProcessControlTest, ClientDisconnectDoesNotCancelAcceptedTakeover) {
    Pair pair;
    pair.run([&]() -> async::Task<void> {
        auto connected = co_await net::UnixStream::connect(
                pair.loop, net::UnixAddress::filesystem(pair.new_control.path()), kAccessControlTimeout);
        EXPECT_TRUE(connected);
        if (!connected) {
            co_return;
        }
        net::UnixStream stream(std::move(*connected));
        auto sent = co_await write_access_control(stream, pair.takeover());
        EXPECT_TRUE(sent);
        // Ensure the service accepted the command, then disconnect before the
        // detached upgrade task has to finish.
        for (unsigned i = 0; i < 1000 && pair.new_control.status().upgrade_id.empty(); ++i) {
            co_await async::yield();
        }
        EXPECT_FALSE(pair.new_control.status().upgrade_id.empty());
        stream.close();
        co_await pair.wait_job();
        EXPECT_EQ(pair.new_control.status().phase, "serving");
        EXPECT_EQ(pair.fresh.activations, 1U);
    });
}

TEST(AccessProcessControlTest, ActivationFailureDoesNotPretendOldListenerWasRestored) {
    Pair pair;
    pair.fresh.fail_activation = true;
    pair.run([&]() -> async::Task<void> {
        auto response = co_await request_access_control(pair.loop, pair.new_control.path(), pair.takeover());
        EXPECT_TRUE(response);
        co_await pair.wait_job();
        EXPECT_EQ(pair.new_control.status().error, "ACTIVATION_FAILED");
        EXPECT_EQ(pair.new_control.status().phase, "failed");
        EXPECT_EQ(pair.old.state, AccessServerRuntimeState::Draining);
        EXPECT_EQ(pair.fresh.state, AccessServerRuntimeState::Stopped);
    });
}

TEST(AccessProcessControlTest, RejectsWrongOldBootBeforeAnyRelease) {
    Pair pair;
    pair.run([&]() -> async::Task<void> {
        auto request = pair.takeover();
        request.old_boot_id = "previous-process-with-same-pid";
        auto accepted = co_await request_access_control(pair.loop, pair.new_control.path(), std::move(request));
        EXPECT_TRUE(accepted);
        co_await pair.wait_job();
        EXPECT_EQ(pair.new_control.status().error, "OLD_BOOT_ID_MISMATCH");
        EXPECT_EQ(pair.old.releases, 0U);
    });
}

TEST(AccessProcessControlTest, UnsupportedHttp3AndIncompatibleSettingsDoNotReleaseOldListener) {
    for (unsigned mode = 0; mode < 3; ++mode) {
        Pair pair(mode == 0 ? "different-environment" : "same", mode == 1, mode == 2);
        pair.run([&]() -> async::Task<void> {
            auto accepted = co_await request_access_control(pair.loop, pair.new_control.path(), pair.takeover());
            EXPECT_TRUE(accepted);
            co_await pair.wait_job();
            constexpr std::array<std::string_view, 3> expected{"UPGRADE_INCOMPATIBLE", "UPGRADE_HTTP3_UNSUPPORTED",
                                                               "UPGRADE_LOG_DIRECTORY_SHARED"};
            EXPECT_EQ(pair.new_control.status().error, expected[mode]);
            EXPECT_EQ(pair.old.releases, 0U);
            EXPECT_EQ(pair.fresh.activations, 0U);
        });
    }
}

TEST(AccessProcessControlTest, RefusesThirdGenerationWhilePreviousGenerationDrains) {
    Pair pair;
    Directory directory;
    Runtime third{.old = &pair.fresh};
    AccessProcessControl third_control(pair.loop, third.actions(),
                                       {.directory = directory.path, .compatibility = "same"});
    ASSERT_TRUE(third_control.initialize());
    pair.run([&]() -> async::Task<void> {
        third_control.start();
        auto accepted = co_await request_access_control(pair.loop, pair.new_control.path(), pair.takeover());
        EXPECT_TRUE(accepted);
        co_await pair.wait_job();
        AccessControlMessage takeover{.request_id = "third",
                                      .command = "TAKEOVER",
                                      .expected_boot_id = third_control.status().boot_id,
                                      .upgrade_id = "third-upgrade",
                                      .old_socket = pair.new_control.path(),
                                      .old_boot_id = pair.new_control.status().boot_id};
        auto attempted = co_await request_access_control(pair.loop, third_control.path(), std::move(takeover));
        EXPECT_TRUE(attempted);
        for (unsigned i = 0; i < 30000 && third_control.status().phase != "failed"; ++i) {
            co_await async::yield();
        }
        EXPECT_EQ(third_control.status().error, "PREVIOUS_PROCESS_DRAINING");
        EXPECT_EQ(pair.fresh.releases, 0U);
        EXPECT_EQ(third.activations, 0U);
        co_await third_control.quiesce();
        co_await third_control.shutdown();
    });
}

TEST(AccessProcessControlTest, HandlesFragmentedFramesAndRejectsOversizedLength) {
    Pair pair;
    pair.run([&]() -> async::Task<void> {
        auto connected = co_await net::UnixStream::connect(
                pair.loop, net::UnixAddress::filesystem(pair.old_control.path()), kAccessControlTimeout);
        EXPECT_TRUE(connected);
        if (!connected) {
            co_return;
        }
        net::UnixStream stream(std::move(*connected));
        AccessControlMessage request{.request_id = "split", .command = "STATUS"};
        auto encoded = encode_access_control(request);
        EXPECT_TRUE(encoded);
        if (!encoded) {
            co_return;
        }
        const auto size = ::htonl(static_cast<std::uint32_t>(encoded->size()));
        std::string frame(reinterpret_cast<const char *>(&size), sizeof(size));
        frame += *encoded;
        for (const char byte: frame) {
            auto written = co_await stream.write(&byte, 1, kAccessControlTimeout);
            EXPECT_TRUE(written);
            co_await async::yield();
        }
        auto response = co_await read_access_control(stream);
        EXPECT_TRUE(response);
        if (response) {
            EXPECT_EQ(response->request_id, "split");
        }
        const auto oversized = ::htonl(static_cast<std::uint32_t>(kAccessControlMaxFrame + 1));
        EXPECT_TRUE(co_await stream.write(&oversized, sizeof(oversized), kAccessControlTimeout));
        EXPECT_FALSE(co_await read_access_control(stream));
        EXPECT_EQ(pair.old.releases, 0U);
    });
}

} // namespace
} // namespace fiber::access_server
