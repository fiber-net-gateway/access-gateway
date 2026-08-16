#include "ProxyRequestPlan.h"

#include "../observability/AccessRequestTelemetry.h"
#include "../routing/ProjectRouteSnapshot.h"
#include "AccessRequestHandler.h"
#include "ResponsePlan.h"

#include <array>
#include <utility>

#include <fiber/common/Assert.h>
#include <fiber/http/HttpExchange.h>
#include <fiber/http/HttpHeaderHash.h>

namespace fiber::access_server {
namespace {

constexpr std::string_view kTraceParentHeader = "traceparent";
constexpr std::uint64_t kTraceParentHeaderHash = http::http_header_name_hash(kTraceParentHeader);
constexpr std::string_view kCallSourceHeader = "x-ploto-source-app";
constexpr std::uint64_t kCallSourceHeaderHash = http::http_header_name_hash(kCallSourceHeader);
constexpr std::string_view kOriginHostHeader = "ploto-origin-host";
constexpr std::uint64_t kOriginHostHeaderHash = http::http_header_name_hash(kOriginHostHeader);

bool is_header(std::string_view actual, std::string_view expected) noexcept {
    return http::http_header_name_equals_ci(actual, expected);
}

bool is_java_filtered_proxy_request_header(std::string_view name) noexcept {
    return is_header(name, "host") || is_java_filtered_response_header(name);
}

bool is_websocket_request(const http::HttpExchange &exchange, const CompiledProxyRoute &proxy) noexcept {
    if (!proxy.websocket_timeout_millis || *proxy.websocket_timeout_millis <= 0) {
        return false;
    }
    return is_header(exchange.header("Upgrade"), "websocket") && is_header(exchange.header("Connection"), "upgrade");
}

std::string_view preserved_request_target(const http::HttpExchange &exchange, std::string &storage) {
    if (!exchange.uri().unparsed_uri.empty()) {
        return exchange.uri().unparsed_uri;
    }
    storage.clear();
    storage.reserve(exchange.uri().path.size() + (exchange.uri().query.empty() ? 0U : exchange.uri().query.size() + 1));
    storage.assign(exchange.uri().path);
    if (!exchange.uri().query.empty()) {
        storage.push_back('?');
        storage.append(exchange.uri().query);
    }
    return std::string_view(storage);
}

void java_escape_uri(std::string_view value, std::string &result) {
    constexpr std::array<std::uint32_t, 8> kEscape{
            0xFFFF'FFFFU, 0xD000'002DU, 0x5000'0000U, 0xB800'0001U,
            0xFFFF'FFFFU, 0xFFFF'FFFFU, 0xFFFF'FFFFU, 0xFFFF'FFFFU,
    };
    constexpr char kHex[] = "0123456789ABCDEF";

    std::size_t escaped_size = value.size();
    for (const unsigned char byte: value) {
        if ((kEscape[byte >> 5U] & (1U << (byte & 0x1FU))) != 0) {
            escaped_size += 2;
        }
    }
    result.clear();
    result.reserve(escaped_size);
    for (const unsigned char byte: value) {
        if ((kEscape[byte >> 5U] & (1U << (byte & 0x1FU))) == 0) {
            result.push_back(static_cast<char>(byte));
            continue;
        }
        result.push_back('%');
        result.push_back(kHex[byte >> 4U]);
        result.push_back(kHex[byte & 0x0FU]);
    }
}

Result<std::string_view> resolve_request_target(const http::HttpExchange &exchange, const CompiledProxyRoute &proxy,
                                                TemplateEvaluator evaluator, std::string &storage) {
    if (!proxy.rewrite) {
        return preserved_request_target(exchange, storage);
    }

    auto rewritten = evaluate_template(*proxy.rewrite, evaluator);
    if (!rewritten) {
        return std::unexpected(rewritten.error());
    }
    const std::string_view rewritten_view = rewritten->view();
    if (rewritten_view.empty()) {
        storage.assign("/");
    } else {
        java_escape_uri(rewritten_view, storage);
    }
    if (!exchange.uri().query.empty()) {
        storage.reserve(storage.size() + exchange.uri().query.size() + 1);
        storage.push_back('?');
        storage.append(exchange.uri().query);
    }
    return std::string_view(storage);
}

Err request_head_build_error() noexcept { return Err::from_error(common::IoErr::NoMem); }

Result<bool> build_request_headers(const ProxyUpstreamEndpoint &endpoint, const http::HttpExchange &exchange,
                                   const CompiledProxyRoute &proxy, const ProxyExecutionInput &input, bool websocket,
                                   const AccessRequestTelemetry &telemetry, http::HttpHeaders &headers,
                                   std::vector<EvaluatedTemplate> &evaluated_values) {
    if (!headers.set("Host", endpoint.host_header)) {
        return std::unexpected(request_head_build_error());
    }
    bool host_uses_selected_endpoint = true;

    if (websocket && (!headers.set("Connection", "upgrade") || !headers.set("Upgrade", "websocket"))) {
        return std::unexpected(request_head_build_error());
    }

    for (const CompiledHeaderTemplates::EntryView header: proxy.proxy_headers) {
        auto value = evaluate_template(header.value(), input.template_evaluator);
        if (!value) {
            return std::unexpected(value.error());
        }
        std::string_view value_view = value->view();
        if (value_view.empty() || is_java_filtered_response_header(header.name())) {
            continue;
        }
        if (!is_valid_http_header_name(header.name()) || !is_valid_http_header_value(value_view)) {
            return std::unexpected(Err::from_exception(Exception::unknown("invalid proxy request header")));
        }
        if (value->owns_storage()) {
            evaluated_values.push_back(std::move(*value));
            value_view = evaluated_values.back().view();
        }
        if (!headers.set_view(header.name(), value_view, header.lowcase_name().data(), header.hash())) {
            return std::unexpected(request_head_build_error());
        }
        if (is_header(header.name(), "Host")) {
            host_uses_selected_endpoint = false;
        }
    }

    for (const http::HttpHeaders::HeaderField &header: exchange.request_headers()) {
        if (header.name_len == 0 || is_java_filtered_proxy_request_header(header.name_view()) ||
            proxy.proxy_headers.contains(header.lowcase_view(), header.name_hash)) {
            continue;
        }
        if (!headers.add_view(header.name_view(), header.value_view(), header.lowcase_name, header.name_hash)) {
            return std::unexpected(request_head_build_error());
        }
    }
    if (exchange.header(kTraceParentHeader).empty() &&
        !proxy.proxy_headers.contains(kTraceParentHeader, kTraceParentHeaderHash) &&
        !telemetry.trace_parent().empty() &&
        !headers.set_view(kTraceParentHeader, telemetry.trace_parent(), kTraceParentHeader.data(),
                          kTraceParentHeaderHash)) {
        return std::unexpected(request_head_build_error());
    }

    if (!headers.set_view(kCallSourceHeader, input.call_source, kCallSourceHeader.data(), kCallSourceHeaderHash)) {
        return std::unexpected(request_head_build_error());
    }
    if (!input.origin_host.empty() &&
        !headers.set_view(kOriginHostHeader, input.origin_host, kOriginHostHeader.data(), kOriginHostHeaderHash)) {
        return std::unexpected(request_head_build_error());
    }
    return host_uses_selected_endpoint;
}

ProxyRequestPlanError plan_error(ProxyRequestPlanErrorPhase phase, Err error) noexcept {
    return ProxyRequestPlanError{
            .phase = phase,
            .error = error,
    };
}

} // namespace

http::HttpBodySpec select_proxy_request_body_spec(http::HttpBodySpec inbound, bool has_content_length_header,
                                                  bool websocket) noexcept {
    if (websocket) {
        return http::HttpBodySpec::None();
    }
    if (has_content_length_header && inbound.is_content_length()) {
        return http::HttpBodySpec::ContentLength(inbound.content_length());
    }
    return http::HttpBodySpec::Chunked();
}

std::optional<std::uint64_t>
normalize_proxy_response_body_limit(std::optional<std::int64_t> configured_limit) noexcept {
    if (!configured_limit || *configured_limit == 0) {
        return std::nullopt;
    }
    if (*configured_limit < 0) {
        return 0;
    }
    return static_cast<std::uint64_t>(*configured_limit);
}

ProxyRequestPlan::ProxyRequestPlan(mem::BufPool &pool, const http::HttpExchange &exchange,
                                   const CompiledProxyRoute &proxy) noexcept :
    headers_(pool), websocket_upgrade_(is_websocket_request(exchange, proxy)) {
    body_spec_ = select_proxy_request_body_spec(exchange.request_body_spec(),
                                                !exchange.header("Content-Length").empty(), websocket_upgrade_);
    request_end_stream_ = body_spec_.is_none() || (body_spec_.is_content_length() && body_spec_.content_length() == 0);
    max_response_body_size_ = normalize_proxy_response_body_limit(proxy.max_response_body_size);
    if (websocket_upgrade_ && proxy.websocket_timeout_millis) {
        websocket_timeout_millis_ = *proxy.websocket_timeout_millis;
    }
}

ProxyRequestPlanResult ProxyRequestPlan::prepare(const ProxyUpstreamEndpoint &endpoint,
                                                 const http::HttpExchange &exchange, const CompiledProxyRoute &proxy,
                                                 const ProxyExecutionInput &input,
                                                 const AccessRequestTelemetry &telemetry) {
    FIBER_ASSERT(!prepared_);
    auto resolved_target = resolve_request_target(exchange, proxy, input.template_evaluator, request_target_storage_);
    if (!resolved_target) {
        return std::unexpected(plan_error(ProxyRequestPlanErrorPhase::BuildRequest, resolved_target.error()));
    }
    request_target_ = *resolved_target;

    evaluated_header_values_.reserve(proxy.proxy_headers.dynamic_size());
    auto built_headers = build_request_headers(endpoint, exchange, proxy, input, websocket_upgrade_, telemetry,
                                               headers_, evaluated_header_values_);
    if (!built_headers) {
        return std::unexpected(plan_error(ProxyRequestPlanErrorPhase::BuildHeaders, built_headers.error()));
    }
    host_uses_selected_endpoint_ = *built_headers;
    prepared_ = true;
    return {};
}

ProxyRequestPlanResult ProxyRequestPlan::rebind_endpoint(const ProxyUpstreamEndpoint &endpoint) noexcept {
    FIBER_ASSERT(prepared_);
    if (host_uses_selected_endpoint_ && !headers_.set("Host", endpoint.host_header)) {
        return std::unexpected(plan_error(ProxyRequestPlanErrorPhase::BuildHeaders, request_head_build_error()));
    }
    return {};
}

http::Http1RequestHead ProxyRequestPlan::request_head(http::HttpMethod method) const noexcept {
    FIBER_ASSERT(prepared_);
    return http::Http1RequestHead{
            .method = method,
            .target = request_target_,
            .headers = &headers_,
            .body = body_spec_,
    };
}

} // namespace fiber::access_server
