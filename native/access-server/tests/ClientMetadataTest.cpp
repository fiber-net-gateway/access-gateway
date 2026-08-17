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

TEST(ClientMetadataTest, SupportsTrustedIpv6PeersAndMultiFieldIpv6Chains) {
    ClientMetadata forwarded =
            resolve(trusted_options(), "2001:db8:ffff::30",
                    {{"Forwarded", "for=\"[2001:4860::1234]:8443\";proto=\"HTTPS\";by=_edge;host=\"api.example:443\";"
                                   "ext=\"a\\\"b,c\""},
                     {"Forwarded", "for=\"[2001:db8:ffff::20]:443\";proto=http;by=\"[2001:db8:ffff::21]\""}});

    EXPECT_TRUE(forwarded.peer_trusted);
    EXPECT_EQ(forwarded.peer_address.to_string(), "2001:db8:ffff::30");
    EXPECT_EQ(forwarded.client_address.to_string(), "2001:4860::1234");
    EXPECT_TRUE(forwarded.secure);
    EXPECT_EQ(forwarded.address_source, ClientAddressSource::Forwarded);
    EXPECT_EQ(forwarded.scheme_source, ClientSchemeSource::Forwarded);
    EXPECT_EQ(forwarded.forwarding_status, ForwardingStatus::Trusted);
    ASSERT_TRUE(forwarded.route_policy_target);
    ASSERT_TRUE(forwarded.gray_target);
    EXPECT_TRUE(forwarded.route_policy_target->matches(ip("2001:4860::1234")));
    EXPECT_TRUE(forwarded.gray_target->matches(ip("2001:4860::1234")));

    ClientMetadata x_forwarded = resolve(
            trusted_options(), "2001:db8:ffff::30",
            {{"X-Forwarded-For", "2001:4860::8, [2001:db8:ffff::20]:443"}, {"X-Forwarded-Proto", "https, http"}});
    EXPECT_EQ(x_forwarded.client_address.to_string(), "2001:4860::8");
    EXPECT_TRUE(x_forwarded.secure);
    EXPECT_EQ(x_forwarded.address_source, ClientAddressSource::XForwardedFor);
    EXPECT_EQ(x_forwarded.scheme_source, ClientSchemeSource::XForwardedProto);

    ClientMetadata real_ip = resolve(trusted_options(), "2001:db8:ffff::30", {{"X-Real-Ip", "[2001:4860::9]:443"}});
    EXPECT_EQ(real_ip.client_address.to_string(), "2001:4860::9");
    EXPECT_EQ(real_ip.address_source, ClientAddressSource::XRealIp);
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

TEST(ClientMetadataTest, RejectsMalformedForwardedElementsWithoutFallingBack) {
    constexpr std::array<std::string_view, 10> kInvalidForwarded = {
            "for=unknown",
            "for=_hidden",
            "for=\"[2001:db8::7\";proto=https",
            "for=\"[2001:db8::7]:65536\";proto=https",
            "for=203.0.113.7;proto=ftp",
            "for=203.0.113.7;by=edge;BY=other",
            "for=203.0.113.7;host=api;host=other",
            "for=203.0.113.7;ext=bad=value",
            "for=203.0.113.7;ext=contains space",
            "for=203.0.113.7,,for=10.0.0.2",
    };
    for (const std::string_view value: kInvalidForwarded) {
        SCOPED_TRACE(value);
        ClientMetadata metadata =
                resolve(trusted_options(), "10.0.0.3",
                        {{"Forwarded", value}, {"X-Forwarded-For", "203.0.113.9"}, {"X-Forwarded-Proto", "https"}});
        EXPECT_EQ(metadata.client_address.to_string(), "10.0.0.3");
        EXPECT_FALSE(metadata.secure);
        EXPECT_EQ(metadata.address_source, ClientAddressSource::SocketPeer);
        EXPECT_EQ(metadata.scheme_source, ClientSchemeSource::Listener);
        EXPECT_EQ(metadata.forwarding_status, ForwardingStatus::Invalid);
        ASSERT_TRUE(metadata.route_policy_target);
        EXPECT_TRUE(metadata.route_policy_target->matches(ip("10.0.0.3")));
    }

    std::string too_many_parameters = "for=203.0.113.7";
    for (std::size_t i = 0; i < 16; ++i) {
        too_many_parameters.append(";x");
        too_many_parameters.append(std::to_string(i));
        too_many_parameters.append("=value");
    }
    ClientMetadata oversized = resolve(trusted_options(), "10.0.0.3", {{"Forwarded", too_many_parameters}});
    EXPECT_EQ(oversized.client_address.to_string(), "10.0.0.3");
    EXPECT_EQ(oversized.forwarding_status, ForwardingStatus::Invalid);
}

TEST(ClientMetadataTest, RejectsMalformedXForwardedAndXRealIpFieldsByDimension) {
    ClientMetadata empty_xff =
            resolve(trusted_options(), "10.0.0.3",
                    {{"X-Forwarded-For", "203.0.113.7, "}, {"X-Real-Ip", "192.0.2.8"}, {"X-Forwarded-Proto", "https"}});
    EXPECT_EQ(empty_xff.client_address.to_string(), "10.0.0.3");
    EXPECT_FALSE(empty_xff.secure);
    EXPECT_EQ(empty_xff.forwarding_status, ForwardingStatus::Invalid);

    ClientMetadata multi_real_ip = resolve(trusted_options(), "10.0.0.3", {{"X-Real-Ip", "192.0.2.8, 192.0.2.9"}});
    EXPECT_EQ(multi_real_ip.client_address.to_string(), "10.0.0.3");
    EXPECT_EQ(multi_real_ip.forwarding_status, ForwardingStatus::Invalid);

    ClientMetadata duplicate_real_ip =
            resolve(trusted_options(), "10.0.0.3", {{"X-Real-Ip", "192.0.2.8"}, {"X-Real-Ip", "192.0.2.9"}});
    EXPECT_EQ(duplicate_real_ip.client_address.to_string(), "10.0.0.3");
    EXPECT_EQ(duplicate_real_ip.forwarding_status, ForwardingStatus::Invalid);

    for (const std::string_view proto: {std::string_view("https, "), std::string_view("https, ftp")}) {
        SCOPED_TRACE(proto);
        ClientMetadata invalid_proto =
                resolve(trusted_options(), "10.0.0.3",
                        {{"X-Forwarded-For", "203.0.113.7, 10.0.0.2"}, {"X-Forwarded-Proto", proto}});
        EXPECT_EQ(invalid_proto.client_address.to_string(), "203.0.113.7");
        EXPECT_FALSE(invalid_proto.secure);
        EXPECT_EQ(invalid_proto.address_source, ClientAddressSource::XForwardedFor);
        EXPECT_EQ(invalid_proto.scheme_source, ClientSchemeSource::Listener);
        EXPECT_EQ(invalid_proto.forwarding_status, ForwardingStatus::Invalid);
    }
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
