#ifndef FIBER_ACCESS_SERVER_ACCESS_HTTP_LISTENER_OPTIONS_H
#define FIBER_ACCESS_SERVER_ACCESS_HTTP_LISTENER_OPTIONS_H

#include <fiber/http/HttpServerTlsOptions.h>

namespace fiber::access_server {

// Traffic-listener policy carried from configuration into the data plane. It maps
// onto the per-protocol fiber endpoint options; the fiber defaults for timeouts
// and header sizing are intentional. The TLS listener serves HTTP/2 with
// HTTP/1.1 negotiation (plus HTTP/3 over QUIC when enabled); the plaintext
// listener serves HTTP/1.1 only.
struct AccessHttpListenerOptions {
    http::HttpServerTlsOptions tls{};
    bool http3_enabled = false;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_ACCESS_HTTP_LISTENER_OPTIONS_H
