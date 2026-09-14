#include "GzipEncoder.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <fiber/common/IoError.h>
#include <fiber/common/mem/BufPool.h>
#include <fiber/compression/GzipEncoder.h>

namespace fiber::access_server {
namespace {

constexpr std::size_t kOutputChunkBytes = 16U << 10U;

GzipEncodeError gzip_error(common::IoErr error) noexcept {
    return error == common::IoErr::NoMem ? GzipEncodeError::NoMemory : GzipEncodeError::CompressionFailed;
}

struct EncoderDeleter {
    void operator()(compression::GzipEncoder *encoder) const noexcept { std::destroy_at(encoder); }
};

using OwnedEncoder = std::unique_ptr<compression::GzipEncoder, EncoderDeleter>;

} // namespace

GzipEncodeResult gzip_encode(std::string_view input, int level) {
    if (level < 1 || level > 9) {
        return std::unexpected(GzipEncodeError::InvalidLevel);
    }

    // Publication-time helper, not a hot path: a private pool backs the
    // encoder's single state allocation and is released with it.
    mem::BufPool pool;
    auto created = compression::GzipEncoder::create(pool, compression::GzipEncoderOptions{level});
    if (!created) {
        return std::unexpected(gzip_error(created.error()));
    }
    const OwnedEncoder encoder{*created};

    std::string output;
    std::vector<std::uint8_t> buffer(kOutputChunkBytes);
    const auto output_space = [&buffer]() noexcept { return std::span<std::uint8_t>(buffer.data(), buffer.size()); };

    std::size_t consumed = 0;
    while (consumed < input.size()) {
        const std::span<const std::uint8_t> pending{reinterpret_cast<const std::uint8_t *>(input.data()) + consumed,
                                                    input.size() - consumed};
        const auto step = encoder->write(pending, output_space());
        if (!step) {
            return std::unexpected(gzip_error(step.error()));
        }
        output.append(reinterpret_cast<const char *>(buffer.data()), step->written);
        consumed += step->consumed;
    }

    for (;;) {
        const auto step = encoder->finish(output_space());
        if (!step) {
            return std::unexpected(gzip_error(step.error()));
        }
        output.append(reinterpret_cast<const char *>(buffer.data()), step->written);
        if (step->status == compression::EncodeStatus::Finished) {
            return output;
        }
    }
}

} // namespace fiber::access_server
