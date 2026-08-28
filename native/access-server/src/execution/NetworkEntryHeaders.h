#ifndef FIBER_ACCESS_SERVER_NETWORK_ENTRY_HEADERS_H
#define FIBER_ACCESS_SERVER_NETWORK_ENTRY_HEADERS_H

#include <string_view>

#include <fiber/net/IpAddress.h>

namespace fiber::http {
class HttpHeaders;
} // namespace fiber::http

namespace fiber::access_server {

// Synthesizes the edge-proxy headers an nginx front end injected for the
// deployment's entry network: X-Entry, X-Real-Ip, X-Forwarded-Proto, and
// X-Forwarded-For. Values replace any client-supplied fields (nginx
// proxy_set_header semantics), so direct clients cannot forge them. X-Entry
// is server-authoritative even with an empty entry: the deployment declared
// no entry network, so a client-supplied X-Entry is removed rather than
// adopted, and entry-gated host policies reject the request. Other client
// headers are untouched when entry is empty. Returns false when header
// allocation fails; the request then proceeds unmodified, which the legacy
// client metadata resolver already tolerates.
[[nodiscard]] bool apply_network_entry_headers(http::HttpHeaders &headers, const net::IpAddress &peer,
                                               std::string_view entry, bool connection_secure) noexcept;

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_NETWORK_ENTRY_HEADERS_H
