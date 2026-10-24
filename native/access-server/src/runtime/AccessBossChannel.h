#ifndef FIBER_ACCESS_SERVER_ACCESS_BOSS_CHANNEL_H
#define FIBER_ACCESS_SERVER_ACCESS_BOSS_CHANNEL_H

#include "AccessControlProtocol.h"
#include "AccessRuntimeCoordinator.h"

#include <optional>
#include <string>
#include <string_view>

#include <fiber/async/Spawn.h>
#include <fiber/async/Task.h>
#include <fiber/async/WaitGroup.h>
#include <fiber/async/Watch.h>
#include <fiber/common/IoError.h>
#include <fiber/event/EventLoop.h>
#include <fiber/net/UnixStream.h>

namespace fiber::access_server {

class AccessServerConfig;

// Borrowed owner-loop callbacks; tests drive the channel without opening Nacos
// or real traffic listeners.
struct AccessBossChildActions {
    void *context = nullptr;
    AccessServerRuntimeState (*state)(void *) noexcept = nullptr;
    bool (*ready)(void *) noexcept = nullptr;
    std::string_view (*registration)(void *) noexcept = nullptr;
    async::Task<common::IoResult<void>> (*activate)(void *, bool background_registration) noexcept = nullptr;
    async::Task<common::IoResult<void>> (*release)(void *) noexcept = nullptr;
};

struct AccessBossChannelOptions {
    // Digest of non-secret settings two generations must share for a handoff.
    std::string compatibility;
    bool http3 = false;
    bool fixed_ports = true;
    // Called once on the owner loop when the boss end closes or breaks, never
    // after a local shutdown. The process uses it to bound its own stop.
    void (*boss_lost)() noexcept = nullptr;
};

[[nodiscard]] AccessBossChannelOptions access_boss_channel_options(const AccessServerConfig &config);
[[nodiscard]] std::string_view access_process_state_name(AccessServerRuntimeState state) noexcept;

// Ties this process to the boss that created the inherited socketpair end:
// requires the peer to be the parent, arms PR_SET_PDEATHSIG(SIGTERM), and
// re-checks the parent so a boss that already exited is not missed.
[[nodiscard]] common::IoResult<void> bind_access_boss_lifetime(int fd) noexcept;

// Serves boss commands (STATUS, ACTIVATE, RELEASE) on the inherited channel.
// Owner-loop only. The boss is the client: each request gets exactly one reply.
class AccessBossChannel final {
public:
    AccessBossChannel(event::EventLoop &loop, int fd, AccessBossChildActions actions, AccessBossChannelOptions options);
    ~AccessBossChannel();
    AccessBossChannel(const AccessBossChannel &) = delete;
    AccessBossChannel &operator=(const AccessBossChannel &) = delete;

    void start();
    // Completes once the process should stop: after RELEASE, a failed
    // activation, or loss of the boss.
    [[nodiscard]] async::Task<void> wait_exit() noexcept;
    // Rejects further mutating commands and waits for running ones.
    [[nodiscard]] async::Task<void> quiesce() noexcept;
    [[nodiscard]] async::Task<void> shutdown() noexcept;
    [[nodiscard]] AccessControlMessage status() const;
    [[nodiscard]] bool failed() const noexcept { return failed_; }
    [[nodiscard]] bool boss_lost() const noexcept { return boss_lost_; }

private:
    [[nodiscard]] async::DetachedTask serve();
    [[nodiscard]] async::DetachedTask activate(bool background_registration);
    [[nodiscard]] async::Task<AccessControlMessage> handle(AccessControlMessage request);
    void request_exit() noexcept;

    event::EventLoop &loop_;
    AccessBossChildActions actions_;
    AccessBossChannelOptions options_;
    net::UnixStream stream_;
    async::WaitGroup tasks_;
    async::WaitGroup operations_;
    async::Watch<bool> exit_{false};
    std::optional<async::Watch<bool>::Publisher> exit_publisher_;
    // idle -> activating -> serving | failed; serving -> released.
    std::string phase_ = "idle";
    bool started_ = false;
    bool stopping_ = false;
    bool quiescing_ = false;
    bool releasing_ = false;
    bool failed_ = false;
    bool boss_lost_ = false;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_ACCESS_BOSS_CHANNEL_H
