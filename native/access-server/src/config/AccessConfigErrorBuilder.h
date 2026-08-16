#ifndef FIBER_ACCESS_SERVER_ACCESS_CONFIG_ERROR_BUILDER_H
#define FIBER_ACCESS_SERVER_ACCESS_CONFIG_ERROR_BUILDER_H

#include "AccessConfigError.h"

#include <cstddef>
#include <expected>
#include <string>
#include <string_view>
#include <utility>

namespace fiber::access_server::config_detail {

template<typename T>
using DecodeResult = std::expected<T, AccessConfigError>;

using DecodeFailure = std::unexpected<AccessConfigError>;

[[nodiscard]] AccessConfigError make_error(AccessConfigErrorCode code, std::string field, std::string message,
                                           std::size_t offset = 0);

[[nodiscard]] DecodeFailure invalid_field(std::string_view field, std::string message);
[[nodiscard]] DecodeFailure out_of_range(std::string_view field, std::string message);
[[nodiscard]] DecodeFailure limit_exceeded(std::string_view field, std::string message);

[[nodiscard]] std::string child_path(std::string_view parent, std::string_view child);
[[nodiscard]] std::string index_path(std::string_view parent, std::size_t index);

} // namespace fiber::access_server::config_detail

#endif // FIBER_ACCESS_SERVER_ACCESS_CONFIG_ERROR_BUILDER_H
