#include "observability/AccessLogChannel.h"
#include "observability/AccessServerLogCategories.h"
#include "runtime/AccessProcessArguments.h"
#include "runtime/AccessProcessControl.h"
#include "runtime/AccessServerConfig.h"
#include "runtime/AccessServerRuntime.h"

#include <csignal>
#include <cstdint>
#include <cstring>
#include <expected>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <cerrno>
#include <fcntl.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>

#include <fiber/async/Signal.h>
#include <fiber/async/Sleep.h>
#include <fiber/async/Spawn.h>
#include <fiber/async/TaskSelect.h>
#include <fiber/async/Timeout.h>
#include <fiber/async/Watch.h>
#include <fiber/async/WhenAny.h>
#include <fiber/common/IoError.h>
#include <fiber/common/util/CpuConcurrency.h>
#include <fiber/event/EventLoop.h>
#include <fiber/event/EventLoopGroup.h>
#include <fiber/event/SignalService.h>
#include <fiber/log/Log.h>
#include <fiber/log/LogConfig.h>
#include <fiber/log/LoggerManager.h>
#include <fiber/net/SocketAddress.h>

namespace {

DEFINE_LOGGER(LOG_LIFECYCLE, fiber::access_server::kAccessServerLifecycleLogger);

constexpr std::string_view kUsage = "usage: access-server [config.env] [--start-mode=normal|prewarm] "
                                    "[--control-dir=/run/access-server/<instance>] [--prewarm-timeout-millis=120000]\n";

fiber::common::IoResult<std::uint16_t> bound_port(int fd) noexcept {
    sockaddr_storage storage{};
    socklen_t length = sizeof(storage);
    if (::getsockname(fd, reinterpret_cast<sockaddr *>(&storage), &length) != 0) {
        return std::unexpected(fiber::common::io_err_from_errno(errno));
    }
    fiber::net::SocketAddress address;
    if (!fiber::net::SocketAddress::from_sockaddr(reinterpret_cast<sockaddr *>(&storage), length, address)) {
        return std::unexpected(fiber::common::IoErr::Invalid);
    }
    return address.port();
}

void print_config_error(std::string_view path, const fiber::access_server::AccessServerConfigError &error) {
    std::cerr << "configuration error in " << path;
    if (error.line != 0) {
        std::cerr << ':' << error.line;
    }
    if (!error.key.empty()) {
        std::cerr << " [" << error.key << ']';
    }
    std::cerr << ": " << error.detail << '\n';
}

void print_runtime_error(const fiber::access_server::AccessServerRuntimeError &error) {
    std::cerr << "access-server startup failed at "
              << fiber::access_server::access_server_runtime_stage_name(error.code);
    if (error.io_error != fiber::common::IoErr::None) {
        std::cerr << ": " << fiber::common::io_err_name(error.io_error);
    }
    if (!error.message.empty()) {
        std::cerr << ": " << error.message;
    }
    std::cerr << '\n';
}

struct AccessLoggingConfig {
    fiber::log::LogConfig config;
    fiber::log::AppenderId primary_appender = fiber::log::kInvalidAppenderId;
    fiber::log::AppenderId access_appender = fiber::log::kInvalidAppenderId;
};

// A FileAppender open failure aborts LoggerManager::initialize (fatal), but the
// gateway must tolerate an unusable access-log directory: probe the path with
// the appender's own open flags first and simply disable the access log.
bool access_log_path_writable(const std::string &path) {
    const int descriptor = ::open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (descriptor < 0) {
        std::cerr << "access log disabled; cannot open " << path << ": errno=" << errno << '\n';
        return false;
    }
    (void) ::close(descriptor);
    return true;
}

fiber::log::LogConfigResult<AccessLoggingConfig> make_log_config(std::string_view access_log_directory) {
    fiber::log::LogConfigBuilder builder;
    auto console = builder.add_console_appender({
            .name = "access_server_stderr",
            .stream = fiber::log::ConsoleStream::Stderr,
    });
    if (!console) {
        return std::unexpected(std::move(console.error()));
    }
    auto queue = builder.set_async_options({
            .backlog_capacity = fiber::log::kDefaultLogBacklogCapacity,
            .full_policy = fiber::log::LogQueueFullPolicy::DropNewest,
    });
    if (!queue) {
        return std::unexpected(std::move(queue.error()));
    }
    auto root = builder.set_root_logger(
            {
                    .level = fiber::log::LogLevel::Info,
            },
            {*console});
    if (!root) {
        return std::unexpected(std::move(root.error()));
    }
    fiber::log::AppenderId access_appender = fiber::log::kInvalidAppenderId;
    if (!access_log_directory.empty()) {
        std::string path{access_log_directory};
        if (!path.ends_with('/')) {
            path.push_back('/');
        }
        path.append("access.log");
        if (access_log_path_writable(path)) {
            // Access lines are pre-encoded TSV records: MessageOnly layout
            // plus a raw (non-escaping) submission keeps the file bytes
            // identical to the Java format. The dedicated non-additive logger
            // keeps the traffic ledger out of the console and independent of
            // diagnostic level changes.
            auto file = builder.add_file_appender({
                    .name = "access_server_access_log",
                    .path = std::move(path),
                    .buffer_size = 63 << 10,
                    .flush_interval = std::chrono::milliseconds(1000),
                    .rotation =
                            fiber::log::FileRotationOptions{
                                    .max_file_size = 100ULL << 20,
                                    .archive_name = "{base}.{utc}.{seq}",
                                    .max_archives = 300,
                            },
                    .layout = fiber::log::FileAppenderLayout::MessageOnly,
            });
            if (!file) {
                return std::unexpected(std::move(file.error()));
            }
            auto access_logger = builder.add_logger(
                    {
                            .name = "access_server.access",
                            .level = fiber::log::LogLevel::Info,
                            .additive = false,
                    },
                    {*file});
            if (!access_logger) {
                return std::unexpected(std::move(access_logger.error()));
            }
            access_appender = *file;
        }
    }
    auto config = builder.finish();
    if (!config) {
        return std::unexpected(std::move(config.error()));
    }
    return AccessLoggingConfig{
            .config = std::move(*config),
            .primary_appender = *console,
            .access_appender = access_appender,
    };
}

class LoggingShutdownGuard {
public:
    ~LoggingShutdownGuard() { fiber::log::LoggerManager::global().shutdown(); }
};

} // namespace

int main(int argc, char **argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        std::cout << kUsage;
        return 0;
    }
    std::vector<std::string_view> arguments;
    for (int i = 1; i < argc; ++i) {
        arguments.emplace_back(argv[i]);
    }
    auto parsed_arguments = fiber::access_server::parse_access_process_arguments(arguments);
    if (!parsed_arguments) {
        std::cerr << kUsage;
        return 1;
    }
    const auto launch = std::move(*parsed_arguments);
    const std::string_view config_path = launch.config_path;
    auto loaded = fiber::access_server::AccessServerConfig::load_from_file(config_path);
    if (!loaded) {
        print_config_error(config_path, loaded.error());
        return 1;
    }
    fiber::access_server::AccessServerConfig config = std::move(*loaded);

    if (launch.prewarm && config.tls_http_server_options().http3_enabled) {
        std::cerr << "prewarm upgrade does not support HTTP/3; disable it explicitly before using this mode\n";
        return 1;
    }

    (void) ::signal(SIGPIPE, SIG_IGN);
    fiber::async::SignalSet shutdown_signals;
    shutdown_signals.add(SIGINT).add(SIGTERM);
    if (::pthread_sigmask(SIG_BLOCK, &shutdown_signals.native(), nullptr) != 0) {
        std::cerr << "failed to block shutdown signals\n";
        return 1;
    }

    auto log_config = make_log_config(config.access_log_directory());
    if (!log_config) {
        std::cerr << "failed to build access-server logging configuration: " << log_config.error().message << '\n';
        return 1;
    }
    const fiber::log::AppenderId primary_log_appender = log_config->primary_appender;
    const fiber::log::AppenderId access_log_appender = log_config->access_appender;
    auto logging_started = fiber::log::LoggerManager::global().initialize(std::move(log_config->config));
    if (!logging_started) {
        std::cerr << "failed to initialize access-server logging: " << logging_started.error().message << '\n';
        return 1;
    }
    LoggingShutdownGuard logging_guard;
    // Java-compatible access log rides the shared LogWorker pipeline, named
    // after this gateway's entry. An empty directory (or an unwritable one)
    // leaves the channel uninstalled; the executor then skips emission.
    if (access_log_appender != fiber::log::kInvalidAppenderId) {
        fiber::access_server::install_access_log_channel("access-server-" + std::string(config.network_entry()));
    }

    const fiber::util::CpuConcurrency cpu = fiber::util::detect_cpu_concurrency();
    fiber::event::EventLoop accept_loop;
    fiber::event::EventLoopGroup http_workers(cpu.effective_count);
    fiber::event::EventLoopGroup nacos_group(1);
    fiber::event::EventLoopGroup cat_group(1);
    const fiber::access_server::AccessProcessMetricsSources process_metrics{
            .logger = &fiber::log::LoggerManager::global(),
            .log_appender = primary_log_appender,
            .access_log_appender = access_log_appender,
    };
    auto created = fiber::access_server::AccessServerRuntime::create(accept_loop, nacos_group.at(0), cat_group.at(0),
                                                                     http_workers, config, fiber::net::ListenOptions{},
                                                                     process_metrics);
    if (!created) {
        print_runtime_error(created.error());
        return 1;
    }
    std::unique_ptr<fiber::access_server::AccessServerRuntime> runtime = std::move(*created);

    std::unique_ptr<fiber::access_server::AccessProcessControl> control;
    if (!launch.control_directory.empty()) {
        auto options = fiber::access_server::access_process_control_options(config, launch.control_directory);
        if (!options || (launch.prewarm && !options->fixed_ports)) {
            std::cerr << "invalid process-control configuration (fixed ports and existing log directory required)\n";
            return 1;
        }
        control = std::make_unique<fiber::access_server::AccessProcessControl>(
                accept_loop, fiber::access_server::access_process_actions(*runtime), std::move(*options));
        auto initialized = control->initialize();
        if (!initialized) {
            std::cerr << "failed to initialize process control: " << fiber::common::io_err_name(initialized.error())
                      << '\n';
            return 1;
        }
        std::cout << "access-server control socket=" << control->path() << std::endl;
    }

    http_workers.start(shutdown_signals);
    nacos_group.start(shutdown_signals);
    cat_group.start(shutdown_signals);

    fiber::event::SignalService signal_service(accept_loop);
    fiber::async::Watch<bool> no_control_exit{false};
    auto wait_control = [&]() -> fiber::async::Task<void> {
        if (control) {
            co_await control->wait_exit();
        } else {
            auto subscriber = no_control_exit.subscribe();
            auto snapshot = subscriber.current();
            (void) co_await subscriber.next(snapshot.version);
        }
    };
    auto stop_runtime = [&]() -> fiber::async::Task<void> {
        if (control) {
            co_await control->quiesce();
        }
        co_await runtime->shutdown();
        if (control) {
            co_await control->shutdown();
        }
    };
    auto wait_idle_prewarm = [&]() -> fiber::async::Task<void> {
        if (launch.prewarm) {
            co_await fiber::async::sleep(std::chrono::seconds(120));
            if (runtime->state() == fiber::access_server::AccessServerRuntimeState::Prepared &&
                control->status().phase == "idle") {
                co_return;
            }
        }
        auto subscriber = no_control_exit.subscribe();
        auto snapshot = subscriber.current();
        (void) co_await subscriber.next(snapshot.version);
    };
    using StartResult = std::expected<void, fiber::access_server::AccessServerRuntimeError>;
    auto prepare_runtime = [&]() -> fiber::async::Task<StartResult> {
        auto prepared = co_await runtime->prepare();
        if (!prepared) {
            co_return std::unexpected(std::move(prepared.error()));
        }
        while (!runtime->upgrade_ready()) {
            co_await fiber::async::sleep(std::chrono::milliseconds(10));
        }
        co_return StartResult{};
    };
    auto start_runtime = [&]() -> fiber::async::Task<StartResult> {
        if (!launch.prewarm) {
            co_return co_await runtime->start();
        }
        auto prepared = co_await fiber::async::timeout_for([&]() { return prepare_runtime().select(); },
                                                           launch.prewarm_timeout);
        if (!prepared) {
            co_return std::unexpected(fiber::access_server::make_access_server_runtime_io_error(
                    fiber::access_server::AccessServerRuntimeErrorCode::InitialConfigTimeout, prepared.error()));
        }
        co_return std::move(*prepared);
    };
    int exit_code = 0;
    fiber::async::spawn(accept_loop, [&]() -> fiber::async::DetachedTask {
        if (!signal_service.attach(shutdown_signals)) {
            LOG(LOG_LIFECYCLE, ERROR) << "failed to attach shutdown signal service";
            std::cerr << "failed to attach shutdown signal service\n";
            exit_code = 1;
            co_await stop_runtime();
            accept_loop.stop();
            co_return;
        }

        if (control) {
            control->start();
        }
        auto startup = co_await fiber::async::when_any(
                [&]() { return start_runtime().select(); }, []() { return fiber::async::wait_signal(SIGINT); },
                []() { return fiber::async::wait_signal(SIGTERM); }, [&]() { return wait_control().select(); });
        if (!startup.is<0>()) {
            if (startup.is<1>()) {
                (void) std::move(startup).get<1>();
            } else if (startup.is<2>()) {
                (void) std::move(startup).get<2>();
            } else {
                std::move(startup).get<3>();
            }
            co_await stop_runtime();
            signal_service.detach();
            accept_loop.stop();
            co_return;
        }

        auto started = std::move(startup).get<0>();
        if (!started) {
            print_runtime_error(started.error());
            exit_code = 1;
            co_await stop_runtime();
            signal_service.detach();
            accept_loop.stop();
            co_return;
        }

        if (!launch.prewarm) {
            auto metrics_port = bound_port(runtime->metrics_fd());
            if (!metrics_port) {
                std::cerr << "failed to resolve metrics listener port: "
                          << fiber::common::io_err_name(metrics_port.error()) << '\n';
                exit_code = 1;
                co_await stop_runtime();
                signal_service.detach();
                accept_loop.stop();
                co_return;
            }
            const fiber::net::SocketAddress metrics_address(config.metrics_listen_address().ip(), *metrics_port);
            const bool tls_enabled = config.tls_http_server_options().tls.enabled();
            const bool http3_enabled = config.tls_http_server_options().http3_enabled;
            const bool plain_enabled = config.plain_listen_enabled();

            std::optional<fiber::net::SocketAddress> tls_address;
            if (tls_enabled) {
                auto tls_port = bound_port(runtime->fd());
                if (!tls_port) {
                    std::cerr << "failed to resolve bound TLS listener port: "
                              << fiber::common::io_err_name(tls_port.error()) << '\n';
                    exit_code = 1;
                    co_await stop_runtime();
                    signal_service.detach();
                    accept_loop.stop();
                    co_return;
                }
                tls_address = fiber::net::SocketAddress(config.tls_listen_address().ip(), *tls_port);
            }
            std::optional<fiber::net::SocketAddress> plain_address;
            if (plain_enabled) {
                auto plain_port = bound_port(runtime->plain_fd());
                if (!plain_port) {
                    std::cerr << "failed to resolve bound plaintext listener port: "
                              << fiber::common::io_err_name(plain_port.error()) << '\n';
                    exit_code = 1;
                    co_await stop_runtime();
                    signal_service.detach();
                    accept_loop.stop();
                    co_return;
                }
                plain_address = fiber::net::SocketAddress(config.plain_listen_address().ip(), *plain_port);
            }

            std::cout << "access-server listening";
            if (tls_address) {
                std::cout << " on https://" << tls_address->to_string() << ", protocols=http/1.1,h2"
                          << (http3_enabled ? ",h3" : "");
            }
            if (plain_address) {
                std::cout << (tls_address ? " and" : " on") << " http://" << plain_address->to_string()
                          << ", protocols=http/1.1";
            }
            std::cout << ", metrics=http://" << metrics_address.to_string() << ", http workers=" << http_workers.size()
                      << ", nacos servers=" << config.nacos_config().server_hosts().size() << std::endl;
            std::string lifecycle_addresses;
            if (tls_address) {
                lifecycle_addresses += " tls_address=\"" + tls_address->to_string() + "\"";
            }
            if (plain_address) {
                lifecycle_addresses += " plain_address=\"" + plain_address->to_string() + "\"";
            }
            lifecycle_addresses += " metrics_address=\"" + metrics_address.to_string() + "\"";
            LOG(LOG_LIFECYCLE, INFO) << "server listening" << lifecycle_addresses
                                     << " http_workers=" << http_workers.size()
                                     << " cpu_affinity=" << cpu.affinity_count
                                     << " cpu_quota_workers=" << cpu.quota_count << " cpu_quota_us=" << cpu.quota_us
                                     << " cpu_period_us=" << cpu.period_us << " cpu_concurrency_source="
                                     << fiber::util::cpu_concurrency_source_name(cpu.source)
                                     << " cgroup_probe_failed=" << cpu.cgroup_probe_failed
                                     << " nacos_servers=" << config.nacos_config().server_hosts().size()
                                     << " cat_enabled=" << config.cat_config().has_value()
                                     << " tls_enabled=" << tls_enabled << " http2_enabled=" << tls_enabled
                                     << " http3_enabled=" << http3_enabled << " plain_enabled=" << plain_enabled
                                     << " network_entry=\"" << config.network_entry() << "\"";
        } else {
            std::cout << "access-server prepared pid=" << ::getpid() << std::endl;
        }

        auto signal = co_await fiber::async::when_any(
                []() { return fiber::async::wait_signal(SIGINT); }, []() { return fiber::async::wait_signal(SIGTERM); },
                [&]() { return wait_control().select(); }, [&]() { return wait_idle_prewarm().select(); });
        if (signal.is<0>()) {
            (void) std::move(signal).get<0>();
        } else if (signal.is<1>()) {
            (void) std::move(signal).get<1>();
        } else if (signal.is<2>()) {
            std::move(signal).get<2>();
        } else {
            std::move(signal).get<3>();
            std::cerr << "idle prewarm expired without a takeover request\n";
            exit_code = 1;
        }
        co_await stop_runtime();
        signal_service.detach();
        accept_loop.stop();
    });
    accept_loop.run();

    http_workers.stop();
    nacos_group.stop();
    cat_group.stop();
    http_workers.join();
    nacos_group.join();
    cat_group.join();
    // Drops the access-log channel after every request worker has stopped
    // submitting lines; the LoggingShutdownGuard drain flushes the tail.
    fiber::access_server::uninstall_access_log_channel();
    return exit_code;
}
