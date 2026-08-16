#ifndef FIBER_ACCESS_SERVER_ROUTE_POLICY_EVALUATOR_H
#define FIBER_ACCESS_SERVER_ROUTE_POLICY_EVALUATOR_H

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace fiber::http {
class HttpBodySpec;
} // namespace fiber::http

namespace fiber::access_server {

struct ClientMetadata;
struct CompiledRoute;
struct HostStrategyConfig;

enum class HostPolicyAction : std::uint8_t {
    Allow,
    EntryRejected,
    InvalidHttps,
    Redirect,
};

struct HostPolicyDecision {
    HostPolicyAction action = HostPolicyAction::Allow;
    int redirect_status = 0;
};

enum class RoutePolicyAction : std::uint8_t {
    Allow,
    BodyTooLarge,
    SourceIpNotAllowed,
};

struct RoutePolicyDecision {
    RoutePolicyAction action = RoutePolicyAction::Allow;
    std::size_t body_limit = 0;
};

// Pure, allocation-free gateway policy. The request handler maps typed
// decisions to Java-compatible exceptions or redirect I/O.
class RoutePolicyEvaluator final {
public:
    explicit RoutePolicyEvaluator(std::size_t default_max_request_body_size) noexcept :
        default_max_request_body_size_(default_max_request_body_size) {}

    [[nodiscard]] HostPolicyDecision evaluate_host(const HostStrategyConfig &strategy, std::string_view entry,
                                                   bool secure) const noexcept;
    [[nodiscard]] RoutePolicyDecision evaluate_route(const CompiledRoute &route, const ClientMetadata &metadata,
                                                     const http::HttpBodySpec &body) const noexcept;
    [[nodiscard]] static bool script_body_supported(const http::HttpBodySpec &body) noexcept;

private:
    [[nodiscard]] std::size_t request_body_limit(const CompiledRoute &route) const noexcept;

    std::size_t default_max_request_body_size_ = 0;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_ROUTE_POLICY_EVALUATOR_H
