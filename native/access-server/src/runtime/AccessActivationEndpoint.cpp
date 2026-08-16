#include "AccessActivationEndpoint.h"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <fiber/common/Assert.h>
#include <fiber/common/json/JsonEncode.h>
#include <fiber/http/HttpBodySpec.h>
#include <fiber/http/HttpExchange.h>
#include <fiber/http/HttpHeaders.h>

namespace fiber::access_server {
namespace {

class StringSink final : public json::OutputSink {
public:
    explicit StringSink(std::string &output) noexcept : output_(&output) {}

    [[nodiscard]] bool write(const char *data, std::size_t length) override {
        output_->append(data, length);
        return true;
    }

private:
    std::string *output_ = nullptr;
};

struct PageRequest {
    std::optional<std::uint64_t> revision;
    std::size_t offset = 0;
    std::size_t limit = kAccessActivationEvidenceDefaultPageSize;
};

bool generated(json::Generator::Result result) noexcept { return result == json::Generator::Result::OK; }

bool key(json::Generator &generator, std::string_view value) {
    return generated(generator.string(value.data(), value.size()));
}

bool text(json::Generator &generator, std::string_view value) {
    return generated(generator.string(value.data(), value.size()));
}

bool nullable_text(json::Generator &generator, std::string_view value) {
    return value.empty() ? generated(generator.null_value()) : text(generator, value);
}

bool decimal(json::Generator &generator, std::uint64_t value) { return text(generator, std::to_string(value)); }

bool nullable_integer(json::Generator &generator, const std::optional<std::int32_t> &value) {
    return value ? generated(generator.integer(*value)) : generated(generator.null_value());
}

std::string_view lifecycle_name(AccessNacosLifecycleState state) noexcept {
    switch (state) {
        case AccessNacosLifecycleState::Created:
            return "created";
        case AccessNacosLifecycleState::Starting:
            return "starting";
        case AccessNacosLifecycleState::Running:
            return "running";
        case AccessNacosLifecycleState::Failed:
            return "failed";
        case AccessNacosLifecycleState::Stopping:
            return "stopping";
        case AccessNacosLifecycleState::Stopped:
            return "stopped";
        case AccessNacosLifecycleState::Count:
            break;
    }
    return "created";
}

bool write_failure(json::Generator &generator, const std::optional<AccessActivationFailure> &failure) {
    if (!failure) {
        return generated(generator.null_value());
    }
    return generated(generator.map_open()) && key(generator, "stage") && text(generator, failure->stage) &&
           key(generator, "code") && text(generator, failure->code) && key(generator, "field") &&
           text(generator, failure->field) && key(generator, "offset") &&
           generated(generator.integer(static_cast<std::int64_t>(failure->offset))) &&
           key(generator, "observedAtUnixMillis") && generated(generator.integer(failure->observed_at_unix_millis)) &&
           generated(generator.map_close());
}

bool write_resource(json::Generator &generator, const AccessActivationResourceEvidence &resource) {
    return generated(generator.map_open()) && key(generator, "dataId") && text(generator, resource.data_id) &&
           key(generator, "group") && text(generator, resource.group) && key(generator, "candidateStatus") &&
           text(generator, access_activation_candidate_status_name(resource.candidate_status)) &&
           key(generator, "observedMd5") && nullable_text(generator, resource.observed_md5) &&
           key(generator, "activeMd5") && nullable_text(generator, resource.active_md5) &&
           key(generator, "observedAtUnixMillis") && generated(generator.integer(resource.observed_at_unix_millis)) &&
           key(generator, "activeAtUnixMillis") && generated(generator.integer(resource.active_at_unix_millis)) &&
           key(generator, "failure") && write_failure(generator, resource.failure) && generated(generator.map_close());
}

bool write_project(json::Generator &generator, const AccessActivationProjectEvidence &project) {
    return generated(generator.map_open()) && key(generator, "name") && text(generator, project.name) &&
           key(generator, "dataId") && text(generator, project.data_id) && key(generator, "group") &&
           text(generator, project.group) && key(generator, "subscriptionState") &&
           text(generator, project.subscription_state) && key(generator, "candidateStatus") &&
           text(generator, access_activation_candidate_status_name(project.candidate_status)) &&
           key(generator, "observedMd5") && nullable_text(generator, project.observed_md5) &&
           key(generator, "observedVersion") && nullable_integer(generator, project.observed_version) &&
           key(generator, "activeMd5") && nullable_text(generator, project.active_md5) &&
           key(generator, "activeVersion") && nullable_integer(generator, project.active_version) &&
           key(generator, "activeSnapshotGeneration") &&
           (project.active_snapshot_generation == 0 ? generated(generator.null_value())
                                                    : decimal(generator, project.active_snapshot_generation)) &&
           key(generator, "activeLoaded") && generated(generator.bool_value(project.active_loaded)) &&
           key(generator, "observedAtUnixMillis") && generated(generator.integer(project.observed_at_unix_millis)) &&
           key(generator, "activeAtUnixMillis") && generated(generator.integer(project.active_at_unix_millis)) &&
           key(generator, "failure") && write_failure(generator, project.failure) && generated(generator.map_close());
}

bool constant_time_equal(std::string_view left, std::string_view right) noexcept {
    std::size_t difference = left.size() ^ right.size();
    for (std::size_t index = 0; index < right.size(); ++index) {
        const std::uint8_t value = index < left.size() ? static_cast<std::uint8_t>(left[index]) : 0U;
        difference |= value ^ static_cast<std::uint8_t>(right[index]);
    }
    return difference == 0;
}

bool authorized(const http::HttpExchange &exchange, std::string_view token) noexcept {
    constexpr std::string_view prefix = "Bearer ";
    const std::string_view header = exchange.header("Authorization");
    return header.starts_with(prefix) && constant_time_equal(header.substr(prefix.size()), token);
}

bool parse_size(std::string_view value, std::size_t &output) noexcept {
    if (value.empty()) {
        return false;
    }
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), output);
    return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size();
}

bool parse_cursor(std::string_view value, PageRequest &request) noexcept {
    std::size_t separator = value.find(':');
    std::size_t separator_size = 1U;
    if (separator == std::string_view::npos) {
        separator = value.find("%3A");
        if (separator == std::string_view::npos) {
            separator = value.find("%3a");
        }
        separator_size = 3U;
    }
    if (separator == std::string_view::npos || value.find(':', separator + separator_size) != std::string_view::npos ||
        value.find("%3A", separator + separator_size) != std::string_view::npos ||
        value.find("%3a", separator + separator_size) != std::string_view::npos) {
        return false;
    }
    std::uint64_t revision = 0;
    const std::string_view revision_text = value.substr(0, separator);
    const auto parsed_revision =
            std::from_chars(revision_text.data(), revision_text.data() + revision_text.size(), revision);
    std::size_t offset = 0;
    if (parsed_revision.ec != std::errc{} || parsed_revision.ptr != revision_text.data() + revision_text.size() ||
        revision == 0 || !parse_size(value.substr(separator + separator_size), offset) || offset == 0) {
        return false;
    }
    request.revision = revision;
    request.offset = offset;
    return true;
}

std::optional<PageRequest> parse_page(std::string_view query) noexcept {
    PageRequest request;
    bool cursor_seen = false;
    bool limit_seen = false;
    while (!query.empty()) {
        const std::size_t separator = query.find('&');
        const std::string_view item = separator == std::string_view::npos ? query : query.substr(0, separator);
        const std::size_t equals = item.find('=');
        if (equals == std::string_view::npos || item.find('=', equals + 1U) != std::string_view::npos) {
            return std::nullopt;
        }
        const std::string_view name = item.substr(0, equals);
        const std::string_view value = item.substr(equals + 1U);
        if (name == "cursor" && !cursor_seen) {
            cursor_seen = true;
            if (!parse_cursor(value, request)) {
                return std::nullopt;
            }
        } else if (name == "limit" && !limit_seen) {
            limit_seen = true;
            if (!parse_size(value, request.limit) || request.limit == 0 ||
                request.limit > kAccessActivationEvidenceMaxPageSize) {
                return std::nullopt;
            }
        } else {
            return std::nullopt;
        }
        query = separator == std::string_view::npos ? std::string_view{} : query.substr(separator + 1U);
        if (separator != std::string_view::npos && query.empty()) {
            return std::nullopt;
        }
    }
    return request;
}

std::string encode_page(const AccessActivationEvidenceSnapshot &snapshot, const AccessDiscoveryStatus &discovery,
                        std::size_t offset, std::size_t limit) {
    std::string output;
    output.reserve(8192U + limit * 1024U);
    StringSink sink(output);
    json::Generator generator(sink);
    generator.set_option(json::Generator::Option::ValidateUtf8);

    const std::size_t end = std::min(snapshot.route.projects.size(), offset + limit);
    bool ok = generated(generator.map_open()) && key(generator, "contractVersion") &&
              generated(generator.integer(kAccessActivationEvidenceContractVersion)) &&
              key(generator, "evidenceRevision") && decimal(generator, snapshot.revision) &&
              key(generator, "instance") && generated(generator.map_open()) && key(generator, "id") &&
              text(generator, snapshot.identity.instance_id) && key(generator, "buildVersion") &&
              text(generator, snapshot.identity.build_version) && key(generator, "buildRevision") &&
              text(generator, snapshot.identity.build_revision) && key(generator, "startedAtUnixMillis") &&
              generated(generator.integer(snapshot.identity.started_at_unix_millis)) &&
              generated(generator.map_close()) && key(generator, "runtime") && generated(generator.map_open()) &&
              key(generator, "state") && text(generator, "running") && generated(generator.map_close()) &&
              key(generator, "routeSnapshot") && generated(generator.map_open()) && key(generator, "generation") &&
              decimal(generator, snapshot.route.snapshot_generation) && key(generator, "fingerprintSha256") &&
              text(generator, snapshot.route_snapshot_fingerprint_sha256) && key(generator, "publishedAtUnixMillis") &&
              generated(generator.integer(snapshot.route.snapshot_published_at_unix_millis)) &&
              key(generator, "publicationMode") && text(generator, "atomic_request_pin") &&
              generated(generator.map_close()) && key(generator, "accessConfig") && generated(generator.map_open()) &&
              key(generator, "watcherState") && text(generator, snapshot.route.watcher_state) &&
              key(generator, "readinessState") && text(generator, snapshot.route.readiness_state) &&
              key(generator, "projectList") && write_resource(generator, snapshot.route.project_list) &&
              generated(generator.map_close()) && key(generator, "projects") && generated(generator.map_open()) &&
              key(generator, "items") && generated(generator.array_open());
    for (std::size_t index = offset; ok && index < end; ++index) {
        ok = write_project(generator, snapshot.route.projects[index]);
    }
    ok = ok && generated(generator.array_close()) && key(generator, "nextCursor");
    if (ok && end < snapshot.route.projects.size()) {
        ok = text(generator, std::to_string(snapshot.revision) + ':' + std::to_string(end));
    } else if (ok) {
        ok = generated(generator.null_value());
    }
    ok = ok && generated(generator.map_close()) && key(generator, "gray") && generated(generator.map_open()) &&
         key(generator, "watcherState") && text(generator, snapshot.gray.watcher_state) && key(generator, "resource") &&
         write_resource(generator, snapshot.gray.resource) && key(generator, "generation") &&
         decimal(generator, snapshot.gray.generation) && key(generator, "ruleCount") &&
         generated(generator.integer(static_cast<std::int64_t>(snapshot.gray.rule_count))) &&
         generated(generator.map_close()) && key(generator, "tls") && generated(generator.map_open()) &&
         key(generator, "enabled") && generated(generator.bool_value(snapshot.tls.enabled)) &&
         key(generator, "watcherState") && text(generator, snapshot.tls.watcher_state) && key(generator, "resource") &&
         write_resource(generator, snapshot.tls.resource) && key(generator, "version") &&
         decimal(generator, snapshot.tls.version) && key(generator, "certificateCount") &&
         generated(generator.integer(static_cast<std::int64_t>(snapshot.tls.certificate_count))) &&
         generated(generator.map_close()) && key(generator, "discovery") && generated(generator.map_open()) &&
         key(generator, "clientState") && text(generator, lifecycle_name(discovery.lifecycle[0])) &&
         key(generator, "configServiceState") && text(generator, lifecycle_name(discovery.lifecycle[1])) &&
         key(generator, "namingServiceState") && text(generator, lifecycle_name(discovery.lifecycle[2])) &&
         key(generator, "readyServices") &&
         generated(generator.integer(static_cast<std::int64_t>(discovery.ready_services))) &&
         key(generator, "selectableEndpoints") &&
         generated(generator.integer(static_cast<std::int64_t>(discovery.selectable_endpoints))) &&
         key(generator, "logicalClusters") &&
         generated(generator.integer(static_cast<std::int64_t>(discovery.logical_clusters))) &&
         key(generator, "selectorLeases") &&
         generated(generator.integer(static_cast<std::int64_t>(discovery.selector_leases))) &&
         generated(generator.map_close()) && generated(generator.map_close());
    return ok ? output : std::string{};
}

async::Task<void> send(http::HttpExchange &exchange, int status, std::string_view content_type, std::string_view body,
                       std::string_view allow = {}) noexcept {
    http::HttpHeaders headers(exchange.pool());
    headers.set_view("Content-Type", content_type);
    headers.set_view("Cache-Control", "no-store");
    if (!allow.empty()) {
        headers.set_view("Allow", allow);
    }
    const auto sent = co_await exchange.send_header({
            .kind = http::OutgoingHeaderKind::Final,
            .status_code = status,
            .headers = &headers,
            .body = http::HttpBodySpec::ContentLength(body.size()),
            .connection_mode = http::ResponseConnectionMode::Auto,
            .end_stream = body.empty(),
    });
    if (sent && !body.empty()) {
        (void) co_await exchange.write_all(reinterpret_cast<const std::uint8_t *>(body.data()), body.size(), true);
    }
}

} // namespace

AccessActivationEndpoint::AccessActivationEndpoint(const AccessActivationEvidenceStore *evidence,
                                                   const AccessDiscoveryMetrics *discovery,
                                                   AccessActivationEndpointOptions options) :
    evidence_(evidence), discovery_(discovery), options_(std::move(options)) {}

async::Task<void> AccessActivationEndpoint::handle(http::HttpExchange &exchange) const noexcept {
    if (exchange.uri().path != kAccessActivationEvidencePath || !options_.enabled || evidence_ == nullptr ||
        discovery_ == nullptr) {
        co_await send(exchange, 404, "application/json; charset=utf-8", R"({"error":"not_found"})");
        co_return;
    }
    if (exchange.method() != http::HttpMethod::Get) {
        co_await send(exchange, 405, "application/json; charset=utf-8", R"({"error":"method_not_allowed"})", "GET");
        co_return;
    }
    if (!authorized(exchange, options_.bearer_token)) {
        co_await send(exchange, 401, "application/json; charset=utf-8", R"({"error":"unauthorized"})");
        co_return;
    }
    const std::optional<PageRequest> page = parse_page(exchange.uri().query);
    if (!page) {
        co_await send(exchange, 400, "application/json; charset=utf-8", R"({"error":"invalid_page"})");
        co_return;
    }

    const std::shared_ptr<const AccessActivationEvidenceSnapshot> snapshot = evidence_->pin();
    if (page->revision && *page->revision != snapshot->revision) {
        co_await send(exchange, 409, "application/json; charset=utf-8", R"({"error":"evidence_changed"})");
        co_return;
    }
    if (page->offset > snapshot->route.projects.size()) {
        co_await send(exchange, 400, "application/json; charset=utf-8", R"({"error":"invalid_page"})");
        co_return;
    }
    const std::string body = encode_page(*snapshot, discovery_->status(), page->offset, page->limit);
    if (body.empty()) {
        co_await send(exchange, 500, "application/json; charset=utf-8", R"({"error":"encoding_failed"})");
        co_return;
    }
    co_await send(exchange, 200, "application/json; charset=utf-8", body);
}

} // namespace fiber::access_server
