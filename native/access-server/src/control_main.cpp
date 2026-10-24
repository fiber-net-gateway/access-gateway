#include "runtime/AccessControlProtocol.h"
#include "runtime/AccessServerBinaryInstall.h"

#include <chrono>
#include <csignal>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <unistd.h>

#include <fiber/async/Spawn.h>
#include <fiber/event/EventLoop.h>

namespace {

using namespace fiber;
using namespace fiber::access_server;

constexpr std::string_view kUsage = "usage: access-serverctl status|abort --socket <boss.sock>\n"
                                    "       access-serverctl upgrade --socket <boss.sock> [--binary <new-binary>]\n";
constexpr auto kPollInterval = std::chrono::milliseconds(100);
// Beyond the boss prewarm deadline: release, activation and CLI polling.
constexpr auto kUpgradeWaitMargin = std::chrono::seconds(60);

// One request on a private loop. File work in this CLI stays outside any loop.
common::IoResult<AccessControlMessage> call(const std::string &socket, std::string_view command,
                                            std::string upgrade_id = {}) {
    event::EventLoop loop;
    common::IoResult<AccessControlMessage> result = std::unexpected(common::IoErr::Unknown);
    AccessControlMessage request;
    request.request_id = "ctl-" + std::to_string(::getpid());
    request.command = command;
    request.upgrade_id = std::move(upgrade_id);
    async::spawn(loop, [&]() -> async::DetachedTask {
        result = co_await request_access_control(loop, socket, std::move(request));
        loop.stop();
    });
    loop.run();
    return result;
}

void print(const AccessControlMessage &message) {
    if (auto json = encode_access_control(message)) {
        std::cout << *json << '\n';
    }
}

// Only has to differ between CLI runs against one boss, not be unpredictable:
// a PID paired with the monotonic clock never repeats before a reboot, which
// also restarts the boss. Avoids getrandom(2) (glibc 2.25, Linux 3.17).
std::string make_upgrade_id() {
    const auto uptime = std::chrono::steady_clock::now().time_since_epoch();
    return "ctl-" + std::to_string(::getpid()) + "-" +
           std::to_string(std::chrono::duration_cast<std::chrono::nanoseconds>(uptime).count());
}

// The request was never delivered when the boss could not even be reached.
bool undelivered(common::IoErr error) noexcept {
    return error == common::IoErr::NotFound || error == common::IoErr::ConnRefused ||
           error == common::IoErr::Permission;
}

bool upgrade_running(std::string_view phase) noexcept {
    return phase != "idle" && phase != "serving" && phase != "failed";
}

void restore(const std::string &target) {
    if (auto restored = restore_access_server_binary(target); restored) {
        std::cerr << "restored previous binary at " << target << '\n';
    } else {
        std::cerr << "failed to restore " << target << " from " << access_server_binary_backup_path(target) << ": "
                  << common::io_err_name(restored.error()) << '\n';
    }
}

int upgrade(const std::string &socket, const std::string &binary) {
    auto current = call(socket, "STATUS");
    if (!current) {
        std::cerr << "boss unreachable: " << common::io_err_name(current.error()) << '\n';
        return 1;
    }
    // Refuse before touching any file.
    if (upgrade_running(current->phase) || current->active_state != "serving" || current->draining_pid != 0 ||
        current->candidate_pid != 0) {
        std::cerr << "boss is not ready for an upgrade (phase=" << current->phase << " active=" << current->active_state
                  << " draining_pid=" << current->draining_pid << ")\n";
        print(*current);
        return 1;
    }
    const std::string target = current->server_path;
    const bool installed = !binary.empty();
    if (installed) {
        if (auto result = install_access_server_binary(binary, target); !result) {
            std::cerr << "failed to install " << binary << " at " << target << ": "
                      << common::io_err_name(result.error()) << '\n';
            return 1;
        }
        std::cerr << "installed " << binary << " at " << target << ", previous kept at "
                  << access_server_binary_backup_path(target) << '\n';
    }
    const auto upgrade_id = make_upgrade_id();
    auto accepted = call(socket, "UPGRADE", upgrade_id);
    if (!accepted) {
        if (undelivered(accepted.error())) {
            std::cerr << "boss unreachable: " << common::io_err_name(accepted.error()) << '\n';
            if (installed) {
                restore(target);
            }
        } else {
            std::cerr << "upgrade request result unknown (" << common::io_err_name(accepted.error())
                      << "); binary left in place, query status before retrying\n";
        }
        return 1;
    }
    if (!accepted->error.empty()) {
        std::cerr << "upgrade rejected: " << accepted->error << '\n';
        if (installed) {
            restore(target);
        }
        print(*accepted);
        return 1;
    }
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(accepted->prewarm_timeout_millis) + kUpgradeWaitMargin;
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(kPollInterval);
        auto status = call(socket, "STATUS");
        // Transient failures are retried until the deadline; the task runs in the boss.
        if (!status || status->upgrade_id != upgrade_id || upgrade_running(status->phase)) {
            continue;
        }
        print(*status);
        if (status->phase == "serving") {
            return 0;
        }
        std::cerr << "upgrade failed: " << status->upgrade_error << '\n';
        if (installed) {
            restore(target);
        }
        return 1;
    }
    std::cerr << "upgrade result unknown after waiting; binary left in place, query status before retrying\n";
    return 1;
}

} // namespace

int main(int argc, char **argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        std::cout << kUsage;
        return 0;
    }
    if (argc < 4) {
        std::cerr << kUsage;
        return 2;
    }
    const std::string_view command(argv[1]);
    std::string socket;
    std::string binary;
    bool valid = (argc - 2) % 2 == 0;
    for (int i = 2; valid && i + 1 < argc; i += 2) {
        const std::string_view flag(argv[i]);
        std::string *target = flag == "--socket" ? &socket : flag == "--binary" ? &binary : nullptr;
        if (!target || !target->empty() || std::string_view(argv[i + 1]).empty()) {
            valid = false;
            break;
        }
        *target = argv[i + 1];
    }
    if (!valid || socket.empty() || (command != "upgrade" && !binary.empty()) ||
        (command != "status" && command != "upgrade" && command != "abort")) {
        std::cerr << kUsage;
        return 2;
    }
    (void) ::signal(SIGPIPE, SIG_IGN);
    if (command == "upgrade") {
        return upgrade(socket, binary);
    }
    auto response = call(socket, command == "status" ? "STATUS" : "ABORT");
    if (!response) {
        std::cerr << "control request failed: " << common::io_err_name(response.error()) << '\n';
        return 1;
    }
    print(*response);
    if (!response->error.empty()) {
        std::cerr << command << " failed: " << response->error << '\n';
        return 1;
    }
    return 0;
}
