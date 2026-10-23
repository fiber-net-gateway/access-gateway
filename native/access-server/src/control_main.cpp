#include "runtime/AccessControlProtocol.h"

#include <csignal>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

#include <unistd.h>

#include <fiber/async/Sleep.h>
#include <fiber/async/Spawn.h>

namespace {

using namespace fiber;
using namespace fiber::access_server;

constexpr std::string_view kUsage =
        "usage: access-serverctl status|stop|abort --socket <path>\n"
        "       access-serverctl upgrade --old-socket <path> --new-socket <path> [--wait]\n";

async::Task<common::IoResult<AccessControlMessage>> query(event::EventLoop &loop, const std::string &path) {
    AccessControlMessage request;
    request.request_id = "ctl-status";
    request.command = "STATUS";
    co_return co_await request_access_control(loop, path, std::move(request));
}

void print(const AccessControlMessage &message) {
    auto json = encode_access_control(message);
    if (json) {
        std::cout << *json << '\n';
    }
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
    const std::string command(argv[1]);
    std::string socket;
    std::string old_socket;
    std::string new_socket;
    bool wait = false;
    bool valid = true;
    for (int i = 2; i < argc; ++i) {
        const std::string_view flag(argv[i]);
        if (flag == "--wait" && !wait) {
            wait = true;
            continue;
        }
        if (i + 1 >= argc) {
            valid = false;
            break;
        }
        std::string *target = flag == "--socket"       ? &socket
                              : flag == "--old-socket" ? &old_socket
                              : flag == "--new-socket" ? &new_socket
                                                       : nullptr;
        if (!target || !target->empty()) {
            valid = false;
            break;
        }
        *target = argv[++i];
    }
    const bool upgrade = command == "upgrade";
    if (!valid ||
        (upgrade ? old_socket.empty() || new_socket.empty() || !socket.empty()
                 : socket.empty() || !old_socket.empty() || !new_socket.empty() || wait) ||
        (command != "upgrade" && command != "status" && command != "stop" && command != "abort")) {
        std::cerr << kUsage;
        return 2;
    }
    (void) ::signal(SIGPIPE, SIG_IGN);
    event::EventLoop loop;
    int exit_code = 1;
    async::spawn(loop, [&]() -> async::DetachedTask {
        // Every path completes before stopping the loop; this wrapper owns all
        // client tasks and their Unix streams.
        auto run = [&]() -> async::Task<void> {
            auto target = co_await query(loop, upgrade ? new_socket : socket);
            if (!target) {
                std::cerr << "control request failed: " << common::io_err_name(target.error()) << '\n';
                co_return;
            }
            if (command == "status") {
                print(*target);
                exit_code = 0;
                co_return;
            }
            AccessControlMessage request;
            request.request_id = "ctl-command";
            request.expected_boot_id = target->boot_id;
            request.upgrade_id = target->boot_id + "-" + std::to_string(::getpid());
            if (upgrade) {
                const auto deadline = loop.now() + std::chrono::seconds(120);
                while (!target->ready && target->state != "serving" && target->state != "stopped") {
                    if (loop.now() >= deadline) {
                        std::cerr << "prewarm wait timed out\n";
                        co_return;
                    }
                    co_await async::sleep(std::chrono::milliseconds(100));
                    target = co_await query(loop, new_socket);
                    if (!target || target->boot_id != request.expected_boot_id) {
                        std::cerr << "new process changed during prewarm\n";
                        co_return;
                    }
                }
                if (!target->ready) {
                    std::cerr << "new process is not prepared\n";
                    co_return;
                }
                auto old = co_await query(loop, old_socket);
                if (!old) {
                    std::cerr << "old process is unreachable\n";
                    co_return;
                }
                request.command = "TAKEOVER";
                request.old_socket = old_socket;
                request.old_boot_id = old->boot_id;
            } else {
                request.command = command == "stop" && target->state == "serving" ? "RELEASE" : "ABORT";
            }
            auto response = co_await request_access_control(loop, upgrade ? new_socket : socket, request);
            if (!response) {
                std::cerr << "command result unconfirmed: " << common::io_err_name(response.error()) << '\n';
                co_return;
            }
            if (!response->error.empty()) {
                print(*response);
                co_return;
            }
            if (upgrade && wait) {
                const auto deadline = loop.now() + std::chrono::seconds(30);
                while (response->phase != "serving" && response->phase != "failed") {
                    if (loop.now() >= deadline) {
                        std::cerr << "upgrade wait timed out; query status before retrying\n";
                        co_return;
                    }
                    co_await async::sleep(std::chrono::milliseconds(20));
                    response = co_await query(loop, new_socket);
                    if (!response || response->boot_id != request.expected_boot_id ||
                        response->upgrade_id != request.upgrade_id) {
                        std::cerr << "upgrade result unconfirmed\n";
                        co_return;
                    }
                }
            }
            print(*response);
            exit_code = response->error.empty() ? 0 : 1;
        };
        co_await run();
        loop.stop();
    });
    loop.run();
    return exit_code;
}
