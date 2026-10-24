#ifndef FIBER_ACCESS_SERVER_ACCESS_SERVER_BINARY_INSTALL_H
#define FIBER_ACCESS_SERVER_ACCESS_SERVER_BINARY_INSTALL_H

#include <string>

#include <fiber/common/IoError.h>

namespace fiber::access_server {

// "<target>.prev": the binary an install replaced, kept for restore.
[[nodiscard]] std::string access_server_binary_backup_path(const std::string &target);

// Copies `source` next to the absolute `target`, fsyncs it, hard-links the
// current target to the backup path and renames the copy over the target. The
// target path exists at every step; running processes keep their old inode.
[[nodiscard]] common::IoResult<void> install_access_server_binary(const std::string &source,
                                                                  const std::string &target) noexcept;

// Atomically puts the backup back at `target`; the backup itself stays.
[[nodiscard]] common::IoResult<void> restore_access_server_binary(const std::string &target) noexcept;

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_ACCESS_SERVER_BINARY_INSTALL_H
