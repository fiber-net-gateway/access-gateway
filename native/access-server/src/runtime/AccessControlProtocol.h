#ifndef FIBER_ACCESS_SERVER_ACCESS_CONTROL_PROTOCOL_H
#define FIBER_ACCESS_SERVER_ACCESS_CONTROL_PROTOCOL_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include <fiber/async/Task.h>
#include <fiber/common/IoError.h>
#include <fiber/event/EventLoop.h>
#include <fiber/net/UnixStream.h>

namespace fiber::access_server {

inline constexpr std::size_t kAccessControlMaxFrame = 8192;
inline constexpr auto kAccessControlTimeout = std::chrono::milliseconds(2000);

// Cold-path, bounded local administration messages. Never include route content,
// credentials, or free-form exception/error strings here.
struct AccessControlMessage {
    std::int64_t protocol_version = 1;
    std::int64_t pid = 0;
    std::string request_id;
    std::string command;
    std::string boot_id;
    std::string expected_boot_id;
    std::string upgrade_id;
    std::string old_socket;
    std::string old_boot_id;
    std::string state;
    std::string phase;
    std::string registration;
    std::string compatibility;
    std::string log_identity;
    std::string error;
    std::string previous_socket;
    std::string previous_boot_id;
    bool ready = false;
    bool http3 = false;
};

[[nodiscard]] common::IoResult<AccessControlMessage> decode_access_control(std::string_view input);
[[nodiscard]] common::IoResult<std::string> encode_access_control(const AccessControlMessage &message);
[[nodiscard]] async::Task<common::IoResult<AccessControlMessage>> read_access_control(net::UnixStream &stream);
[[nodiscard]] async::Task<common::IoResult<void>> write_access_control(net::UnixStream &stream,
                                                                       const AccessControlMessage &message);
[[nodiscard]] common::IoResult<void> validate_access_control_peer(int fd) noexcept;
[[nodiscard]] async::Task<common::IoResult<AccessControlMessage>> exchange_access_control(net::UnixStream &stream,
                                                                                          AccessControlMessage request);
[[nodiscard]] async::Task<common::IoResult<AccessControlMessage>>
request_access_control(event::EventLoop &loop, std::string path, AccessControlMessage request);

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_ACCESS_CONTROL_PROTOCOL_H
