#ifndef FIBER_ACCESS_SERVER_ACCESS_HTTP_SCRIPT_SERVICES_H
#define FIBER_ACCESS_SERVER_ACCESS_HTTP_SCRIPT_SERVICES_H

#include "ProxyUpstreamConnection.h"

#include <fiber/http/StealableHttp1ConnectionPoolSet.h>
#include <fiber/http_script/HttpScriptServices.h>

namespace fiber::access_server {

// Bridges route-script HTTP directives to the same per-worker DNS, connection
// pool, TLS policy, and Happy Eyeballs implementation used by PROXY routes.
class AccessHttpScriptServices final : public http_script::HttpScriptServices {
public:
    AccessHttpScriptServices(http::StealableHttp1ConnectionPoolSet &pool, ProxyDnsResolver dns_resolver,
                             UpstreamTlsClientPolicy tls_policy = {},
                             ProxyHappyEyeballsPolicy happy_eyeballs = {}) noexcept;

    [[nodiscard]] async::Task<common::IoResult<std::unique_ptr<http_script::HttpUpstreamConnection>>>
    acquire(const http_script::HttpTargetSpec &target, std::chrono::milliseconds connect_timeout) noexcept override;

private:
    http::StealableHttp1ConnectionPoolSet *pool_ = nullptr;
    ProxyDnsResolver dns_resolver_;
    UpstreamTlsClientPolicy tls_policy_;
    ProxyHappyEyeballsPolicy happy_eyeballs_;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_ACCESS_HTTP_SCRIPT_SERVICES_H
