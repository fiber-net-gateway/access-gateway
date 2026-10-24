#include "AccessProcessArguments.h"

#include <charconv>
#include <cstdint>
#include <sys/un.h>

namespace fiber::access_server {
namespace {

constexpr std::string_view kBossFd = "--boss-fd=";
constexpr std::string_view kSocket = "--socket=";
constexpr std::string_view kServer = "--server=";
constexpr std::string_view kConfig = "--config=";
constexpr std::string_view kPrewarmTimeout = "--prewarm-timeout-millis=";

template<class Integer>
bool parse_integer(std::string_view value, Integer minimum, Integer maximum, Integer &out) noexcept {
    Integer parsed{};
    auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || parsed < minimum || parsed > maximum) {
        return false;
    }
    out = parsed;
    return true;
}

bool absolute_path(std::string_view path) noexcept {
    return !path.empty() && path.front() == '/' && path.find('\0') == std::string_view::npos;
}

} // namespace

common::IoResult<AccessProcessArguments> parse_access_process_arguments(std::span<const std::string_view> arguments) {
    AccessProcessArguments result;
    bool config_seen = false;
    bool boss_seen = false;
    for (auto argument: arguments) {
        if (argument.starts_with(kBossFd)) {
            // Descriptors 0-2 stay stdio; the boss always passes 3.
            if (boss_seen || !parse_integer(argument.substr(kBossFd.size()), 3, 1023, result.boss_fd)) {
                return std::unexpected(common::IoErr::Invalid);
            }
            boss_seen = true;
        } else {
            if (argument.empty() || argument.starts_with('-') || config_seen ||
                argument.find('\0') != std::string_view::npos) {
                return std::unexpected(common::IoErr::Invalid);
            }
            config_seen = true;
            result.config_path = argument;
        }
    }
    return result;
}

common::IoResult<AccessBossArguments> parse_access_boss_arguments(std::span<const std::string_view> arguments) {
    AccessBossArguments result;
    bool timeout_seen = false;
    for (auto argument: arguments) {
        std::string *target = argument.starts_with(kSocket)   ? &result.socket_path
                              : argument.starts_with(kServer) ? &result.server_path
                              : argument.starts_with(kConfig) ? &result.config_path
                                                              : nullptr;
        if (target) {
            auto value = argument.substr(argument.find('=') + 1);
            if (!target->empty() || !absolute_path(value)) {
                return std::unexpected(common::IoErr::Invalid);
            }
            *target = value;
        } else if (argument.starts_with(kPrewarmTimeout)) {
            std::int64_t millis = 0;
            if (timeout_seen ||
                !parse_integer<std::int64_t>(argument.substr(kPrewarmTimeout.size()), 1, 3600000, millis)) {
                return std::unexpected(common::IoErr::Invalid);
            }
            timeout_seen = true;
            result.prewarm_timeout = std::chrono::milliseconds(millis);
        } else {
            return std::unexpected(common::IoErr::Invalid);
        }
    }
    if (result.socket_path.empty() || result.server_path.empty() || result.config_path.empty() ||
        result.socket_path.size() >= sizeof(sockaddr_un::sun_path)) {
        return std::unexpected(common::IoErr::Invalid);
    }
    return result;
}

} // namespace fiber::access_server
