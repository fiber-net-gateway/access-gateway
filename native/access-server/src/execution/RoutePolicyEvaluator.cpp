#include "RoutePolicyEvaluator.h"

#include "../config/AccessConfig.h"
#include "../routing/ProjectRouteSnapshot.h"
#include "ClientMetadata.h"

#include <limits>
#include <span>

#include <fiber/http/HttpBodySpec.h>

namespace fiber::access_server {
namespace {

std::uint8_t entry_bit(std::string_view entry) noexcept {
    if (entry == "vdi") {
        return kNetVdi;
    }
    if (entry == "desktop") {
        return kNetOffice;
    }
    if (entry == "internet") {
        return kNetInternet;
    }
    return 0;
}

bool cidr_matches_any(std::span<const Cidr> cidrs, const Cidr &target) noexcept {
    for (const Cidr &cidr: cidrs) {
        if (cidr.matches(target)) {
            return true;
        }
    }
    return false;
}

bool source_ip_allowed(const CompiledRoute &route, const ClientMetadata &metadata) noexcept {
    if (!metadata.route_policy_target) {
        // Only explicit legacy mode can omit the target. Preserve the Java
        // behavior of skipping CIDR policy for a missing or invalid header.
        return true;
    }
    if (!route.allow_cidrs.empty() && !cidr_matches_any(route.allow_cidrs, *metadata.route_policy_target)) {
        return false;
    }
    return route.deny_cidrs.empty() || !cidr_matches_any(route.deny_cidrs, *metadata.route_policy_target);
}

} // namespace

HostPolicyDecision RoutePolicyEvaluator::evaluate_host(const HostStrategyConfig &strategy, std::string_view entry,
                                                       bool secure) const noexcept {
    if (strategy.net_mask != 0 && (strategy.net_mask & entry_bit(entry)) == 0) {
        return HostPolicyDecision{.action = HostPolicyAction::EntryRejected};
    }
    if (!strategy.https) {
        return HostPolicyDecision{.action = secure ? HostPolicyAction::Allow : HostPolicyAction::InvalidHttps};
    }
    if (*strategy.https != HttpsStrategy::NotRequired && !secure) {
        return HostPolicyDecision{
                .action = HostPolicyAction::Redirect,
                .redirect_status = static_cast<int>(*strategy.https),
        };
    }
    return {};
}

RoutePolicyDecision RoutePolicyEvaluator::evaluate_route(const CompiledRoute &route, const ClientMetadata &metadata,
                                                         const http::HttpBodySpec &body) const noexcept {
    const std::size_t body_limit = request_body_limit(route);
    if (body_limit != 0 && body.is_content_length() && body.content_length() > body_limit) {
        return RoutePolicyDecision{
                .action = RoutePolicyAction::BodyTooLarge,
                .body_limit = body_limit,
        };
    }
    if (!source_ip_allowed(route, metadata)) {
        return RoutePolicyDecision{
                .action = RoutePolicyAction::SourceIpNotAllowed,
                .body_limit = body_limit,
        };
    }
    return RoutePolicyDecision{.body_limit = body_limit};
}

bool RoutePolicyEvaluator::script_body_supported(const http::HttpBodySpec &body) noexcept {
    return body.is_none() || body.is_content_length();
}

std::size_t RoutePolicyEvaluator::request_body_limit(const CompiledRoute &route) const noexcept {
    if (!route.max_client_body_size || *route.max_client_body_size == 0) {
        return default_max_request_body_size_;
    }
    if (*route.max_client_body_size < 0) {
        // AbstractRouteExecution clamps an explicit negative value to zero;
        // ReqHandler treats zero as unlimited.
        return 0;
    }
    const auto value = static_cast<std::uint64_t>(*route.max_client_body_size);
    if (value > std::numeric_limits<std::size_t>::max()) {
        return std::numeric_limits<std::size_t>::max();
    }
    return static_cast<std::size_t>(value);
}

} // namespace fiber::access_server
