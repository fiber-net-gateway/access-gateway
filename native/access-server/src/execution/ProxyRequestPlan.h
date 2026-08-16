#ifndef FIBER_ACCESS_SERVER_PROXY_REQUEST_PLAN_H
#define FIBER_ACCESS_SERVER_PROXY_REQUEST_PLAN_H

#include "AccessResult.h"
#include "TemplateEvaluator.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <fiber/common/NonCopyable.h>
#include <fiber/common/NonMovable.h>
#include <fiber/http/ClientHttp1Types.h>
#include <fiber/http/HttpBodySpec.h>
#include <fiber/http/HttpHeaders.h>

namespace fiber::http {
class HttpExchange;
}

namespace fiber::access_server {

class AccessRequestTelemetry;
struct CompiledProxyRoute;
struct ProxyExecutionInput;
struct ProxyUpstreamEndpoint;

enum class ProxyRequestPlanErrorPhase : std::uint8_t {
    BuildRequest,
    BuildHeaders,
};

struct ProxyRequestPlanError {
    ProxyRequestPlanErrorPhase phase = ProxyRequestPlanErrorPhase::BuildRequest;
    Err error;
};

using ProxyRequestPlanResult = std::expected<void, ProxyRequestPlanError>;

[[nodiscard]] http::HttpBodySpec
select_proxy_request_body_spec(http::HttpBodySpec inbound, bool has_content_length_header, bool websocket) noexcept;
[[nodiscard]] std::optional<std::uint64_t>
normalize_proxy_response_body_limit(std::optional<std::int64_t> configured_limit) noexcept;

// Synchronous, request-owned plan. Template evaluation and header/target
// materialization happen once; endpoint retries may only rebind an implicit
// Host header.
class ProxyRequestPlan final : public common::NonCopyable, public common::NonMovable {
public:
    ProxyRequestPlan(mem::BufPool &pool, const http::HttpExchange &exchange, const CompiledProxyRoute &proxy) noexcept;

    [[nodiscard]] ProxyRequestPlanResult prepare(const ProxyUpstreamEndpoint &endpoint,
                                                 const http::HttpExchange &exchange, const CompiledProxyRoute &proxy,
                                                 const ProxyExecutionInput &input,
                                                 const AccessRequestTelemetry &telemetry);
    [[nodiscard]] ProxyRequestPlanResult rebind_endpoint(const ProxyUpstreamEndpoint &endpoint) noexcept;

    [[nodiscard]] bool prepared() const noexcept { return prepared_; }
    [[nodiscard]] bool websocket_upgrade() const noexcept { return websocket_upgrade_; }
    [[nodiscard]] std::int32_t websocket_timeout_millis() const noexcept { return websocket_timeout_millis_; }
    [[nodiscard]] const http::HttpBodySpec &body_spec() const noexcept { return body_spec_; }
    [[nodiscard]] bool request_end_stream() const noexcept { return request_end_stream_; }
    [[nodiscard]] const std::optional<std::uint64_t> &max_response_body_size() const noexcept {
        return max_response_body_size_;
    }
    [[nodiscard]] http::HttpHeaders &headers() noexcept { return headers_; }
    [[nodiscard]] http::Http1RequestHead request_head(http::HttpMethod method) const noexcept;

private:
    std::string request_target_storage_;
    std::string_view request_target_;
    http::HttpHeaders headers_;
    std::vector<EvaluatedTemplate> evaluated_header_values_;
    http::HttpBodySpec body_spec_{http::HttpBodySpec::None()};
    std::optional<std::uint64_t> max_response_body_size_;
    std::int32_t websocket_timeout_millis_ = 0;
    bool prepared_ = false;
    bool host_uses_selected_endpoint_ = true;
    bool websocket_upgrade_ = false;
    bool request_end_stream_ = true;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_PROXY_REQUEST_PLAN_H
