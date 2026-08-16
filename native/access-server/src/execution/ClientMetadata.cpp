#include "ClientMetadata.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <utility>

#include <fiber/http/HttpExchange.h>
#include <fiber/http/HttpHeaderHash.h>
#include <fiber/http/HttpHeaders.h>

namespace fiber::access_server {
namespace {

constexpr std::string_view kForwarded = "forwarded";
constexpr std::string_view kXForwardedFor = "x-forwarded-for";
constexpr std::string_view kXRealIp = "x-real-ip";
constexpr std::string_view kXForwardedProto = "x-forwarded-proto";
constexpr std::uint64_t kForwardedHash = http::http_header_name_hash(kForwarded);
constexpr std::uint64_t kXForwardedForHash = http::http_header_name_hash(kXForwardedFor);
constexpr std::uint64_t kXRealIpHash = http::http_header_name_hash(kXRealIp);
constexpr std::uint64_t kXForwardedProtoHash = http::http_header_name_hash(kXForwardedProto);

enum class ForwardedProto : std::uint8_t {
    Missing,
    Http,
    Https,
};

struct IpChain {
    std::array<net::IpAddress, kMaxForwardedHops> values{};
    std::size_t size = 0;
    bool present = false;
    bool valid = true;
};

struct ProtoChain {
    std::array<ForwardedProto, kMaxForwardedHops> values{};
    std::size_t size = 0;
    bool present = false;
    bool valid = true;
};

struct ForwardedChain {
    IpChain addresses;
    ProtoChain protocols;
};

std::string_view trim_ows(std::string_view value) noexcept {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
        value.remove_suffix(1);
    }
    return value;
}

bool equals_ci(std::string_view left, std::string_view right) noexcept {
    return left.size() == right.size() && http::http_header_name_equals_ci(left, right);
}

bool is_token(std::string_view value) noexcept {
    if (value.empty()) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](const unsigned char character) {
        return (character >= '0' && character <= '9') || (character >= 'A' && character <= 'Z') ||
               (character >= 'a' && character <= 'z') || character == '!' || character == '#' || character == '$' ||
               character == '%' || character == '&' || character == '\'' || character == '*' || character == '+' ||
               character == '-' || character == '.' || character == '^' || character == '_' || character == '`' ||
               character == '|' || character == '~';
    });
}

bool parse_port(std::string_view text) noexcept {
    if (text.empty()) {
        return false;
    }
    std::uint32_t port = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), port);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && port <= 65535;
}

bool unwrap_quoted(std::string_view value, std::string_view &output) noexcept {
    if (value.empty() || value.front() != '"') {
        output = value;
        return value.find('"') == std::string_view::npos;
    }
    if (value.size() < 2 || value.back() != '"') {
        return false;
    }
    value.remove_prefix(1);
    value.remove_suffix(1);
    for (const unsigned char character: value) {
        if (character == '\\' || character == '"' || character < 0x20 || character == 0x7F) {
            return false;
        }
    }
    output = value;
    return true;
}

bool parse_ip_endpoint(std::string_view value, net::IpAddress &output) noexcept {
    value = trim_ows(value);
    std::string_view unquoted;
    if (!unwrap_quoted(value, unquoted)) {
        return false;
    }
    value = unquoted;
    if (value.empty() || equals_ci(value, "unknown") || value.front() == '_') {
        return false;
    }

    if (value.front() == '[') {
        const std::size_t bracket = value.find(']');
        if (bracket == std::string_view::npos || !net::IpAddress::parse(value.substr(1, bracket - 1), output) ||
            !output.is_v6()) {
            return false;
        }
        const std::string_view suffix = value.substr(bracket + 1);
        return suffix.empty() || (suffix.front() == ':' && parse_port(suffix.substr(1)));
    }

    if (net::IpAddress::parse(value, output)) {
        return true;
    }
    const std::size_t colon = value.rfind(':');
    if (colon == std::string_view::npos || value.find(':') != colon || !parse_port(value.substr(colon + 1))) {
        return false;
    }
    return net::IpAddress::parse(value.substr(0, colon), output) && output.is_v4();
}

ForwardedProto parse_proto(std::string_view value) noexcept {
    value = trim_ows(value);
    std::string_view unquoted;
    if (!unwrap_quoted(value, unquoted)) {
        return ForwardedProto::Missing;
    }
    if (equals_ci(unquoted, "http")) {
        return ForwardedProto::Http;
    }
    if (equals_ci(unquoted, "https")) {
        return ForwardedProto::Https;
    }
    return ForwardedProto::Missing;
}

template<typename Function>
bool for_each_delimited(std::string_view input, char delimiter, Function &&function) noexcept {
    std::size_t begin = 0;
    bool quoted = false;
    bool escaped = false;
    for (std::size_t i = 0; i <= input.size(); ++i) {
        if (i == input.size() || (input[i] == delimiter && !quoted)) {
            const std::string_view item = trim_ows(input.substr(begin, i - begin));
            if (item.empty() || !function(item)) {
                return false;
            }
            begin = i + 1;
            continue;
        }
        const char character = input[i];
        if (escaped) {
            escaped = false;
        } else if (character == '\\' && quoted) {
            escaped = true;
        } else if (character == '"') {
            quoted = !quoted;
        }
    }
    return !quoted && !escaped;
}

template<typename Function>
bool for_each_header_field(const http::HttpHeaders &headers, std::string_view name, std::uint64_t hash,
                           Function &&function) noexcept {
    std::array<const http::HttpHeaders::HeaderField *, kMaxForwardedHops> fields{};
    std::size_t size = 0;
    for (const http::HttpHeaders::HeaderField &field: headers.get_all(name, hash)) {
        if (size == fields.size()) {
            return false;
        }
        fields[size++] = &field;
    }
    while (size != 0) {
        if (!function(*fields[--size])) {
            return false;
        }
    }
    return true;
}

bool append_ip_list(std::string_view value, IpChain &chain) noexcept {
    chain.present = true;
    return for_each_delimited(value, ',', [&](std::string_view item) noexcept {
        if (chain.size == chain.values.size()) {
            return false;
        }
        net::IpAddress address;
        if (!parse_ip_endpoint(item, address)) {
            return false;
        }
        chain.values[chain.size++] = address;
        return true;
    });
}

IpChain parse_ip_headers(const http::HttpHeaders &headers, std::string_view name, std::uint64_t hash) noexcept {
    IpChain chain;
    if (!for_each_header_field(headers, name, hash, [&](const http::HttpHeaders::HeaderField &field) noexcept {
            return append_ip_list(field.value_view(), chain);
        })) {
        chain.valid = false;
        return chain;
    }
    chain.valid = !chain.present || chain.size != 0;
    return chain;
}

ProtoChain parse_proto_headers(const http::HttpHeaders &headers) noexcept {
    ProtoChain chain;
    if (!for_each_header_field(
                headers, kXForwardedProto, kXForwardedProtoHash,
                [&](const http::HttpHeaders::HeaderField &field) noexcept {
                    chain.present = true;
                    return for_each_delimited(field.value_view(), ',', [&](std::string_view item) noexcept {
                        if (chain.size == chain.values.size()) {
                            return false;
                        }
                        const ForwardedProto proto = parse_proto(item);
                        if (proto == ForwardedProto::Missing) {
                            return false;
                        }
                        chain.values[chain.size++] = proto;
                        return true;
                    });
                })) {
        chain.valid = false;
        return chain;
    }
    chain.valid = !chain.present || chain.size != 0;
    return chain;
}

bool parse_forwarded_element(std::string_view element, net::IpAddress &address, ForwardedProto &proto) noexcept {
    bool found_for = false;
    bool found_proto = false;
    return for_each_delimited(element, ';', [&](std::string_view parameter) noexcept {
        const std::size_t equals = parameter.find('=');
        if (equals == std::string_view::npos) {
            return false;
        }
        const std::string_view name = trim_ows(parameter.substr(0, equals));
        const std::string_view value = trim_ows(parameter.substr(equals + 1));
        if (!is_token(name) || value.empty()) {
            return false;
        }
        if (equals_ci(name, "for")) {
            if (found_for || !parse_ip_endpoint(value, address)) {
                return false;
            }
            found_for = true;
        } else if (equals_ci(name, "proto")) {
            if (found_proto) {
                return false;
            }
            proto = parse_proto(value);
            if (proto == ForwardedProto::Missing) {
                return false;
            }
            found_proto = true;
        } else {
            std::string_view ignored;
            if (!unwrap_quoted(value, ignored)) {
                return false;
            }
        }
        return true;
    }) && found_for;
}

ForwardedChain parse_forwarded_headers(const http::HttpHeaders &headers) noexcept {
    ForwardedChain chain;
    if (!for_each_header_field(headers, kForwarded, kForwardedHash,
                               [&](const http::HttpHeaders::HeaderField &field) noexcept {
            chain.addresses.present = true;
            chain.protocols.present = true;
            return for_each_delimited(field.value_view(), ',', [&](std::string_view element) noexcept {
                if (chain.addresses.size == chain.addresses.values.size()) {
                    return false;
                }
                net::IpAddress address;
                ForwardedProto proto = ForwardedProto::Missing;
                if (!parse_forwarded_element(element, address, proto)) {
                    return false;
                }
                chain.addresses.values[chain.addresses.size++] = address;
                chain.protocols.values[chain.protocols.size++] = proto;
                return true;
            });
        })) {
        chain.addresses.valid = false;
        chain.protocols.valid = false;
        return chain;
    }
    chain.addresses.valid = !chain.addresses.present || chain.addresses.size != 0;
    chain.protocols.valid = chain.addresses.valid;
    return chain;
}

bool has_header(const http::HttpHeaders &headers, std::string_view name, std::uint64_t hash) noexcept {
    const http::HttpHeaders::MatchRange fields = headers.get_all(name, hash);
    return fields.begin() != fields.end();
}

bool has_forwarding_headers(const http::HttpHeaders &headers) noexcept {
    return has_header(headers, kForwarded, kForwardedHash) ||
           has_header(headers, kXForwardedFor, kXForwardedForHash) ||
           has_header(headers, kXRealIp, kXRealIpHash) ||
           has_header(headers, kXForwardedProto, kXForwardedProtoHash);
}

void set_exact_targets(ClientMetadata &metadata, const net::IpAddress &address) noexcept {
    const Cidr target = Cidr::from_address(address);
    metadata.route_policy_target = target;
    metadata.gray_target = target;
}

void set_listener_scheme(ClientMetadata &metadata, bool secure) noexcept {
    metadata.secure = secure;
    metadata.external_scheme = secure ? std::string_view("https") : std::string_view("http");
    metadata.scheme_source = ClientSchemeSource::Listener;
}

void set_forwarded_scheme(ClientMetadata &metadata, ForwardedProto proto, ClientSchemeSource source) noexcept {
    metadata.secure = proto == ForwardedProto::Https;
    metadata.external_scheme = metadata.secure ? std::string_view("https") : std::string_view("http");
    metadata.scheme_source = source;
}

ClientMetadata direct_metadata(const net::SocketAddress &peer, bool secure, ForwardingStatus status) noexcept {
    ClientMetadata metadata;
    metadata.peer_address = peer.ip();
    metadata.client_address = peer.ip();
    metadata.forwarding_status = status;
    set_exact_targets(metadata, peer.ip());
    set_listener_scheme(metadata, secure);
    return metadata;
}

ClientMetadata legacy_metadata(const net::SocketAddress &peer, const http::HttpHeaders &headers,
                               bool connection_secure) noexcept {
    const bool forwarding_present = has_forwarding_headers(headers);
    ClientMetadata metadata = direct_metadata(peer, connection_secure,
                                              forwarding_present ? ForwardingStatus::Legacy
                                                                 : ForwardingStatus::NotPresent);
    metadata.route_policy_target.reset();
    metadata.gray_target.reset();
    metadata.has_client_address = false;

    const std::string_view real_ip = headers.get(kXRealIp, kXRealIpHash);
    if (!real_ip.empty()) {
        metadata.address_source = ClientAddressSource::LegacyXRealIp;
        const std::size_t colon = real_ip.rfind(':');
        const std::size_t address_end = colon == std::string_view::npos ? real_ip.size() : colon;
        auto route_target = Cidr::parse(real_ip.substr(0, address_end), "X-Real-Ip");
        if (route_target) {
            metadata.route_policy_target = *route_target;
        }

        net::IpAddress address;
        if (net::IpAddress::parse(real_ip, address)) {
            metadata.client_address = address;
            metadata.has_client_address = true;
            metadata.gray_target = Cidr::from_address(address);
        } else if (real_ip.find('/') != std::string_view::npos) {
            auto gray_target = Cidr::parse(real_ip, "X-Real-Ip");
            if (gray_target) {
                metadata.gray_target = *gray_target;
            }
        } else if (parse_ip_endpoint(real_ip, address)) {
            metadata.client_address = address;
            metadata.has_client_address = true;
        }
    }

    const std::string_view forwarded_proto = headers.get(kXForwardedProto, kXForwardedProtoHash);
    if (!forwarded_proto.empty()) {
        const ForwardedProto proto = parse_proto(forwarded_proto);
        if (proto != ForwardedProto::Missing) {
            set_forwarded_scheme(metadata, proto, ClientSchemeSource::LegacyXForwardedProto);
            metadata.secure = metadata.secure || connection_secure;
            if (connection_secure) {
                metadata.external_scheme = "https";
            }
        } else {
            metadata.forwarding_status = ForwardingStatus::Invalid;
        }
    }
    return metadata;
}

} // namespace

ClientMetadataResolver::ClientMetadataResolver(ClientMetadataResolverOptions options) noexcept :
    options_(std::move(options)) {}

ClientMetadata ClientMetadataResolver::resolve(const http::HttpExchange &exchange) const noexcept {
    return resolve(exchange.remote_addr(), exchange.request_headers());
}

ClientMetadata ClientMetadataResolver::resolve(const net::SocketAddress &peer,
                                               const http::HttpHeaders &headers) const noexcept {
    if (options_.mode == ClientMetadataMode::LegacyHeaders) {
        return legacy_metadata(peer, headers, options_.connection_secure);
    }

    const bool forwarding_present = has_forwarding_headers(headers);
    if (options_.mode == ClientMetadataMode::Direct) {
        return direct_metadata(peer, options_.connection_secure,
                               forwarding_present ? ForwardingStatus::IgnoredDirectMode
                                                  : ForwardingStatus::NotPresent);
    }

    ClientMetadata metadata = direct_metadata(peer, options_.connection_secure, ForwardingStatus::NotPresent);
    metadata.peer_trusted = is_trusted_proxy(peer.ip());
    if (!metadata.peer_trusted) {
        metadata.forwarding_status = forwarding_present ? ForwardingStatus::IgnoredUntrustedPeer
                                                        : ForwardingStatus::NotPresent;
        return metadata;
    }
    if (!forwarding_present) {
        return metadata;
    }

    IpChain address_chain;
    ProtoChain forwarded_protocols;
    ClientAddressSource address_source = ClientAddressSource::SocketPeer;
    const bool forwarded_present = has_header(headers, kForwarded, kForwardedHash);
    const bool x_forwarded_for_present = has_header(headers, kXForwardedFor, kXForwardedForHash);
    const bool x_real_ip_present = has_header(headers, kXRealIp, kXRealIpHash);
    if (forwarded_present) {
        ForwardedChain forwarded = parse_forwarded_headers(headers);
        address_chain = forwarded.addresses;
        forwarded_protocols = forwarded.protocols;
        address_source = ClientAddressSource::Forwarded;
    } else if (x_forwarded_for_present) {
        address_chain = parse_ip_headers(headers, kXForwardedFor, kXForwardedForHash);
        address_source = ClientAddressSource::XForwardedFor;
    } else if (x_real_ip_present) {
        address_chain = parse_ip_headers(headers, kXRealIp, kXRealIpHash);
        if (address_chain.size != 1) {
            address_chain.valid = false;
        }
        address_source = ClientAddressSource::XRealIp;
    }

    if (address_chain.present && !address_chain.valid) {
        metadata.forwarding_status = ForwardingStatus::Invalid;
        return metadata;
    }

    std::size_t selected_index = 0;
    if (address_chain.size != 0) {
        selected_index = address_chain.size - 1;
        for (std::size_t i = address_chain.size; i > 0; --i) {
            selected_index = i - 1;
            if (!is_trusted_proxy(address_chain.values[selected_index])) {
                break;
            }
        }
        metadata.client_address = address_chain.values[selected_index];
        metadata.has_client_address = true;
        metadata.address_source = address_source;
        set_exact_targets(metadata, metadata.client_address);
    }

    bool scheme_invalid = false;
    if (forwarded_present && forwarded_protocols.values[selected_index] != ForwardedProto::Missing) {
        set_forwarded_scheme(metadata, forwarded_protocols.values[selected_index], ClientSchemeSource::Forwarded);
    } else {
        const ProtoChain protocols = parse_proto_headers(headers);
        if (!protocols.valid) {
            scheme_invalid = true;
        } else if (protocols.present) {
            std::optional<std::size_t> protocol_index;
            if (protocols.size == 1) {
                protocol_index = 0;
            } else if (address_chain.size != 0 && protocols.size == address_chain.size) {
                protocol_index = selected_index;
            }
            if (protocol_index) {
                set_forwarded_scheme(metadata, protocols.values[*protocol_index],
                                     ClientSchemeSource::XForwardedProto);
            } else {
                scheme_invalid = true;
            }
        }
    }
    metadata.forwarding_status = scheme_invalid ? ForwardingStatus::Invalid : ForwardingStatus::Trusted;
    return metadata;
}

bool ClientMetadataResolver::is_trusted_proxy(const net::IpAddress &address) const noexcept {
    return std::any_of(options_.trusted_proxy_cidrs.begin(), options_.trusted_proxy_cidrs.end(),
                       [&address](const Cidr &cidr) { return cidr.matches(address); });
}

std::string_view client_metadata_mode_name(ClientMetadataMode mode) noexcept {
    switch (mode) {
        case ClientMetadataMode::Direct:
            return "direct";
        case ClientMetadataMode::TrustedProxy:
            return "trusted_proxy";
        case ClientMetadataMode::LegacyHeaders:
            return "legacy_headers";
    }
    return "unknown";
}

std::string_view client_address_source_name(ClientAddressSource source) noexcept {
    switch (source) {
        case ClientAddressSource::SocketPeer:
            return "socket_peer";
        case ClientAddressSource::Forwarded:
            return "forwarded";
        case ClientAddressSource::XForwardedFor:
            return "x_forwarded_for";
        case ClientAddressSource::XRealIp:
            return "x_real_ip";
        case ClientAddressSource::LegacyXRealIp:
            return "legacy_x_real_ip";
    }
    return "unknown";
}

std::string_view client_scheme_source_name(ClientSchemeSource source) noexcept {
    switch (source) {
        case ClientSchemeSource::Listener:
            return "listener";
        case ClientSchemeSource::Forwarded:
            return "forwarded";
        case ClientSchemeSource::XForwardedProto:
            return "x_forwarded_proto";
        case ClientSchemeSource::LegacyXForwardedProto:
            return "legacy_x_forwarded_proto";
    }
    return "unknown";
}

std::string_view forwarding_status_name(ForwardingStatus status) noexcept {
    switch (status) {
        case ForwardingStatus::NotPresent:
            return "not_present";
        case ForwardingStatus::Trusted:
            return "trusted";
        case ForwardingStatus::IgnoredUntrustedPeer:
            return "ignored_untrusted_peer";
        case ForwardingStatus::IgnoredDirectMode:
            return "ignored_direct_mode";
        case ForwardingStatus::Invalid:
            return "invalid";
        case ForwardingStatus::Legacy:
            return "legacy";
    }
    return "unknown";
}

} // namespace fiber::access_server
