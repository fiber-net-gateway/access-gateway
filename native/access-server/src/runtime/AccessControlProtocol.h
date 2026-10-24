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

inline constexpr std::int64_t kAccessControlProtocolVersion = 2;
inline constexpr std::size_t kAccessControlMaxFrame = 8192;
inline constexpr auto kAccessControlTimeout = std::chrono::milliseconds(2000);

// Cold-path, bounded local administration messages shared by ctl <-> boss and
// boss <-> access-server. Never include route content, credentials, or
// free-form exception/error strings here; errors are stable codes.
struct AccessControlMessage {
    std::int64_t protocol_version = kAccessControlProtocolVersion;
    std::int64_t pid = 0;
    std::int64_t active_pid = 0;
    std::int64_t draining_pid = 0;
    std::int64_t candidate_pid = 0;
    std::int64_t prewarm_timeout_millis = 0;
    std::string request_id;
    std::string command;
    std::string upgrade_id;
    std::string state;
    std::string phase;
    std::string registration;
    std::string compatibility;
    std::string error;
    std::string upgrade_error;
    std::string server_path;
    std::string active_state;
    std::string active_registration;
    std::string candidate_state;
    bool ready = false;
    bool http3 = false;
    bool fixed_ports = false;
    bool background = false;
};

[[nodiscard]] common::IoResult<AccessControlMessage> decode_access_control(std::string_view input);
[[nodiscard]] common::IoResult<std::string> encode_access_control(const AccessControlMessage &message);
// Reads one length-prefixed frame. `frame_started`, when provided, reports
// whether any header byte arrived: a TimedOut with no byte consumed means the
// stream merely reported a stale readiness edge (the fd was marked ready by an
// earlier exchange and never drained to EAGAIN), not a protocol failure — the
// caller keeps waiting instead of dropping the channel.
[[nodiscard]] async::Task<common::IoResult<AccessControlMessage>> read_access_control(net::UnixStream &stream,
                                                                                      bool *frame_started = nullptr);
[[nodiscard]] async::Task<common::IoResult<void>> write_access_control(net::UnixStream &stream,
                                                                       const AccessControlMessage &message);
// Accepts a peer with the same effective UID, or when either side is root:
// operators usually run access-serverctl as root against the service user's boss.
[[nodiscard]] common::IoResult<void> validate_access_control_peer(int fd) noexcept;
[[nodiscard]] async::Task<common::IoResult<AccessControlMessage>> exchange_access_control(net::UnixStream &stream,
                                                                                          AccessControlMessage request);
[[nodiscard]] async::Task<common::IoResult<AccessControlMessage>>
request_access_control(event::EventLoop &loop, std::string path, AccessControlMessage request);

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_ACCESS_CONTROL_PROTOCOL_H
