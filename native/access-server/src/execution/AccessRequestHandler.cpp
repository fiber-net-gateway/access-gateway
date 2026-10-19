#include "AccessRequestHandler.h"
#include "../observability/AccessRequestTelemetry.h"
#include "ResponsePlan.h"

#include <fiber/common/Assert.h>
#include <fiber/http/HttpBodySpec.h>
#include <fiber/http/HttpExchange.h>
#include <fiber/http/HttpHeaderHash.h>
#include <fiber/http/HttpHeaders.h>

#include <utility>

namespace fiber::access_server {
namespace {

constexpr std::string_view kTraceCluster = "HI-TRACE-CLUSTER";
constexpr std::string_view kGrayCluster = "gray"; // Java PlotoConstants.GRAY_CLUSTER.
constexpr std::string_view kOriginHostHeader = "ploto-origin-host";
constexpr std::string_view kHostHeaderName = "host";
constexpr std::string_view kHostHeaderLowcase = "host";
constexpr std::uint64_t kHostHeaderHash = http::http_header_name_hash(kHostHeaderLowcase);
constexpr std::string_view kStrictTransportSecurity = "Strict-Transport-Security";
constexpr std::string_view kStrictTransportSecurityLowcase = "strict-transport-security";
constexpr std::uint64_t kStrictTransportSecurityHash = http::http_header_name_hash(kStrictTransportSecurityLowcase);
constexpr std::string_view kStrictTransportSecurityValue = "max-age=31536000";

struct RequestHostContext {
    std::string normalized_host;
    std::string_view effective_host;
    // Non-empty only when the test-mode host rewrite happened; gates the outbound
    // ploto-origin-host header.
    std::string_view origin_host;
};

struct RequestEvaluationContext {
    AccessRequestScriptAdapter adapter;
    AccessRequestTelemetry &telemetry;
};

bool uses_dynamic_response_compression(const CompiledRoute &route) noexcept {
    if (!route.gzip_level) {
        return false;
    }
    if (route.type == RouteType::Proxy || route.type == RouteType::Script) {
        return true;
    }
    return route.response && route.response->body_kind == ResponseBodyKind::Template;
}

Result<void> evaluate_template(void *context, const script::Script &program, std::string_view expression,
                               std::string &output) noexcept {
    auto &request = *static_cast<RequestEvaluationContext *>(context);
    if (!request.adapter.evaluate_template) {
        return std::unexpected(Err::from_exception(Exception{
                .name = "TEMPLATE_SCRIPT",
                .message = "error exec for template expression: template evaluator is not configured",
                .status = 500,
        }));
    }
    return request.adapter.evaluate_template(request.adapter.context, request.telemetry.script_context(), program,
                                             expression, output);
}

struct RouteMatch {
    const CompiledRoute *route = nullptr;
    std::span<const PathVariable> path_variables;

    [[nodiscard]] explicit operator bool() const noexcept { return route != nullptr; }
};

class RouteMatchContext {
public:
    RouteMatchContext(const std::vector<CompiledRoute> &routes, std::span<PathVariable> path_variables,
                      std::string_view method, RequestEvaluationContext &request) noexcept :
        routes_(routes), path_variables_(path_variables), method_(method), request_(request) {}

    bool matched(std::uint32_t, std::uint32_t route_index) noexcept {
        const CompiledRoute &route = routes_[route_index];
        if (route.method && *route.method != method_) {
            return false;
        }
        const std::span<const PathVariable> captures = path_variables_.first(path_variable_count_);
        std::size_t bound_count = 0;
        for (; bound_count < route.path_constant_indices.size() && bound_count < captures.size(); ++bound_count) {
            const http_script::ConstIndex index = route.path_constant_indices[bound_count];
            if (index != http_script::kInvalidConstIndex &&
                !request_.telemetry.script_context().bind_constant(index, captures[bound_count].value)) {
                request_.telemetry.script_context().clear_constants(route.path_constant_indices);
                return false;
            }
        }
        if (route.condition_program &&
            (!request_.adapter.evaluate_condition ||
             !request_.adapter.evaluate_condition(request_.adapter.context, request_.telemetry.script_context(),
                                                  *route.condition_program))) {
            request_.telemetry.script_context().clear_constants(route.path_constant_indices);
            return false;
        }
        matched_route_ = &route;
        matched_path_variables_ = captures;
        return true;
    }

    void add_path_var(std::string_view name, std::string_view value) noexcept {
        path_variables_[path_variable_count_++] = PathVariable{
                .name = name,
                .value = value,
        };
    }

    void pop_path_var() noexcept { --path_variable_count_; }

    [[nodiscard]] RouteMatch result() const noexcept {
        return RouteMatch{
                .route = matched_route_,
                .path_variables = matched_path_variables_,
        };
    }

private:
    const std::vector<CompiledRoute> &routes_;
    std::span<PathVariable> path_variables_;
    std::string_view method_;
    RequestEvaluationContext &request_;
    const CompiledRoute *matched_route_ = nullptr;
    std::span<const PathVariable> matched_path_variables_;
    std::size_t path_variable_count_ = 0;
};

common::IoResult<RouteMatch> match_route(const ProjectRouteSnapshot &project, http::HttpExchange &exchange,
                                         RequestEvaluationContext &evaluation) noexcept {
    const std::size_t variable_capacity = project.max_path_variable_count();
    PathVariable *variable_data = nullptr;
    if (variable_capacity != 0) {
        variable_data = static_cast<PathVariable *>(
                exchange.pool().alloc(variable_capacity * sizeof(PathVariable), alignof(PathVariable)));
        if (!variable_data) {
            return std::unexpected(common::IoErr::NoMem);
        }
    }

    std::span<PathVariable> path_variables(variable_data, variable_capacity);
    RouteMatchContext context(project.routes(), path_variables, exchange.method_view(), evaluation);
    (void) project.match_route_path(exchange.uri().path, context);
    return context.result();
}

// Mirrors Java ExtClusterNameFetcher: in test mode the request Host header may carry a
// "_<cluster>" segment before the first dot ("api_gray.example.com"). The suffix selects the
// upstream cluster and the exchange's Host header is rewritten to the stripped form, so every
// later reader — $header.host templates and route conditions, Location/Refresh rewriting, the
// websocket session record — observes the same host Java would have set. The original host
// stays in RequestHostContext::origin_host for the outbound ploto-origin-host header.
// The rewrite uses the pool-copying set() overload and reseats the first-wins host ref, per
// the upstream mutable-request-phase contract; it must run before the first $header access.
//
// request_cluster mirrors Java HeaderToCtxInterceptor: the test-mode request-specified
// cluster (host suffix wins over the HI-TRACE-CLUSTER request header; empty in production)
// is written into the trace context here, once, before host matching — every later reader
// (cluster selection, CAT root data, the compat access log) looks it up there, exactly as
// the Java gateway reads the same key from trace userData.
std::expected<RequestHostContext, Err> resolve_request_host(http::HttpExchange &exchange, bool test_mode,
                                                            AccessRequestTelemetry &telemetry) noexcept {
    RequestHostContext result;
    const std::string_view origin_host = exchange.header("Host");
    result.effective_host = origin_host;
    std::string_view request_cluster;
    if (test_mode) {
        const std::size_t underscore = origin_host.find('_');
        const std::size_t dot =
                underscore == std::string_view::npos ? std::string_view::npos : origin_host.find('.', underscore);
        if (underscore > 0 && underscore != std::string_view::npos && dot > underscore &&
            dot != std::string_view::npos) {
            result.normalized_host.reserve(origin_host.size() - (dot - underscore));
            result.normalized_host.append(origin_host.substr(0, underscore));
            result.normalized_host.append(origin_host.substr(dot));
            result.effective_host = result.normalized_host;
            result.origin_host = origin_host;
            request_cluster = origin_host.substr(underscore + 1, dot - underscore - 1);
            http::HttpHeaders::HeaderField *const host_field = exchange.request_headers().set(
                    kHostHeaderName, result.normalized_host, kHostHeaderLowcase.data(), kHostHeaderHash);
            if (host_field == nullptr) {
                return std::unexpected(Err::from_error(common::IoErr::NoMem));
            }
            exchange.request_header_refs().host = host_field;
        } else {
            request_cluster = exchange.header(kTraceCluster);
        }
    }
    // The put is guarded: an empty value would re-seat a propagated entry to empty and
    // corrupt the encoded tracestate.
    if (!request_cluster.empty()) {
        auto stored = telemetry.put_trace_context(kTraceCluster, request_cluster);
        if (!stored) {
            return std::unexpected(Err::from_error(stored.error()));
        }
    }
    return result;
}

async::Task<Result<void>> send_redirect(http::HttpExchange &exchange, int status, std::string_view host,
                                        std::chrono::milliseconds timeout, AccessRequestTelemetry &telemetry) noexcept {
    std::string location = "https://";
    location.append(host);
    location.append(exchange.uri().unparsed_uri);

    http::HttpHeaders &headers = telemetry.response_headers();
    if (!headers.set("Location", location) || !telemetry.finalize_response_headers()) {
        co_return std::unexpected(Err::from_error(common::IoErr::NoMem));
    }

    auto sent = co_await telemetry.response_writer().send_header(
            {
                    .kind = http::OutgoingHeaderKind::Final,
                    .status_code = status,
                    .headers = &headers,
                    .body = http::HttpBodySpec::ContentLength(0),
                    .connection_mode = http::ResponseConnectionMode::Auto,
                    .end_stream = true,
            },
            timeout);
    if (!sent) {
        co_return std::unexpected(Err::from_error(sent.error()));
    }
    co_return Result<void>{};
}

} // namespace

AccessRequestHandler::AccessRequestHandler(AccessRouteSnapshotProvider snapshot_provider,
                                           AccessRequestScriptAdapter script_adapter,
                                           AccessRequestHandlerOptions options,
                                           AccessProxyAdapter proxy_adapter) noexcept :
    snapshot_provider_(snapshot_provider), script_adapter_(script_adapter), options_(options),
    proxy_adapter_(proxy_adapter), policy_evaluator_(options.default_max_request_body_size),
    response_executor_(options.response),
    error_responder_(ErrorResponderOptions{.write_timeout = options.response.write_timeout}) {
    FIBER_ASSERT(snapshot_provider_.context != nullptr);
    FIBER_ASSERT(snapshot_provider_.pin != nullptr);
}

async::Task<void> AccessRequestHandler::handle(http::HttpExchange &exchange,
                                               AccessRequestTelemetry &telemetry) const noexcept {
    auto result = co_await handle_and_finalize(exchange, telemetry);
    if (!result) {
        (void) exchange.abort(result.error());
    }
}

async::Task<common::IoResult<void>>
AccessRequestHandler::handle_and_finalize(http::HttpExchange &exchange,
                                          AccessRequestTelemetry &telemetry) const noexcept {
    auto result = co_await handle_impl(exchange, telemetry);
    if (result) {
        co_return common::IoResult<void>{};
    }
    const Err error = result.error();
    const bool response_header_sent = exchange.response_stats().header_sent;
    if (response_header_sent || exchange.response_channel_closed()) {
        switch (error.kind) {
            case Err::Kind::Error:
                telemetry.mark_io_error(error.error);
                co_return std::unexpected(error.error);
            case Err::Kind::Exception:
                telemetry.record_exception(error.exception);
                break;
            case Err::Kind::UpstreamException:
                telemetry.record_upstream_exception(error.exception);
                break;
        }
        co_return std::unexpected(response_header_sent ? common::IoErr::Already : common::IoErr::Canceled);
    }

    Exception response_error = Exception::unknown();
    switch (error.kind) {
        case Err::Kind::Error:
            telemetry.record_exception(response_error);
            break;
        case Err::Kind::Exception:
            response_error = error.exception;
            telemetry.record_exception(response_error);
            break;
        case Err::Kind::UpstreamException:
            response_error = error.exception;
            telemetry.record_upstream_exception(response_error);
            break;
    }
    auto sent = co_await error_responder_.send(exchange, telemetry, response_error);
    if (!sent) {
        telemetry.record_error_response_failure(sent.error());
    }
    co_return sent;
}

async::Task<Result<void>> AccessRequestHandler::handle_impl(http::HttpExchange &exchange,
                                                            AccessRequestTelemetry &telemetry) const noexcept {
    const std::shared_ptr<const AccessRouteSnapshot> snapshot = snapshot_provider_.pin(snapshot_provider_.context);
    const std::expected<RequestHostContext, Err> resolved_host =
            resolve_request_host(exchange, options_.test_mode, telemetry);
    if (!resolved_host) {
        co_return std::unexpected(resolved_host.error());
    }
    RequestHostContext request_host = std::move(*resolved_host);
    // Java InternalIpGrayInterceptor (order 10, before routing — production only; test mode
    // installs HeaderToCtxInterceptor instead, and resolve_request_host above is its native
    // counterpart, so the two never coexist). A match only writes the trace context: route
    // conditions read it, route context templates may override it, and cluster selection and
    // the compat access log read the final value back from there.
    if (!options_.test_mode && options_.gray_matcher.matches != nullptr &&
        options_.gray_matcher.matches(options_.gray_matcher.context, exchange.header("X-Entry"),
                                      telemetry.client_metadata())) {
        auto gray_stored = telemetry.put_trace_context(kTraceCluster, kGrayCluster);
        if (!gray_stored) {
            co_return std::unexpected(Err::from_error(gray_stored.error()));
        }
    }
    const ProjectHostMatch host_match = snapshot->match_host(request_host.effective_host);
    if (!host_match) {
        co_return std::unexpected(Err::from_exception(Exception::router_not_found()));
    }
    telemetry.set_project(host_match.project->project(), request_host.effective_host,
                          host_match.host != nullptr ? host_match.host->pattern : std::string_view{});
    if (!telemetry.response_headers().set_view(kStrictTransportSecurity, kStrictTransportSecurityValue,
                                               kStrictTransportSecurityLowcase.data(), kStrictTransportSecurityHash)) {
        co_return std::unexpected(Err::from_error(common::IoErr::NoMem));
    }

    const HostPolicyDecision host_policy = policy_evaluator_.evaluate_host(
            host_match.host->strategy, exchange.header("X-Entry"), telemetry.client_metadata().secure);
    switch (host_policy.action) {
        case HostPolicyAction::Allow:
            break;
        case HostPolicyAction::EntryRejected:
            co_return std::unexpected(Err::from_exception(Exception::entry_error()));
        case HostPolicyAction::InvalidHttps:
            co_return std::unexpected(Err::from_exception(Exception::unknown("invalid HTTPS strategy")));
        case HostPolicyAction::Redirect:
            co_return co_await send_redirect(exchange, host_policy.redirect_status, request_host.effective_host,
                                             options_.response.write_timeout, telemetry);
    }

    auto constants_ready = telemetry.script_context().prepare_constants(host_match.project->const_package());
    if (!constants_ready) {
        co_return std::unexpected(Err::from_error(constants_ready.error()));
    }
    auto trace_context_ready = telemetry.bind_trace_context(host_match.project->const_package());
    if (!trace_context_ready) {
        co_return std::unexpected(Err::from_error(trace_context_ready.error()));
    }

    RequestEvaluationContext evaluation{
            .adapter = script_adapter_,
            .telemetry = telemetry,
    };
    auto route_match_result = match_route(*host_match.project, exchange, evaluation);
    if (!route_match_result) {
        co_return std::unexpected(Err::from_error(route_match_result.error()));
    }
    const RouteMatch route_match = *route_match_result;
    if (!route_match) {
        auto exception = make_url_not_matched_exception(exchange.pool(), host_match.project->project());
        if (!exception) {
            co_return std::unexpected(Err::from_error(exception.error()));
        }
        co_return std::unexpected(Err::from_exception(*exception));
    }

    const CompiledRoute &route = *route_match.route;
    telemetry.set_route(route);
    const http::HttpBodySpec body_spec = exchange.request_body_spec();
    const RoutePolicyDecision route_policy =
            policy_evaluator_.evaluate_route(route, telemetry.client_metadata(), body_spec);
    switch (route_policy.action) {
        case RoutePolicyAction::Allow:
            break;
        case RoutePolicyAction::BodyTooLarge:
            co_return std::unexpected(Err::from_exception(Exception::request_body_too_large()));
        case RoutePolicyAction::SourceIpNotAllowed:
            co_return std::unexpected(Err::from_exception(Exception::source_ip_not_allowed()));
    }
    const std::size_t body_limit = route_policy.body_limit;

    if (uses_dynamic_response_compression(route)) {
        const ResponseContentCoding coding = select_response_content_coding(exchange.request_headers());
        if (coding == ResponseContentCoding::NotAcceptable) {
            if (!telemetry.response_headers().set("Vary", "Accept-Encoding")) {
                co_return std::unexpected(Err::from_error(common::IoErr::NoMem));
            }
            telemetry.record_response_compression_not_acceptable();
            co_return std::unexpected(Err::from_exception(Exception::not_acceptable()));
        }
        auto compression =
                telemetry.enable_response_compression(*route.gzip_level, coding == ResponseContentCoding::Gzip);
        if (!compression) {
            co_return std::unexpected(Err::from_error(compression.error()));
        }
    }

    TemplateEvaluator template_evaluator;
    if (script_adapter_.evaluate_template) {
        template_evaluator = TemplateEvaluator{
                .context = &evaluation,
                .evaluate = evaluate_template,
        };
    }

    if (route.type == RouteType::Proxy) {
        if (!proxy_adapter_.execute) {
            co_return std::unexpected(Err::from_exception(Exception::unknown("proxy executor is not configured")));
        }
        // origin_host is non-empty only when the test-mode host rewrite happened, so its
        // presence here is the extracted-cluster marker.
        const std::string_view origin_host =
                !request_host.origin_host.empty() && !route.proxy->proxy_headers.contains(kOriginHostHeader)
                        ? request_host.origin_host
                        : std::string_view{};
        co_return co_await proxy_adapter_.execute(proxy_adapter_.context, exchange, *route.proxy,
                                                  ProxyExecutionInput{
                                                          .call_source = host_match.project->call_source(),
                                                          .origin_host = origin_host,
                                                          .template_evaluator = template_evaluator,
                                                          .max_request_body_size = body_limit,
                                                  },
                                                  telemetry);
    }

    if (route.type == RouteType::Script) {
        if (!RoutePolicyEvaluator::script_body_supported(body_spec)) {
            co_return std::unexpected(Err::from_exception(Exception::request_body_too_large()));
        }
        if (!route.script_program || !script_adapter_.execute_route_script) {
            co_return std::unexpected(
                    Err::from_exception(Exception::unknown("route script executor is not configured")));
        }
        if (!telemetry.finalize_response_headers()) {
            co_return std::unexpected(Err::from_error(common::IoErr::NoMem));
        }
        for (const http::HttpHeaders::HeaderField &field: telemetry.response_headers()) {
            telemetry.script_context().set_response_header(field.name_view(), field.value_view());
        }
        co_return co_await script_adapter_.execute_route_script(script_adapter_.context, telemetry.script_context(),
                                                                *route.script_program);
    }

    co_return co_await response_executor_.execute(exchange, route, telemetry, template_evaluator, body_limit);
}

} // namespace fiber::access_server
