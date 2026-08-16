#include "AccessRequestTelemetry.h"

#include <fiber/http/HttpExchange.h>

namespace fiber::access_server {
namespace {

constexpr std::string_view kTraceCluster = "HI-TRACE-CLUSTER";

const ClientMetadataResolver &default_client_metadata_resolver() noexcept {
    static const ClientMetadataResolver resolver;
    return resolver;
}

} // namespace

AccessRequestTelemetry::AccessRequestTelemetry(http::HttpExchange &exchange, AccessServerMetrics::Worker *metrics,
                                               cat::CatClient *cat_client, const AccessLogPolicy *access_log_policy,
                                               const ClientMetadataResolver *client_metadata_resolver) noexcept :
    execution_(exchange),
    client_metadata_((client_metadata_resolver ? *client_metadata_resolver : default_client_metadata_resolver())
                             .resolve(exchange)),
    trace_(exchange.pool()),
    observability_(exchange, metrics, cat_client, access_log_policy, client_metadata_, trace_) {}

AccessRequestTelemetry::~AccessRequestTelemetry() noexcept {
    observability_.finish(execution_.exchange(), client_metadata_, trace_.trace_id());
}

void AccessRequestTelemetry::set_project(std::string_view project, std::string_view effective_host,
                                         std::string_view context_cluster) noexcept {
    observability_.set_project(execution_, project, effective_host, context_cluster);
    if (!context_cluster.empty()) {
        (void) trace_.put_trace_context(execution_, observability_.root_transaction(), kTraceCluster, context_cluster);
    }
}

} // namespace fiber::access_server
