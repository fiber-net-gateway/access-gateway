#include "AccessProcessArguments.h"

#include <charconv>
#include <sys/un.h>

namespace fiber::access_server {

common::IoResult<AccessProcessArguments> parse_access_process_arguments(std::span<const std::string_view> arguments) {
    AccessProcessArguments result;
    unsigned seen = 0;
    for (auto argument: arguments) {
        if (argument.starts_with("--start-mode=")) {
            if ((seen & 1U) != 0) {
                return std::unexpected(common::IoErr::Invalid);
            }
            seen |= 1U;
            auto mode = argument.substr(13);
            if (mode != "normal" && mode != "prewarm") {
                return std::unexpected(common::IoErr::Invalid);
            }
            result.prewarm = mode == "prewarm";
        } else if (argument.starts_with("--control-dir=")) {
            if ((seen & 2U) != 0) {
                return std::unexpected(common::IoErr::Invalid);
            }
            seen |= 2U;
            auto path = argument.substr(14);
            if (path.empty() || path.front() != '/' || path.find('\0') != std::string_view::npos ||
                path.size() + 24 >= sizeof(sockaddr_un::sun_path)) {
                return std::unexpected(common::IoErr::Invalid);
            }
            result.control_directory = path;
        } else if (argument.starts_with("--prewarm-timeout-millis=")) {
            if ((seen & 4U) != 0) {
                return std::unexpected(common::IoErr::Invalid);
            }
            seen |= 4U;
            auto value = argument.substr(std::string_view("--prewarm-timeout-millis=").size());
            std::int64_t millis = 0;
            auto parsed = std::from_chars(value.data(), value.data() + value.size(), millis);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || millis < 1 ||
                millis > 3600000) {
                return std::unexpected(common::IoErr::Invalid);
            }
            result.prewarm_timeout = std::chrono::milliseconds(millis);
        } else {
            if (argument.empty() || argument.starts_with('-') || (seen & 8U) != 0 ||
                argument.find('\0') != std::string_view::npos) {
                return std::unexpected(common::IoErr::Invalid);
            }
            seen |= 8U;
            result.config_path = argument;
        }
    }
    if (result.prewarm && result.control_directory.empty()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    return result;
}

} // namespace fiber::access_server
