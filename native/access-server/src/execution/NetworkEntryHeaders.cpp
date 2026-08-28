#include "NetworkEntryHeaders.h"

#include "ClientMetadata.h"

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

#include <fiber/http/HttpHeaderHash.h>
#include <fiber/http/HttpHeaders.h>

namespace fiber::access_server {
namespace {

constexpr std::string_view kInternetEntry = "internet";
constexpr std::string_view kEntryHeader = "X-Entry";
constexpr std::string_view kEntryLowcase = "x-entry";
constexpr std::string_view kRealIpHeader = "X-Real-Ip";
constexpr std::string_view kRealIpLowcase = "x-real-ip";
constexpr std::string_view kForwardedProtoHeader = "X-Forwarded-Proto";
constexpr std::string_view kForwardedProtoLowcase = "x-forwarded-proto";
constexpr std::string_view kForwardedForHeader = "X-Forwarded-For";
constexpr std::string_view kForwardedForLowcase = "x-forwarded-for";
constexpr std::string_view kEoConnectingIpLowcase = "eo-connecting-ip";
constexpr std::uint64_t kEntryHash = http::http_header_name_hash(kEntryLowcase);
constexpr std::uint64_t kRealIpHash = http::http_header_name_hash(kRealIpLowcase);
constexpr std::uint64_t kForwardedProtoHash = http::http_header_name_hash(kForwardedProtoLowcase);
constexpr std::uint64_t kForwardedForHash = http::http_header_name_hash(kForwardedForLowcase);
constexpr std::uint64_t kEoConnectingIpHash = http::http_header_name_hash(kEoConnectingIpLowcase);

} // namespace

bool apply_network_entry_headers(http::HttpHeaders &headers, const net::IpAddress &peer, std::string_view entry,
                                 bool connection_secure) noexcept {
    if (entry.empty()) {
        return true;
    }

    const std::string peer_text = peer.to_string();
    if (!headers.set(kEntryHeader, entry, kEntryLowcase.data(), kEntryHash)) {
        return false;
    }

    // nginx parity: the internet edge trusts the CDN's EO-Connecting-IP value
    // verbatim; every other entry (and a missing or empty header) falls back
    // to the connection peer.
    std::string_view real_ip = peer_text;
    if (entry == kInternetEntry) {
        const std::string_view eo_connecting_ip = headers.get(kEoConnectingIpLowcase, kEoConnectingIpHash);
        if (!eo_connecting_ip.empty()) {
            real_ip = eo_connecting_ip;
        }
    }
    if (!headers.set(kRealIpHeader, real_ip, kRealIpLowcase.data(), kRealIpHash)) {
        return false;
    }
    if (!headers.set(kForwardedProtoHeader, connection_secure ? std::string_view("https") : std::string_view("http"),
                     kForwardedProtoLowcase.data(), kForwardedProtoHash)) {
        return false;
    }

    // Same-name fields come back from the hash bucket in reverse insertion
    // order; collect then walk backwards to restore wire order (the
    // ClientMetadata precedent). The hop cap matches the resolver's; overflow
    // drops the oldest outer values.
    std::array<std::string_view, kMaxForwardedHops> incoming{};
    std::size_t incoming_count = 0;
    for (const http::HttpHeaders::HeaderField &field: headers.get_all(kForwardedForLowcase, kForwardedForHash)) {
        if (incoming_count == incoming.size()) {
            break;
        }
        incoming[incoming_count++] = field.value_view();
    }
    std::string forwarded_for;
    bool has_incoming = false;
    while (incoming_count != 0) {
        if (!forwarded_for.empty()) {
            forwarded_for.append(", ");
        }
        forwarded_for.append(incoming[--incoming_count]);
        has_incoming = true;
    }
    if (has_incoming) {
        forwarded_for.append(", ");
    }
    forwarded_for.append(peer_text);
    return headers.set(kForwardedForHeader, forwarded_for, kForwardedForLowcase.data(), kForwardedForHash) != nullptr;
}

} // namespace fiber::access_server
