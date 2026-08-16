#include "AccessConfigErrorBuilder.h"

#include <array>
#include <charconv>
#include <utility>

namespace fiber::access_server::config_detail {

AccessConfigError make_error(AccessConfigErrorCode code, std::string field, std::string message, std::size_t offset) {
    return AccessConfigError{
            .code = code,
            .offset = offset,
            .field = std::move(field),
            .message = std::move(message),
    };
}

DecodeFailure invalid_field(std::string_view field, std::string message) {
    return std::unexpected(make_error(AccessConfigErrorCode::InvalidField, std::string(field), std::move(message)));
}

DecodeFailure out_of_range(std::string_view field, std::string message) {
    return std::unexpected(make_error(AccessConfigErrorCode::OutOfRange, std::string(field), std::move(message)));
}

DecodeFailure limit_exceeded(std::string_view field, std::string message) {
    return std::unexpected(make_error(AccessConfigErrorCode::LimitExceeded, std::string(field), std::move(message)));
}

std::string child_path(std::string_view parent, std::string_view child) {
    if (parent.empty()) {
        return std::string(child);
    }
    std::string path;
    path.reserve(parent.size() + 1 + child.size());
    path.append(parent);
    path.push_back('.');
    path.append(child);
    return path;
}

std::string index_path(std::string_view parent, std::size_t index) {
    std::array<char, 32> digits{};
    const auto conversion = std::to_chars(digits.data(), digits.data() + digits.size(), index);
    std::string path;
    path.reserve(parent.size() + 2 + static_cast<std::size_t>(conversion.ptr - digits.data()));
    path.append(parent);
    path.push_back('[');
    path.append(digits.data(), conversion.ptr);
    path.push_back(']');
    return path;
}

} // namespace fiber::access_server::config_detail
