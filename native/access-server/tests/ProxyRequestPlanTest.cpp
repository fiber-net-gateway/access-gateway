#include "execution/ProxyRequestPlan.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>

#include <fiber/http/HttpBodySpec.h>

namespace fiber::access_server {
namespace {

TEST(ProxyRequestPlanTest, PreservesJavaRequestBodyFraming) {
    const http::HttpBodySpec known = select_proxy_request_body_spec(http::HttpBodySpec::ContentLength(17), true, false);
    EXPECT_TRUE(known.is_content_length());
    EXPECT_EQ(known.content_length(), 17U);

    EXPECT_TRUE(select_proxy_request_body_spec(http::HttpBodySpec::ContentLength(17), false, false).is_chunked());
    EXPECT_TRUE(select_proxy_request_body_spec(http::HttpBodySpec::Chunked(), true, false).is_chunked());
    EXPECT_TRUE(select_proxy_request_body_spec(http::HttpBodySpec::None(), false, false).is_chunked());
    EXPECT_TRUE(select_proxy_request_body_spec(http::HttpBodySpec::Stream(), false, true).is_none());
}

TEST(ProxyRequestPlanTest, NormalizesJavaResponseBodyLimitSemantics) {
    constexpr std::size_t kServerDefault = 600U << 20U;

    // Route not configured (absent or zero) falls back to the server default,
    // which itself mirrors Java's fiber.http.client.maxBodySize.
    const std::optional<std::uint64_t> absent = normalize_proxy_response_body_limit(std::nullopt, kServerDefault);
    ASSERT_TRUE(absent);
    EXPECT_EQ(*absent, 600U << 20U);
    const std::optional<std::uint64_t> zero = normalize_proxy_response_body_limit(0, kServerDefault);
    ASSERT_TRUE(zero);
    EXPECT_EQ(*zero, 600U << 20U);

    // Zero server default disables the cap entirely.
    EXPECT_FALSE(normalize_proxy_response_body_limit(std::nullopt, 0));
    EXPECT_FALSE(normalize_proxy_response_body_limit(0, 0));

    // Negative route value stays an explicit opt-out even with a server default.
    const std::optional<std::uint64_t> negative = normalize_proxy_response_body_limit(-1, kServerDefault);
    ASSERT_TRUE(negative);
    EXPECT_EQ(*negative, 0U);

    // A configured route value wins over the server default.
    const std::optional<std::uint64_t> positive = normalize_proxy_response_body_limit(4096, kServerDefault);
    ASSERT_TRUE(positive);
    EXPECT_EQ(*positive, 4096U);
}

} // namespace
} // namespace fiber::access_server
