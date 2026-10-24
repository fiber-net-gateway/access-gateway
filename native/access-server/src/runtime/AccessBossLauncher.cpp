#include "AccessBossLauncher.h"

#include <array>
#include <cerrno>
#include <csignal>
#include <utility>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

extern char **environ;

namespace fiber::access_server {
namespace {

constexpr char kBossFdArgument[] = "--boss-fd=3";
static_assert(kAccessBossChildChannelFd == 3);

[[noreturn]] void exec_child(int binary_fd, int channel_fd, char *const *argv) noexcept {
    // Between fork and exec only async-signal-safe calls are allowed.
    if (binary_fd == kAccessBossChildChannelFd) {
        binary_fd = ::fcntl(binary_fd, F_DUPFD_CLOEXEC, kAccessBossChildChannelFd + 1);
        if (binary_fd < 0) {
            ::_exit(127);
        }
    }
    if (channel_fd == kAccessBossChildChannelFd) {
        if (::fcntl(channel_fd, F_SETFD, 0) != 0) {
            ::_exit(127);
        }
    } else if (::dup2(channel_fd, kAccessBossChildChannelFd) < 0) {
        ::_exit(127);
    }
    // The boss blocks its signals for signalfd and ignores SIGPIPE; neither may
    // leak into the child, which installs its own handling.
    sigset_t none;
    ::sigemptyset(&none);
    (void) ::sigprocmask(SIG_SETMASK, &none, nullptr);
    (void) ::signal(SIGPIPE, SIG_DFL);
    ::fexecve(binary_fd, argv, environ);
    ::_exit(127);
}

} // namespace

common::IoResult<int> open_access_server_binary(const std::string &path) noexcept {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return std::unexpected(common::io_err_from_errno(errno));
    }
    struct stat st{};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || (st.st_mode & 0111U) == 0 || (st.st_mode & 022U) != 0) {
        ::close(fd);
        return std::unexpected(common::IoErr::Permission);
    }
    return fd;
}

AccessBossProcessLauncher::AccessBossProcessLauncher(std::string server_path, std::string config_path) :
    server_path_(std::move(server_path)), config_path_(std::move(config_path)) {}

common::IoResult<int> AccessBossProcessLauncher::open_binary() noexcept {
    return open_access_server_binary(server_path_);
}

common::IoResult<AccessBossChild> AccessBossProcessLauncher::spawn(int binary_fd) noexcept {
    std::array<int, 2> pair{-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, pair.data()) != 0) {
        return std::unexpected(common::io_err_from_errno(errno));
    }
    // Built before fork: the child must not allocate.
    std::array<char *, 4> argv{const_cast<char *>(server_path_.c_str()), const_cast<char *>(config_path_.c_str()),
                               const_cast<char *>(kBossFdArgument), nullptr};
    const pid_t pid = ::fork();
    if (pid < 0) {
        const int error = errno;
        ::close(pair[0]);
        ::close(pair[1]);
        return std::unexpected(common::io_err_from_errno(error));
    }
    if (pid == 0) {
        exec_child(binary_fd, pair[1], argv.data());
    }
    ::close(pair[1]);
    return AccessBossChild{.pid = pid, .channel_fd = pair[0]};
}

void AccessBossProcessLauncher::terminate(pid_t pid) noexcept { (void) ::kill(pid, SIGTERM); }

} // namespace fiber::access_server
