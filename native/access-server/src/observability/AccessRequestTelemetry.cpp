#include "AccessRequestTelemetry.h"

#include <fiber/http/HttpExchange.h>

#include <array>

namespace fiber::access_server {
namespace {

constexpr std::string_view kTraceCluster = "HI-TRACE-CLUSTER";

// access-server deliberately keeps this list fixed. It covers the text and
// structured-data representations emitted by the gateway while avoiding
// already-compressed media and arbitrary binary payloads.
constexpr std::array<std::string_view, 10> kGzipMimeTypes{
        "text/html",
        "text/plain",
        "text/css",
        "text/javascript",
        "application/javascript",
        "application/json",
        "application/xml",
        "application/rss+xml",
        "application/wasm",
        "image/svg+xml",
};

const ClientMetadataResolver &default_client_metadata_resolver() noexcept {
    static const ClientMetadataResolver resolver;
    return resolver;
}

} // namespace

AccessRequestTelemetry::AccessRequestTelemetry(http::HttpExchange &exchange, AccessServerMetrics::Worker *metrics,
                                               cat::CatClient *cat_client, const AccessLogPolicy *access_log_policy,
                                               const ClientMetadataResolver *client_metadata_resolver) noexcept :
    execution_(exchange), response_writer_(http::make_http_response_writer(exchange)),
    client_metadata_((client_metadata_resolver ? *client_metadata_resolver : default_client_metadata_resolver())
                             .resolve(exchange)),
    trace_(exchange.pool()),
    observability_(exchange, metrics, cat_client, access_log_policy, client_metadata_, trace_) {}

AccessRequestTelemetry::~AccessRequestTelemetry() noexcept {
    if (gzip_writer_ && !response_compression_recorded_) {
        switch (gzip_writer_->stats().decision) {
            case http::GzipResponseDecision::Active:
            case http::GzipResponseDecision::Completed:
                observability_.record_response_compression(true);
                break;
            case http::GzipResponseDecision::Bypassed:
                observability_.record_response_compression(false);
                break;
            case http::GzipResponseDecision::Undecided:
            case http::GzipResponseDecision::Failed:
                break;
        }
    }
    observability_.finish(execution_.exchange(), client_metadata_, trace_.trace_id());
}

common::IoResult<void> AccessRequestTelemetry::enable_response_compression(std::uint8_t level,
                                                                           bool request_accepts_gzip) noexcept {
    if (level < 1 || level > 9) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (gzip_writer_) {
        return std::unexpected(common::IoErr::Already);
    }

    const http::GzipResponseWriterOptions options{
            .enabled = true,
            .any_type = false,
            .types = {},
            .type_views = kGzipMimeTypes,
            .min_length = 20,
            .compression_level = static_cast<int>(level),
            .request_accepts_gzip = request_accepts_gzip,
            .all_body_statuses = true,
    };
    gzip_writer_.emplace(execution_.exchange(), response_writer_, options);
    response_writer_ = gzip_writer_->writer();
    execution_.script_context().set_response_writer(response_writer_);
    return {};
}

void AccessRequestTelemetry::set_project(std::string_view project, std::string_view effective_host,
                                         std::string_view context_cluster) noexcept {
    observability_.set_project(execution_, project, effective_host, context_cluster);
    if (!context_cluster.empty()) {
        (void) trace_.put_trace_context(execution_, observability_.root_transaction(), kTraceCluster, context_cluster);
    }
}

} // namespace fiber::access_server
