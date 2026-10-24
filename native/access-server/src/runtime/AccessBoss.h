#ifndef FIBER_ACCESS_SERVER_ACCESS_BOSS_H
#define FIBER_ACCESS_SERVER_ACCESS_BOSS_H

#include "AccessBossLauncher.h"
#include "AccessControlProtocol.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <fiber/async/Spawn.h>
#include <fiber/async/Task.h>
#include <fiber/async/WaitGroup.h>
#include <fiber/async/Watch.h>
#include <fiber/event/EventLoop.h>
#include <fiber/net/UnixListener.h>

namespace fiber::access_server {

struct AccessBossOptions {
    // Control socket for access-serverctl; empty disables it (tests call handle()).
    std::string socket_path;
    // Reported to the CLI, which replaces this file before an upgrade.
    std::string server_path;
    std::chrono::milliseconds prewarm_timeout{120000};
    std::chrono::milliseconds restart_initial_delay{1000};
    std::chrono::milliseconds restart_max_delay{30000};
    std::chrono::milliseconds poll_interval{50};
    // Longest wait for an old generation to exit after its RELEASE reply was lost.
    std::chrono::milliseconds release_exit_timeout{10000};
};

// Single-threaded supervisor of access-server generations. Every member runs on
// the owner loop. Children are reported through child_exited() by whoever
// reaps them (the boss process reaps on SIGCHLD).
class AccessBoss final {
public:
    AccessBoss(event::EventLoop &loop, AccessBossLauncher &launcher, AccessBossOptions options);
    ~AccessBoss();
    AccessBoss(const AccessBoss &) = delete;
    AccessBoss &operator=(const AccessBoss &) = delete;

    // Binds the control socket before the loop runs. Never unlinks a live
    // endpoint, and records the inode it later removes.
    [[nodiscard]] common::IoResult<void> initialize();
    // Starts the control listener and the first (cold) generation.
    void start();
    void child_exited(pid_t pid, int wait_status) noexcept;
    [[nodiscard]] async::Task<AccessControlMessage> handle(AccessControlMessage request);
    // Abandons any upgrade, terminates every child and waits for all of them.
    [[nodiscard]] async::Task<void> stop() noexcept;
    [[nodiscard]] AccessControlMessage status() const;
    [[nodiscard]] const std::string &path() const noexcept { return options_.socket_path; }

private:
    struct Generation;
    class GenerationRef;
    enum class PollResult : std::uint8_t { Reached, Exited, Lost, Cancelled, TimedOut };
    using StatusPredicate = bool (*)(const AccessControlMessage &) noexcept;

    [[nodiscard]] Generation *spawn_generation(int binary_fd);
    [[nodiscard]] common::IoResult<int> cold_binary() noexcept;
    void collect(Generation &generation) noexcept;
    void mark_serving(Generation &generation) noexcept;
    void ensure_serving() noexcept;
    [[nodiscard]] bool other_generation_alive() const noexcept;
    [[nodiscard]] std::string abandon(Generation &generation, std::string_view error) noexcept;

    [[nodiscard]] async::Task<common::IoResult<AccessControlMessage>> exchange(Generation &generation,
                                                                               AccessControlMessage request);
    [[nodiscard]] async::Task<PollResult> poll(Generation &generation, StatusPredicate predicate,
                                               std::optional<std::chrono::steady_clock::time_point> deadline,
                                               bool abortable);
    [[nodiscard]] async::Task<bool> wait_exited(Generation &generation, std::chrono::milliseconds timeout);
    [[nodiscard]] async::Task<void> pause(std::chrono::milliseconds delay);
    [[nodiscard]] async::Task<void> refresh(Generation *generation);

    [[nodiscard]] async::DetachedTask cold_start(std::chrono::milliseconds delay);
    [[nodiscard]] async::DetachedTask run_cold(GenerationRef generation);
    [[nodiscard]] async::DetachedTask run_upgrade();
    [[nodiscard]] async::Task<std::string> upgrade();

    [[nodiscard]] async::DetachedTask accept_connections();
    [[nodiscard]] async::DetachedTask serve_connection(std::size_t slot);
    void cleanup_path() noexcept;

    event::EventLoop &loop_;
    AccessBossLauncher &launcher_;
    AccessBossOptions options_;
    net::UnixListener listener_;
    std::array<std::unique_ptr<net::UnixStream>, 16> streams_;
    std::vector<std::unique_ptr<Generation>> generations_;
    // The serving (or cold-starting) generation; null between crash and restart.
    Generation *active_ = nullptr;
    Generation *candidate_ = nullptr;
    std::vector<Generation *> draining_;
    async::WaitGroup control_tasks_;
    async::WaitGroup flows_;
    async::WaitGroup alive_;
    async::Watch<bool> stop_signal_{false};
    std::optional<async::Watch<bool>::Publisher> stop_publisher_;
    std::string upgrade_id_;
    std::string upgrade_phase_ = "idle";
    std::string upgrade_error_;
    std::chrono::milliseconds restart_delay_{0};
    std::uint64_t request_sequence_ = 0;
    std::uint64_t socket_device_ = 0;
    std::uint64_t socket_inode_ = 0;
    // Binary of the last generation that reached serving; restarts run this inode.
    int known_good_binary_ = -1;
    bool started_ = false;
    bool stopping_ = false;
    bool upgrade_running_ = false;
    bool abort_requested_ = false;
    bool cold_scheduled_ = false;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_ACCESS_BOSS_H
