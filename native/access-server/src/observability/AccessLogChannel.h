#ifndef FIBER_ACCESS_SERVER_ACCESS_LOG_CHANNEL_H
#define FIBER_ACCESS_SERVER_ACCESS_LOG_CHANNEL_H

#include <string>
#include <string_view>

#include <fiber/common/NonCopyable.h>
#include <fiber/common/NonMovable.h>

namespace fiber::access_server {

// Process-wide emission point for the Java-compatible access log. Lines go
// through Fiber's shared LogWorker pipeline onto a MessageOnly file appender;
// there is no dedicated writer thread. The channel carries only the per-line
// constants resolved once at startup; submission never blocks (the shared
// queue is DropNewest) and never throws.
class AccessLogChannel final : public common::NonCopyable, public common::NonMovable {
public:
    explicit AccessLogChannel(std::string gateway_name);

    // Submits one encoded line. The encoder terminates lines with '\n' while
    // the pipeline appends its own newline, so the trailing '\n' is stripped
    // here to keep the file bytes unchanged.
    void submit(std::string line) noexcept;

    [[nodiscard]] std::string_view gateway_name() const noexcept { return gateway_name_; }
    [[nodiscard]] std::string_view server_ip() const noexcept { return server_ip_; }
    [[nodiscard]] int utc_offset_minutes() const noexcept { return utc_offset_minutes_; }

private:
    const std::string gateway_name_;
    const std::string server_ip_;
    const int utc_offset_minutes_;
};

// Process-wide channel. install_access_log_channel installs it once the Fiber
// logger is initialized with the access appender; until then (or after
// uninstall) access_log_channel() returns null and callers skip logging.
// Installation resolves the server IP once and captures the local UTC offset
// the way the Java gateway does (fixed for the process lifetime).
void install_access_log_channel(std::string gateway_name) noexcept;
[[nodiscard]] AccessLogChannel *access_log_channel() noexcept;
void uninstall_access_log_channel() noexcept;

// Localhost IPv4 chosen by the Java NetUtil rules: the lowest-index up
// interface's last non-loopback IPv4 address, or 127.0.0.1 when none exists.
[[nodiscard]] std::string resolve_local_server_ip() noexcept;

// Current local UTC offset in minutes (fixed at first call).
[[nodiscard]] int local_utc_offset_minutes() noexcept;

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_ACCESS_LOG_CHANNEL_H
