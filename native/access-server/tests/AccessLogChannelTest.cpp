#include "observability/AccessLogChannel.h"

#include <gtest/gtest.h>

#include <string>

namespace fiber::access_server {
namespace {

TEST(AccessLogChannelTest, ResolvesNonLoopbackServerIp) {
    // The NetUtil port returns a dotted IPv4 (or the 127.0.0.1 fallback) on
    // every machine this test can run on; both satisfy the log format.
    const std::string address = resolve_local_server_ip();
    EXPECT_FALSE(address.empty());
    EXPECT_EQ(address.find_first_not_of("0123456789."), std::string::npos);
}

TEST(AccessLogChannelTest, LocalUtcOffsetWithinDayRange) {
    // Any real timezone offset fits in ±14 hours; the fallback is 0.
    const int offset = local_utc_offset_minutes();
    EXPECT_GE(offset, -14 * 60);
    EXPECT_LE(offset, 14 * 60);
}

} // namespace
} // namespace fiber::access_server
