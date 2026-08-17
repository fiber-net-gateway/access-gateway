#include "AccessDnsService.h"

#include <chrono>
#include <new>
#include <utility>

#include <fiber/async/Spawn.h>
#include <fiber/common/Assert.h>
#include <fiber/event/EventLoop.h>
#include <fiber/event/EventLoopGroup.h>
#include <fiber/net/SocketAddress.h>

namespace fiber::access_server {
namespace {

bool create_system_resolver(void *, event::EventLoop &loop, dns::SharedDnsCache2 &cache,
                            const dns::DnsClient::Options &client_options,
                            std::unique_ptr<dns::DnsResolverLocal> &local,
                            std::unique_ptr<dns::DnsResolver> &resolver) noexcept {
    local.reset(new (std::nothrow) dns::DnsResolverLocal());
    if (!local || !local->init(loop, cache, client_options)) {
        return false;
    }
    resolver.reset(new (std::nothrow) dns::DnsResolver());
    return resolver && resolver->init(*local);
}

} // namespace

AccessDnsServiceOptions AccessDnsServiceOptions::local_default() noexcept {
    AccessDnsServiceOptions options;
    const bool added = options.client.nameservers.add(net::SocketAddress(net::IpAddress::v4({127, 0, 0, 1}), 53));
    FIBER_ASSERT(added);
    options.client.timeout = std::chrono::milliseconds(2000);
    options.client.attempts = 2;
    return options;
}

AccessDnsResolverFactory AccessDnsResolverFactory::system() noexcept {
    return AccessDnsResolverFactory{
            .create = create_system_resolver,
    };
}

AccessDnsService::AccessDnsService() noexcept : AccessDnsService(AccessDnsServiceOptions::local_default()) {}

AccessDnsService::AccessDnsService(AccessDnsResolverFactory resolver_factory) noexcept :
    AccessDnsService(AccessDnsServiceOptions::local_default(), resolver_factory) {}

AccessDnsService::AccessDnsService(AccessDnsServiceOptions options,
                                   AccessDnsResolverFactory resolver_factory) noexcept :
    options_(std::move(options)), resolver_factory_(resolver_factory) {
    FIBER_ASSERT(resolver_factory_.create != nullptr);
    FIBER_ASSERT(!options_.client.nameservers.empty());
    options_.metrics.configure(options_.source, options_.client.nameservers.size(), options_.unsupported);
}

AccessDnsService::~AccessDnsService() noexcept { FIBER_ASSERT(state_ == State::Stopped); }

async::Task<bool> AccessDnsService::init(event::EventLoopGroup &group) noexcept {
    event::EventLoop *current_loop = event::EventLoop::current_or_null();
    FIBER_ASSERT(current_loop != nullptr);
    if (state_ == State::Running) {
        FIBER_ASSERT(control_loop_ == current_loop);
        co_return true;
    }
    FIBER_ASSERT(state_ == State::Stopped);
    if (group.size() == 0) {
        options_.metrics.initialized(false);
        options_.metrics.set_state(AccessDnsResolverState::Failed, 0);
        co_return false;
    }
    options_.metrics.set_state(AccessDnsResolverState::Starting, 0);
    control_loop_ = current_loop;
    cache_loop_ = &group.at(0);
    if (!cache_.init(*cache_loop_)) {
        control_loop_ = nullptr;
        cache_loop_ = nullptr;
        options_.metrics.initialized(false);
        options_.metrics.set_state(AccessDnsResolverState::Failed, 0);
        co_return false;
    }
    state_ = State::Starting;

    entries_.reserve(group.size());
    for (std::size_t i = 0; i < group.size(); ++i) {
        LoopEntry entry;
        entry.loop = &group.at(i);
        const bool initialized = resolver_factory_.create(resolver_factory_.context, *entry.loop, cache_,
                                                          options_.client, entry.local, entry.resolver);
        const bool valid =
                initialized && entry.local && entry.local->valid() && entry.resolver && entry.resolver->valid();
        entries_.push_back(std::move(entry));
        if (!valid) {
            options_.metrics.initialized(false);
            options_.metrics.set_state(AccessDnsResolverState::Failed, entries_.size() - 1);
            co_await shutdown();
            co_return false;
        }
        options_.metrics.set_state(AccessDnsResolverState::Starting, entries_.size());
    }
    state_ = State::Running;
    options_.metrics.initialized(true);
    options_.metrics.set_state(AccessDnsResolverState::Ready, entries_.size());
    co_return true;
}

async::Task<void> AccessDnsService::shutdown() noexcept {
    if (state_ == State::Stopped) {
        co_return;
    }
    FIBER_ASSERT(control_loop_ != nullptr);
    FIBER_ASSERT(control_loop_->in_loop());
    FIBER_ASSERT(state_ == State::Starting || state_ == State::Running);
    state_ = State::Stopping;
    options_.metrics.set_state(AccessDnsResolverState::Stopping, entries_.size());

    if (!entries_.empty()) {
        release_tasks_.add(entries_.size());
        for (LoopEntry &entry: entries_) {
            LoopEntry *current = &entry;
            async::spawn(*entry.loop, [this, current]() -> async::DetachedTask { co_await release_entry(current); });
        }
        co_await release_tasks_.join();
    }
    entries_.clear();

    if (cache_loop_) {
        release_tasks_.add();
        async::spawn(*cache_loop_, [this]() -> async::DetachedTask { co_await shutdown_cache(); });
        co_await release_tasks_.join();
    }
    cache_loop_ = nullptr;
    control_loop_ = nullptr;
    state_ = State::Stopped;
    options_.metrics.set_state(AccessDnsResolverState::Stopped, 0);
}

async::Task<void> AccessDnsService::release_entry(LoopEntry *entry) noexcept {
    FIBER_ASSERT(entry != nullptr);
    FIBER_ASSERT(entry->loop != nullptr);
    FIBER_ASSERT(entry->loop->in_loop());
    if (entry->resolver) {
        entry->resolver->release();
        entry->resolver.reset();
    }
    if (entry->local) {
        entry->local->release();
        entry->local.reset();
    }
    release_tasks_.done();
    co_return;
}

async::Task<void> AccessDnsService::shutdown_cache() noexcept {
    FIBER_ASSERT(cache_loop_ != nullptr);
    FIBER_ASSERT(cache_loop_->in_loop());
    cache_.shutdown();
    release_tasks_.done();
    co_return;
}

ProxyDnsResolver AccessDnsService::adapter() noexcept {
    return ProxyDnsResolver{
            .context = this,
            .resolve = &AccessDnsService::resolve,
    };
}

async::Task<common::IoResult<std::vector<net::IpAddress>>> AccessDnsService::resolve(void *context,
                                                                                     std::string_view host) noexcept {
    auto &self = *static_cast<AccessDnsService *>(context);
    event::EventLoop *current = event::EventLoop::current_or_null();
    dns::DnsResolver *resolver = nullptr;
    for (const LoopEntry &entry: self.entries_) {
        if (entry.loop == current) {
            resolver = entry.resolver.get();
            break;
        }
    }
    if (!resolver) {
        co_return std::unexpected(common::IoErr::Invalid);
    }

    dns::AddressResolveResult result;
    if (!result.init()) {
        co_return std::unexpected(common::IoErr::NoMem);
    }
    auto resolved = co_await resolver->resolve_host(host, result);
    if (!resolved) {
        co_return std::unexpected(resolved.error());
    }
    if (result.record_count() == 0) {
        co_return std::unexpected(common::IoErr::NotFound);
    }
    std::vector<net::IpAddress> addresses;
    addresses.reserve(result.record_count());
    for (std::uint16_t i = 0; i < result.record_count(); ++i) {
        addresses.push_back(result.records()[i]);
    }
    co_return addresses;
}

} // namespace fiber::access_server
