#include "observability/AccessServerLogCategories.h"
#include "runtime/AccessBoss.h"
#include "runtime/AccessBossLauncher.h"
#include "runtime/AccessProcessArguments.h"

#include <csignal>
#include <iostream>
#include <string_view>
#include <utility>
#include <vector>

#include <pthread.h>
#include <sys/wait.h>

#include <fiber/async/Signal.h>
#include <fiber/async/Spawn.h>
#include <fiber/async/WaitGroup.h>
#include <fiber/async/Watch.h>
#include <fiber/async/WhenAny.h>
#include <fiber/common/IoError.h>
#include <fiber/event/EventLoop.h>
#include <fiber/event/SignalService.h>
#include <fiber/log/Log.h>
#include <fiber/log/LogConfig.h>
#include <fiber/log/LoggerManager.h>

namespace {

DEFINE_LOGGER(LOG_BOSS, fiber::access_server::kAccessServerBossLogger);

constexpr std::string_view kUsage = "usage: access-server-boss --socket=<path> --server=<path> --config=<path> "
                                    "[--prewarm-timeout-millis=120000]\n";

fiber::log::LogConfigResult<fiber::log::LogConfig> make_log_config() {
    fiber::log::LogConfigBuilder builder;
    auto console = builder.add_console_appender({
            .name = "access_server_boss_stderr",
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
    auto root = builder.set_root_logger({.level = fiber::log::LogLevel::Info}, {*console});
    if (!root) {
        return std::unexpected(std::move(root.error()));
    }
    return builder.finish();
}

class LoggingShutdownGuard {
public:
    ~LoggingShutdownGuard() { fiber::log::LoggerManager::global().shutdown(); }
};

// Reaps every exited child after each SIGCHLD (signals coalesce) until `done`.
fiber::async::DetachedTask reap_children(fiber::access_server::AccessBoss &boss, fiber::async::Watch<bool> &done,
                                         fiber::async::WaitGroup &tasks) {
    auto subscriber = done.subscribe();
    for (;;) {
        int status = 0;
        pid_t pid = 0;
        while ((pid = ::waitpid(-1, &status, WNOHANG)) > 0) {
            boss.child_exited(pid, status);
        }
        auto snapshot = subscriber.current();
        if (snapshot.value && *snapshot.value) {
            break;
        }
        auto woke = co_await fiber::async::when_any(
                []() { return fiber::async::wait_signal(SIGCHLD); },
                [&subscriber, version = snapshot.version]() { return subscriber.next(version); });
        if (woke.is<0>()) {
            (void) std::move(woke).get<0>();
        } else {
            (void) std::move(woke).get<1>();
        }
    }
    tasks.done();
}

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
    auto parsed = fiber::access_server::parse_access_boss_arguments(arguments);
    if (!parsed) {
        std::cerr << kUsage;
        return 2;
    }
    const auto launch = std::move(*parsed);

    // Children get a clean mask and default SIGPIPE back in the launcher.
    (void) ::signal(SIGPIPE, SIG_IGN);
    fiber::async::SignalSet signals;
    signals.add(SIGCHLD).add(SIGINT).add(SIGTERM);
    if (::pthread_sigmask(SIG_BLOCK, &signals.native(), nullptr) != 0) {
        std::cerr << "failed to block boss signals\n";
        return 1;
    }
    auto log_config = make_log_config();
    if (!log_config) {
        std::cerr << "failed to build boss logging configuration: " << log_config.error().message << '\n';
        return 1;
    }
    auto logging_started = fiber::log::LoggerManager::global().initialize(std::move(*log_config));
    if (!logging_started) {
        std::cerr << "failed to initialize boss logging: " << logging_started.error().message << '\n';
        return 1;
    }
    LoggingShutdownGuard logging_guard;

    fiber::event::EventLoop loop;
    fiber::access_server::AccessBossProcessLauncher launcher(launch.server_path, launch.config_path);
    fiber::access_server::AccessBoss boss(loop, launcher,
                                          {
                                                  .socket_path = launch.socket_path,
                                                  .server_path = launch.server_path,
                                                  .prewarm_timeout = launch.prewarm_timeout,
                                          });
    if (auto initialized = boss.initialize(); !initialized) {
        std::cerr << "failed to initialize boss control socket " << launch.socket_path << ": "
                  << fiber::common::io_err_name(initialized.error()) << '\n';
        return 1;
    }
    std::cout << "access-server-boss control socket=" << boss.path() << std::endl;

    fiber::event::SignalService signal_service(loop);
    fiber::async::Watch<bool> reaper_done{false};
    auto reaper_done_publisher = reaper_done.acquire_publisher();
    fiber::async::WaitGroup tasks;
    int exit_code = 0;
    fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
        if (!signal_service.attach(signals)) {
            std::cerr << "failed to attach boss signal service\n";
            exit_code = 1;
            loop.stop();
            co_return;
        }
        tasks.add();
        fiber::async::spawn(loop, [&]() { return reap_children(boss, reaper_done, tasks); });
        boss.start();
        auto stop = co_await fiber::async::when_any([]() { return fiber::async::wait_signal(SIGTERM); },
                                                    []() { return fiber::async::wait_signal(SIGINT); });
        if (stop.is<0>()) {
            (void) std::move(stop).get<0>();
        } else {
            (void) std::move(stop).get<1>();
        }
        LOG(LOG_BOSS, INFO) << "shutdown signal received";
        // Every child is reaped before stop() completes, so the reaper outlives it.
        co_await boss.stop();
        reaper_done_publisher->publish(true);
        co_await tasks.join();
        signal_service.detach();
        loop.stop();
    });
    loop.run();
    return exit_code;
}
