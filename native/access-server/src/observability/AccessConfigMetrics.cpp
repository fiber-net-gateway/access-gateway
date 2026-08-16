#include "AccessConfigMetrics.h"

#include "../routing/AccessRouteSnapshot.h"

#include <array>
#include <charconv>
#include <limits>
#include <string_view>

#include <fiber/common/Assert.h>
#include <fiber/event/EventLoop.h>

namespace fiber::access_server {
namespace {

struct EventDescription {
    std::string_view resource;
    std::string_view result;
    std::string_view reason;
};

constexpr std::array<EventDescription, static_cast<std::size_t>(AccessConfigMetricEvent::Count)> kEvents{
        EventDescription{"project_list", "success", "accepted"},
        EventDescription{"project_list", "failure", "subscription"},
        EventDescription{"project_list", "failure", "decode"},
        EventDescription{"project_route", "ignored", "empty"},
        EventDescription{"project_route", "ignored", "version_unchanged"},
        EventDescription{"project_route", "success", "published"},
        EventDescription{"project_route", "success", "unloaded"},
        EventDescription{"project_route", "success", "removed"},
        EventDescription{"project_route", "failure", "subscription"},
        EventDescription{"project_route", "failure", "decode"},
        EventDescription{"project_route", "failure", "compile"},
        EventDescription{"project_route", "failure", "service_ready"},
        EventDescription{"project_route", "failure", "publish"},
};

constexpr std::array<std::string_view, 5> kReadinessStates{
        "waiting_for_project_list", "synchronizing_projects", "ready", "unavailable", "stopped",
};

void append_unsigned(std::string &output, std::uint64_t value) {
    std::array<char, std::numeric_limits<std::uint64_t>::digits10 + 2> buffer{};
    const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    FIBER_ASSERT(converted.ec == std::errc{});
    output.append(buffer.data(), converted.ptr);
}

void append_series(std::string &output, std::string_view name, std::string_view labels, std::uint64_t value) {
    output.append(name);
    output.append(labels);
    output.push_back(' ');
    append_unsigned(output, value);
    output.push_back('\n');
}

std::int64_t steady_nanoseconds(std::chrono::steady_clock::time_point value) noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(value.time_since_epoch()).count();
}

} // namespace

AccessConfigMetrics::AccessConfigMetrics(event::EventLoop &owner) noexcept : owner_(&owner) {}

AccessConfigMetricsObserver AccessConfigMetrics::observer() noexcept {
    return AccessConfigMetricsObserver{
            .context = this,
            .on_event = &observe_event,
            .on_readiness = &observe_readiness,
            .on_snapshot = &observe_snapshot,
    };
}

void AccessConfigMetrics::observe_event(void *context, AccessConfigMetricEvent event) noexcept {
    static_cast<AccessConfigMetrics *>(context)->record_event(event);
}

void AccessConfigMetrics::observe_readiness(void *context, const AccessConfigMetricReadiness &readiness) noexcept {
    static_cast<AccessConfigMetrics *>(context)->update_readiness(readiness);
}

void AccessConfigMetrics::observe_snapshot(void *context, const AccessRouteSnapshot &snapshot) noexcept {
    static_cast<AccessConfigMetrics *>(context)->update_snapshot(snapshot);
}

void AccessConfigMetrics::record_event(AccessConfigMetricEvent event) noexcept {
    FIBER_ASSERT(owner_->in_loop());
    const std::size_t index = static_cast<std::size_t>(event);
    FIBER_ASSERT(index < events_.size());
    events_[index].fetch_add(1, std::memory_order_relaxed);
}

void AccessConfigMetrics::update_readiness(const AccessConfigMetricReadiness &readiness) noexcept {
    FIBER_ASSERT(owner_->in_loop());
    begin_update();
    readiness_state_.store(static_cast<std::uint8_t>(readiness.state), std::memory_order_relaxed);
    desired_projects_.store(readiness.desired_projects, std::memory_order_relaxed);
    subscribed_projects_.store(readiness.subscribed_projects, std::memory_order_relaxed);
    synchronized_projects_.store(readiness.synchronized_projects, std::memory_order_relaxed);
    retrying_projects_.store(readiness.retrying_projects, std::memory_order_relaxed);
    processing_projects_.store(readiness.processing_projects, std::memory_order_relaxed);
    rejected_projects_.store(readiness.rejected_projects, std::memory_order_relaxed);
    finish_update();
}

void AccessConfigMetrics::update_snapshot(const AccessRouteSnapshot &snapshot) noexcept {
    FIBER_ASSERT(owner_->in_loop());
    begin_update();
    snapshot_projects_.store(snapshot.projects().size(), std::memory_order_relaxed);
    snapshot_hosts_.store(snapshot.host_count(), std::memory_order_relaxed);
    snapshot_routes_.store(snapshot.route_count(), std::memory_order_relaxed);
    snapshot_compiled_programs_.store(snapshot.compiled_program_count(), std::memory_order_relaxed);
    snapshot_estimated_bytes_.store(snapshot.estimated_memory_bytes(), std::memory_order_relaxed);
    snapshot_static_response_bytes_.store(snapshot.static_response_bytes(), std::memory_order_relaxed);
    snapshot_published_at_nanoseconds_.store(steady_nanoseconds(event::EventLoop::current().now()),
                                             std::memory_order_relaxed);
    const std::uint64_t generation = snapshot_generation_.load(std::memory_order_relaxed);
    FIBER_ASSERT(generation != std::numeric_limits<std::uint64_t>::max());
    snapshot_generation_.store(generation + 1, std::memory_order_relaxed);
    finish_update();
}

void AccessConfigMetrics::begin_update() noexcept {
    const std::uint64_t previous = sequence_.fetch_add(1, std::memory_order_acq_rel);
    FIBER_ASSERT((previous & 1U) == 0);
}

void AccessConfigMetrics::finish_update() noexcept {
    const std::uint64_t previous = sequence_.fetch_add(1, std::memory_order_release);
    FIBER_ASSERT((previous & 1U) != 0);
}

AccessConfigMetrics::Snapshot AccessConfigMetrics::load_snapshot() const noexcept {
    Snapshot snapshot;
    for (;;) {
        const std::uint64_t before = sequence_.load(std::memory_order_acquire);
        if ((before & 1U) != 0) {
            continue;
        }
        snapshot.readiness.state =
                static_cast<AccessConfigMetricReadinessState>(readiness_state_.load(std::memory_order_relaxed));
        snapshot.readiness.desired_projects = desired_projects_.load(std::memory_order_relaxed);
        snapshot.readiness.subscribed_projects = subscribed_projects_.load(std::memory_order_relaxed);
        snapshot.readiness.synchronized_projects = synchronized_projects_.load(std::memory_order_relaxed);
        snapshot.readiness.retrying_projects = retrying_projects_.load(std::memory_order_relaxed);
        snapshot.readiness.processing_projects = processing_projects_.load(std::memory_order_relaxed);
        snapshot.readiness.rejected_projects = rejected_projects_.load(std::memory_order_relaxed);
        snapshot.generation = snapshot_generation_.load(std::memory_order_relaxed);
        snapshot.projects = snapshot_projects_.load(std::memory_order_relaxed);
        snapshot.hosts = snapshot_hosts_.load(std::memory_order_relaxed);
        snapshot.routes = snapshot_routes_.load(std::memory_order_relaxed);
        snapshot.compiled_programs = snapshot_compiled_programs_.load(std::memory_order_relaxed);
        snapshot.estimated_bytes = snapshot_estimated_bytes_.load(std::memory_order_relaxed);
        snapshot.static_response_bytes = snapshot_static_response_bytes_.load(std::memory_order_relaxed);
        snapshot.published_at_nanoseconds = snapshot_published_at_nanoseconds_.load(std::memory_order_relaxed);
        if (sequence_.load(std::memory_order_acquire) == before) {
            return snapshot;
        }
    }
}

void AccessConfigMetrics::append_prometheus(std::string &output, std::chrono::steady_clock::time_point now) const {
    const Snapshot snapshot = load_snapshot();
    output.reserve(output.size() + 4096);

    output.append("# HELP access_server_config_updates_total Processed access-server configuration updates.\n");
    output.append("# TYPE access_server_config_updates_total counter\n");
    for (std::size_t i = 0; i < kEvents.size(); ++i) {
        const EventDescription &event = kEvents[i];
        output.append("access_server_config_updates_total{resource=\"");
        output.append(event.resource);
        output.append("\",result=\"");
        output.append(event.result);
        output.append("\",reason=\"");
        output.append(event.reason);
        output.append("\"} ");
        append_unsigned(output, events_[i].load(std::memory_order_relaxed));
        output.push_back('\n');
    }

    output.append("# HELP access_server_config_readiness Current aggregate route configuration readiness.\n");
    output.append("# TYPE access_server_config_readiness gauge\n");
    const std::size_t readiness_index = static_cast<std::size_t>(snapshot.readiness.state);
    FIBER_ASSERT(readiness_index < kReadinessStates.size());
    for (std::size_t i = 0; i < kReadinessStates.size(); ++i) {
        output.append("access_server_config_readiness{state=\"");
        output.append(kReadinessStates[i]);
        output.append("\"} ");
        output.push_back(i == readiness_index ? '1' : '0');
        output.push_back('\n');
    }

    output.append("# HELP access_server_config_projects Current aggregate project synchronization counts.\n");
    output.append("# TYPE access_server_config_projects gauge\n");
    append_series(output, "access_server_config_projects", "{state=\"desired\"}", snapshot.readiness.desired_projects);
    append_series(output, "access_server_config_projects", "{state=\"subscribed\"}",
                  snapshot.readiness.subscribed_projects);
    append_series(output, "access_server_config_projects", "{state=\"synchronized\"}",
                  snapshot.readiness.synchronized_projects);
    append_series(output, "access_server_config_projects", "{state=\"retrying\"}",
                  snapshot.readiness.retrying_projects);
    append_series(output, "access_server_config_projects", "{state=\"processing\"}",
                  snapshot.readiness.processing_projects);
    append_series(output, "access_server_config_projects", "{state=\"rejected\"}",
                  snapshot.readiness.rejected_projects);

    output.append("# HELP access_server_route_snapshot_resources Resources in the active global route snapshot.\n");
    output.append("# TYPE access_server_route_snapshot_resources gauge\n");
    append_series(output, "access_server_route_snapshot_resources", "{resource=\"project\"}", snapshot.projects);
    append_series(output, "access_server_route_snapshot_resources", "{resource=\"host\"}", snapshot.hosts);
    append_series(output, "access_server_route_snapshot_resources", "{resource=\"route\"}", snapshot.routes);
    append_series(output, "access_server_route_snapshot_resources", "{resource=\"compiled_program\"}",
                  snapshot.compiled_programs);

    output.append("# HELP access_server_route_snapshot_generation Local route snapshot publication generation.\n");
    output.append("# TYPE access_server_route_snapshot_generation gauge\n");
    append_series(output, "access_server_route_snapshot_generation", {}, snapshot.generation);
    output.append(
            "# HELP access_server_route_snapshot_age_seconds Seconds since the active route snapshot was published.\n");
    output.append("# TYPE access_server_route_snapshot_age_seconds gauge\n");
    std::uint64_t age_seconds = 0;
    const std::int64_t now_nanoseconds = steady_nanoseconds(now);
    if (snapshot.generation != 0 && now_nanoseconds > snapshot.published_at_nanoseconds) {
        age_seconds = static_cast<std::uint64_t>(now_nanoseconds - snapshot.published_at_nanoseconds) / 1000000000U;
    }
    append_series(output, "access_server_route_snapshot_age_seconds", {}, age_seconds);
    output.append(
            "# HELP access_server_route_snapshot_estimated_bytes Estimated bytes retained by Project snapshots.\n");
    output.append("# TYPE access_server_route_snapshot_estimated_bytes gauge\n");
    append_series(output, "access_server_route_snapshot_estimated_bytes", {}, snapshot.estimated_bytes);
    output.append("# HELP access_server_route_snapshot_static_response_bytes Static response bytes retained by Project "
                  "snapshots.\n");
    output.append("# TYPE access_server_route_snapshot_static_response_bytes gauge\n");
    append_series(output, "access_server_route_snapshot_static_response_bytes", {}, snapshot.static_response_bytes);
}

} // namespace fiber::access_server
