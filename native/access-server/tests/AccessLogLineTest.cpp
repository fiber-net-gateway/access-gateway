#include "../src/observability/AccessLogLine.h"

#include <gtest/gtest.h>

#include <array>
#include <string>

namespace fiber::access_server {
namespace {

TEST(AccessLogLineTest, FormatsTimestampAtKnownInstants) {
    struct Case {
        std::int64_t epoch_ms;
        int offset_minutes;
        std::string_view expected;
    };
    const Case cases[] = {
            {0, 0, "1970-01-01 00:00:00.000"},
            {0, 480, "1970-01-01 08:00:00.000"},
            {1760000000123, 480, "2025-10-09 16:53:20.123"},
            {1760000000123, 0, "2025-10-09 08:53:20.123"},
            // One millisecond before the epoch: floor division must keep the
            // previous day.
            {-1, 0, "1969-12-31 23:59:59.999"},
            {-1, 480, "1970-01-01 07:59:59.999"},
            // Leap day and the day after a short February.
            {951782400000, 0, "2000-02-29 00:00:00.000"},
            {1709164800000, 0, "2024-02-29 00:00:00.000"},
            {1583020800000, 0, "2020-03-01 00:00:00.000"},
            // 2100 is not a leap year (century rule).
            {4107542400000, 480, "2100-03-01 08:00:00.000"},
    };
    for (const Case &item: cases) {
        std::array<char, 32> buffer{};
        const std::size_t length = format_access_log_timestamp(buffer.data(), item.epoch_ms, item.offset_minutes);
        ASSERT_EQ(length, 23u) << item.expected;
        EXPECT_EQ(std::string_view(buffer.data(), length), item.expected);
    }
}

AccessLogRecord typical_record() {
    AccessLogRecord record{};
    record.timestamp_ms = 1760000000123;
    record.gateway_name = "access-server-vdi";
    record.trace_id = "3f2a1c9d0b7e4f5a";
    record.hi_trace_id = "0";
    record.trace_parent = "00-3f2a1c9d0b7e4f5a2c9d1e4f7a8b-1b2c3d4e5f607a8b-01";
    record.http_method = "GET";
    record.host = "a.b.c";
    record.client_ip = "1.2.3.4";
    record.received_path = "/x";
    record.server_ip = "5.6.7.8";
    record.app_name = "ploto-orders";
    record.gw_cluster = "default";
    record.ctx_cluster = "-";
    record.ultimate_cluster = "default";
    record.backend_ip = "10.0.0.1";
    record.forward_uri = "/x";
    record.http_code = 200;
    record.connect_nano = -7;
    record.response_millis = 42;
    return record;
}

TEST(AccessLogLineTest, EncodesJavaByteCompatibleSuccessLine) {
    AccessLogRecord record = typical_record();
    std::string line;
    encode_access_log_line(record, 480, line);
    EXPECT_EQ(line, "2025-10-09 16:53:20.123\t[INFO ploto.ACCESS_LOG] - -\taccess-server-vdi\t3f2a1c9d0b7e4f5a\t0"
                    "\t00-3f2a1c9d0b7e4f5a2c9d1e4f7a8b-1b2c3d4e5f607a8b-01\tGET\ta.b.c\t1.2.3.4\t/x\t5.6.7.8"
                    "\tploto-orders\tdefault\t-\tdefault\t10.0.0.1\t/x\t200\t-7\t42\n");
}

TEST(AccessLogLineTest, EncodesFailedAttemptStatusAsThreeDigits) {
    // Java renders the never-committed status 1 as exactly three digits.
    AccessLogRecord record = typical_record();
    record.http_code = 1;
    record.connect_nano = 1200000;
    record.response_millis = 30;
    std::string line;
    encode_access_log_line(record, 480, line);
    EXPECT_TRUE(line.ends_with("\t001\t1200000\t30\n")) << line;
}

TEST(AccessLogLineTest, EncodesNonPositiveStatusAsDash) {
    AccessLogRecord record = typical_record();
    record.http_code = 0;
    std::string line;
    encode_access_log_line(record, 480, line);
    EXPECT_TRUE(line.ends_with("\t-\t-7\t42\n")) << line;
}

TEST(AccessLogLineTest, EncodesEmptyFieldsAsDash) {
    AccessLogRecord record{};
    record.timestamp_ms = 0;
    record.http_code = 200;
    record.connect_nano = 0;
    record.response_millis = 0;
    std::string line;
    encode_access_log_line(record, 0, line);
    EXPECT_EQ(line, "1970-01-01 00:00:00.000\t[INFO ploto.ACCESS_LOG] - -\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-"
                    "\t200\t0\t0\n");
}

TEST(AccessLogLineTest, EncodesRawBytesWithoutEscapingOrTruncation) {
    AccessLogRecord record = typical_record();
    record.received_path = "/search/naïve%20path?q=1";
    record.host = "host with space";
    const std::string long_uri = std::string("/very-long-") + std::string(2048, 'z');
    record.forward_uri = long_uri;
    std::string line;
    encode_access_log_line(record, 480, line);
    EXPECT_NE(line.find("/search/naïve%20path?q=1"), std::string::npos);
    EXPECT_NE(line.find("host with space"), std::string::npos);
    EXPECT_NE(line.find(std::string(2048, 'z')), std::string::npos);
}

TEST(AccessLogLineTest, AppendsToExistingOutput) {
    AccessLogRecord record = typical_record();
    std::string line = "prefix";
    encode_access_log_line(record, 480, line);
    EXPECT_TRUE(line.starts_with("prefix2025-10-09"));
    EXPECT_TRUE(line.ends_with("\n"));
}

} // namespace
} // namespace fiber::access_server
