#include "../src/observability/AccessDnsMetrics.h"

#include <gtest/gtest.h>

#include <string>

namespace fiber::access_server {
namespace {

TEST(AccessDnsMetricsTest, ExposesOnlyBoundedConfigurationAndLifecycleDimensions) {
    AccessDnsMetrics metrics;
    const AccessDnsMetricsObserver observer = metrics.observer();
    observer.configure(AccessDnsConfigSource::System, 3,
                       dns::ResolverUnsupportedFeature::Search | dns::ResolverUnsupportedFeature::Ndots);
    observer.set_state(AccessDnsResolverState::Starting, 1);
    observer.initialized(false);
    observer.set_state(AccessDnsResolverState::Ready, 4);
    observer.initialized(true);

    const AccessDnsMetricsStatus status = metrics.status();
    EXPECT_EQ(status.source, AccessDnsConfigSource::System);
    EXPECT_EQ(status.state, AccessDnsResolverState::Ready);
    EXPECT_EQ(status.configured_nameservers, 3U);
    EXPECT_EQ(status.active_resolvers, 4U);
    EXPECT_EQ(status.initialization_successes, 1U);
    EXPECT_EQ(status.initialization_failures, 1U);

    std::string output;
    metrics.append_prometheus(output);
    EXPECT_NE(output.find("access_server_dns_config{source=\"system\"} 1"), std::string::npos);
    EXPECT_NE(output.find("access_server_dns_nameservers 3"), std::string::npos);
    EXPECT_NE(output.find("access_server_dns_resolver_state{state=\"ready\"} 1"), std::string::npos);
    EXPECT_NE(output.find("access_server_dns_resolvers_active 4"), std::string::npos);
    EXPECT_NE(output.find("access_server_dns_initializations_total{result=\"success\"} 1"), std::string::npos);
    EXPECT_NE(output.find("access_server_dns_config_unsupported{feature=\"search\"} 1"), std::string::npos);
    EXPECT_NE(output.find("access_server_dns_config_unsupported{feature=\"ndots\"} 1"), std::string::npos);
    EXPECT_NE(output.find("access_server_dns_config_unsupported{feature=\"sortlist\"} 0"), std::string::npos);
    EXPECT_EQ(output.find("nameserver="), std::string::npos);
    EXPECT_EQ(output.find("address="), std::string::npos);
}

} // namespace
} // namespace fiber::access_server
