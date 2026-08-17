#ifndef FIBER_ACCESS_SERVER_GZIP_ENCODER_H
#define FIBER_ACCESS_SERVER_GZIP_ENCODER_H

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace fiber::access_server {

enum class GzipEncodeError : std::uint8_t {
    InvalidLevel,
    NoMemory,
    CompressionFailed,
};

using GzipEncodeResult = std::expected<std::string, GzipEncodeError>;

// Produces one deterministic RFC 1952 member. The header carries no file
// metadata and uses an OS-independent value so immutable route snapshots have
// stable bytes across supported Linux hosts.
[[nodiscard]] GzipEncodeResult gzip_encode(std::string_view input, int level);

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_GZIP_ENCODER_H
