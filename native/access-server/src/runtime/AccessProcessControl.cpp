#include "AccessProcessControl.h"

#include "AccessServerConfig.h"
#include "AccessServerRuntime.h"

#include <algorithm>
#include <cerrno>
#include <utility>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <fiber/common/Assert.h>
#include <fiber/log/Log.h>
#include <fiber/net/UnixAddress.h>
#include <openssl/sha.h>
#include "../observability/AccessServerLogCategories.h"

namespace fiber::access_server {
namespace {

DEFINE_LOGGER(LOG_UPGRADE, kAccessServerLifecycleLogger);

AccessServerRuntime &runtime(void *context) noexcept { return *static_cast<AccessServerRuntime *>(context); }

async::Task<common::IoResult<void>> activate_runtime(void *context) noexcept {
    auto result = co_await runtime(context).activate_upgrade();
    if (!result) {
        co_return std::unexpected(result.error().io_error);
    }
    co_return common::IoResult<void>{};
}

async::Task<common::IoResult<void>> release_runtime(void *context) noexcept {
    auto result = co_await runtime(context).release();
    if (!result) {
        co_return std::unexpected(result.error().io_error);
    }
    co_return common::IoResult<void>{};
}

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

bool same_socket(const struct stat &st, std::uint64_t device, std::uint64_t inode) noexcept {
    return S_ISSOCK(st.st_mode) && st.st_uid == ::geteuid() && static_cast<std::uint64_t>(st.st_dev) == device &&
           static_cast<std::uint64_t>(st.st_ino) == inode;
}

} // namespace

AccessProcessActions access_process_actions(AccessServerRuntime &instance) noexcept {
    return AccessProcessActions{
            .context = &instance,
            .state = [](void *p) noexcept { return runtime(p).state(); },
            .ready = [](void *p) noexcept { return runtime(p).upgrade_ready(); },
            .registration = [](void *p) noexcept { return runtime(p).registration_state(); },
            .activate = &activate_runtime,
            .release = &release_runtime,
    };
}

common::IoResult<AccessProcessControlOptions> access_process_control_options(const AccessServerConfig &config,
                                                                             std::string directory) {
    // A digest of length-delimited settings, not authentication. Only non-secret
    // process/environment settings participate. The UDS peer is authenticated separately.
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
    identity = hex(digest.data(), digest.size());
    std::string log_identity;
    if (!config.access_log_directory().empty()) {
        struct stat st{};
        if (::stat(std::string(config.access_log_directory()).c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
            return std::unexpected(common::IoErr::Invalid);
        }
        log_identity = std::to_string(st.st_dev) + ":" + std::to_string(st.st_ino);
    }
    return AccessProcessControlOptions{
            .directory = std::move(directory),
            .compatibility = std::move(identity),
            .log_identity = std::move(log_identity),
            .http3 = config.tls_http_server_options().http3_enabled,
            .fixed_ports = (!tls || config.tls_listen_address().port() != 0) &&
                           (!config.plain_listen_enabled() || config.plain_listen_address().port() != 0) &&
                           config.metrics_listen_address().port() != 0,
    };
}

std::string_view access_process_state_name(AccessServerRuntimeState state) noexcept {
    switch (state) {
        case AccessServerRuntimeState::Created:
            return "created";
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

AccessProcessControl::AccessProcessControl(event::EventLoop &loop, AccessProcessActions actions,
                                           AccessProcessControlOptions options) :
    loop_(loop), actions_(actions), options_(std::move(options)), listener_(loop) {
    FIBER_ASSERT(actions_.state && actions_.ready && actions_.registration && actions_.activate && actions_.release);
    exit_publisher_ = exit_.acquire_publisher();
    FIBER_ASSERT(exit_publisher_);
}

AccessProcessControl::~AccessProcessControl() {
    FIBER_ASSERT(tasks_.empty());
    cleanup_path();
    if (lock_fd_ >= 0) {
        ::close(lock_fd_);
    }
    if (directory_fd_ >= 0) {
        ::close(directory_fd_);
    }
}

common::IoResult<void> AccessProcessControl::initialize() {
    if (directory_fd_ >= 0) {
        return std::unexpected(common::IoErr::Already);
    }
    while (options_.directory.size() > 1 && options_.directory.back() == '/') {
        options_.directory.pop_back();
    }
    if (options_.directory.empty() || options_.directory.front() != '/' ||
        options_.directory.find('\0') != std::string::npos) {
        return std::unexpected(common::IoErr::Invalid);
    }
    path_ = options_.directory + "/" + std::to_string(::getpid()) + ".sock";
    if (path_.size() >= sizeof(sockaddr_un::sun_path)) {
        return std::unexpected(common::IoErr::Invalid);
    }
    directory_fd_ = ::open(options_.directory.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directory_fd_ < 0) {
        return std::unexpected(common::io_err_from_errno(errno));
    }
    struct stat directory{};
    if (::fstat(directory_fd_, &directory) != 0 || directory.st_uid != ::geteuid() || (directory.st_mode & 077U) != 0) {
        return std::unexpected(common::IoErr::Permission);
    }
    lock_fd_ = ::openat(directory_fd_, "upgrade.lock", O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    struct stat lock{};
    if (lock_fd_ < 0 || ::fstat(lock_fd_, &lock) != 0 || !S_ISREG(lock.st_mode) || lock.st_uid != ::geteuid() ||
        (lock.st_mode & 077U) != 0 || lock.st_nlink != 1) {
        return std::unexpected(common::IoErr::Permission);
    }
    std::array<unsigned char, 16> random{};
    std::size_t offset = 0;
    while (offset < random.size()) {
        const auto size = ::getrandom(random.data() + offset, random.size() - offset, 0);
        if (size < 0 && errno == EINTR) {
            continue;
        }
        if (size <= 0) {
            return std::unexpected(common::IoErr::Unknown);
        }
        offset += static_cast<std::size_t>(size);
    }
    boot_id_ = hex(random.data(), random.size());
    struct stat existing{};
    if (::lstat(path_.c_str(), &existing) == 0) {
        if (!S_ISSOCK(existing.st_mode) || existing.st_uid != ::geteuid()) {
            return std::unexpected(common::IoErr::Permission);
        }
        // A PID from a prior incarnation may have left a socket. Only remove it
        // after connect positively reports a stale listening endpoint.
        const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0) {
            return std::unexpected(common::io_err_from_errno(errno));
        }
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::copy(path_.begin(), path_.end(), address.sun_path);
        const int connected = ::connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address));
        const int connect_error = errno;
        ::close(fd);
        if (connected == 0 || connect_error != ECONNREFUSED) {
            return std::unexpected(common::IoErr::AddrInUse);
        }
        struct stat current{};
        if (::lstat(path_.c_str(), &current) != 0 || !same_socket(current, existing.st_dev, existing.st_ino) ||
            ::unlink(path_.c_str()) != 0) {
            return std::unexpected(common::IoErr::Busy);
        }
    } else if (errno != ENOENT) {
        return std::unexpected(common::io_err_from_errno(errno));
    }
    auto bound = listener_.bind(net::UnixAddress::filesystem(path_), {});
    if (!bound) {
        return bound;
    }
    struct stat socket{};
    if (::lstat(path_.c_str(), &socket) != 0) {
        return std::unexpected(common::IoErr::Unknown);
    }
    socket_device_ = socket.st_dev;
    socket_inode_ = socket.st_ino;
    if (::chmod(path_.c_str(), 0600) != 0) {
        return std::unexpected(common::io_err_from_errno(errno));
    }
    return {};
}

void AccessProcessControl::cleanup_path() noexcept {
    if (socket_inode_ == 0) {
        return;
    }
    struct stat current{};
    if (::lstat(path_.c_str(), &current) == 0 && same_socket(current, socket_device_, socket_inode_)) {
        (void) ::unlink(path_.c_str());
    }
    socket_inode_ = 0;
}

void AccessProcessControl::start() {
    FIBER_ASSERT(loop_.in_loop() && listener_.valid() && !started_);
    started_ = true;
    tasks_.add();
    async::spawn(loop_, [this]() { return accept_connections(); });
}

async::DetachedTask AccessProcessControl::accept_connections() {
    while (!stopping_) {
        auto accepted = co_await listener_.accept();
        if (!accepted) {
            break;
        }
        const int fd = accepted->fd;
        auto available = std::find_if(streams_.begin(), streams_.end(), [](const auto &stream) { return !stream; });
        if (available == streams_.end() || !validate_access_control_peer(fd)) {
            ::close(fd);
            continue;
        }
        const auto slot = static_cast<std::size_t>(available - streams_.begin());
        *available = std::make_unique<net::UnixStream>(loop_, fd);
        tasks_.add();
        async::spawn(loop_, [this, slot]() { return serve_connection(slot); });
    }
    tasks_.done();
}

async::DetachedTask AccessProcessControl::serve_connection(std::size_t slot) {
    auto &stream = *streams_[slot];
    for (unsigned count = 0; count < 64 && !stopping_; ++count) {
        auto request = co_await read_access_control(stream);
        if (!request) {
            break;
        }
        const bool release_command = request->command == "RELEASE";
        const bool abort_command = request->command == "ABORT";
        auto response = co_await handle(std::move(*request));
        const bool exit_after_reply =
                ((release_command && released_) || (abort_command && abort_requested_)) && response.error.empty();
        auto sent = co_await write_access_control(stream, response);
        if (exit_after_reply) {
            request_exit();
        }
        if (!sent || exit_after_reply) {
            break;
        }
    }
    streams_[slot].reset();
    tasks_.done();
}

AccessControlMessage AccessProcessControl::status() const {
    const auto state = actions_.state(actions_.context);
    return AccessControlMessage{
            .pid = ::getpid(),
            .request_id = "status",
            .boot_id = boot_id_,
            .upgrade_id = released_ || release_running_ ? release_id_ : upgrade_id_,
            .state = std::string(access_process_state_name(state)),
            .phase = phase_,
            .registration = std::string(actions_.registration(actions_.context)),
            .compatibility = options_.compatibility,
            .log_identity = options_.log_identity,
            .error = job_error_,
            .previous_socket = old_socket_,
            .previous_boot_id = old_boot_id_,
            .ready = state == AccessServerRuntimeState::Prepared && !stopping_ && actions_.ready(actions_.context),
            .http3 = options_.http3,
    };
}

async::Task<AccessControlMessage> AccessProcessControl::handle(AccessControlMessage request) {
    auto response = status();
    response.request_id = request.request_id;
    response.error.clear();
    if (request.command == "STATUS") {
        response = status();
        response.request_id = request.request_id;
        co_return response;
    }
    if (quiescing_) {
        response.error = "PROCESS_STOPPING";
        co_return response;
    }
    if (request.expected_boot_id != boot_id_) {
        response.error = "BOOT_ID_MISMATCH";
        co_return response;
    }
    if (request.command == "ABORT") {
        if (actions_.state(actions_.context) == AccessServerRuntimeState::Running ||
            actions_.state(actions_.context) == AccessServerRuntimeState::Draining ||
            actions_.state(actions_.context) == AccessServerRuntimeState::Activating || job_running_) {
            response.error = "UPGRADE_BUSY";
        } else {
            abort_requested_ = true;
        }
        co_return response;
    }
    if (request.command != "TAKEOVER" && request.command != "RELEASE") {
        response.error = "UNKNOWN_COMMAND";
        co_return response;
    }
    if (request.upgrade_id.empty()) {
        response.error = "UPGRADE_ID_REQUIRED";
        co_return response;
    }
    if (options_.http3) {
        response.error = "UPGRADE_HTTP3_UNSUPPORTED";
        co_return response;
    }
    if (!options_.fixed_ports) {
        response.error = "UPGRADE_FIXED_PORTS_REQUIRED";
        co_return response;
    }
    if (request.command == "RELEASE") {
        if (released_) {
            if (release_id_ != request.upgrade_id) {
                response.error = "ALREADY_RELEASED";
            } else {
                response.phase = "released";
            }
            co_return response;
        }
        if (release_running_ || actions_.state(actions_.context) != AccessServerRuntimeState::Running) {
            response.error = "UPGRADE_BUSY";
            co_return response;
        }
        operations_.add();
        release_running_ = true;
        release_id_ = request.upgrade_id;
        phase_ = "releasing";
        auto released = co_await actions_.release(actions_.context);
        release_running_ = false;
        operations_.done();
        if (!released) {
            response.error = "RELEASE_FAILED";
            co_return response;
        }
        released_ = true;
        phase_ = "released";
        LOG(LOG_UPGRADE, INFO) << "listeners released" << " boot_id=" << boot_id_;
    } else {
        if (!upgrade_id_.empty()) {
            if (upgrade_id_ != request.upgrade_id || old_socket_ != request.old_socket ||
                old_boot_id_ != request.old_boot_id) {
                response.error = "UPGRADE_CONFLICT";
                co_return response;
            }
        } else {
            if (!response.ready || request.old_socket.empty() || request.old_boot_id.empty() ||
                request.old_socket == path_) {
                response.error = "NOT_PREPARED";
                co_return response;
            }
            if (::flock(lock_fd_, LOCK_EX | LOCK_NB) != 0) {
                response.error = "UPGRADE_BUSY";
                co_return response;
            }
            upgrade_id_ = request.upgrade_id;
            old_socket_ = request.old_socket;
            old_boot_id_ = request.old_boot_id;
            job_running_ = true;
            phase_ = "checking";
            tasks_.add();
            operations_.add();
            async::spawn(loop_,
                         [this, request = std::move(request)]() mutable { return takeover(std::move(request)); });
        }
    }
    auto updated = status();
    updated.request_id = response.request_id;
    co_return updated;
}

async::Task<bool> AccessProcessControl::previous_is_drained(const AccessControlMessage &old) {
    if (old.previous_socket.empty()) {
        co_return true;
    }
    AccessControlMessage query;
    query.request_id = "previous";
    query.command = "STATUS";
    auto previous = co_await request_access_control(loop_, old.previous_socket, std::move(query));
    if (!previous) {
        co_return previous.error() == common::IoErr::NotFound || previous.error() == common::IoErr::ConnRefused;
    }
    co_return previous->boot_id != old.previous_boot_id || previous->state == "stopped";
}

async::DetachedTask AccessProcessControl::takeover(AccessControlMessage request) {
    struct Guard {
        AccessProcessControl &self;
        ~Guard() {
            (void) ::flock(self.lock_fd_, LOCK_UN);
            self.job_running_ = false;
            if (!self.job_error_.empty()) {
                self.phase_ = "failed";
            }
            LOG(LOG_UPGRADE, INFO) << "upgrade finished" << " boot_id=" << self.boot_id_ << " phase=" << self.phase_
                                   << " error=" << self.job_error_;
            self.operations_.done();
            self.tasks_.done();
        }
    } guard{*this};
    auto connected = co_await net::UnixStream::connect(loop_, net::UnixAddress::filesystem(request.old_socket),
                                                       kAccessControlTimeout);
    if (!connected) {
        job_error_ = "OLD_PROCESS_UNREACHABLE";
        co_return;
    }
    net::UnixStream stream(std::move(*connected));
    if (!validate_access_control_peer(stream.fd())) {
        job_error_ = "OLD_PROCESS_UNAUTHORIZED";
        co_return;
    }
    AccessControlMessage query;
    query.request_id = "takeover-status";
    query.command = "STATUS";
    auto old = co_await exchange_access_control(stream, std::move(query));
    if (!old || old->boot_id != request.old_boot_id) {
        job_error_ = "OLD_BOOT_ID_MISMATCH";
        co_return;
    }
    if (old->http3) {
        job_error_ = "UPGRADE_HTTP3_UNSUPPORTED";
        co_return;
    }
    if (old->state != "serving" || old->compatibility != options_.compatibility) {
        job_error_ = "UPGRADE_INCOMPATIBLE";
        co_return;
    }
    if (!options_.log_identity.empty() && old->log_identity == options_.log_identity) {
        job_error_ = "UPGRADE_LOG_DIRECTORY_SHARED";
        co_return;
    }
    if (!co_await previous_is_drained(*old)) {
        job_error_ = "PREVIOUS_PROCESS_DRAINING";
        co_return;
    }
    if (quiescing_ || !actions_.ready(actions_.context)) {
        job_error_ = "NOT_PREPARED";
        co_return;
    }
    phase_ = "releasing";
    AccessControlMessage release;
    release.request_id = "takeover-release";
    release.command = "RELEASE";
    release.expected_boot_id = request.old_boot_id;
    release.upgrade_id = request.upgrade_id;
    auto ack = co_await exchange_access_control(stream, std::move(release));
    if (!ack) {
        // Lost acknowledgement: only an explicit status for this boot/upgrade
        // may authorize bind. EOF or a failed port probe is not confirmation.
        AccessControlMessage retry;
        retry.request_id = "release-status";
        retry.command = "STATUS";
        ack = co_await request_access_control(loop_, request.old_socket, std::move(retry));
    }
    if (!ack || !ack->error.empty() || ack->boot_id != request.old_boot_id || ack->upgrade_id != request.upgrade_id ||
        ack->phase != "released") {
        job_error_ = "RELEASE_UNCONFIRMED";
        co_return;
    }
    phase_ = "activating";
    auto activated = co_await actions_.activate(actions_.context);
    if (!activated) {
        job_error_ = "ACTIVATION_FAILED";
        co_return;
    }
    phase_ = "serving";
}

void AccessProcessControl::request_exit() { exit_publisher_->publish(true); }

async::Task<void> AccessProcessControl::wait_exit() noexcept {
    auto subscriber = exit_.subscribe();
    auto snapshot = subscriber.current();
    while (!snapshot.value || !*snapshot.value) {
        snapshot = co_await subscriber.next(snapshot.version);
    }
}

async::Task<void> AccessProcessControl::quiesce() noexcept {
    quiescing_ = true;
    co_await operations_.join();
}

async::Task<void> AccessProcessControl::shutdown() noexcept {
    FIBER_ASSERT(loop_.in_loop());
    stopping_ = true;
    listener_.close();
    for (auto &stream: streams_) {
        if (stream) {
            stream->close();
        }
    }
    co_await tasks_.join();
    cleanup_path();
}

} // namespace fiber::access_server
