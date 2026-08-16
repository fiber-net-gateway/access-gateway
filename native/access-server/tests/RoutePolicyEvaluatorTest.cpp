#include "config/AccessConfig.h"
#include "execution/ClientMetadata.h"
#include "execution/RoutePolicyEvaluator.h"
#include "routing/ProjectRouteSnapshot.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>

#include <fiber/http/HttpBodySpec.h>
#include <fiber/net/IpAddress.h>

namespace fiber::access_server {
namespace {

Cidr strict_cidr(std::string_view text) {
    auto parsed = Cidr::parse_strict(text, "test");
    if (!parsed) {
        ADD_FAILURE() << parsed.error().message;
        return Cidr::from_address(net::IpAddress::any_v4());
    }
    return *parsed;
}

ClientMetadata metadata_for(std::string_view address) {
    net::IpAddress parsed;
    EXPECT_TRUE(net::IpAddress::parse(address, parsed));
    return ClientMetadata{
            .client_address = parsed,
            .route_policy_target = Cidr::from_address(parsed),
    };
}

TEST(RoutePolicyEvaluatorTest, EvaluatesEntryBeforeHttpsAndPreservesRedirectStatus) {
    const RoutePolicyEvaluator evaluator(1024);
    HostStrategyConfig strategy{
            .https = std::nullopt,
            .net_mask = kNetVdi,
    };

    EXPECT_EQ(evaluator.evaluate_host(strategy, "desktop", false).action, HostPolicyAction::EntryRejected);
    EXPECT_EQ(evaluator.evaluate_host(strategy, "vdi", false).action, HostPolicyAction::InvalidHttps);
    EXPECT_EQ(evaluator.evaluate_host(strategy, "vdi", true).action, HostPolicyAction::Allow);

    constexpr std::array redirects{
            HttpsStrategy::Redirect301,
            HttpsStrategy::Redirect302,
            HttpsStrategy::Redirect307,
            HttpsStrategy::Redirect308,
    };
    strategy.net_mask = 0;
    for (const HttpsStrategy redirect: redirects) {
        strategy.https = redirect;
        const HostPolicyDecision decision = evaluator.evaluate_host(strategy, {}, false);
        EXPECT_EQ(decision.action, HostPolicyAction::Redirect);
        EXPECT_EQ(decision.redirect_status, static_cast<int>(redirect));
        EXPECT_EQ(evaluator.evaluate_host(strategy, {}, true).action, HostPolicyAction::Allow);
    }

    strategy.https = HttpsStrategy::NotRequired;
    EXPECT_EQ(evaluator.evaluate_host(strategy, {}, false).action, HostPolicyAction::Allow);
}

TEST(RoutePolicyEvaluatorTest, ChecksKnownBodyLengthBeforeCidrAndPreservesLimitSemantics) {
    const RoutePolicyEvaluator evaluator(4);
    CompiledRoute route;
    route.allow_cidrs.push_back(strict_cidr("10.0.0.0/8"));
    const ClientMetadata denied = metadata_for("192.0.2.1");
    const ClientMetadata allowed = metadata_for("10.1.2.3");

    RoutePolicyDecision decision = evaluator.evaluate_route(route, denied, http::HttpBodySpec::ContentLength(5));
    EXPECT_EQ(decision.action, RoutePolicyAction::BodyTooLarge);
    EXPECT_EQ(decision.body_limit, 4U);

    decision = evaluator.evaluate_route(route, denied, http::HttpBodySpec::ContentLength(4));
    EXPECT_EQ(decision.action, RoutePolicyAction::SourceIpNotAllowed);
    EXPECT_EQ(decision.body_limit, 4U);

    route.max_client_body_size = 6;
    decision = evaluator.evaluate_route(route, allowed, http::HttpBodySpec::ContentLength(5));
    EXPECT_EQ(decision.action, RoutePolicyAction::Allow);
    EXPECT_EQ(decision.body_limit, 6U);

    route.max_client_body_size = 0;
    decision = evaluator.evaluate_route(route, allowed, http::HttpBodySpec::ContentLength(5));
    EXPECT_EQ(decision.action, RoutePolicyAction::BodyTooLarge);
    EXPECT_EQ(decision.body_limit, 4U);

    route.max_client_body_size = -1;
    decision = evaluator.evaluate_route(route, allowed,
                                        http::HttpBodySpec::ContentLength(std::numeric_limits<std::size_t>::max()));
    EXPECT_EQ(decision.action, RoutePolicyAction::Allow);
    EXPECT_EQ(decision.body_limit, 0U);
}

TEST(RoutePolicyEvaluatorTest, AppliesIpv4AndIpv6AllowDenyPoliciesAndLegacySkip) {
    const RoutePolicyEvaluator evaluator(1024);
    CompiledRoute ipv4_route;
    ipv4_route.allow_cidrs.push_back(strict_cidr("10.0.0.0/8"));
    ipv4_route.deny_cidrs.push_back(strict_cidr("10.2.0.0/16"));

    EXPECT_EQ(evaluator.evaluate_route(ipv4_route, metadata_for("10.1.2.3"), http::HttpBodySpec::None()).action,
              RoutePolicyAction::Allow);
    EXPECT_EQ(evaluator.evaluate_route(ipv4_route, metadata_for("10.2.3.4"), http::HttpBodySpec::None()).action,
              RoutePolicyAction::SourceIpNotAllowed);
    EXPECT_EQ(evaluator.evaluate_route(ipv4_route, metadata_for("192.0.2.1"), http::HttpBodySpec::None()).action,
              RoutePolicyAction::SourceIpNotAllowed);

    const ClientMetadata legacy_without_target;
    EXPECT_EQ(evaluator.evaluate_route(ipv4_route, legacy_without_target, http::HttpBodySpec::None()).action,
              RoutePolicyAction::Allow);

    CompiledRoute ipv6_route;
    ipv6_route.allow_cidrs.push_back(strict_cidr("2001:db8::/32"));
    ipv6_route.deny_cidrs.push_back(strict_cidr("2001:db8:dead::/48"));
    EXPECT_EQ(evaluator.evaluate_route(ipv6_route, metadata_for("2001:db8:1::7"), http::HttpBodySpec::None()).action,
              RoutePolicyAction::Allow);
    EXPECT_EQ(evaluator.evaluate_route(ipv6_route, metadata_for("2001:db8:dead::7"), http::HttpBodySpec::None()).action,
              RoutePolicyAction::SourceIpNotAllowed);
    EXPECT_EQ(evaluator.evaluate_route(ipv6_route, metadata_for("10.1.2.3"), http::HttpBodySpec::None()).action,
              RoutePolicyAction::SourceIpNotAllowed);
}

TEST(RoutePolicyEvaluatorTest, AcceptsOnlyEmptyOrKnownLengthScriptBodies) {
    EXPECT_TRUE(RoutePolicyEvaluator::script_body_supported(http::HttpBodySpec::None()));
    EXPECT_TRUE(RoutePolicyEvaluator::script_body_supported(http::HttpBodySpec::ContentLength(0)));
    EXPECT_TRUE(RoutePolicyEvaluator::script_body_supported(http::HttpBodySpec::ContentLength(17)));
    EXPECT_FALSE(RoutePolicyEvaluator::script_body_supported(http::HttpBodySpec::Chunked()));
    EXPECT_FALSE(RoutePolicyEvaluator::script_body_supported(http::HttpBodySpec::Stream()));
    EXPECT_FALSE(RoutePolicyEvaluator::script_body_supported(http::HttpBodySpec::Auto()));
}

} // namespace
} // namespace fiber::access_server
