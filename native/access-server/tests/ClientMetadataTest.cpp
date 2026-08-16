#include "execution/ClientMetadata.h"

#include <gtest/gtest.h>

#include <array>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fiber/common/mem/BufPool.h>
#include <fiber/http/HttpHeaders.h>

namespace fiber::access_server {
namespace {

Cidr strict_cidr(std::string_view text) {
    auto parsed = Cidr::parse_strict(text, "test");
    if (!parsed) {
        ADD_FAILURE() << parsed.error().message;
        return Cidr::from_address(net::IpAddress::any_v4());
    }
    return *parsed;
}

net::IpAddress ip(std::string_view text) {
    net::IpAddress parsed;
    EXPECT_TRUE(net::IpAddress::parse(text, parsed));
    return parsed;
}

ClientMetadata resolve(ClientMetadataResolverOptions options, std::string_view peer,
                       std::initializer_list<std::pair<std::string_view, std::string_view>> fields) {
    mem::BufPool pool;
    http::HttpHeaders headers(pool);
    for (const auto &[name, value]: fields) {
        EXPECT_NE(headers.add(name, value), nullptr);
    }
    ClientMetadataResolver resolver(std::move(options));
    return resolver.resolve(net::SocketAddress(ip(peer), 12345), headers);
}

ClientMetadataResolverOptions trusted_options(bool secure = false) {
    return ClientMetadataResolverOptions{
            .mode = ClientMetadataMode::TrustedProxy,
            .trusted_proxy_cidrs = {strict_cidr("10.0.0.0/8"), strict_cidr("2001:db8:ffff::/48")},
            .connection_secure = secure,
    };
}

TEST(ClientMetadataTest, DirectModeIgnoresAllForwardingHeaders) {
    ClientMetadata metadata = resolve(
            {}, "198.51.100.9",
            {{"Forwarded", "for=10.1.2.3;proto=https"}, {"X-Real-Ip", "10.2.3.4"},
             {"X-Forwarded-Proto", "https"}});

    EXPECT_EQ(metadata.client_address.to_string(), "198.51.100.9");
    EXPECT_FALSE(metadata.secure);
    EXPECT_EQ(metadata.external_scheme, "http");
    EXPECT_EQ(metadata.address_source, ClientAddressSource::SocketPeer);
    EXPECT_EQ(metadata.forwarding_status, ForwardingStatus::IgnoredDirectMode);
}

TEST(ClientMetadataTest, TrustedModeIgnoresHeadersFromAnUntrustedPeer) {
    ClientMetadata metadata = resolve(trusted_options(), "198.51.100.9",
                                      {{"X-Forwarded-For", "203.0.113.7"},
                                       {"X-Forwarded-Proto", "https"}});

    EXPECT_EQ(metadata.client_address.to_string(), "198.51.100.9");
    EXPECT_FALSE(metadata.peer_trusted);
    EXPECT_FALSE(metadata.secure);
    EXPECT_EQ(metadata.forwarding_status, ForwardingStatus::IgnoredUntrustedPeer);
}

TEST(ClientMetadataTest, WalksXForwardedForFromTheTrustedSocketPeer) {
    ClientMetadata metadata = resolve(trusted_options(), "10.0.0.3",
                                      {{"X-Forwarded-For", "203.0.113.7, 10.0.0.2"},
                                       {"X-Forwarded-Proto", "https, http"}});

    EXPECT_TRUE(metadata.peer_trusted);
    EXPECT_EQ(metadata.client_address.to_string(), "203.0.113.7");
    EXPECT_TRUE(metadata.secure);
    EXPECT_EQ(metadata.address_source, ClientAddressSource::XForwardedFor);
    EXPECT_EQ(metadata.scheme_source, ClientSchemeSource::XForwardedProto);
    EXPECT_EQ(metadata.forwarding_status, ForwardingStatus::Trusted);
}

TEST(ClientMetadataTest, GivesForwardedPriorityAndParsesBracketedIpv6WithPort) {
    ClientMetadata metadata = resolve(
            trusted_options(), "10.0.0.3",
            {{"Forwarded", "for=\"[2001:db8::7]:8443\";proto=HTTPS, for=10.0.0.2;proto=http"},
             {"X-Forwarded-For", "192.0.2.55"}, {"X-Forwarded-Proto", "http"}});

    EXPECT_EQ(metadata.client_address.to_string(), "2001:db8::7");
    EXPECT_TRUE(metadata.secure);
    EXPECT_EQ(metadata.address_source, ClientAddressSource::Forwarded);
    EXPECT_EQ(metadata.scheme_source, ClientSchemeSource::Forwarded);
}

TEST(ClientMetadataTest, SupportsMultipleHeaderFieldsAndXRealIpPort) {
    ClientMetadata chained = resolve(trusted_options(), "10.0.0.3",
                                     {{"X-Forwarded-For", "203.0.113.9"},
                                      {"X-Forwarded-For", "10.0.0.2"},
                                      {"X-Forwarded-Proto", "https"},
                                      {"X-Forwarded-Proto", "http"}});
    EXPECT_EQ(chained.client_address.to_string(), "203.0.113.9");
    EXPECT_TRUE(chained.secure);

    ClientMetadata real_ip = resolve(trusted_options(), "10.0.0.3",
                                     {{"X-Real-Ip", "192.0.2.8:4312"}});
    EXPECT_EQ(real_ip.client_address.to_string(), "192.0.2.8");
    EXPECT_EQ(real_ip.address_source, ClientAddressSource::XRealIp);
}

TEST(ClientMetadataTest, InvalidHigherPriorityHeaderDoesNotDowngrade) {
    ClientMetadata metadata = resolve(trusted_options(true), "10.0.0.3",
                                      {{"Forwarded", "for=unknown;proto=http"},
                                       {"X-Forwarded-For", "203.0.113.7"},
                                       {"X-Forwarded-Proto", "http"}});

    EXPECT_EQ(metadata.client_address.to_string(), "10.0.0.3");
    EXPECT_TRUE(metadata.secure);
    EXPECT_EQ(metadata.address_source, ClientAddressSource::SocketPeer);
    EXPECT_EQ(metadata.forwarding_status, ForwardingStatus::Invalid);
}

TEST(ClientMetadataTest, MisalignedProtoFallsBackToListenerWithoutDiscardingAddress) {
    ClientMetadata metadata = resolve(trusted_options(), "10.0.0.3",
                                      {{"X-Forwarded-For", "203.0.113.7, 10.0.0.2"},
                                       {"X-Forwarded-Proto", "https, http, https"}});

    EXPECT_EQ(metadata.client_address.to_string(), "203.0.113.7");
    EXPECT_FALSE(metadata.secure);
    EXPECT_EQ(metadata.scheme_source, ClientSchemeSource::Listener);
    EXPECT_EQ(metadata.forwarding_status, ForwardingStatus::Invalid);
}

TEST(ClientMetadataTest, RejectsDuplicateForwardedParametersAndOversizedChains) {
    ClientMetadata duplicate = resolve(trusted_options(), "10.0.0.3",
                                       {{"Forwarded", "for=203.0.113.7;for=192.0.2.1"}});
    EXPECT_EQ(duplicate.client_address.to_string(), "10.0.0.3");
    EXPECT_EQ(duplicate.forwarding_status, ForwardingStatus::Invalid);

    std::string chain;
    for (std::size_t i = 0; i < kMaxForwardedHops + 1; ++i) {
        if (!chain.empty()) {
            chain.append(", ");
        }
        chain.append("10.0.0.2");
    }
    ClientMetadata oversized = resolve(trusted_options(), "10.0.0.3", {{"X-Forwarded-For", chain}});
    EXPECT_EQ(oversized.client_address.to_string(), "10.0.0.3");
    EXPECT_EQ(oversized.forwarding_status, ForwardingStatus::Invalid);
}

TEST(ClientMetadataTest, LegacyModePreservesCidrSkipAndRawIpv6GrayBehavior) {
    ClientMetadataResolverOptions options{
            .mode = ClientMetadataMode::LegacyHeaders,
    };
    ClientMetadata invalid = resolve(options, "198.51.100.9",
                                     {{"X-Real-Ip", "not-an-ip"}, {"X-Forwarded-Proto", "https"}});
    EXPECT_FALSE(invalid.route_policy_target);
    EXPECT_FALSE(invalid.gray_target);
    EXPECT_TRUE(invalid.secure);
    EXPECT_EQ(invalid.forwarding_status, ForwardingStatus::Legacy);

    ClientMetadata ipv6 = resolve(options, "198.51.100.9", {{"X-Real-Ip", "2001:db8::7"}});
    EXPECT_FALSE(ipv6.route_policy_target);
    ASSERT_TRUE(ipv6.gray_target);
    EXPECT_TRUE(ipv6.has_client_address);
    EXPECT_EQ(ipv6.client_address.to_string(), "2001:db8::7");
}

TEST(ClientMetadataTest, StrictCidrParserRejectsJavaCompatibilityQuirks) {
    EXPECT_TRUE(Cidr::parse_strict("192.0.2.0/24", "trusted"));
    EXPECT_TRUE(Cidr::parse_strict("2001:db8::/32", "trusted"));
    EXPECT_FALSE(Cidr::parse_strict(".1.2.3/8", "trusted"));
    EXPECT_FALSE(Cidr::parse_strict("[2001:db8::1]/128", "trusted"));
    EXPECT_FALSE(Cidr::parse_strict("192.0.2.1/+24", "trusted"));
    EXPECT_FALSE(Cidr::parse_strict("192.0.2.1/24/1", "trusted"));
}

} // namespace
} // namespace fiber::access_server
