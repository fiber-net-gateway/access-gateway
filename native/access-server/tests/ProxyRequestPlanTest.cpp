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
    EXPECT_FALSE(normalize_proxy_response_body_limit(std::nullopt));
    EXPECT_FALSE(normalize_proxy_response_body_limit(0));

    const std::optional<std::uint64_t> negative = normalize_proxy_response_body_limit(-1);
    ASSERT_TRUE(negative);
    EXPECT_EQ(*negative, 0U);

    const std::optional<std::uint64_t> positive = normalize_proxy_response_body_limit(4096);
    ASSERT_TRUE(positive);
    EXPECT_EQ(*positive, 4096U);
}

} // namespace
} // namespace fiber::access_server
