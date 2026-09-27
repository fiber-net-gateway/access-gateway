#ifndef FIBER_ACCESS_SERVER_ACCESS_LOG_LINE_H
#define FIBER_ACCESS_SERVER_ACCESS_LOG_LINE_H

#include <cstdint>
#include <string>
#include <string_view>

namespace fiber::access_server {

// Java access-log line contract (ploto-gateway AccessLog.encodeToBuf): a fixed
// prefix, 19 tab-separated fields, '\n' terminated. Fields carry raw request
// bytes without escaping or truncation; absent fields are "-".
struct AccessLogRecord {
    // Wall-clock milliseconds of the proxy attempt start, rendered in the
    // process-local UTC offset captured at startup.
    std::int64_t timestamp_ms = 0;
    std::string_view gateway_name;
    std::string_view trace_id;
    std::string_view hi_trace_id;
    std::string_view trace_parent;
    std::string_view http_method;
    std::string_view host;
    std::string_view client_ip;
    std::string_view received_path;
    std::string_view server_ip;
    std::string_view app_name;
    std::string_view gw_cluster;
    std::string_view ctx_cluster;
    std::string_view ultimate_cluster;
    std::string_view backend_ip;
    std::string_view forward_uri;
    // Upstream status of this attempt; <= 0 renders as "-" (never committed).
    int http_code = 0;
    // Negative when the upstream connection was reused (negated served count);
    // otherwise nanoseconds from attempt start to connect (microsecond grid).
    std::int64_t connect_nano = 0;
    // Milliseconds from attempt start to response head (or failure).
    std::int64_t response_millis = 0;
};

// Renders "yyyy-MM-dd HH:mm:ss.SSS" for epoch milliseconds shifted by the
// given UTC offset in minutes. Returns the number of bytes written (23 for
// years 0..9999). `buffer` must hold at least 32 bytes.
std::size_t format_access_log_timestamp(char *buffer, std::int64_t epoch_ms, int utc_offset_minutes) noexcept;

// Appends the complete access-log line for `record` to `output`. The timestamp
// is rendered in the given process-local UTC offset (minutes).
void encode_access_log_line(const AccessLogRecord &record, int utc_offset_minutes, std::string &output);

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_ACCESS_LOG_LINE_H
