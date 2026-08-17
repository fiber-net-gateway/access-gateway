#include "AccessDnsMetrics.h"

#include <array>
#include <charconv>
#include <limits>
#include <string_view>

#include <fiber/common/Assert.h>

namespace fiber::access_server {
namespace {

constexpr std::array<std::string_view, static_cast<std::size_t>(AccessDnsConfigSource::Count)> kSources{
        "system",
        "override",
};

constexpr std::array<std::string_view, static_cast<std::size_t>(AccessDnsResolverState::Count)> kStates{
        "stopped",
        "starting",
        "ready",
        "failed",
        "stopping",
};

struct UnsupportedFeatureDescription {
    std::string_view name;
    dns::ResolverUnsupportedFeature feature;
};

constexpr std::array<UnsupportedFeatureDescription, 5> kUnsupportedFeatures{
        UnsupportedFeatureDescription{"search", dns::ResolverUnsupportedFeature::Search},
        UnsupportedFeatureDescription{"ndots", dns::ResolverUnsupportedFeature::Ndots},
        UnsupportedFeatureDescription{"sortlist", dns::ResolverUnsupportedFeature::SortList},
        UnsupportedFeatureDescription{"option", dns::ResolverUnsupportedFeature::Option},
        UnsupportedFeatureDescription{"directive", dns::ResolverUnsupportedFeature::Directive},
};

void append_unsigned(std::string &output, std::uint64_t value) {
    std::array<char, std::numeric_limits<std::uint64_t>::digits10 + 2> buffer{};
    const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    FIBER_ASSERT(converted.ec == std::errc{});
    output.append(buffer.data(), converted.ptr);
}

} // namespace

AccessDnsMetricsObserver AccessDnsMetrics::observer() noexcept {
    return AccessDnsMetricsObserver{
            .context = this,
            .on_configure = &observe_configure,
            .on_state = &observe_state,
            .on_initialization = &observe_initialization,
    };
}

void AccessDnsMetrics::observe_configure(void *context, AccessDnsConfigSource source, std::size_t nameservers,
                                         dns::ResolverUnsupportedFeature unsupported) noexcept {
    auto &metrics = *static_cast<AccessDnsMetrics *>(context);
    FIBER_ASSERT(static_cast<std::size_t>(source) < kSources.size());
    metrics.source_.store(static_cast<std::uint8_t>(source), std::memory_order_relaxed);
    metrics.configured_nameservers_.store(nameservers, std::memory_order_relaxed);
    metrics.unsupported_.store(static_cast<std::uint32_t>(unsupported), std::memory_order_relaxed);
}

void AccessDnsMetrics::observe_state(void *context, AccessDnsResolverState state,
                                     std::size_t active_resolvers) noexcept {
    auto &metrics = *static_cast<AccessDnsMetrics *>(context);
    FIBER_ASSERT(static_cast<std::size_t>(state) < kStates.size());
    metrics.active_resolvers_.store(active_resolvers, std::memory_order_relaxed);
    metrics.state_.store(static_cast<std::uint8_t>(state), std::memory_order_release);
}

void AccessDnsMetrics::observe_initialization(void *context, bool success) noexcept {
    auto &metrics = *static_cast<AccessDnsMetrics *>(context);
    std::atomic<std::uint64_t> &counter =
            success ? metrics.initialization_successes_ : metrics.initialization_failures_;
    std::uint64_t current = counter.load(std::memory_order_relaxed);
    while (current != std::numeric_limits<std::uint64_t>::max() &&
           !counter.compare_exchange_weak(current, current + 1, std::memory_order_relaxed)) {
    }
}

AccessDnsMetricsStatus AccessDnsMetrics::status() const noexcept {
    const std::uint8_t source = source_.load(std::memory_order_relaxed);
    const std::uint8_t state = state_.load(std::memory_order_acquire);
    FIBER_ASSERT(source < static_cast<std::uint8_t>(AccessDnsConfigSource::Count));
    FIBER_ASSERT(state < static_cast<std::uint8_t>(AccessDnsResolverState::Count));
    return AccessDnsMetricsStatus{
            .source = static_cast<AccessDnsConfigSource>(source),
            .state = static_cast<AccessDnsResolverState>(state),
            .unsupported = static_cast<dns::ResolverUnsupportedFeature>(unsupported_.load(std::memory_order_relaxed)),
            .configured_nameservers = configured_nameservers_.load(std::memory_order_relaxed),
            .active_resolvers = active_resolvers_.load(std::memory_order_relaxed),
            .initialization_successes = initialization_successes_.load(std::memory_order_relaxed),
            .initialization_failures = initialization_failures_.load(std::memory_order_relaxed),
    };
}

void AccessDnsMetrics::append_prometheus(std::string &output) const {
    const AccessDnsMetricsStatus snapshot = status();
    output.reserve(output.size() + 1536);

    output.append("# HELP access_server_dns_config DNS resolver configuration source.\n");
    output.append("# TYPE access_server_dns_config gauge\n");
    for (std::size_t source = 0; source < kSources.size(); ++source) {
        output.append("access_server_dns_config{source=\"");
        output.append(kSources[source]);
        output.append("\"} ");
        output.push_back(source == static_cast<std::size_t>(snapshot.source) ? '1' : '0');
        output.push_back('\n');
    }

    output.append("# HELP access_server_dns_nameservers Configured bounded DNS upstream count.\n");
    output.append("# TYPE access_server_dns_nameservers gauge\n");
    output.append("access_server_dns_nameservers ");
    append_unsigned(output, snapshot.configured_nameservers);
    output.push_back('\n');

    output.append("# HELP access_server_dns_resolver_state HTTP worker DNS resolver lifecycle.\n");
    output.append("# TYPE access_server_dns_resolver_state gauge\n");
    for (std::size_t state = 0; state < kStates.size(); ++state) {
        output.append("access_server_dns_resolver_state{state=\"");
        output.append(kStates[state]);
        output.append("\"} ");
        output.push_back(state == static_cast<std::size_t>(snapshot.state) ? '1' : '0');
        output.push_back('\n');
    }
    output.append("# HELP access_server_dns_resolvers_active Active loop-affine HTTP worker DNS resolvers.\n");
    output.append("# TYPE access_server_dns_resolvers_active gauge\n");
    output.append("access_server_dns_resolvers_active ");
    append_unsigned(output, snapshot.active_resolvers);
    output.push_back('\n');

    output.append("# HELP access_server_dns_initializations_total DNS resolver-stack initialization outcomes.\n");
    output.append("# TYPE access_server_dns_initializations_total counter\n");
    output.append("access_server_dns_initializations_total{result=\"success\"} ");
    append_unsigned(output, snapshot.initialization_successes);
    output.push_back('\n');
    output.append("access_server_dns_initializations_total{result=\"failure\"} ");
    append_unsigned(output, snapshot.initialization_failures);
    output.push_back('\n');

    output.append("# HELP access_server_dns_config_unsupported Resolver-file features retained but not applied.\n");
    output.append("# TYPE access_server_dns_config_unsupported gauge\n");
    for (const UnsupportedFeatureDescription &feature: kUnsupportedFeatures) {
        output.append("access_server_dns_config_unsupported{feature=\"");
        output.append(feature.name);
        output.append("\"} ");
        output.push_back(dns::has_unsupported_feature(snapshot.unsupported, feature.feature) ? '1' : '0');
        output.push_back('\n');
    }
}

} // namespace fiber::access_server
