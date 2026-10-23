#ifndef FIBER_ACCESS_SERVER_ACCESS_PROCESS_CONTROL_H
#define FIBER_ACCESS_SERVER_ACCESS_PROCESS_CONTROL_H

#include "AccessControlProtocol.h"
#include "AccessRuntimeCoordinator.h"

#include <array>
#include <memory>
#include <optional>

#include <fiber/async/Spawn.h>
#include <fiber/async/WaitGroup.h>
#include <fiber/async/Watch.h>
#include <fiber/net/UnixListener.h>

namespace fiber::access_server {

class AccessServerConfig;
class AccessServerRuntime;

// Borrowed owner-loop callbacks also allow deterministic lifecycle tests without
// opening a database, Nacos, or real traffic listener.
struct AccessProcessActions {
    void *context = nullptr;
    AccessServerRuntimeState (*state)(void *) noexcept = nullptr;
    bool (*ready)(void *) noexcept = nullptr;
    std::string_view (*registration)(void *) noexcept = nullptr;
    async::Task<common::IoResult<void>> (*activate)(void *) noexcept = nullptr;
    async::Task<common::IoResult<void>> (*release)(void *) noexcept = nullptr;
};

struct AccessProcessControlOptions {
    std::string directory;
    std::string compatibility;
    std::string log_identity;
    bool http3 = false;
    bool fixed_ports = true;
};

[[nodiscard]] AccessProcessActions access_process_actions(AccessServerRuntime &runtime) noexcept;
[[nodiscard]] common::IoResult<AccessProcessControlOptions>
access_process_control_options(const AccessServerConfig &config, std::string directory);
[[nodiscard]] std::string_view access_process_state_name(AccessServerRuntimeState state) noexcept;

class AccessProcessControl final {
public:
    AccessProcessControl(event::EventLoop &loop, AccessProcessActions actions, AccessProcessControlOptions options);
    ~AccessProcessControl();
    AccessProcessControl(const AccessProcessControl &) = delete;
    AccessProcessControl &operator=(const AccessProcessControl &) = delete;

    // Initialize filesystem state before running the EventLoop. Does not unlink
    // another live socket, and records the inode used for cleanup.
    [[nodiscard]] common::IoResult<void> initialize();
    void start();
    [[nodiscard]] async::Task<void> quiesce() noexcept;
    [[nodiscard]] async::Task<void> shutdown() noexcept;
    [[nodiscard]] async::Task<void> wait_exit() noexcept;
    [[nodiscard]] AccessControlMessage status() const;
    [[nodiscard]] const std::string &path() const noexcept { return path_; }
    [[nodiscard]] bool failed() const noexcept { return !job_error_.empty(); }

private:
    [[nodiscard]] async::DetachedTask accept_connections();
    [[nodiscard]] async::DetachedTask serve_connection(std::size_t slot);
    [[nodiscard]] async::DetachedTask takeover(AccessControlMessage request);
    [[nodiscard]] async::Task<AccessControlMessage> handle(AccessControlMessage request);
    [[nodiscard]] async::Task<bool> previous_is_drained(const AccessControlMessage &old);
    void request_exit();
    void cleanup_path() noexcept;

    event::EventLoop &loop_;
    AccessProcessActions actions_;
    AccessProcessControlOptions options_;
    net::UnixListener listener_;
    std::array<std::unique_ptr<net::UnixStream>, 16> streams_;
    async::WaitGroup tasks_;
    async::WaitGroup operations_;
    async::Watch<bool> exit_{false};
    std::optional<async::Watch<bool>::Publisher> exit_publisher_;
    std::string path_;
    std::string boot_id_;
    std::string upgrade_id_;
    std::string old_socket_;
    std::string old_boot_id_;
    std::string release_id_;
    std::string phase_ = "idle";
    std::string job_error_;
    std::uint64_t socket_device_ = 0;
    std::uint64_t socket_inode_ = 0;
    int directory_fd_ = -1;
    int lock_fd_ = -1;
    bool started_ = false;
    bool stopping_ = false;
    bool quiescing_ = false;
    bool job_running_ = false;
    bool release_running_ = false;
    bool released_ = false;
    bool abort_requested_ = false;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_ACCESS_PROCESS_CONTROL_H
