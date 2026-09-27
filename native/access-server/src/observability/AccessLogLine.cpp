#include "AccessLogLine.h"

#include <array>
#include <charconv>
#include <limits>

namespace fiber::access_server {
namespace {

constexpr long kMillisPerSecond = 1000;
constexpr long kMillisPerMinute = 60 * kMillisPerSecond;
constexpr long kMillisPerHour = 60 * kMillisPerMinute;
constexpr long kMillisPerDay = 24 * kMillisPerHour;
constexpr long kDaysPerCycle = 146097;
constexpr long kDays0000To1970 = 719528;

long floor_div(long x, long y) noexcept {
    long r = x / y;
    const long m = x % y;
    if (m != 0 && ((x < 0) != (y < 0))) {
        --r;
    }
    return r;
}

long floor_mod(long x, long y) noexcept {
    long m = x % y;
    if (m != 0 && ((x < 0) != (y < 0))) {
        m += y;
    }
    return m;
}

struct PackedDate {
    int year;
    int month;
    int day;
};

// Civil-date conversion matching ManualTimestampFormatter.toPackedDate.
PackedDate packed_date(long epoch_day) noexcept {
    long zero_day = epoch_day + kDays0000To1970 - 60;
    long adjust = 0;
    if (zero_day < 0) {
        const long adjust_cycles = (zero_day + 1) / kDaysPerCycle - 1;
        zero_day -= adjust_cycles * kDaysPerCycle;
        adjust = adjust_cycles * 400;
    }
    long year_est = (400 * zero_day + 591) / kDaysPerCycle;
    long doy_est = zero_day - (365 * year_est + year_est / 4 - year_est / 100 + year_est / 400);
    if (doy_est < 0) {
        --year_est;
        doy_est = zero_day - (365 * year_est + year_est / 4 - year_est / 100 + year_est / 400);
    }
    year_est += adjust;
    const int march_doy0 = static_cast<int>(doy_est);
    const int march_month0 = (march_doy0 * 5 + 2) / 153;
    const int month = (march_month0 + 2) % 12 + 1;
    const int day = march_doy0 - (march_month0 * 306 + 5) / 10 + 1;
    const int year = static_cast<int>(year_est + march_month0 / 10);
    return {year, month, day};
}

void write_four_digits(char *buffer, int value) noexcept {
    buffer[0] = static_cast<char>('0' + value / 1000);
    value %= 1000;
    buffer[1] = static_cast<char>('0' + value / 100);
    value %= 100;
    buffer[2] = static_cast<char>('0' + value / 10);
    buffer[3] = static_cast<char>('0' + value % 10);
}

void write_two_digits(char *buffer, int value) noexcept {
    buffer[0] = static_cast<char>('0' + value / 10);
    buffer[1] = static_cast<char>('0' + value % 10);
}

void append_field(std::string_view value, std::string &output) {
    output.push_back('\t');
    if (value.empty()) {
        // The Java writer never emits an empty field: unset values default to
        // "-" at reset time.
        output.push_back('-');
        return;
    }
    output.append(value);
}

void append_long(std::int64_t value, std::string &output) {
    std::array<char, std::numeric_limits<std::int64_t>::digits10 + 3> buffer{};
    const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    output.append(buffer.data(), static_cast<std::size_t>(converted.ptr - buffer.data()));
}

void append_status(int status, std::string &output) {
    output.push_back('\t');
    if (status > 0) {
        // Java writes exactly three digits: '0' + status/100, '0' +
        // (status/10)%10, '0' + status%10.
        output.push_back(static_cast<char>('0' + status / 100));
        output.push_back(static_cast<char>('0' + (status / 10) % 10));
        output.push_back(static_cast<char>('0' + status % 10));
    } else {
        output.push_back('-');
    }
}

} // namespace

std::size_t format_access_log_timestamp(char *buffer, std::int64_t epoch_ms, int utc_offset_minutes) noexcept {
    const long adjusted_ms = static_cast<long>(epoch_ms) + static_cast<long>(utc_offset_minutes) * kMillisPerMinute;
    const long epoch_day = floor_div(adjusted_ms, kMillisPerDay);
    long millis_of_day = floor_mod(adjusted_ms, kMillisPerDay);

    const int hour = static_cast<int>(millis_of_day / kMillisPerHour);
    millis_of_day -= hour * kMillisPerHour;
    const int minute = static_cast<int>(millis_of_day / kMillisPerMinute);
    millis_of_day -= minute * kMillisPerMinute;
    const int second = static_cast<int>(millis_of_day / kMillisPerSecond);
    const int millis = static_cast<int>(millis_of_day - second * kMillisPerSecond);

    const PackedDate date = packed_date(epoch_day);
    std::size_t position = 0;
    if (date.year >= 0 && date.year <= 9999) {
        write_four_digits(buffer + position, date.year);
        position += 4;
    } else {
        // Years outside 0..9999 only occur for extreme timestamps; keep the
        // field width stable rather than replicating Java's sign/plus forms.
        write_four_digits(buffer + position, 0);
        position += 4;
    }
    buffer[position++] = '-';
    write_two_digits(buffer + position, date.month);
    position += 2;
    buffer[position++] = '-';
    write_two_digits(buffer + position, date.day);
    position += 2;
    buffer[position++] = ' ';
    write_two_digits(buffer + position, hour);
    position += 2;
    buffer[position++] = ':';
    write_two_digits(buffer + position, minute);
    position += 2;
    buffer[position++] = ':';
    write_two_digits(buffer + position, second);
    position += 2;
    buffer[position++] = '.';
    buffer[position++] = static_cast<char>('0' + millis / 100);
    const int millis_rest = millis % 100;
    buffer[position++] = static_cast<char>('0' + millis_rest / 10);
    buffer[position++] = static_cast<char>('0' + millis_rest % 10);
    return position;
}

void encode_access_log_line(const AccessLogRecord &record, int utc_offset_minutes, std::string &output) {
    std::array<char, 32> timestamp{};
    const std::size_t timestamp_length =
            format_access_log_timestamp(timestamp.data(), record.timestamp_ms, utc_offset_minutes);
    output.append(timestamp.data(), timestamp_length);
    output.append("\t[INFO ploto.ACCESS_LOG] - -");
    append_field(record.gateway_name, output);
    append_field(record.trace_id, output);
    append_field(record.hi_trace_id, output);
    append_field(record.trace_parent, output);
    append_field(record.http_method, output);
    append_field(record.host, output);
    append_field(record.client_ip, output);
    append_field(record.received_path, output);
    append_field(record.server_ip, output);
    append_field(record.app_name, output);
    append_field(record.gw_cluster, output);
    append_field(record.ctx_cluster, output);
    append_field(record.ultimate_cluster, output);
    append_field(record.backend_ip, output);
    append_field(record.forward_uri, output);
    append_status(record.http_code, output);
    output.push_back('\t');
    append_long(record.connect_nano, output);
    output.push_back('\t');
    append_long(record.response_millis, output);
    output.push_back('\n');
}

} // namespace fiber::access_server
