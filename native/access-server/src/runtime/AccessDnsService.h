#ifndef FIBER_ACCESS_SERVER_ACCESS_DNS_SERVICE_H
#define FIBER_ACCESS_SERVER_ACCESS_DNS_SERVICE_H

#include "../execution/ProxyUpstreamConnection.h"
#include "../observability/AccessDnsMetrics.h"

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include <fiber/async/Task.h>
#include <fiber/async/WaitGroup.h>
#include <fiber/common/NonCopyable.h>
#include <fiber/common/NonMovable.h>
#include <fiber/dns/DnsCache2.h>
#include <fiber/dns/DnsClient.h>
#include <fiber/dns/DnsResolver.h>

namespace fiber::event {
class EventLoop;
class EventLoopGroup;
} // namespace fiber::event

namespace fiber::access_server {

struct AccessDnsResolverFactory {
    // Cold-path factory. On false, output owners may still be populated; the
    // service adopts them and releases them asynchronously on `loop`.
    using CreateFunction = bool (*)(void *context, event::EventLoop &loop, dns::SharedDnsCache2 &cache,
                                    const dns::DnsClient::Options &client_options,
                                    std::unique_ptr<dns::DnsResolverLocal> &local,
                                    std::unique_ptr<dns::DnsResolver> &resolver) noexcept;

    void *context = nullptr;
    CreateFunction create = nullptr;

    [[nodiscard]] static AccessDnsResolverFactory system() noexcept;
};

struct AccessDnsServiceOptions {
    dns::DnsClient::Options client;
    AccessDnsConfigSource source = AccessDnsConfigSource::Override;
    dns::ResolverUnsupportedFeature unsupported = dns::ResolverUnsupportedFeature::None;
    AccessDnsMetricsObserver metrics;

    // Direct unit construction uses a local, fail-closed resolver endpoint.
    // Production always replaces this with the validated system/override
    // options prepared before EventLoops start.
    [[nodiscard]] static AccessDnsServiceOptions local_default() noexcept;
};

// DnsResolver is loop-affine. This service creates one resolver stack per
// request worker and shares only the thread-safe cache between those stacks.
class AccessDnsService final : public common::NonCopyable, public common::NonMovable {
public:
    AccessDnsService() noexcept;
    explicit AccessDnsService(AccessDnsResolverFactory resolver_factory) noexcept;
    explicit AccessDnsService(AccessDnsServiceOptions options,
                              AccessDnsResolverFactory resolver_factory = AccessDnsResolverFactory::system()) noexcept;
    ~AccessDnsService() noexcept;

    // Both operations run on one control loop. The worker group must remain serviceable until shutdown completes.
    [[nodiscard]] async::Task<bool> init(event::EventLoopGroup &group) noexcept;
    [[nodiscard]] async::Task<void> shutdown() noexcept;
    [[nodiscard]] ProxyDnsResolver adapter() noexcept;

private:
    enum class State : std::uint8_t {
        Stopped,
        Starting,
        Running,
        Stopping,
    };

    struct LoopEntry {
        event::EventLoop *loop = nullptr;
        std::unique_ptr<dns::DnsResolverLocal> local;
        std::unique_ptr<dns::DnsResolver> resolver;
    };

    [[nodiscard]] async::Task<void> release_entry(LoopEntry *entry) noexcept;
    [[nodiscard]] async::Task<void> shutdown_cache() noexcept;
    [[nodiscard]] static async::Task<common::IoResult<std::vector<net::IpAddress>>>
    resolve(void *context, std::string_view host) noexcept;

    dns::SharedDnsCache2 cache_;
    AccessDnsServiceOptions options_;
    AccessDnsResolverFactory resolver_factory_;
    event::EventLoop *control_loop_ = nullptr;
    event::EventLoop *cache_loop_ = nullptr;
    std::vector<LoopEntry> entries_;
    async::WaitGroup release_tasks_;
    State state_ = State::Stopped;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_ACCESS_DNS_SERVICE_H
