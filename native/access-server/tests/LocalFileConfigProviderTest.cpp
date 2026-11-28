#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <fiber/async/Spawn.h>
#include <fiber/event/EventLoop.h>
#include <fiber/event/EventLoopGroup.h>
#include <fiber/net/SocketAddress.h>

#include "config/AccessConfigCodec.h"
#include "config/LocalFileConfigSource.h"
#include "config/LocalFileConfigYaml.h"
#include "runtime/AccessControlPlaneSupervisor.h"
#include "runtime/AccessServerConfig.h"
#include "runtime/AccessServerRuntime.h"
#include "runtime/LocalFileConfigService.h"

namespace fiber::access_server {
namespace {

class TempConfigDir final {
public:
    TempConfigDir() {
        static std::atomic<std::uint64_t> counter{0};
        const std::uint64_t id = counter.fetch_add(1, std::memory_order_relaxed);
        path_ = std::filesystem::temp_directory_path() /
                ("access-server-file-cfg-test-" + std::to_string(::getpid()) + "-" + std::to_string(id));
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
        std::filesystem::create_directories(path_, ec);
        EXPECT_FALSE(ec) << ec.message();
    }

    ~TempConfigDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    [[nodiscard]] const std::filesystem::path &path() const noexcept { return path_; }

    void write_file(std::string_view relative, std::string_view content) const {
        const std::filesystem::path full = path_ / std::filesystem::path(relative);
        std::error_code ec;
        std::filesystem::create_directories(full.parent_path(), ec);
        ASSERT_FALSE(ec) << ec.message();
        std::ofstream out(full, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.is_open()) << full.string();
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        ASSERT_TRUE(out.good()) << full.string();
    }

    void remove_file(std::string_view relative) const {
        std::error_code ec;
        std::filesystem::remove(path_ / std::filesystem::path(relative), ec);
        ASSERT_FALSE(ec) << ec.message();
    }

private:
    std::filesystem::path path_;
};

[[nodiscard]] std::uint16_t bound_port_of(int fd) {
    sockaddr_storage storage{};
    socklen_t len = sizeof(storage);
    EXPECT_EQ(::getsockname(fd, reinterpret_cast<sockaddr *>(&storage), &len), 0);
    net::SocketAddress addr;
    EXPECT_TRUE(net::SocketAddress::from_sockaddr(reinterpret_cast<sockaddr *>(&storage), len, addr));
    return addr.port();
}

[[nodiscard]] std::string send_http1_request(std::uint16_t port, std::string_view host, std::string_view path) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return {};
    }
    timeval tv{.tv_sec = 5, .tv_usec = 0};
    (void) ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    (void) ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    (void) ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return {};
    }

    std::string req = "GET ";
    req.append(path);
    req.append(" HTTP/1.1\r\nHost: ");
    req.append(host);
    req.append("\r\nConnection: close\r\n\r\n");

    std::size_t sent = 0;
    while (sent < req.size()) {
        const ssize_t n = ::send(fd, req.data() + sent, req.size() - sent, 0);
        if (n <= 0) {
            ::close(fd);
            return {};
        }
        sent += static_cast<std::size_t>(n);
    }

    std::string resp;
    char buf[4096];
    for (;;) {
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) {
            break;
        }
        resp.append(buf, static_cast<std::size_t>(n));
    }
    ::close(fd);
    return resp;
}

[[nodiscard]] std::string wait_for_http_body_contains(std::uint16_t port, std::string_view host, std::string_view path,
                                                      std::string_view expected,
                                                      std::chrono::milliseconds timeout = std::chrono::seconds(3)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::string last;
    while (std::chrono::steady_clock::now() < deadline) {
        last = send_http1_request(port, host, path);
        if (last.find(expected) != std::string::npos) {
            return last;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    return last;
}

[[nodiscard]] std::uint16_t allocate_ephemeral_port() {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    EXPECT_GE(fd, 0);
    int opt = 1;
    (void) ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    (void) ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    EXPECT_EQ(::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)), 0);
    const std::uint16_t port = bound_port_of(fd);
    ::close(fd);
    return port;
}

[[nodiscard]] std::expected<AccessServerConfig, AccessServerConfigError>
make_standalone_server_config(const std::filesystem::path &config_dir, bool tls_enabled = false,
                              std::uint64_t poll_interval_millis = 0) {
    const std::uint16_t plain_port = allocate_ephemeral_port();
    std::uint16_t metrics_port = allocate_ephemeral_port();
    while (metrics_port == plain_port) {
        metrics_port = allocate_ephemeral_port();
    }
    std::uint16_t tls_port = allocate_ephemeral_port();
    while (tls_port == plain_port || tls_port == metrics_port) {
        tls_port = allocate_ephemeral_port();
    }
    std::string env = "ACCESS_SERVER_NETWORK_ENTRY=internet\n"
                      "ACCESS_SERVER_CONFIG_PROVIDER=file\n"
                      "ACCESS_SERVER_CONFIG_DIR=" +
                      config_dir.string() +
                      "\n"
                      "ACCESS_SERVER_CONFIG_POLL_INTERVAL_MILLIS=" +
                      std::to_string(poll_interval_millis) +
                      "\n"
                      "ACCESS_SERVER_TLS_ENABLED=" +
                      std::string(tls_enabled ? "true" : "false") +
                      "\n"
                      "ACCESS_SERVER_TLS_LISTEN_ADDRESS=127.0.0.1\n"
                      "ACCESS_SERVER_TLS_LISTEN_PORT=" +
                      std::to_string(tls_port) +
                      "\n"
                      "ACCESS_SERVER_HTTP3_ENABLED=false\n"
                      "ACCESS_SERVER_PLAIN_LISTEN_ENABLED=true\n"
                      "ACCESS_SERVER_PLAIN_LISTEN_ADDRESS=127.0.0.1\n"
                      "ACCESS_SERVER_PLAIN_LISTEN_PORT=" +
                      std::to_string(plain_port) +
                      "\n"
                      "ACCESS_SERVER_METRICS_LISTEN_ADDRESS=127.0.0.1\n"
                      "ACCESS_SERVER_METRICS_LISTEN_PORT=" +
                      std::to_string(metrics_port) +
                      "\n"
                      "ACCESS_SERVER_ACCESS_LOG_DIR=\n"
                      "ACCESS_SERVER_INITIAL_CONFIG_TIMEOUT_MILLIS=5000\n";
    return AccessServerConfig::load_from_string(env);
}

TEST(LocalFileConfigYamlTest, ConvertsRouteYamlWithBlockScalarAndFlowToValidProjectConfig) {
    constexpr std::string_view kYaml = R"(
# Standalone route config in YAML
version: 3
host:
  demo.local: {}
  "*.demo.local": {}
routes:
  - path: /health
    method: GET
    type: RESPONSE
    status: 200
    response_headers:
      X-Source: local-yaml
      X-Colon-Url: http://127.0.0.1:8080/status
    body:
      type: TEXT
      content: healthy
  - path: /script
    type: SCRIPT
    script: |
      return {
        ok: true,
        source: "yaml"
      };
)";

    auto json = convert_yaml_to_json(kYaml);
    ASSERT_TRUE(json) << "line=" << json.error().line << " error=" << json.error().message;

    auto parsed = parse_project_config(*json);
    ASSERT_TRUE(parsed) << parsed.error().message;
    ASSERT_TRUE(parsed->has_value());
    EXPECT_EQ((*parsed)->version, 3);
    ASSERT_TRUE((*parsed)->hosts.has_value());
    EXPECT_EQ((*parsed)->hosts->size(), 2U);
    ASSERT_TRUE((*parsed)->routes.has_value());
    ASSERT_EQ((*parsed)->routes->size(), 2U);
    ASSERT_TRUE((*(*parsed)->routes)[0].has_value());
    ASSERT_TRUE((*(*parsed)->routes)[1].has_value());
    EXPECT_EQ((*(*parsed)->routes)[0]->path, "/health");
    EXPECT_EQ((*(*parsed)->routes)[0]->type, RouteType::Response);
    EXPECT_EQ((*(*parsed)->routes)[1]->type, RouteType::Script);
    ASSERT_TRUE((*(*parsed)->routes)[1]->script.has_value());
    EXPECT_NE((*(*parsed)->routes)[1]->script->find("source: \"yaml\""), std::string::npos);
}

TEST(LocalFileConfigYamlTest, RejectsInvalidYamlSyntax) {
    auto tab_err = convert_yaml_to_json("version: 1\n\troutes: []\n");
    EXPECT_FALSE(tab_err);

    auto quote_err = convert_yaml_to_json("version: \"unterminated\n");
    EXPECT_FALSE(quote_err);

    auto anchor_err = convert_yaml_to_json("base: &a\n  x: 1\n");
    EXPECT_FALSE(anchor_err);
}

TEST(LocalFileConfigSourceTest, AutoDerivesProjectsAndRejectsAmbiguousDuplicates) {
    TempConfigDir dir;
    dir.write_file("routes/alpha.yaml", R"(
version: 1
host:
  alpha.local: {}
routes:
  - path: /
    type: RESPONSE
    status: 200
    body:
      type: TEXT
      content: alpha
)");
    dir.write_file(
            "routes/beta.json",
            R"({"version":1,"host":{"beta.local":{}},"routes":[{"path":"/","type":"RESPONSE","status":200,"body":{"type":"TEXT","content":"beta"}}]})");

    LocalFileConfigSource source(dir.path().string());
    auto loaded = source.load();
    ASSERT_TRUE(loaded) << loaded.error().detail;

    const LocalFileConfigKey list_key{
            .data_id = std::string(kProjectListDataId),
            .group = std::string(kProjectRouteGroup),
    };
    ASSERT_TRUE(loaded->documents.contains(list_key));
    EXPECT_EQ(loaded->documents.at(list_key).content, "alpha;beta");
    EXPECT_EQ(loaded->documents.at(list_key).md5.size(), 32U);

    // Adding a duplicate format for the same project must fail closed.
    dir.write_file("routes/alpha.json", R"({"version":1})");
    auto conflict = source.load();
    EXPECT_FALSE(conflict);
}

TEST(LocalFileConfigSourceTest, RespectsExplicitProjectListFile) {
    TempConfigDir dir;
    dir.write_file("projects.txt", "# explicit project list\nbeta\nalpha\n");
    dir.write_file(
            "routes/alpha.json",
            R"({"version":1,"host":{"alpha.local":{}},"routes":[{"path":"/","type":"RESPONSE","status":200,"body":{"type":"TEXT","content":"alpha"}}]})");
    dir.write_file(
            "routes/beta.json",
            R"({"version":1,"host":{"beta.local":{}},"routes":[{"path":"/","type":"RESPONSE","status":200,"body":{"type":"TEXT","content":"beta"}}]})");

    LocalFileConfigSource source(dir.path().string());
    auto loaded = source.load();
    ASSERT_TRUE(loaded) << loaded.error().detail;

    const LocalFileConfigKey list_key{
            .data_id = std::string(kProjectListDataId),
            .group = std::string(kProjectRouteGroup),
    };
    ASSERT_TRUE(loaded->documents.contains(list_key));
    EXPECT_EQ(loaded->documents.at(list_key).content, "beta;alpha");
}

TEST(AccessServerConfigFileProviderTest, ParsesStandaloneFileProviderWithoutNacosServers) {
    auto config = AccessServerConfig::load_from_string(R"(
        ACCESS_SERVER_NETWORK_ENTRY=internet
        ACCESS_SERVER_CONFIG_PROVIDER=file
        ACCESS_SERVER_CONFIG_DIR=/etc/access-server/config
        ACCESS_SERVER_CONFIG_POLL_INTERVAL_MILLIS=0
    )");
    ASSERT_TRUE(config) << config.error().detail;
    EXPECT_EQ(config->config_provider(), AccessConfigProvider::File);
    EXPECT_EQ(config->config_dir(), "/etc/access-server/config");
    EXPECT_EQ(config->config_poll_interval(), std::chrono::milliseconds(0));
    EXPECT_FALSE(config->has_nacos_config());
}

TEST(AccessServerConfigFileProviderTest, RejectsInvalidProviderCombinations) {
    auto missing_dir = AccessServerConfig::load_from_string(R"(
        ACCESS_SERVER_NETWORK_ENTRY=internet
        ACCESS_SERVER_CONFIG_PROVIDER=file
    )");
    ASSERT_FALSE(missing_dir);
    EXPECT_EQ(missing_dir.error().code, AccessServerConfigErrorCode::MissingRequiredKey);

    auto dir_in_nacos_mode = AccessServerConfig::load_from_string(R"(
        ACCESS_SERVER_NETWORK_ENTRY=internet
        NACOS_SERVER_ADDRESSES=127.0.0.1
        ACCESS_SERVER_CONFIG_DIR=/tmp/cfg
    )");
    ASSERT_FALSE(dir_in_nacos_mode);
    EXPECT_EQ(dir_in_nacos_mode.error().code, AccessServerConfigErrorCode::InvalidValue);

    auto bad_poll = AccessServerConfig::load_from_string(R"(
        ACCESS_SERVER_NETWORK_ENTRY=internet
        ACCESS_SERVER_CONFIG_PROVIDER=file
        ACCESS_SERVER_CONFIG_DIR=/tmp/cfg
        ACCESS_SERVER_CONFIG_POLL_INTERVAL_MILLIS=50
    )");
    ASSERT_FALSE(bad_poll);
    EXPECT_EQ(bad_poll.error().code, AccessServerConfigErrorCode::InvalidValue);
}

TEST(LocalFileConfigRuntimeTest, FailsClosedOnMissingDirectoryOrInvalidInitialConfig) {
    // 1. Missing directory fails closed at config service startup.
    {
        const std::filesystem::path missing =
                std::filesystem::temp_directory_path() / ("access-server-missing-dir-" + std::to_string(::getpid()));
        std::error_code ec;
        std::filesystem::remove_all(missing, ec);

        auto cfg = make_standalone_server_config(missing, false, 0);
        ASSERT_TRUE(cfg) << cfg.error().detail;

        event::EventLoop accept_loop;
        event::EventLoopGroup workers(1);
        event::EventLoopGroup config_group(1);
        event::EventLoopGroup cat_group(1);
        auto runtime = AccessServerRuntime::create(accept_loop, config_group.at(0), cat_group.at(0), workers, *cfg);
        ASSERT_TRUE(runtime) << runtime.error().message;

        workers.start();
        config_group.start();
        cat_group.start();

        std::optional<AccessServerRuntimeErrorCode> err_code;
        async::spawn(accept_loop, [&]() -> async::DetachedTask {
            auto started = co_await (*runtime)->start();
            if (!started) {
                err_code = started.error().code;
            }
            co_await (*runtime)->shutdown();
            accept_loop.stop();
        });
        accept_loop.run();
        workers.stop();
        config_group.stop();
        cat_group.stop();
        workers.join();
        config_group.join();
        cat_group.join();

        ASSERT_TRUE(err_code.has_value());
        EXPECT_EQ(*err_code, AccessServerRuntimeErrorCode::StartConfigService);
    }

    // 2. Invalid route schema fails closed on initial startup.
    {
        TempConfigDir dir;
        dir.write_file("routes/broken.yaml", R"(
version: 1
host:
  broken.local: {}
routes:
  - path: /
    type: NOT_A_VALID_TYPE
)");
        auto cfg = make_standalone_server_config(dir.path(), false, 0);
        ASSERT_TRUE(cfg) << cfg.error().detail;

        event::EventLoop accept_loop;
        event::EventLoopGroup workers(1);
        event::EventLoopGroup config_group(1);
        event::EventLoopGroup cat_group(1);
        auto runtime = AccessServerRuntime::create(accept_loop, config_group.at(0), cat_group.at(0), workers, *cfg);
        ASSERT_TRUE(runtime) << runtime.error().message;

        workers.start();
        config_group.start();
        cat_group.start();

        std::optional<AccessServerRuntimeErrorCode> err_code;
        async::spawn(accept_loop, [&]() -> async::DetachedTask {
            auto started = co_await (*runtime)->start();
            if (!started) {
                err_code = started.error().code;
            }
            co_await (*runtime)->shutdown();
            accept_loop.stop();
        });
        accept_loop.run();
        workers.stop();
        config_group.stop();
        cat_group.stop();
        workers.join();
        config_group.join();
        cat_group.join();

        ASSERT_TRUE(err_code.has_value());
        EXPECT_EQ(*err_code, AccessServerRuntimeErrorCode::InitialConfigUnavailable);
    }

    // 3. TLS enabled without tls-certificates file fails closed.
    {
        TempConfigDir dir;
        dir.write_file(
                "routes/demo.json",
                R"({"version":1,"host":{"demo.local":{}},"routes":[{"path":"/","type":"RESPONSE","status":200,"body":{"type":"TEXT","content":"ok"}}]})");
        auto cfg = make_standalone_server_config(dir.path(), true, 0);
        ASSERT_TRUE(cfg) << cfg.error().detail;

        event::EventLoop accept_loop;
        event::EventLoopGroup workers(1);
        event::EventLoopGroup config_group(1);
        event::EventLoopGroup cat_group(1);
        auto runtime = AccessServerRuntime::create(accept_loop, config_group.at(0), cat_group.at(0), workers, *cfg);
        ASSERT_TRUE(runtime) << runtime.error().message;

        workers.start();
        config_group.start();
        cat_group.start();

        std::optional<AccessServerRuntimeErrorCode> err_code;
        async::spawn(accept_loop, [&]() -> async::DetachedTask {
            auto started = co_await (*runtime)->start();
            if (!started) {
                err_code = started.error().code;
            }
            co_await (*runtime)->shutdown();
            accept_loop.stop();
        });
        accept_loop.run();
        workers.stop();
        config_group.stop();
        cat_group.stop();
        workers.join();
        config_group.join();
        cat_group.join();

        ASSERT_TRUE(err_code.has_value());
        EXPECT_EQ(*err_code, AccessServerRuntimeErrorCode::InitialTlsCertificateUnavailable);
    }
}

TEST(LocalFileConfigRuntimeTest, SupportsValidHotReloadAndRetainsOldSnapshotOnInvalidReload) {
    TempConfigDir dir;
    dir.write_file("routes/demo.yaml", R"(
version: 1
host:
  demo.local: {}
routes:
  - path: /hello
    method: GET
    type: RESPONSE
    status: 200
    body:
      type: TEXT
      content: ok-v1
)");

    auto cfg = make_standalone_server_config(dir.path(), false, 0);
    ASSERT_TRUE(cfg) << cfg.error().detail;

    event::EventLoop accept_loop;
    event::EventLoopGroup workers(1);
    event::EventLoopGroup config_group(1);
    event::EventLoopGroup cat_group(1);
    auto runtime = AccessServerRuntime::create(accept_loop, config_group.at(0), cat_group.at(0), workers, *cfg);
    ASSERT_TRUE(runtime) << runtime.error().message;

    workers.start();
    config_group.start();
    cat_group.start();

    std::atomic<std::uint16_t> plain_port{0};
    std::atomic<bool> started_ok{false};
    std::atomic<bool> start_finished{false};

    std::thread accept_thread([&]() {
        async::spawn(accept_loop, [&]() -> async::DetachedTask {
            auto started = co_await (*runtime)->start();
            if (started) {
                plain_port.store(bound_port_of((*runtime)->plain_fd()), std::memory_order_release);
                started_ok.store(true, std::memory_order_release);
            }
            start_finished.store(true, std::memory_order_release);
            co_return;
        });
        accept_loop.run();
    });

    while (!start_finished.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_TRUE(started_ok.load(std::memory_order_acquire));
    const std::uint16_t port = plain_port.load(std::memory_order_acquire);
    ASSERT_NE(port, 0U);

    auto trigger_reload = [&]() {
        std::atomic<bool> posted{false};
        async::spawn(accept_loop, [&]() -> async::DetachedTask {
            (*runtime)->trigger_config_reload();
            posted.store(true, std::memory_order_release);
            co_return;
        });
        while (!posted.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    };

    // Initial snapshot serves ok-v1.
    const std::string v1 = send_http1_request(port, "demo.local", "/hello");
    EXPECT_NE(v1.find("200 OK"), std::string::npos) << v1;
    EXPECT_NE(v1.find("ok-v1"), std::string::npos) << v1;

    // 1. Valid YAML hot reload (version 2) + newly added JSON project.
    dir.write_file("routes/demo.yaml", R"(
version: 2
host:
  demo.local: {}
routes:
  - path: /hello
    method: GET
    type: RESPONSE
    status: 200
    body:
      type: TEXT
      content: ok-v2
)");
    dir.write_file(
            "routes/extra.json",
            R"({"version":1,"host":{"extra.local":{}},"routes":[{"path":"/ping","type":"RESPONSE","status":200,"body":{"type":"TEXT","content":"pong"}}]})");
    trigger_reload();

    const std::string v2 = wait_for_http_body_contains(port, "demo.local", "/hello", "ok-v2");
    EXPECT_NE(v2.find("ok-v2"), std::string::npos) << v2;
    const std::string extra = wait_for_http_body_contains(port, "extra.local", "/ping", "pong");
    EXPECT_NE(extra.find("pong"), std::string::npos) << extra;

    // 2. Invalid YAML syntax on reload must retain active ok-v2 snapshot.
    dir.write_file("routes/demo.yaml", "version: 3\n\tbroken_tab: true\n");
    trigger_reload();
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    const std::string retained_after_yaml_err = send_http1_request(port, "demo.local", "/hello");
    EXPECT_NE(retained_after_yaml_err.find("200 OK"), std::string::npos) << retained_after_yaml_err;
    EXPECT_NE(retained_after_yaml_err.find("ok-v2"), std::string::npos) << retained_after_yaml_err;

    // 3. Syntactically valid YAML with invalid route schema must retain active ok-v2 snapshot.
    dir.write_file("routes/demo.yaml", R"(
version: 3
host:
  demo.local: {}
routes:
  - path: /hello
    type: INVALID_ROUTE_TYPE
)");
    trigger_reload();
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    const std::string retained_after_schema_err = send_http1_request(port, "demo.local", "/hello");
    EXPECT_NE(retained_after_schema_err.find("200 OK"), std::string::npos) << retained_after_schema_err;
    EXPECT_NE(retained_after_schema_err.find("ok-v2"), std::string::npos) << retained_after_schema_err;

    // 4. Removing extra.json unloads extra.local while recovering demo.yaml to version 4.
    dir.remove_file("routes/extra.json");
    dir.write_file("routes/demo.yaml", R"(
version: 4
host:
  demo.local: {}
routes:
  - path: /hello
    method: GET
    type: RESPONSE
    status: 200
    body:
      type: TEXT
      content: ok-v4
)");
    trigger_reload();

    const std::string v4 = wait_for_http_body_contains(port, "demo.local", "/hello", "ok-v4");
    EXPECT_NE(v4.find("ok-v4"), std::string::npos) << v4;
    const std::string extra_removed = wait_for_http_body_contains(port, "extra.local", "/ping", "404");
    EXPECT_NE(extra_removed.find("404"), std::string::npos) << extra_removed;

    // Clean ordered shutdown on accept_loop.
    async::spawn(accept_loop, [&]() -> async::DetachedTask {
        co_await (*runtime)->shutdown();
        accept_loop.stop();
    });
    accept_thread.join();

    workers.stop();
    config_group.stop();
    cat_group.stop();
    workers.join();
    config_group.join();
    cat_group.join();
}

TEST(LocalFileConfigRuntimeTest, PollsDirectoryChangesAutomaticallyWhenPollIntervalEnabled) {
    TempConfigDir dir;
    dir.write_file(
            "routes/demo.json",
            R"({"version":1,"host":{"poll.local":{}},"routes":[{"path":"/","type":"RESPONSE","status":200,"body":{"type":"TEXT","content":"poll-v1"}}]})");

    auto cfg = make_standalone_server_config(dir.path(), false, 100);
    ASSERT_TRUE(cfg) << cfg.error().detail;

    event::EventLoop accept_loop;
    event::EventLoopGroup workers(1);
    event::EventLoopGroup config_group(1);
    event::EventLoopGroup cat_group(1);
    auto runtime = AccessServerRuntime::create(accept_loop, config_group.at(0), cat_group.at(0), workers, *cfg);
    ASSERT_TRUE(runtime) << runtime.error().message;

    workers.start();
    config_group.start();
    cat_group.start();

    std::atomic<std::uint16_t> plain_port{0};
    std::atomic<bool> started_ok{false};
    std::atomic<bool> start_finished{false};

    std::thread accept_thread([&]() {
        async::spawn(accept_loop, [&]() -> async::DetachedTask {
            auto started = co_await (*runtime)->start();
            if (started) {
                plain_port.store(bound_port_of((*runtime)->plain_fd()), std::memory_order_release);
                started_ok.store(true, std::memory_order_release);
            }
            start_finished.store(true, std::memory_order_release);
            co_return;
        });
        accept_loop.run();
    });

    while (!start_finished.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_TRUE(started_ok.load(std::memory_order_acquire));
    const std::uint16_t port = plain_port.load(std::memory_order_acquire);

    const std::string initial = send_http1_request(port, "poll.local", "/");
    EXPECT_NE(initial.find("poll-v1"), std::string::npos) << initial;

    // Update file on disk without manual trigger; periodic poll must pick it up.
    dir.write_file(
            "routes/demo.json",
            R"({"version":2,"host":{"poll.local":{}},"routes":[{"path":"/","type":"RESPONSE","status":200,"body":{"type":"TEXT","content":"poll-v2"}}]})");
    const std::string updated = wait_for_http_body_contains(port, "poll.local", "/", "poll-v2");
    EXPECT_NE(updated.find("poll-v2"), std::string::npos) << updated;

    async::spawn(accept_loop, [&]() -> async::DetachedTask {
        co_await (*runtime)->shutdown();
        accept_loop.stop();
    });
    accept_thread.join();

    workers.stop();
    config_group.stop();
    cat_group.stop();
    workers.join();
    config_group.join();
    cat_group.join();
}

} // namespace
} // namespace fiber::access_server
