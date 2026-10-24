#include "AccessBoss.h"

#include <algorithm>
#include <cerrno>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <fiber/async/Mutex.h>
#include <fiber/async/Sleep.h>
#include <fiber/async/Spawn.h>
#include <fiber/async/TaskSelect.h>
#include <fiber/async/WhenAny.h>
#include <fiber/common/Assert.h>
#include <fiber/log/Log.h>
#include <fiber/net/UnixAddress.h>
#include "../observability/AccessServerLogCategories.h"

namespace fiber::access_server {
namespace {

DEFINE_LOGGER(LOG_BOSS, kAccessServerBossLogger);

constexpr unsigned kMaxRequestsPerConnection = 64;

bool is_prepared(const AccessControlMessage &status) noexcept { return status.state == "prepared"; }

bool is_ready(const AccessControlMessage &status) noexcept { return status.state == "prepared" && status.ready; }

bool activation_settled(const AccessControlMessage &status) noexcept {
    return status.state == "serving" || status.phase == "failed" || status.state == "stopped";
}

bool abortable_phase(std::string_view phase) noexcept {
    return phase == "starting" || phase == "preparing" || phase == "checking";
}

std::string describe_exit(int wait_status) {
    if (WIFEXITED(wait_status)) {
        return "exit=" + std::to_string(WEXITSTATUS(wait_status));
    }
    if (WIFSIGNALED(wait_status)) {
        return "signal=" + std::to_string(WTERMSIG(wait_status));
    }
    return "status=" + std::to_string(wait_status);
}

bool same_socket(const struct stat &st, std::uint64_t device, std::uint64_t inode) noexcept {
    return S_ISSOCK(st.st_mode) && st.st_uid == ::geteuid() && static_cast<std::uint64_t>(st.st_dev) == device &&
           static_cast<std::uint64_t>(st.st_ino) == inode;
}

async::Task<void> sleep_for(std::chrono::milliseconds delay) { co_await async::sleep(delay); }

} // namespace

struct AccessBoss::Generation {
    Generation(event::EventLoop &loop, AccessBossChild child, int binary) noexcept :
        pid(child.pid), binary_fd(binary), channel(loop, child.channel_fd) {}
    ~Generation() {
        if (binary_fd >= 0) {
            ::close(binary_fd);
        }
    }
    Generation(const Generation &) = delete;
    Generation &operator=(const Generation &) = delete;

    pid_t pid;
    int binary_fd;
    net::UnixStream channel;
    // One request/reply in flight per child; a reply always follows its request.
    async::Mutex io;
    AccessControlMessage last;
    unsigned users = 0;
    bool exited = false;
};

// Keeps a generation (its channel and pinned binary) alive across suspension.
// Exited generations are destroyed once the last reference drops.
class AccessBoss::GenerationRef {
public:
    GenerationRef(AccessBoss &boss, Generation &generation) noexcept : boss_(&boss), generation_(&generation) {
        ++generation.users;
    }
    GenerationRef(GenerationRef &&other) noexcept :
        boss_(other.boss_), generation_(std::exchange(other.generation_, nullptr)) {}
    GenerationRef(const GenerationRef &) = delete;
    GenerationRef &operator=(const GenerationRef &) = delete;
    GenerationRef &operator=(GenerationRef &&) = delete;
    ~GenerationRef() {
        if (generation_) {
            --generation_->users;
            boss_->collect(*generation_);
        }
    }

    Generation &operator*() const noexcept { return *generation_; }
    Generation *operator->() const noexcept { return generation_; }
    [[nodiscard]] Generation *get() const noexcept { return generation_; }

private:
    AccessBoss *boss_;
    Generation *generation_;
};

AccessBoss::AccessBoss(event::EventLoop &loop, AccessBossLauncher &launcher, AccessBossOptions options) :
    loop_(loop), launcher_(launcher), options_(std::move(options)), listener_(loop) {
    stop_publisher_ = stop_signal_.acquire_publisher();
    FIBER_ASSERT(stop_publisher_);
}

AccessBoss::~AccessBoss() {
    FIBER_ASSERT(control_tasks_.empty() && flows_.empty() && alive_.empty());
    cleanup_path();
    if (known_good_binary_ >= 0) {
        ::close(known_good_binary_);
    }
}

common::IoResult<void> AccessBoss::initialize() {
    const auto &path = options_.socket_path;
    if (path.empty()) {
        return {};
    }
    if (socket_inode_ != 0) {
        return std::unexpected(common::IoErr::Already);
    }
    if (path.front() != '/' || path.size() >= sizeof(sockaddr_un::sun_path) || path.find('\0') != std::string::npos) {
        return std::unexpected(common::IoErr::Invalid);
    }
    const auto slash = path.find_last_of('/');
    const std::string directory = slash == 0 ? "/" : path.substr(0, slash);
    const int directory_fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directory_fd < 0) {
        return std::unexpected(common::io_err_from_errno(errno));
    }
    // Only the boss user may create or replace entries next to the socket.
    struct stat owner{};
    const bool private_directory =
            ::fstat(directory_fd, &owner) == 0 && owner.st_uid == ::geteuid() && (owner.st_mode & 022U) == 0;
    ::close(directory_fd);
    if (!private_directory) {
        return std::unexpected(common::IoErr::Permission);
    }
    struct stat existing{};
    if (::lstat(path.c_str(), &existing) == 0) {
        if (!S_ISSOCK(existing.st_mode) || existing.st_uid != ::geteuid()) {
            return std::unexpected(common::IoErr::Permission);
        }
        // A previous boss may have left its socket. Only remove it after connect
        // positively reports a stale endpoint; a live boss keeps the path.
        const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0) {
            return std::unexpected(common::io_err_from_errno(errno));
        }
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::copy(path.begin(), path.end(), address.sun_path);
        const int connected = ::connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address));
        const int connect_error = errno;
        ::close(fd);
        if (connected == 0 || connect_error != ECONNREFUSED) {
            return std::unexpected(common::IoErr::AddrInUse);
        }
        struct stat current{};
        if (::lstat(path.c_str(), &current) != 0 || !same_socket(current, existing.st_dev, existing.st_ino) ||
            ::unlink(path.c_str()) != 0) {
            return std::unexpected(common::IoErr::Busy);
        }
    } else if (errno != ENOENT) {
        return std::unexpected(common::io_err_from_errno(errno));
    }
    auto bound = listener_.bind(net::UnixAddress::filesystem(path), {});
    if (!bound) {
        return bound;
    }
    struct stat socket{};
    if (::lstat(path.c_str(), &socket) != 0) {
        return std::unexpected(common::IoErr::Unknown);
    }
    socket_device_ = socket.st_dev;
    socket_inode_ = socket.st_ino;
    if (::chmod(path.c_str(), 0600) != 0) {
        return std::unexpected(common::io_err_from_errno(errno));
    }
    return {};
}

void AccessBoss::cleanup_path() noexcept {
    if (socket_inode_ == 0) {
        return;
    }
    struct stat current{};
    if (::lstat(options_.socket_path.c_str(), &current) == 0 && same_socket(current, socket_device_, socket_inode_)) {
        (void) ::unlink(options_.socket_path.c_str());
    }
    socket_inode_ = 0;
}

void AccessBoss::start() {
    FIBER_ASSERT(loop_.in_loop() && !started_);
    started_ = true;
    if (listener_.valid()) {
        control_tasks_.add();
        async::spawn(loop_, [this]() { return accept_connections(); });
    }
    ensure_serving();
}

AccessBoss::Generation *AccessBoss::spawn_generation(int binary_fd) {
    auto child = launcher_.spawn(binary_fd);
    if (!child) {
        LOG(LOG_BOSS, ERROR) << "failed to start access-server" << " error=" << common::io_err_name(child.error());
        ::close(binary_fd);
        return nullptr;
    }
    generations_.push_back(std::make_unique<Generation>(loop_, *child, binary_fd));
    alive_.add();
    LOG(LOG_BOSS, INFO) << "started access-server" << " pid=" << child->pid;
    return generations_.back().get();
}

common::IoResult<int> AccessBoss::cold_binary() noexcept {
    if (known_good_binary_ < 0) {
        return launcher_.open_binary();
    }
    const int binary = ::fcntl(known_good_binary_, F_DUPFD_CLOEXEC, 0);
    if (binary < 0) {
        return std::unexpected(common::io_err_from_errno(errno));
    }
    return binary;
}

void AccessBoss::collect(Generation &generation) noexcept {
    if (!generation.exited || generation.users != 0) {
        return;
    }
    FIBER_ASSERT(active_ != &generation && candidate_ != &generation);
    auto found = std::find_if(generations_.begin(), generations_.end(),
                              [&generation](const auto &item) { return item.get() == &generation; });
    FIBER_ASSERT(found != generations_.end());
    generations_.erase(found);
    alive_.done();
}

void AccessBoss::mark_serving(Generation &generation) noexcept {
    restart_delay_ = std::chrono::milliseconds(0);
    const int binary = ::fcntl(generation.binary_fd, F_DUPFD_CLOEXEC, 0);
    if (binary >= 0) {
        if (known_good_binary_ >= 0) {
            ::close(known_good_binary_);
        }
        known_good_binary_ = binary;
    } else {
        LOG(LOG_BOSS, WARN) << "failed to retain known-good binary" << " pid=" << generation.pid;
    }
    LOG(LOG_BOSS, INFO) << "access-server serving" << " pid=" << generation.pid;
}

void AccessBoss::ensure_serving() noexcept {
    if (!started_ || stopping_ || active_ || upgrade_running_ || cold_scheduled_) {
        return;
    }
    cold_scheduled_ = true;
    const auto delay = restart_delay_;
    restart_delay_ = std::min(std::max(options_.restart_initial_delay, restart_delay_ * 2), options_.restart_max_delay);
    flows_.add();
    async::spawn(loop_, [this, delay]() { return cold_start(delay); });
}

bool AccessBoss::other_generation_alive() const noexcept {
    return std::any_of(generations_.begin(), generations_.end(),
                       [this](const auto &item) { return !item->exited && item.get() != active_; });
}

std::string AccessBoss::abandon(Generation &generation, std::string_view error) noexcept {
    if (!generation.exited) {
        launcher_.terminate(generation.pid);
    }
    return std::string(error);
}

async::Task<common::IoResult<AccessControlMessage>> AccessBoss::exchange(Generation &generation,
                                                                         AccessControlMessage request) {
    auto lock = co_await generation.io.lock();
    if (generation.exited || !generation.channel.valid()) {
        co_return std::unexpected(common::IoErr::ConnReset);
    }
    request.request_id = "boss-" + std::to_string(++request_sequence_);
    auto response = co_await exchange_access_control(generation.channel, std::move(request));
    if (!response) {
        // Never reuse a desynchronized stream. The child treats the closed end
        // as a lost boss and stops within its bounded deadline.
        LOG(LOG_BOSS, WARN) << "access-server channel lost" << " pid=" << generation.pid
                            << " error=" << common::io_err_name(response.error());
        generation.channel.close();
        co_return response;
    }
    generation.last = *response;
    co_return response;
}

async::Task<AccessBoss::PollResult> AccessBoss::poll(Generation &generation, StatusPredicate predicate,
                                                     std::optional<std::chrono::steady_clock::time_point> deadline,
                                                     bool abortable) {
    for (;;) {
        if (generation.exited) {
            co_return PollResult::Exited;
        }
        if (stopping_ || (abortable && abort_requested_)) {
            co_return PollResult::Cancelled;
        }
        AccessControlMessage request;
        request.command = "STATUS";
        auto status = co_await exchange(generation, std::move(request));
        if (!status) {
            co_return generation.exited ? PollResult::Exited : PollResult::Lost;
        }
        if (predicate(*status)) {
            co_return PollResult::Reached;
        }
        if (deadline && loop_.now() >= *deadline) {
            co_return PollResult::TimedOut;
        }
        co_await pause(options_.poll_interval);
    }
}

async::Task<bool> AccessBoss::wait_exited(Generation &generation, std::chrono::milliseconds timeout) {
    const auto deadline = loop_.now() + timeout;
    while (!generation.exited && !stopping_ && loop_.now() < deadline) {
        co_await pause(options_.poll_interval);
    }
    co_return generation.exited;
}

async::Task<void> AccessBoss::pause(std::chrono::milliseconds delay) {
    auto stopped = stop_signal_.subscribe();
    auto snapshot = stopped.current();
    if (snapshot.value && *snapshot.value) {
        co_return;
    }
    auto result = co_await async::when_any([delay]() { return sleep_for(delay).select(); },
                                           [&stopped, version = snapshot.version]() { return stopped.next(version); });
    if (result.is<0>()) {
        std::move(result).get<0>();
    } else {
        (void) std::move(result).get<1>();
    }
}

async::Task<void> AccessBoss::refresh(Generation *generation) {
    // Skip a child another flow is already talking to; that flow keeps `last` fresh.
    if (!generation || generation->exited || generation->io.locked()) {
        co_return;
    }
    GenerationRef reference(*this, *generation);
    AccessControlMessage request;
    request.command = "STATUS";
    (void) co_await exchange(*reference, std::move(request));
}

async::DetachedTask AccessBoss::cold_start(std::chrono::milliseconds delay) {
    if (delay.count() > 0) {
        LOG(LOG_BOSS, INFO) << "restarting access-server" << " delay_ms=" << delay.count();
        co_await pause(delay);
    }
    cold_scheduled_ = false;
    if (!stopping_ && !active_ && !upgrade_running_) {
        auto binary = cold_binary();
        Generation *generation = nullptr;
        if (binary) {
            generation = spawn_generation(*binary);
        } else {
            LOG(LOG_BOSS, ERROR) << "cannot open access-server binary"
                                 << " error=" << common::io_err_name(binary.error());
        }
        if (generation) {
            active_ = generation;
            flows_.add();
            async::spawn(loop_, [this, reference = GenerationRef(*this, *generation)]() mutable {
                return run_cold(std::move(reference));
            });
        } else {
            ensure_serving();
        }
    }
    flows_.done();
}

async::DetachedTask AccessBoss::run_cold(GenerationRef generation) {
    // Cold start keeps the original order: prepare, then bind, register in the
    // foreground and serve. Failures exit the child; child_exited() restarts.
    bool serving = false;
    if (co_await poll(*generation, &is_prepared, std::nullopt, false) == PollResult::Reached) {
        AccessControlMessage request;
        request.command = "ACTIVATE";
        auto accepted = co_await exchange(*generation, std::move(request));
        serving = accepted && accepted->error.empty() &&
                  co_await poll(*generation, &activation_settled, std::nullopt, false) == PollResult::Reached &&
                  generation->last.state == "serving" && active_ == generation.get();
    }
    if (serving) {
        mark_serving(*generation);
    } else if (!generation->exited && !stopping_) {
        launcher_.terminate(generation->pid);
    }
    flows_.done();
}

async::DetachedTask AccessBoss::run_upgrade() {
    auto error = co_await upgrade();
    if (!error.empty() && stopping_) {
        // Whatever step observed it, the boss shutting down ended the task.
        error = "PROCESS_STOPPING";
    }
    upgrade_error_ = error;
    upgrade_phase_ = error.empty() ? "serving" : "failed";
    if (error.empty()) {
        LOG(LOG_BOSS, INFO) << "upgrade finished" << " upgrade_id=" << upgrade_id_;
    } else {
        LOG(LOG_BOSS, WARN) << "upgrade failed" << " upgrade_id=" << upgrade_id_ << " error=" << error;
    }
    upgrade_running_ = false;
    abort_requested_ = false;
    ensure_serving();
    flows_.done();
}

async::Task<std::string> AccessBoss::upgrade() {
    upgrade_phase_ = "starting";
    if (stopping_) {
        co_return "PROCESS_STOPPING";
    }
    auto binary = launcher_.open_binary();
    if (!binary) {
        co_return "OPEN_BINARY_FAILED";
    }
    Generation *spawned = spawn_generation(*binary);
    if (!spawned) {
        co_return "SPAWN_FAILED";
    }
    GenerationRef candidate(*this, *spawned);
    candidate_ = spawned;
    // Declared after the reference: clears the slot before the reference drops.
    struct CandidateSlot {
        AccessBoss &boss;
        ~CandidateSlot() { boss.candidate_ = nullptr; }
    } slot{*this};

    upgrade_phase_ = "preparing";
    const auto prepared = co_await poll(*candidate, &is_ready, loop_.now() + options_.prewarm_timeout, true);
    if (prepared != PollResult::Reached) {
        co_return abandon(*candidate, prepared == PollResult::TimedOut    ? "PREWARM_TIMEOUT"
                                      : prepared == PollResult::Cancelled ? (stopping_ ? "PROCESS_STOPPING" : "ABORTED")
                                                                          : "PREWARM_FAILED");
    }

    upgrade_phase_ = "checking";
    std::optional<GenerationRef> old;
    if (active_) {
        old.emplace(*this, *active_);
        AccessControlMessage query;
        query.command = "STATUS";
        auto current = co_await exchange(**old, std::move(query));
        if (!current) {
            // The old channel is closed now and that generation stops on its own;
            // its exit frees the ports, so the candidate can still take over.
            if (!co_await wait_exited(**old, options_.release_exit_timeout)) {
                co_return abandon(*candidate, "ACTIVE_UNREACHABLE");
            }
        } else if (!(*old)->exited) {
            if (current->state != "serving") {
                co_return abandon(*candidate, "ACTIVE_NOT_SERVING");
            }
            if (!current->fixed_ports || !candidate->last.fixed_ports) {
                co_return abandon(*candidate, "UPGRADE_FIXED_PORTS_REQUIRED");
            }
            if (current->compatibility != candidate->last.compatibility) {
                co_return abandon(*candidate, "UPGRADE_INCOMPATIBLE");
            }
        }
    }
    AccessControlMessage confirm;
    confirm.command = "STATUS";
    auto fresh = co_await exchange(*candidate, std::move(confirm));
    if (!fresh || !is_ready(*fresh)) {
        co_return abandon(*candidate, "NOT_PREPARED");
    }
    if (stopping_ || abort_requested_) {
        co_return abandon(*candidate, stopping_ ? "PROCESS_STOPPING" : "ABORTED");
    }

    // Irreversible from here: the old generation stops accepting.
    upgrade_phase_ = "releasing";
    if (old && !(*old)->exited) {
        AccessControlMessage release;
        release.command = "RELEASE";
        release.upgrade_id = upgrade_id_;
        auto released = co_await exchange(**old, std::move(release));
        if (released && !released->error.empty()) {
            co_return abandon(*candidate, "RELEASE_FAILED");
        }
        // Only an explicit reply or the process exit proves the listeners are
        // closed; a lost reply is never taken as permission to bind.
        if ((!released || released->phase != "released") &&
            !co_await wait_exited(**old, options_.release_exit_timeout)) {
            co_return abandon(*candidate, "RELEASE_UNCONFIRMED");
        }
        if (active_ == old->get()) {
            active_ = nullptr;
        }
        if (!(*old)->exited) {
            draining_.push_back(old->get());
        }
        LOG(LOG_BOSS, INFO) << "old generation released" << " pid=" << (*old)->pid;
    }

    upgrade_phase_ = "activating";
    AccessControlMessage activate;
    activate.command = "ACTIVATE";
    activate.background = true;
    auto accepted = co_await exchange(*candidate, std::move(activate));
    if (!accepted || !accepted->error.empty() ||
        co_await poll(*candidate, &activation_settled, std::nullopt, false) != PollResult::Reached ||
        candidate->last.state != "serving" || candidate->exited) {
        auto error = abandon(*candidate, "ACTIVATION_FAILED");
        // Nothing listens any more. Let the failed candidate give back ports it
        // may have bound before the cold restart tries to bind them.
        (void) co_await wait_exited(*candidate, options_.release_exit_timeout);
        co_return error;
    }
    FIBER_ASSERT(active_ == nullptr);
    active_ = candidate.get();
    mark_serving(*candidate);
    co_return std::string{};
}

void AccessBoss::child_exited(pid_t pid, int wait_status) noexcept {
    auto found = std::find_if(generations_.begin(), generations_.end(),
                              [pid](const auto &item) { return item->pid == pid && !item->exited; });
    if (found == generations_.end()) {
        return;
    }
    Generation &generation = **found;
    generation.exited = true;
    const bool active = active_ == &generation;
    if (active) {
        active_ = nullptr;
    }
    if (active && !stopping_) {
        LOG(LOG_BOSS, WARN) << "active access-server exited" << " pid=" << pid << " " << describe_exit(wait_status);
    } else {
        LOG(LOG_BOSS, INFO) << "access-server exited" << " pid=" << pid << " " << describe_exit(wait_status);
    }
    std::erase(draining_, &generation);
    collect(generation);
    ensure_serving();
}

async::Task<AccessControlMessage> AccessBoss::handle(AccessControlMessage request) {
    std::string error;
    if (request.command == "STATUS") {
        co_await refresh(active_);
        co_await refresh(candidate_);
    } else if (request.command == "UPGRADE") {
        if (request.upgrade_id.empty()) {
            error = "UPGRADE_ID_REQUIRED";
        } else if (request.upgrade_id == upgrade_id_) {
            // A retried request reports the task it already started.
        } else if (stopping_) {
            error = "PROCESS_STOPPING";
        } else if (upgrade_running_) {
            error = "UPGRADE_BUSY";
        } else if (!active_ || active_->last.state != "serving") {
            error = "NOT_SERVING";
        } else if (!draining_.empty()) {
            error = "PREVIOUS_PROCESS_DRAINING";
        } else if (other_generation_alive()) {
            error = "PREVIOUS_PROCESS_STOPPING";
        } else {
            upgrade_id_ = request.upgrade_id;
            upgrade_phase_ = "starting";
            upgrade_error_.clear();
            upgrade_running_ = true;
            abort_requested_ = false;
            LOG(LOG_BOSS, INFO) << "upgrade accepted" << " upgrade_id=" << upgrade_id_;
            flows_.add();
            async::spawn(loop_, [this]() { return run_upgrade(); });
        }
    } else if (request.command == "ABORT") {
        if (!upgrade_running_) {
            error = "NO_UPGRADE";
        } else if (!request.upgrade_id.empty() && request.upgrade_id != upgrade_id_) {
            error = "UPGRADE_CONFLICT";
        } else if (!abortable_phase(upgrade_phase_)) {
            error = "UPGRADE_NOT_ABORTABLE";
        } else {
            abort_requested_ = true;
        }
    } else {
        error = "UNKNOWN_COMMAND";
    }
    auto response = status();
    response.request_id = request.request_id;
    response.error = std::move(error);
    co_return response;
}

AccessControlMessage AccessBoss::status() const {
    AccessControlMessage status{
            .pid = ::getpid(),
            .prewarm_timeout_millis = options_.prewarm_timeout.count(),
            .request_id = "status",
            .upgrade_id = upgrade_id_,
            .state = stopping_ ? "stopping" : "running",
            .phase = upgrade_phase_,
            .upgrade_error = upgrade_error_,
            .server_path = options_.server_path,
    };
    if (active_) {
        status.active_pid = active_->pid;
        status.active_state = active_->last.state.empty() ? "starting" : active_->last.state;
        status.active_registration = active_->last.registration;
    }
    if (!draining_.empty()) {
        status.draining_pid = draining_.front()->pid;
    }
    if (candidate_) {
        status.candidate_pid = candidate_->pid;
        status.candidate_state = candidate_->last.state.empty() ? "starting" : candidate_->last.state;
    }
    return status;
}

async::Task<void> AccessBoss::stop() noexcept {
    FIBER_ASSERT(loop_.in_loop());
    if (!stopping_) {
        stopping_ = true;
        abort_requested_ = true;
        stop_publisher_->publish(true);
        LOG(LOG_BOSS, INFO) << "stopping all access-server generations";
    }
    const auto terminate_all = [this]() {
        for (const auto &generation: generations_) {
            if (!generation->exited) {
                launcher_.terminate(generation->pid);
            }
        }
    };
    terminate_all();
    co_await flows_.join();
    // A flow may have started a child between the first pass and observing stop.
    terminate_all();
    co_await alive_.join();
    listener_.close();
    for (auto &stream: streams_) {
        if (stream) {
            stream->close();
        }
    }
    co_await control_tasks_.join();
    cleanup_path();
}

async::DetachedTask AccessBoss::accept_connections() {
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
        control_tasks_.add();
        async::spawn(loop_, [this, slot]() { return serve_connection(slot); });
    }
    control_tasks_.done();
}

async::DetachedTask AccessBoss::serve_connection(std::size_t slot) {
    auto &stream = *streams_[slot];
    for (unsigned count = 0; count < kMaxRequestsPerConnection; ++count) {
        auto request = co_await read_access_control(stream);
        if (!request) {
            break;
        }
        auto response = co_await handle(std::move(*request));
        if (!co_await write_access_control(stream, response)) {
            break;
        }
    }
    streams_[slot].reset();
    control_tasks_.done();
}

} // namespace fiber::access_server
