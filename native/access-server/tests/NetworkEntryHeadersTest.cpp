#include "execution/NetworkEntryHeaders.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>

#include <fiber/common/mem/BufPool.h>
#include <fiber/http/HttpHeaders.h>
#include <fiber/net/IpAddress.h>

namespace fiber::access_server {
namespace {

net::IpAddress ip(std::string_view text) {
    net::IpAddress parsed;
    EXPECT_TRUE(net::IpAddress::parse(text, parsed));
    return parsed;
}

std::size_t count_fields(const http::HttpHeaders &headers, std::string_view name) {
    std::size_t count = 0;
    for (const http::HttpHeaders::HeaderField &field [[maybe_unused]]: headers.get_all(name)) {
        ++count;
    }
    return count;
}

TEST(NetworkEntryHeadersTest, EmptyEntryLeavesRequestHeadersUntouched) {
    mem::BufPool pool;
    http::HttpHeaders headers(pool);
    ASSERT_NE(headers.add("X-Real-Ip", "198.51.100.7"), nullptr);

    EXPECT_TRUE(apply_network_entry_headers(headers, ip("203.0.113.9"), "", true));

    EXPECT_EQ(headers.get("X-Real-Ip"), "198.51.100.7");
    EXPECT_FALSE(headers.contains("X-Entry"));
    EXPECT_FALSE(headers.contains("X-Forwarded-Proto"));
    EXPECT_FALSE(headers.contains("X-Forwarded-For"));
}

// X-Entry is server-authoritative: a deployment that declares no entry
// network must not adopt a client-supplied value, which would forge the host
// entry-policy gate. Empty entry removes the header, while other client
// headers stay untouched (their consumers are opt-in via client metadata
// mode).
TEST(NetworkEntryHeadersTest, EmptyEntryStripsClientSuppliedEntryHeader) {
    mem::BufPool pool;
    http::HttpHeaders headers(pool);
    ASSERT_NE(headers.add("X-Entry", "vdi"), nullptr);
    ASSERT_NE(headers.add("X-Real-Ip", "198.51.100.7"), nullptr);

    EXPECT_TRUE(apply_network_entry_headers(headers, ip("203.0.113.9"), "", true));

    EXPECT_FALSE(headers.contains("X-Entry"));
    EXPECT_EQ(headers.get("X-Real-Ip"), "198.51.100.7");
}

TEST(NetworkEntryHeadersTest, InternetEntryInjectsProxyHeadersFromScratch) {
    mem::BufPool pool;
    http::HttpHeaders headers(pool);

    EXPECT_TRUE(apply_network_entry_headers(headers, ip("203.0.113.9"), "internet", true));

    EXPECT_EQ(headers.get("X-Entry"), "internet");
    EXPECT_EQ(headers.get("X-Real-Ip"), "203.0.113.9");
    EXPECT_EQ(headers.get("X-Forwarded-Proto"), "https");
    EXPECT_EQ(headers.get("X-Forwarded-For"), "203.0.113.9");
}

TEST(NetworkEntryHeadersTest, InternetEntryPrefersEoConnectingIpVerbatim) {
    mem::BufPool pool;
    http::HttpHeaders headers(pool);
    ASSERT_NE(headers.add("EO-Connecting-IP", "198.51.100.7"), nullptr);

    EXPECT_TRUE(apply_network_entry_headers(headers, ip("203.0.113.9"), "internet", true));

    EXPECT_EQ(headers.get("X-Real-Ip"), "198.51.100.7");
    EXPECT_EQ(headers.get("X-Forwarded-For"), "203.0.113.9");
}

TEST(NetworkEntryHeadersTest, EoConnectingIpLookupIsCaseInsensitive) {
    mem::BufPool pool;
    http::HttpHeaders headers(pool);
    ASSERT_NE(headers.add("eo-connecting-ip", "198.51.100.7"), nullptr);

    EXPECT_TRUE(apply_network_entry_headers(headers, ip("203.0.113.9"), "internet", false));

    EXPECT_EQ(headers.get("X-Real-Ip"), "198.51.100.7");
}

TEST(NetworkEntryHeadersTest, EmptyEoConnectingIpFallsBackToPeer) {
    mem::BufPool pool;
    http::HttpHeaders headers(pool);
    ASSERT_NE(headers.add("EO-Connecting-IP", ""), nullptr);

    EXPECT_TRUE(apply_network_entry_headers(headers, ip("203.0.113.9"), "internet", true));

    EXPECT_EQ(headers.get("X-Real-Ip"), "203.0.113.9");
}

TEST(NetworkEntryHeadersTest, NonInternetEntryIgnoresEoConnectingIp) {
    mem::BufPool pool;
    http::HttpHeaders headers(pool);
    ASSERT_NE(headers.add("EO-Connecting-IP", "198.51.100.7"), nullptr);

    EXPECT_TRUE(apply_network_entry_headers(headers, ip("203.0.113.9"), "desktop", true));

    EXPECT_EQ(headers.get("X-Entry"), "desktop");
    EXPECT_EQ(headers.get("X-Real-Ip"), "203.0.113.9");
}

TEST(NetworkEntryHeadersTest, ReplacesSpoofedClientSuppliedValues) {
    mem::BufPool pool;
    http::HttpHeaders headers(pool);
    ASSERT_NE(headers.add("X-Entry", "desktop"), nullptr);
    ASSERT_NE(headers.add("X-Real-Ip", "1.2.3.4"), nullptr);
    ASSERT_NE(headers.add("X-Forwarded-Proto", "http"), nullptr);
    ASSERT_NE(headers.add("X-Forwarded-For", "9.9.9.9"), nullptr);

    EXPECT_TRUE(apply_network_entry_headers(headers, ip("203.0.113.9"), "internet", true));

    EXPECT_EQ(headers.get("X-Entry"), "internet");
    EXPECT_EQ(headers.get("X-Real-Ip"), "203.0.113.9");
    EXPECT_EQ(headers.get("X-Forwarded-Proto"), "https");
    EXPECT_EQ(headers.get("X-Forwarded-For"), "9.9.9.9, 203.0.113.9");
    EXPECT_EQ(count_fields(headers, "X-Entry"), 1u);
    EXPECT_EQ(count_fields(headers, "X-Real-Ip"), 1u);
    EXPECT_EQ(count_fields(headers, "X-Forwarded-Proto"), 1u);
    EXPECT_EQ(count_fields(headers, "X-Forwarded-For"), 1u);
}

TEST(NetworkEntryHeadersTest, AppendsPeerToIncomingForwardedForFields) {
    mem::BufPool pool;
    http::HttpHeaders headers(pool);
    ASSERT_NE(headers.add("X-Forwarded-For", "198.51.100.1"), nullptr);
    ASSERT_NE(headers.add("X-Forwarded-For", "198.51.100.2"), nullptr);

    EXPECT_TRUE(apply_network_entry_headers(headers, ip("203.0.113.9"), "vdi", true));

    EXPECT_EQ(headers.get("X-Forwarded-For"), "198.51.100.1, 198.51.100.2, 203.0.113.9");
    EXPECT_EQ(count_fields(headers, "X-Forwarded-For"), 1u);
}

TEST(NetworkEntryHeadersTest, PlainListenerUsesHttpSchemeAndFormatsIpv6Peer) {
    mem::BufPool pool;
    http::HttpHeaders headers(pool);

    EXPECT_TRUE(apply_network_entry_headers(headers, ip("2001:db8::1"), "vdi", false));

    EXPECT_EQ(headers.get("X-Forwarded-Proto"), "http");
    EXPECT_EQ(headers.get("X-Real-Ip"), "2001:db8::1");
    EXPECT_EQ(headers.get("X-Forwarded-For"), "2001:db8::1");
}

} // namespace
} // namespace fiber::access_server
