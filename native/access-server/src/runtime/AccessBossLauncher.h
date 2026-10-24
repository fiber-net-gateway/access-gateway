#ifndef FIBER_ACCESS_SERVER_ACCESS_BOSS_LAUNCHER_H
#define FIBER_ACCESS_SERVER_ACCESS_BOSS_LAUNCHER_H

#include <string>

#include <sys/types.h>

#include <fiber/common/IoError.h>

namespace fiber::access_server {

// Descriptor the child finds its end of the boss socketpair on.
inline constexpr int kAccessBossChildChannelFd = 3;

struct AccessBossChild {
    pid_t pid = -1;
    // Boss end of the socketpair, nonblocking and close-on-exec. Owned by the caller.
    int channel_fd = -1;
};

// Process operations behind the boss state machine, so tests can supervise
// in-process fake children without fork/exec.
class AccessBossLauncher {
public:
    virtual ~AccessBossLauncher() = default;
    // Opens and validates the configured server binary. The descriptor pins
    // that inode: later replacement of the path does not change what it runs.
    [[nodiscard]] virtual common::IoResult<int> open_binary() noexcept = 0;
    // Starts a child from `binary_fd` (borrowed) with a fresh channel on fd 3.
    [[nodiscard]] virtual common::IoResult<AccessBossChild> spawn(int binary_fd) noexcept = 0;
    // Asks the child for a graceful stop (SIGTERM).
    virtual void terminate(pid_t pid) noexcept = 0;
};

// Requires a regular, executable file that group/other cannot write.
[[nodiscard]] common::IoResult<int> open_access_server_binary(const std::string &path) noexcept;

class AccessBossProcessLauncher final : public AccessBossLauncher {
public:
    AccessBossProcessLauncher(std::string server_path, std::string config_path);

    [[nodiscard]] common::IoResult<int> open_binary() noexcept override;
    [[nodiscard]] common::IoResult<AccessBossChild> spawn(int binary_fd) noexcept override;
    void terminate(pid_t pid) noexcept override;

private:
    std::string server_path_;
    std::string config_path_;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_ACCESS_BOSS_LAUNCHER_H
