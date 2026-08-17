#include "GzipEncoder.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>

#include <zlib.h>

namespace fiber::access_server {
namespace {

constexpr std::size_t kOutputChunkBytes = 16U << 10U;

GzipEncodeError gzip_error(int code) noexcept {
    return code == Z_MEM_ERROR ? GzipEncodeError::NoMemory : GzipEncodeError::CompressionFailed;
}

void append_output(std::string &output, const std::array<unsigned char, kOutputChunkBytes> &buffer,
                   const z_stream &stream) {
    const std::size_t produced = buffer.size() - stream.avail_out;
    if (produced != 0) {
        output.append(reinterpret_cast<const char *>(buffer.data()), produced);
    }
}

} // namespace

GzipEncodeResult gzip_encode(std::string_view input, int level) {
    if (level < 1 || level > 9) {
        return std::unexpected(GzipEncodeError::InvalidLevel);
    }

    z_stream stream{};
    int result = deflateInit2(&stream, level, Z_DEFLATED, MAX_WBITS + 16, 8, Z_DEFAULT_STRATEGY);
    if (result != Z_OK) {
        return std::unexpected(gzip_error(result));
    }

    gz_header header{};
    header.os = 255;
    result = deflateSetHeader(&stream, &header);
    if (result != Z_OK) {
        (void) deflateEnd(&stream);
        return std::unexpected(gzip_error(result));
    }

    std::string output;
    std::array<unsigned char, kOutputChunkBytes> buffer{};
    std::size_t consumed = 0;
    while (consumed < input.size()) {
        const std::size_t remaining = input.size() - consumed;
        const std::size_t chunk = std::min<std::size_t>(remaining, std::numeric_limits<uInt>::max());
        stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(input.data() + consumed));
        stream.avail_in = static_cast<uInt>(chunk);
        while (stream.avail_in != 0) {
            stream.next_out = buffer.data();
            stream.avail_out = static_cast<uInt>(buffer.size());
            result = deflate(&stream, Z_NO_FLUSH);
            if (result != Z_OK) {
                (void) deflateEnd(&stream);
                return std::unexpected(gzip_error(result));
            }
            append_output(output, buffer, stream);
        }
        consumed += chunk;
    }

    do {
        stream.next_out = buffer.data();
        stream.avail_out = static_cast<uInt>(buffer.size());
        result = deflate(&stream, Z_FINISH);
        if (result != Z_OK && result != Z_STREAM_END) {
            (void) deflateEnd(&stream);
            return std::unexpected(gzip_error(result));
        }
        append_output(output, buffer, stream);
    } while (result != Z_STREAM_END);

    result = deflateEnd(&stream);
    if (result != Z_OK) {
        return std::unexpected(gzip_error(result));
    }
    return output;
}

} // namespace fiber::access_server
