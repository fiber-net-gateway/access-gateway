#include "observability/AccessLogPolicy.h"

#include <gtest/gtest.h>

#include <string>

namespace fiber::access_server {
namespace {

TEST(AccessLogPolicyTest, DefaultsToPathOnlyAndEncodesUnsafeBytes) {
    AccessLogPolicy policy;
    ASSERT_TRUE(policy.initialize());
    std::string path = "/orders/";
    path.push_back('\n');
    path.push_back(static_cast<char>(0xff));

    const AccessLogUri rendered = policy.render_uri(http::HttpUri{
            .path = path,
            .query = "token=secret&session_id=also-secret",
            .unparsed_uri = "/orders?token=secret&session_id=also-secret",
    });

    EXPECT_EQ(rendered.path(), "/orders/%0A%FF");
    EXPECT_TRUE(rendered.query.empty());
    EXPECT_TRUE(rendered.query_hash.empty());
    EXPECT_TRUE(rendered.query_filtered);
    EXPECT_FALSE(rendered.query_redacted);
    EXPECT_EQ(rendered.path().find("secret"), std::string::npos);
}

TEST(AccessLogPolicyTest, AllowlistsKeysAndAlwaysRedactsSensitiveValues) {
    AccessLogOptions options;
    options.query_allowlist = {"page", "token", "Token", "client_secret", "custom", "q"};
    options.additional_sensitive_query_keys = {"custom"};
    AccessLogPolicy policy(std::move(options));
    ASSERT_TRUE(policy.initialize());

    const AccessLogUri rendered = policy.render_uri(http::HttpUri{
            .path = "/orders",
            .query = "page=2&token=super-secret&T%6Fken=case-secret&client_secret=oauth-secret&custom=value&q=hello%"
                     "20world&ignored=leak",
    });

    EXPECT_EQ(rendered.query,
              "page=2&token=[REDACTED]&Token=[REDACTED]&client_secret=[REDACTED]&custom=[REDACTED]&q=hello%20world");
    EXPECT_EQ(rendered.path(), "/orders");
    EXPECT_TRUE(rendered.path_storage.empty());
    EXPECT_TRUE(rendered.query_filtered);
    EXPECT_TRUE(rendered.query_redacted);
    EXPECT_EQ(rendered.query.find("super-secret"), std::string::npos);
    EXPECT_EQ(rendered.query.find("case-secret"), std::string::npos);
    EXPECT_EQ(rendered.query.find("oauth-secret"), std::string::npos);
    EXPECT_EQ(rendered.query.find("value"), std::string::npos);
    EXPECT_EQ(rendered.query.find("leak"), std::string::npos);
}

TEST(AccessLogPolicyTest, RejectsMalformedKeysAndSafelyEncodesAllowedRawValues) {
    AccessLogOptions options;
    options.query_allowlist = {"safe"};
    AccessLogPolicy policy(std::move(options));
    ASSERT_TRUE(policy.initialize());
    std::string query = "safe=line";
    query.push_back('\n');
    query.push_back(static_cast<char>(0xff));
    query.append("&%zz=secret&safe=ok=still-value");

    const AccessLogUri rendered = policy.render_uri(http::HttpUri{
            .path = "/",
            .query = query,
    });

    EXPECT_EQ(rendered.query, "safe=line%0A%FF&safe=ok%3Dstill-value");
    EXPECT_TRUE(rendered.query_filtered);
    EXPECT_FALSE(rendered.query_redacted);
    EXPECT_EQ(rendered.query.find("secret"), std::string::npos);
}

TEST(AccessLogPolicyTest, TruncatesAtEncodingBoundariesWithinConfiguredLimits) {
    AccessLogOptions options;
    options.query_allowlist = {"safe"};
    options.max_path_bytes = kMinAccessLogFieldBytes;
    options.max_query_bytes = kMinAccessLogFieldBytes;
    AccessLogPolicy policy(std::move(options));
    ASSERT_TRUE(policy.initialize());

    const AccessLogUri rendered = policy.render_uri(http::HttpUri{
            .path = "/abcdefghijklmnop",
            .query = "safe=0123456789abcdef",
    });

    EXPECT_EQ(rendered.path(), "/abcdefghijkl...");
    EXPECT_EQ(rendered.query, "safe=01234567...");
    EXPECT_EQ(rendered.path().size(), kMinAccessLogFieldBytes);
    EXPECT_EQ(rendered.query.size(), kMinAccessLogFieldBytes);
    EXPECT_TRUE(rendered.path_truncated);
    EXPECT_TRUE(rendered.query_truncated);
}

TEST(AccessLogPolicyTest, UsesAnEphemeralHmacForOptionalQueryCorrelation) {
    AccessLogOptions options;
    options.query_hash_enabled = true;
    AccessLogPolicy policy(std::move(options));

    const AccessLogUri unavailable = policy.render_uri(http::HttpUri{.path = "/", .query = "token=secret"});
    EXPECT_TRUE(unavailable.query_hash.empty());
    EXPECT_TRUE(unavailable.query_hash_failed);

    ASSERT_TRUE(policy.initialize());
    const AccessLogUri first = policy.render_uri(http::HttpUri{.path = "/", .query = "token=secret"});
    const AccessLogUri repeated = policy.render_uri(http::HttpUri{.path = "/", .query = "token=secret"});
    const AccessLogUri different = policy.render_uri(http::HttpUri{.path = "/", .query = "token=other"});

    EXPECT_TRUE(first.query.empty());
    EXPECT_TRUE(first.query_filtered);
    EXPECT_FALSE(first.query_hash_failed);
    EXPECT_TRUE(first.query_hash.starts_with("hmac-sha256:"));
    EXPECT_EQ(first.query_hash.size(), 76u);
    EXPECT_EQ(first.query_hash, repeated.query_hash);
    EXPECT_NE(first.query_hash, different.query_hash);
    EXPECT_EQ(first.query_hash.find("secret"), std::string::npos);
}

TEST(AccessLogPolicyTest, SamplesOnlySuccessfulRequests) {
    AccessLogOptions disabled;
    disabled.success_sample_rate_bps = 0;
    AccessLogPolicy disabled_policy(std::move(disabled));
    EXPECT_FALSE(disabled_policy.should_log(false, 0));
    EXPECT_TRUE(disabled_policy.should_log(true, 9999));

    AccessLogOptions half;
    half.success_sample_rate_bps = 5000;
    AccessLogPolicy half_policy(std::move(half));
    EXPECT_TRUE(half_policy.should_log(false, 4999));
    EXPECT_FALSE(half_policy.should_log(false, 5000));

    AccessLogPolicy default_policy;
    EXPECT_TRUE(default_policy.should_log(false, 9999));
}

} // namespace
} // namespace fiber::access_server
