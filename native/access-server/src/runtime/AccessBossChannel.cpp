#include "AccessBossChannel.h"

#include "AccessServerConfig.h"

#include <array>
#include <cerrno>
#include <csignal>
#include <utility>

#include <fcntl.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <fiber/async/Spawn.h>
#include <fiber/common/Assert.h>
#include <fiber/log/Log.h>
#include <openssl/sha.h>
#include "../observability/AccessServerLogCategories.h"

namespace fiber::access_server {
namespace {

DEFINE_LOGGER(LOG_UPGRADE, kAccessServerLifecycleLogger);

std::string hex(const unsigned char *data, std::size_t size) {
    constexpr char alphabet[] = "0123456789abcdef";
    std::string result;
    result.reserve(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        result.push_back(alphabet[data[i] >> 4U]);
        result.push_back(alphabet[data[i] & 15U]);
    }
    return result;
}

} // namespace

AccessBossChannelOptions access_boss_channel_options(const AccessServerConfig &config) {
    // A digest of length-delimited settings, not authentication. Only non-secret
    // process/environment settings participate.
    std::string identity;
    auto add = [&identity](std::string_view value) {
        identity += std::to_string(value.size()) + ":";
        identity.append(value);
    };
    const bool tls = config.tls_http_server_options().tls.enabled();
    add(tls ? config.tls_listen_address().to_string() : "tls-off");
    add(config.plain_listen_enabled() ? config.plain_listen_address().to_string() : "plain-off");
    add(config.metrics_listen_address().to_string());
    add(config.network_entry());
    add(config.test_mode() ? "test" : "production");
    add(config.nacos_config().namespace_id());
    add(config.nacos_config().tenant());
    for (const auto &host: config.nacos_config().server_hosts()) {
        add(host.value());
    }
    add(std::to_string(config.nacos_config().grpc_port()));
    add(config.watcher_options().project_list_data_id);
    add(config.watcher_options().project_route_data_id_prefix);
    add(config.watcher_options().project_route_group);
    add(config.gray_watcher_options().data_id);
    add(config.gray_watcher_options().group);
    add(config.temp_cluster_watcher_options().data_id);
    add(config.tls_certificate_watcher_options().data_id);
    const auto &registration = config.instance_registration_options();
    add(registration.service_name);
    add(registration.group);
    add(registration.cluster_name);
    add(registration.advertise_address ? registration.advertise_address->to_string() : "auto");
    add(config.registration_listener() == AccessRegistrationListener::Tls ? "tls" : "plain");
    std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
    (void) SHA256(reinterpret_cast<const unsigned char *>(identity.data()), identity.size(), digest.data());
    return AccessBossChannelOptions{
            .compatibility = hex(digest.data(), digest.size()),
            .http3 = config.tls_http_server_options().http3_enabled,
            .fixed_ports = (!tls || config.tls_listen_address().port() != 0) &&
                           (!config.plain_listen_enabled() || config.plain_listen_address().port() != 0) &&
                           config.metrics_listen_address().port() != 0,
    };
}

std::string_view access_process_state_name(AccessServerRuntimeState state) noexcept {
    switch (state) {
        case AccessServerRuntimeState::Created:
        case AccessServerRuntimeState::Starting:
            return "preparing";
        case AccessServerRuntimeState::Prepared:
            return "prepared";
        case AccessServerRuntimeState::Activating:
            return "activating";
        case AccessServerRuntimeState::Running:
            return "serving";
        case AccessServerRuntimeState::Draining:
            return "draining";
        case AccessServerRuntimeState::Stopping:
            return "stopping";
        case AccessServerRuntimeState::Stopped:
            return "stopped";
    }
    return "unknown";
}

common::IoResult<void> bind_access_boss_lifetime(int fd) noexcept {
    int domain = 0;
    int type = 0;
    socklen_t length = sizeof(domain);
    if (::getsockopt(fd, SOL_SOCKET, SO_DOMAIN, &domain, &length) != 0 || domain != AF_UNIX) {
        return std::unexpected(common::IoErr::Invalid);
    }
    length = sizeof(type);
    if (::getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &length) != 0 || type != SOCK_STREAM) {
        return std::unexpected(common::IoErr::Invalid);
    }
    // A socketpair reports the credentials of the process that created it.
    ucred peer{};
    length = sizeof(peer);
    if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &length) != 0 || length != sizeof(peer) ||
        peer.pid != ::getppid()) {
        return std::unexpected(common::IoErr::Permission);
    }
    if (::prctl(PR_SET_PDEATHSIG, SIGTERM) != 0) {
        return std::unexpected(common::io_err_from_errno(errno));
    }
    if (::getppid() != peer.pid) {
        return std::unexpected(common::IoErr::ConnReset);
    }
    const int flags = ::fcntl(fd, F_GETFL);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0 || ::fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) {
        return std::unexpected(common::io_err_from_errno(errno));
    }
    return {};
}

AccessBossChannel::AccessBossChannel(event::EventLoop &loop, int fd, AccessBossChildActions actions,
                                     AccessBossChannelOptions options) :
    loop_(loop), actions_(actions), options_(std::move(options)), stream_(loop, fd) {
    FIBER_ASSERT(actions_.state && actions_.ready && actions_.registration && actions_.activate && actions_.release);
    exit_publisher_ = exit_.acquire_publisher();
    FIBER_ASSERT(exit_publisher_);
}

AccessBossChannel::~AccessBossChannel() { FIBER_ASSERT(tasks_.empty()); }

void AccessBossChannel::start() {
    FIBER_ASSERT(loop_.in_loop() && !started_);
    started_ = true;
    tasks_.add();
    async::spawn(loop_, [this]() { return serve(); });
}

async::DetachedTask AccessBossChannel::serve() {
    for (;;) {
        // Idle between requests without a deadline; a started frame must then
        // complete within the protocol timeout.
        auto readable = co_await stream_.wait_readable();
        if (stopping_ || !readable) {
            break;
        }
        bool frame_started = false;
        auto request = co_await read_access_control(stream_, &frame_started);
        if (!request) {
            if (request.error() == common::IoErr::TimedOut && !frame_started) {
                // A stale readiness edge with no frame bytes: keep waiting.
                // Only a started frame is bound by the protocol timeout.
                continue;
            }
            break;
        }
        const bool release_command = request->command == "RELEASE";
        auto response = co_await handle(std::move(*request));
        const bool exit_after_reply = release_command && response.error.empty() && phase_ == "released";
        auto sent = co_await write_access_control(stream_, response);
        if (exit_after_reply) {
            request_exit();
        }
        if (!sent) {
            break;
        }
    }
    if (!stopping_) {
        // EOF, a broken frame, or a failed reply: the boss is gone or no longer
        // in sync. Nothing serves without a boss.
        boss_lost_ = true;
        LOG(LOG_UPGRADE, WARN) << "boss channel lost; stopping";
        if (options_.boss_lost) {
            options_.boss_lost();
        }
        request_exit();
    }
    tasks_.done();
}

AccessControlMessage AccessBossChannel::status() const {
    const auto state = actions_.state(actions_.context);
    return AccessControlMessage{
            .pid = ::getpid(),
            .request_id = "status",
            .state = std::string(access_process_state_name(state)),
            .phase = phase_,
            .registration = std::string(actions_.registration(actions_.context)),
            .compatibility = options_.compatibility,
            .ready = state == AccessServerRuntimeState::Prepared && !quiescing_ && actions_.ready(actions_.context),
            .http3 = options_.http3,
            .fixed_ports = options_.fixed_ports,
    };
}

async::Task<AccessControlMessage> AccessBossChannel::handle(AccessControlMessage request) {
    auto response = status();
    response.request_id = request.request_id;
    if (request.command == "STATUS") {
        co_return response;
    }
    if (quiescing_) {
        response.error = "PROCESS_STOPPING";
        co_return response;
    }
    const auto state = actions_.state(actions_.context);
    if (request.command == "ACTIVATE") {
        // Readiness was confirmed by the boss before it released the old
        // generation; refusing here would only leave no generation serving.
        if (phase_ != "idle" || state != AccessServerRuntimeState::Prepared) {
            response.error = "NOT_PREPARED";
            co_return response;
        }
        phase_ = "activating";
        operations_.add();
        tasks_.add();
        async::spawn(loop_, [this, background = request.background]() { return activate(background); });
    } else if (request.command == "RELEASE") {
        if (phase_ == "released") {
            co_return response;
        }
        if (releasing_ || state != AccessServerRuntimeState::Running) {
            response.error = "NOT_SERVING";
            co_return response;
        }
        operations_.add();
        releasing_ = true;
        auto released = co_await actions_.release(actions_.context);
        releasing_ = false;
        operations_.done();
        if (!released) {
            response = status();
            response.request_id = request.request_id;
            response.error = "RELEASE_FAILED";
            co_return response;
        }
        phase_ = "released";
        LOG(LOG_UPGRADE, INFO) << "TCP listeners released" << " pid=" << ::getpid();
    } else {
        response.error = "UNKNOWN_COMMAND";
        co_return response;
    }
    auto updated = status();
    updated.request_id = response.request_id;
    co_return updated;
}

async::DetachedTask AccessBossChannel::activate(bool background_registration) {
    auto activated = co_await actions_.activate(actions_.context, background_registration);
    if (activated) {
        phase_ = "serving";
    } else {
        phase_ = "failed";
        failed_ = true;
        LOG(LOG_UPGRADE, ERROR) << "activation failed" << " pid=" << ::getpid()
                                << " error=" << common::io_err_name(activated.error());
        request_exit();
    }
    operations_.done();
    tasks_.done();
}

void AccessBossChannel::request_exit() noexcept { exit_publisher_->publish(true); }

async::Task<void> AccessBossChannel::wait_exit() noexcept {
    auto subscriber = exit_.subscribe();
    auto snapshot = subscriber.current();
    while (!snapshot.value || !*snapshot.value) {
        snapshot = co_await subscriber.next(snapshot.version);
    }
}

async::Task<void> AccessBossChannel::quiesce() noexcept {
    quiescing_ = true;
    co_await operations_.join();
}

async::Task<void> AccessBossChannel::shutdown() noexcept {
    FIBER_ASSERT(loop_.in_loop());
    stopping_ = true;
    stream_.close();
    co_await tasks_.join();
}

} // namespace fiber::access_server
