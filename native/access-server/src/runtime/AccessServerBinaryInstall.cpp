#include "AccessServerBinaryInstall.h"

#include <array>
#include <cerrno>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace fiber::access_server {
namespace {

struct TargetPath {
    std::string directory;
    std::string name;
};

common::IoResult<TargetPath> split_target(const std::string &target) {
    const auto slash = target.find_last_of('/');
    if (target.empty() || target.front() != '/' || target.find('\0') != std::string::npos ||
        slash == std::string::npos || slash + 1 == target.size()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    return TargetPath{
            .directory = slash == 0 ? "/" : target.substr(0, slash),
            .name = target.substr(slash + 1),
    };
}

std::string sibling(const TargetPath &path, std::string_view purpose) {
    // Hidden, PID-qualified names never collide with a concurrent CLI.
    return (path.directory == "/" ? "" : path.directory) + "/." + path.name + "." + std::string(purpose) + "-" +
           std::to_string(::getpid());
}

common::IoResult<void> sync_directory(const std::string &directory) noexcept {
    const int fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        return std::unexpected(common::io_err_from_errno(errno));
    }
    const int synced = ::fsync(fd);
    const int error = errno;
    ::close(fd);
    if (synced != 0) {
        return std::unexpected(common::io_err_from_errno(error));
    }
    return {};
}

common::IoResult<void> copy_file(int source, int target) noexcept {
    std::array<char, 64 * 1024> buffer{};
    for (;;) {
        const ssize_t read = ::read(source, buffer.data(), buffer.size());
        if (read < 0 && errno == EINTR) {
            continue;
        }
        if (read < 0) {
            return std::unexpected(common::io_err_from_errno(errno));
        }
        if (read == 0) {
            return {};
        }
        std::size_t offset = 0;
        while (offset < static_cast<std::size_t>(read)) {
            const ssize_t written = ::write(target, buffer.data() + offset, static_cast<std::size_t>(read) - offset);
            if (written < 0 && errno == EINTR) {
                continue;
            }
            if (written <= 0) {
                return std::unexpected(common::io_err_from_errno(written < 0 ? errno : EIO));
            }
            offset += static_cast<std::size_t>(written);
        }
    }
}

// Writes a durable, executable copy of `source` at `temporary`.
common::IoResult<void> write_copy(const std::string &source, const std::string &temporary,
                                  const struct stat &target_stat, bool target_exists) noexcept {
    const int input = ::open(source.c_str(), O_RDONLY | O_CLOEXEC);
    if (input < 0) {
        return std::unexpected(common::io_err_from_errno(errno));
    }
    struct stat input_stat{};
    if (::fstat(input, &input_stat) != 0 || !S_ISREG(input_stat.st_mode) ||
        (target_exists && input_stat.st_dev == target_stat.st_dev && input_stat.st_ino == target_stat.st_ino)) {
        ::close(input);
        return std::unexpected(common::IoErr::Invalid);
    }
    const int output = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0700);
    if (output < 0) {
        const int error = errno;
        ::close(input);
        return std::unexpected(common::io_err_from_errno(error));
    }
    auto copied = copy_file(input, output);
    // The boss refuses binaries that group/other can write.
    if (copied && (::fchmod(output, 0755) != 0 || ::fsync(output) != 0)) {
        copied = std::unexpected(common::io_err_from_errno(errno));
    }
    ::close(input);
    if (::close(output) != 0 && copied) {
        copied = std::unexpected(common::io_err_from_errno(errno));
    }
    return copied;
}

} // namespace

std::string access_server_binary_backup_path(const std::string &target) { return target + ".prev"; }

common::IoResult<void> install_access_server_binary(const std::string &source, const std::string &target) noexcept {
    auto path = split_target(target);
    if (!path) {
        return std::unexpected(path.error());
    }
    struct stat target_stat{};
    const bool target_exists = ::lstat(target.c_str(), &target_stat) == 0;
    if (target_exists && !S_ISREG(target_stat.st_mode)) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (!target_exists && errno != ENOENT) {
        return std::unexpected(common::io_err_from_errno(errno));
    }
    const std::string temporary = sibling(*path, "install");
    auto written = write_copy(source, temporary, target_stat, target_exists);
    if (!written) {
        (void) ::unlink(temporary.c_str());
        return written;
    }
    if (target_exists) {
        // Link first, then rename: an existing backup is replaced atomically and
        // the target never disappears.
        const std::string backup = sibling(*path, "prev");
        if (::link(target.c_str(), backup.c_str()) != 0 ||
            ::rename(backup.c_str(), access_server_binary_backup_path(target).c_str()) != 0) {
            const int error = errno;
            (void) ::unlink(backup.c_str());
            (void) ::unlink(temporary.c_str());
            return std::unexpected(common::io_err_from_errno(error));
        }
    }
    if (::rename(temporary.c_str(), target.c_str()) != 0) {
        const int error = errno;
        (void) ::unlink(temporary.c_str());
        return std::unexpected(common::io_err_from_errno(error));
    }
    return sync_directory(path->directory);
}

common::IoResult<void> restore_access_server_binary(const std::string &target) noexcept {
    auto path = split_target(target);
    if (!path) {
        return std::unexpected(path.error());
    }
    const std::string backup = access_server_binary_backup_path(target);
    struct stat backup_stat{};
    if (::lstat(backup.c_str(), &backup_stat) != 0) {
        return std::unexpected(common::io_err_from_errno(errno));
    }
    if (!S_ISREG(backup_stat.st_mode)) {
        return std::unexpected(common::IoErr::Invalid);
    }
    const std::string temporary = sibling(*path, "restore");
    if (::link(backup.c_str(), temporary.c_str()) != 0 || ::rename(temporary.c_str(), target.c_str()) != 0) {
        const int error = errno;
        (void) ::unlink(temporary.c_str());
        return std::unexpected(common::io_err_from_errno(error));
    }
    return sync_directory(path->directory);
}

} // namespace fiber::access_server
