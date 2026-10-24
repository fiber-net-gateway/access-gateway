#include "runtime/AccessBoss.h"
#include "runtime/AccessBossChannel.h"
#include "runtime/AccessBossLauncher.h"
#include "runtime/AccessServerBinaryInstall.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <csignal>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <fiber/async/Sleep.h>
#include <fiber/async/Spawn.h>

namespace fiber::access_server {
namespace {

using namespace std::chrono_literals;

class Directory {
public:
    Directory() {
        std::array<char, 40> pattern{};
        constexpr std::string_view prefix = "/tmp/access-boss-XXXXXX";
        std::copy(prefix.begin(), prefix.end(), pattern.begin());
        if (auto *created = ::mkdtemp(pattern.data())) {
            path = created;
        }
    }
    ~Directory() {
        std::error_code error;
        if (!path.empty()) {
            std::filesystem::remove_all(path, error);
        }
    }
    std::string path;
};

void write_file(const std::string &path, std::string_view content, mode_t mode) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    ASSERT_GE(fd, 0) << path;
    ASSERT_EQ(::write(fd, content.data(), content.size()), static_cast<ssize_t>(content.size()));
    ASSERT_EQ(::fchmod(fd, mode), 0);
    ::close(fd);
}

std::string read_file(const std::string &path) {
    std::string content;
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return content;
    }
    std::array<char, 256> buffer{};
    ssize_t read = 0;
    while ((read = ::read(fd, buffer.data(), buffer.size())) > 0) {
        content.append(buffer.data(), static_cast<std::size_t>(read));
    }
    ::close(fd);
    return content;
}

struct stat stat_of(const std::string &path) {
    struct stat st{};
    EXPECT_EQ(::stat(path.c_str(), &st), 0) << path;
    return st;
}

class FakeLauncher;

// An in-process child: the real AccessBossChannel over a socketpair, backed by
// a scripted runtime instead of Nacos and listeners.
struct FakeChild {
    FakeLauncher *launcher = nullptr;
    pid_t pid = 0;
    int channel_fd = -1;
    ino_t binary_inode = 0;
    std::unique_ptr<AccessBossChannel> channel;
    AccessServerRuntimeState state = AccessServerRuntimeState::Starting;
    std::string compatibility = "same";
    bool fixed_ports = true;
    bool hold_prepare = false;
    bool hold_drain = false;
    bool crash_on_start = false;
    bool fail_activation = false;
    bool fail_release = false;
    bool drop_release_reply = false;
    bool background = false;
    unsigned activations = 0;
    unsigned releases = 0;
    unsigned serving_peers_at_activation = 0;
    unsigned live_peers_at_activation = 0;
    bool terminated = false;
    bool crashed = false;
    bool exited = false;

    AccessBossChildActions actions() noexcept;
    [[nodiscard]] bool exit_requested() const {
        return channel->failed() || channel->boss_lost() || channel->status().phase == "released";
    }
};

class FakeLauncher final : public AccessBossLauncher {
public:
    explicit FakeLauncher(event::EventLoop &loop) : loop_(loop) {}

    common::IoResult<int> open_binary() noexcept override { return open_access_server_binary(binary_path); }

    common::IoResult<AccessBossChild> spawn(int binary_fd) noexcept override {
        std::array<int, 2> pair{-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, pair.data()) != 0) {
            return std::unexpected(common::IoErr::Unknown);
        }
        struct stat binary{};
        EXPECT_EQ(::fstat(binary_fd, &binary), 0);
        auto child = std::make_unique<FakeChild>();
        child->launcher = this;
        child->pid = next_pid_++;
        child->channel_fd = pair[1];
        child->binary_inode = binary.st_ino;
        if (configure) {
            configure(children.size(), *child);
        }
        child->channel = std::make_unique<AccessBossChannel>(
                loop_, pair[1], child->actions(),
                AccessBossChannelOptions{.compatibility = child->compatibility, .fixed_ports = child->fixed_ports});
        auto *raw = child.get();
        children.push_back(std::move(child));
        running.add();
        async::spawn(loop_, [this, raw]() { return run_child(*raw); });
        return AccessBossChild{.pid = raw->pid, .channel_fd = pair[0]};
    }

    void terminate(pid_t pid) noexcept override {
        for (auto &child: children) {
            if (child->pid == pid) {
                child->terminated = true;
            }
        }
    }

    FakeChild &at(std::size_t index) { return *children.at(index); }

    [[nodiscard]] unsigned peers(const FakeChild &self, bool serving_only) const {
        return static_cast<unsigned>(std::count_if(children.begin(), children.end(), [&](const auto &child) {
            return child.get() != &self && !child->exited &&
                   (!serving_only || child->state == AccessServerRuntimeState::Running);
        }));
    }

    std::string binary_path;
    AccessBoss *boss = nullptr;
    std::function<void(std::size_t, FakeChild &)> configure;
    std::vector<std::unique_ptr<FakeChild>> children;
    async::WaitGroup running;

private:
    // Mirrors main(): prepare, then wait for the channel or a signal, then stop.
    async::DetachedTask run_child(FakeChild &child) {
        child.channel->start();
        while (!child.crash_on_start && !child.crashed && !child.terminated && child.hold_prepare) {
            co_await async::sleep(1ms);
        }
        if (!child.crash_on_start && !child.crashed && !child.terminated) {
            child.state = AccessServerRuntimeState::Prepared;
        }
        while (!child.crash_on_start && !child.crashed && !child.terminated && !child.exit_requested()) {
            co_await async::sleep(1ms);
        }
        while (!child.crashed && !child.terminated && child.hold_drain &&
               child.state == AccessServerRuntimeState::Draining) {
            co_await async::sleep(1ms);
        }
        const bool failed =
                child.crash_on_start || child.crashed || child.channel->failed() || child.channel->boss_lost();
        co_await child.channel->quiesce();
        co_await child.channel->shutdown();
        child.state = AccessServerRuntimeState::Stopped;
        child.exited = true;
        boss->child_exited(child.pid, failed ? (1 << 8) : 0);
        running.done();
    }

    event::EventLoop &loop_;
    pid_t next_pid_ = 1000;
};

AccessBossChildActions FakeChild::actions() noexcept {
    return {
            .context = this,
            .state = [](void *p) noexcept { return static_cast<FakeChild *>(p)->state; },
            .ready =
                    [](void *p) noexcept {
                        return static_cast<FakeChild *>(p)->state == AccessServerRuntimeState::Prepared;
                    },
            .registration = [](void *) noexcept -> std::string_view { return "pending"; },
            .activate = [](void *p, bool background) noexcept -> async::Task<common::IoResult<void>> {
                auto &self = *static_cast<FakeChild *>(p);
                ++self.activations;
                self.background = background;
                self.serving_peers_at_activation = self.launcher->peers(self, true);
                self.live_peers_at_activation = self.launcher->peers(self, false);
                self.state = AccessServerRuntimeState::Activating;
                co_await async::sleep(1ms);
                if (self.fail_activation) {
                    self.state = AccessServerRuntimeState::Stopped;
                    co_return std::unexpected(common::IoErr::AddrInUse);
                }
                self.state = AccessServerRuntimeState::Running;
                co_return common::IoResult<void>{};
            },
            .release = [](void *p) noexcept -> async::Task<common::IoResult<void>> {
                auto &self = *static_cast<FakeChild *>(p);
                ++self.releases;
                if (self.fail_release) {
                    co_return std::unexpected(common::IoErr::Busy);
                }
                self.state = AccessServerRuntimeState::Draining;
                if (self.drop_release_reply) {
                    // Listeners are gone but the reply never reaches the boss.
                    (void) ::shutdown(self.channel_fd, SHUT_RDWR);
                }
                co_return common::IoResult<void>{};
            },
    };
}

struct Harness {
    explicit Harness(std::chrono::milliseconds prewarm = 5s, std::string socket_path = {}) {
        EXPECT_FALSE(dir.path.empty());
        v1 = dir.path + "/v1";
        v2 = dir.path + "/v2";
        write_file(v1, "v1", 0755);
        write_file(v2, "v2", 0755);
        launcher.binary_path = v1;
        boss = std::make_unique<AccessBoss>(loop, launcher,
                                            AccessBossOptions{
                                                    .socket_path = std::move(socket_path),
                                                    .server_path = v1,
                                                    .prewarm_timeout = prewarm,
                                                    .restart_initial_delay = 1ms,
                                                    .restart_max_delay = 4ms,
                                                    .poll_interval = 1ms,
                                                    .release_exit_timeout = 1s,
                                            });
        launcher.boss = boss.get();
    }

    template<class Function>
    void run(Function function) {
        async::spawn(loop, [&]() -> async::DetachedTask {
            boss->start();
            co_await function();
            co_await boss->stop();
            co_await launcher.running.join();
            loop.stop();
        });
        loop.run();
        for (const auto &child: launcher.children) {
            EXPECT_TRUE(child->exited) << child->pid;
        }
    }

    async::Task<AccessControlMessage> command(std::string name, std::string upgrade_id = {}) {
        AccessControlMessage request;
        request.request_id = "test";
        request.command = std::move(name);
        request.upgrade_id = std::move(upgrade_id);
        co_return co_await boss->handle(std::move(request));
    }

    template<class Predicate>
    async::Task<bool> wait_for(Predicate predicate) {
        for (unsigned i = 0; i < 5000; ++i) {
            if (predicate()) {
                co_return true;
            }
            co_await async::sleep(1ms);
        }
        co_return predicate();
    }

    async::Task<bool> serving() {
        co_return co_await wait_for([this]() { return boss->status().active_state == "serving"; });
    }

    async::Task<AccessControlMessage> finished(std::string upgrade_id) {
        EXPECT_TRUE(co_await wait_for([&]() {
            const auto status = boss->status();
            return status.upgrade_id == upgrade_id && (status.phase == "serving" || status.phase == "failed");
        })) << upgrade_id;
        co_return boss->status();
    }

    async::Task<bool> preparing() {
        co_return co_await wait_for([this]() {
            const auto status = boss->status();
            return status.phase == "preparing" && status.candidate_pid != 0;
        });
    }

    event::EventLoop loop;
    Directory dir;
    std::string v1;
    std::string v2;
    FakeLauncher launcher{loop};
    std::unique_ptr<AccessBoss> boss;
};

TEST(AccessBossTest, ColdStartActivatesInTheForegroundFromTheConfiguredBinary) {
    Harness harness;
    harness.run([&]() -> async::Task<void> {
        EXPECT_TRUE(co_await harness.serving());
        auto &first = harness.launcher.at(0);
        EXPECT_EQ(first.activations, 1U);
        EXPECT_FALSE(first.background);
        EXPECT_EQ(first.binary_inode, stat_of(harness.v1).st_ino);
        const auto status = harness.boss->status();
        EXPECT_EQ(status.active_pid, first.pid);
        EXPECT_EQ(status.active_registration, "pending");
        EXPECT_EQ(status.phase, "idle");
        EXPECT_EQ(status.state, "running");
        EXPECT_EQ(status.server_path, harness.v1);
        EXPECT_EQ(status.prewarm_timeout_millis, 5000);
    });
    EXPECT_TRUE(harness.launcher.at(0).terminated);
}

TEST(AccessBossTest, UpgradeReleasesTheOldGenerationBeforeActivatingTheCandidate) {
    Harness harness;
    harness.launcher.configure = [](std::size_t index, FakeChild &child) { child.hold_drain = index == 0; };
    harness.run([&]() -> async::Task<void> {
        EXPECT_TRUE(co_await harness.serving());
        harness.launcher.binary_path = harness.v2;
        auto accepted = co_await harness.command("UPGRADE", "u1");
        EXPECT_TRUE(accepted.error.empty()) << accepted.error;
        auto done = co_await harness.finished("u1");
        EXPECT_EQ(done.phase, "serving");
        EXPECT_TRUE(done.upgrade_error.empty()) << done.upgrade_error;

        auto &old = harness.launcher.at(0);
        auto &fresh = harness.launcher.at(1);
        EXPECT_EQ(old.releases, 1U);
        EXPECT_EQ(fresh.activations, 1U);
        EXPECT_TRUE(fresh.background);
        EXPECT_EQ(fresh.serving_peers_at_activation, 0U);
        EXPECT_EQ(fresh.live_peers_at_activation, 1U);
        EXPECT_EQ(fresh.binary_inode, stat_of(harness.v2).st_ino);
        EXPECT_EQ(done.active_pid, fresh.pid);
        EXPECT_EQ(done.draining_pid, old.pid);

        // A retried request reports the same task instead of starting another.
        auto retried = co_await harness.command("UPGRADE", "u1");
        EXPECT_TRUE(retried.error.empty());
        EXPECT_EQ(retried.phase, "serving");
        EXPECT_EQ(harness.launcher.children.size(), 2U);
        auto blocked = co_await harness.command("UPGRADE", "u2");
        EXPECT_EQ(blocked.error, "PREVIOUS_PROCESS_DRAINING");

        old.hold_drain = false;
        EXPECT_TRUE(co_await harness.wait_for([&]() { return old.exited; }));
        EXPECT_EQ(harness.boss->status().draining_pid, 0);
        auto third = co_await harness.command("UPGRADE", "u3");
        EXPECT_TRUE(third.error.empty()) << third.error;
        EXPECT_EQ((co_await harness.finished("u3")).phase, "serving");
        EXPECT_EQ(fresh.releases, 1U);
        EXPECT_EQ(harness.launcher.at(2).binary_inode, stat_of(harness.v2).st_ino);
    });
}

TEST(AccessBossTest, IncompatibleOrEphemeralCandidatesNeverReleaseTheOldGeneration) {
    Harness harness;
    harness.launcher.configure = [](std::size_t index, FakeChild &child) {
        if (index == 1) {
            child.compatibility = "other";
        } else if (index == 2) {
            child.fixed_ports = false;
        }
    };
    harness.run([&]() -> async::Task<void> {
        EXPECT_TRUE(co_await harness.serving());
        (void) co_await harness.command("UPGRADE", "u1");
        EXPECT_EQ((co_await harness.finished("u1")).upgrade_error, "UPGRADE_INCOMPATIBLE");
        EXPECT_TRUE(harness.launcher.at(1).terminated);
        EXPECT_TRUE(co_await harness.wait_for([&]() { return harness.launcher.at(1).exited; }));

        (void) co_await harness.command("UPGRADE", "u2");
        EXPECT_EQ((co_await harness.finished("u2")).upgrade_error, "UPGRADE_FIXED_PORTS_REQUIRED");
        EXPECT_EQ(harness.launcher.at(0).releases, 0U);
        EXPECT_EQ(harness.launcher.at(1).activations, 0U);
        EXPECT_EQ(harness.launcher.at(2).activations, 0U);
        const auto status = harness.boss->status();
        EXPECT_EQ(status.active_pid, harness.launcher.at(0).pid);
        EXPECT_EQ(status.active_state, "serving");
    });
}

TEST(AccessBossTest, PrewarmTimeoutLeavesTheOldGenerationServing) {
    Harness harness(20ms);
    harness.launcher.configure = [](std::size_t index, FakeChild &child) { child.hold_prepare = index == 1; };
    harness.run([&]() -> async::Task<void> {
        EXPECT_TRUE(co_await harness.serving());
        (void) co_await harness.command("UPGRADE", "u1");
        EXPECT_EQ((co_await harness.finished("u1")).upgrade_error, "PREWARM_TIMEOUT");
        EXPECT_EQ(harness.launcher.at(0).releases, 0U);
        EXPECT_TRUE(harness.launcher.at(1).terminated);
        EXPECT_EQ(harness.boss->status().active_pid, harness.launcher.at(0).pid);
    });
}

TEST(AccessBossTest, RequestsAreValidatedAndAbortOnlyCancelsBeforeRelease) {
    Harness harness;
    harness.launcher.configure = [](std::size_t index, FakeChild &child) { child.hold_prepare = index <= 1; };
    harness.run([&]() -> async::Task<void> {
        EXPECT_EQ((co_await harness.command("UPGRADE")).error, "UPGRADE_ID_REQUIRED");
        EXPECT_EQ((co_await harness.command("UPGRADE", "u1")).error, "NOT_SERVING");
        EXPECT_EQ((co_await harness.command("ABORT")).error, "NO_UPGRADE");
        EXPECT_EQ((co_await harness.command("NOPE")).error, "UNKNOWN_COMMAND");
        EXPECT_TRUE(co_await harness.wait_for([&]() { return !harness.launcher.children.empty(); }));
        EXPECT_EQ((co_await harness.command("UPGRADE", "u1")).error, "NOT_SERVING");
        harness.launcher.at(0).hold_prepare = false;
        EXPECT_TRUE(co_await harness.serving());

        EXPECT_TRUE((co_await harness.command("UPGRADE", "u1")).error.empty());
        EXPECT_TRUE(co_await harness.preparing());
        EXPECT_EQ((co_await harness.command("UPGRADE", "u2")).error, "UPGRADE_BUSY");
        EXPECT_EQ((co_await harness.command("ABORT", "other")).error, "UPGRADE_CONFLICT");
        EXPECT_TRUE((co_await harness.command("ABORT", "u1")).error.empty());
        EXPECT_EQ((co_await harness.finished("u1")).upgrade_error, "ABORTED");
        EXPECT_EQ(harness.launcher.at(0).releases, 0U);
        EXPECT_TRUE(harness.launcher.at(1).terminated);
        EXPECT_EQ((co_await harness.command("ABORT")).error, "NO_UPGRADE");
    });
}

TEST(AccessBossTest, CandidateExitDuringPrewarmFailsTheUpgrade) {
    Harness harness;
    harness.launcher.configure = [](std::size_t index, FakeChild &child) { child.hold_prepare = index == 1; };
    harness.run([&]() -> async::Task<void> {
        EXPECT_TRUE(co_await harness.serving());
        (void) co_await harness.command("UPGRADE", "u1");
        EXPECT_TRUE(co_await harness.preparing());
        harness.launcher.at(1).crashed = true;
        EXPECT_EQ((co_await harness.finished("u1")).upgrade_error, "PREWARM_FAILED");
        EXPECT_EQ(harness.launcher.at(0).releases, 0U);
        EXPECT_EQ(harness.boss->status().active_state, "serving");
        EXPECT_EQ(harness.launcher.children.size(), 2U);
    });
}

TEST(AccessBossTest, RefusedReleaseKeepsTheOldGenerationServing) {
    Harness harness;
    harness.launcher.configure = [](std::size_t index, FakeChild &child) { child.fail_release = index == 0; };
    harness.run([&]() -> async::Task<void> {
        EXPECT_TRUE(co_await harness.serving());
        (void) co_await harness.command("UPGRADE", "u1");
        EXPECT_EQ((co_await harness.finished("u1")).upgrade_error, "RELEASE_FAILED");
        EXPECT_EQ(harness.launcher.at(0).releases, 1U);
        EXPECT_EQ(harness.launcher.at(0).state, AccessServerRuntimeState::Running);
        EXPECT_EQ(harness.launcher.at(1).activations, 0U);
        EXPECT_TRUE(harness.launcher.at(1).terminated);
        EXPECT_EQ(harness.boss->status().active_pid, harness.launcher.at(0).pid);
    });
}

TEST(AccessBossTest, LostReleaseReplyActivatesOnlyAfterTheOldGenerationExits) {
    Harness harness;
    harness.launcher.configure = [](std::size_t index, FakeChild &child) { child.drop_release_reply = index == 0; };
    harness.run([&]() -> async::Task<void> {
        EXPECT_TRUE(co_await harness.serving());
        (void) co_await harness.command("UPGRADE", "u1");
        auto done = co_await harness.finished("u1");
        EXPECT_EQ(done.phase, "serving") << done.upgrade_error;
        EXPECT_TRUE(harness.launcher.at(0).exited);
        EXPECT_TRUE(harness.launcher.at(0).channel->boss_lost());
        EXPECT_EQ(harness.launcher.at(1).live_peers_at_activation, 0U);
        EXPECT_EQ(done.active_pid, harness.launcher.at(1).pid);
    });
}

TEST(AccessBossTest, ActivationFailureAfterReleaseColdStartsTheKnownGoodBinary) {
    Harness harness;
    harness.launcher.configure = [](std::size_t index, FakeChild &child) { child.fail_activation = index == 1; };
    harness.run([&]() -> async::Task<void> {
        EXPECT_TRUE(co_await harness.serving());
        harness.launcher.binary_path = harness.v2;
        (void) co_await harness.command("UPGRADE", "u1");
        EXPECT_EQ((co_await harness.finished("u1")).upgrade_error, "ACTIVATION_FAILED");
        EXPECT_EQ(harness.launcher.at(0).releases, 1U);
        EXPECT_TRUE(co_await harness.wait_for([&]() {
            return harness.launcher.children.size() == 3 && harness.boss->status().active_state == "serving";
        }));
        auto &recovered = harness.launcher.at(2);
        // The path now names v2, but recovery runs the inode that last served.
        EXPECT_EQ(recovered.binary_inode, stat_of(harness.v1).st_ino);
        EXPECT_FALSE(recovered.background);
        EXPECT_TRUE(harness.launcher.at(1).exited);
        EXPECT_EQ(recovered.serving_peers_at_activation, 0U);
        EXPECT_EQ(harness.boss->status().active_pid, recovered.pid);
    });
}

TEST(AccessBossTest, ActiveCrashDuringUpgradeActivatesTheCandidateWithoutRelease) {
    Harness harness;
    harness.launcher.configure = [](std::size_t index, FakeChild &child) { child.hold_prepare = index == 1; };
    harness.run([&]() -> async::Task<void> {
        EXPECT_TRUE(co_await harness.serving());
        (void) co_await harness.command("UPGRADE", "u1");
        EXPECT_TRUE(co_await harness.preparing());
        harness.launcher.at(0).crashed = true;
        EXPECT_TRUE(co_await harness.wait_for([&]() { return harness.launcher.at(0).exited; }));
        co_await async::sleep(10ms);
        // No competing cold start while the upgrade owns recovery.
        EXPECT_EQ(harness.launcher.children.size(), 2U);
        harness.launcher.at(1).hold_prepare = false;
        auto done = co_await harness.finished("u1");
        EXPECT_EQ(done.phase, "serving") << done.upgrade_error;
        EXPECT_EQ(harness.launcher.at(0).releases, 0U);
        EXPECT_TRUE(harness.launcher.at(1).background);
        EXPECT_EQ(done.active_pid, harness.launcher.at(1).pid);
    });
}

TEST(AccessBossTest, ServingCrashRestartsTheKnownGoodBinaryWithBackoff) {
    Harness harness;
    harness.launcher.configure = [](std::size_t index, FakeChild &child) {
        child.crash_on_start = index == 1 || index == 2;
    };
    harness.run([&]() -> async::Task<void> {
        EXPECT_TRUE(co_await harness.serving());
        harness.launcher.binary_path = harness.v2;
        harness.launcher.at(0).crashed = true;
        EXPECT_TRUE(co_await harness.wait_for([&]() {
            return harness.launcher.children.size() == 4 && harness.boss->status().active_state == "serving";
        }));
        for (std::size_t index = 1; index < 4; ++index) {
            EXPECT_EQ(harness.launcher.at(index).binary_inode, stat_of(harness.v1).st_ino) << index;
        }
        EXPECT_EQ(harness.launcher.at(3).activations, 1U);
        EXPECT_FALSE(harness.launcher.at(3).background);
    });
}

TEST(AccessBossTest, StopTerminatesEveryGenerationAndAbandonsTheUpgrade) {
    Harness harness;
    harness.launcher.configure = [](std::size_t index, FakeChild &child) { child.hold_prepare = index == 1; };
    harness.run([&]() -> async::Task<void> {
        EXPECT_TRUE(co_await harness.serving());
        (void) co_await harness.command("UPGRADE", "u1");
        EXPECT_TRUE(co_await harness.preparing());
        co_await harness.boss->stop();
        for (const auto &child: harness.launcher.children) {
            EXPECT_TRUE(child->terminated && child->exited) << child->pid;
        }
        const auto status = harness.boss->status();
        EXPECT_EQ(status.state, "stopping");
        EXPECT_EQ(status.phase, "failed");
        EXPECT_EQ(status.upgrade_error, "PROCESS_STOPPING");
        EXPECT_EQ(status.active_pid, 0);
        EXPECT_EQ((co_await harness.command("UPGRADE", "u2")).error, "PROCESS_STOPPING");
        EXPECT_EQ(harness.launcher.children.size(), 2U);
    });
}

TEST(AccessBossTest, ControlSocketProtectsLiveEndpointsAndServesTheCli) {
    Directory directory;
    ASSERT_FALSE(directory.path.empty());
    ASSERT_EQ(::chmod(directory.path.c_str(), 0700), 0);
    const std::string path = directory.path + "/boss.sock";
    {
        Directory open;
        ASSERT_EQ(::chmod(open.path.c_str(), 0777), 0);
        Harness refused(5s, open.path + "/boss.sock");
        auto initialized = refused.boss->initialize();
        ASSERT_FALSE(initialized);
        EXPECT_EQ(initialized.error(), common::IoErr::Permission);
    }
    write_file(path, "not a socket", 0600);
    {
        Harness refused(5s, path);
        auto initialized = refused.boss->initialize();
        ASSERT_FALSE(initialized);
        EXPECT_EQ(initialized.error(), common::IoErr::Permission);
        EXPECT_EQ(read_file(path), "not a socket");
    }
    ASSERT_EQ(::unlink(path.c_str()), 0);
    {
        // A crashed boss leaves its socket inode behind; connect proves it stale.
        const int stale = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        ASSERT_GE(stale, 0);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::copy(path.begin(), path.end(), address.sun_path);
        ASSERT_EQ(::bind(stale, reinterpret_cast<sockaddr *>(&address), sizeof(address)), 0);
        ::close(stale);
    }
    struct stat leftover{};
    ASSERT_EQ(::lstat(path.c_str(), &leftover), 0);

    Harness harness(5s, path);
    ASSERT_TRUE(harness.boss->initialize());
    EXPECT_EQ(stat_of(path).st_mode & 0777U, 0600U);
    {
        Harness second(5s, path);
        auto initialized = second.boss->initialize();
        ASSERT_FALSE(initialized);
        EXPECT_EQ(initialized.error(), common::IoErr::AddrInUse);
    }
    harness.run([&]() -> async::Task<void> {
        EXPECT_TRUE(co_await harness.serving());
        AccessControlMessage request;
        request.request_id = "cli";
        request.command = "STATUS";
        auto response = co_await request_access_control(harness.loop, path, std::move(request));
        EXPECT_TRUE(response);
        if (response) {
            EXPECT_EQ(response->request_id, "cli");
            EXPECT_EQ(response->active_state, "serving");
            EXPECT_EQ(response->server_path, harness.v1);
        }
    });
    struct stat removed{};
    EXPECT_NE(::lstat(path.c_str(), &removed), 0);
}

TEST(AccessBossProcessLauncherTest, ValidatesTheBinaryBeforePinningIt) {
    Directory directory;
    ASSERT_FALSE(directory.path.empty());
    const std::string writable = directory.path + "/group-writable";
    const std::string plain = directory.path + "/not-executable";
    write_file(writable, "x", 0775);
    write_file(plain, "x", 0644);
    for (const auto &path: {writable, plain, directory.path}) {
        auto opened = open_access_server_binary(path);
        ASSERT_FALSE(opened) << path;
        EXPECT_EQ(opened.error(), common::IoErr::Permission) << path;
    }
    EXPECT_FALSE(open_access_server_binary(directory.path + "/missing"));
}

TEST(AccessBossProcessLauncherTest, ExecsThePinnedBinaryWithTheChannelOnFdThreeAndACleanSignalMask) {
    Directory directory;
    ASSERT_FALSE(directory.path.empty());
    // /bin/sh stands in for access-server: it runs the "config" argument as a script.
    const std::string script = directory.path + "/child.sh";
    write_file(script,
               "printf '%s|' \"$1\" >&3\n"
               "grep '^SigBlk' /proc/$$/status >&3\n",
               0600);
    AccessBossProcessLauncher launcher("/bin/sh", script);
    auto binary = launcher.open_binary();
    ASSERT_TRUE(binary) << common::io_err_name(binary.error());

    sigset_t blocked;
    sigset_t previous;
    ::sigemptyset(&blocked);
    ::sigaddset(&blocked, SIGTERM);
    ASSERT_EQ(::pthread_sigmask(SIG_BLOCK, &blocked, &previous), 0);
    auto child = launcher.spawn(*binary);
    ASSERT_EQ(::pthread_sigmask(SIG_SETMASK, &previous, nullptr), 0);
    ::close(*binary);
    ASSERT_TRUE(child) << common::io_err_name(child.error());

    std::string output;
    std::array<char, 256> buffer{};
    for (;;) {
        pollfd readable{.fd = child->channel_fd, .events = POLLIN};
        ASSERT_EQ(::poll(&readable, 1, 5000), 1);
        const ssize_t read = ::read(child->channel_fd, buffer.data(), buffer.size());
        if (read <= 0) {
            break;
        }
        output.append(buffer.data(), static_cast<std::size_t>(read));
    }
    ::close(child->channel_fd);
    int status = 0;
    ASSERT_EQ(::waitpid(child->pid, &status, 0), child->pid);
    EXPECT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0) << status;
    EXPECT_EQ(output, "--boss-fd=3|SigBlk:\t0000000000000000\n");
}

TEST(AccessServerBinaryInstallTest, KeepsTheTargetPresentAndRestoresThePreviousInode) {
    Directory directory;
    ASSERT_FALSE(directory.path.empty());
    const std::string target = directory.path + "/access-server";
    const std::string source = directory.path + "/build-output";
    write_file(target, "old", 0755);
    write_file(source, "new", 0644);
    const auto old_inode = stat_of(target).st_ino;

    ASSERT_TRUE(install_access_server_binary(source, target));
    EXPECT_EQ(read_file(target), "new");
    EXPECT_EQ(stat_of(target).st_mode & 0777U, 0755U);
    const std::string backup = access_server_binary_backup_path(target);
    EXPECT_EQ(backup, target + ".prev");
    EXPECT_EQ(stat_of(backup).st_ino, old_inode);
    EXPECT_EQ(read_file(backup), "old");
    for (const auto &entry: std::filesystem::directory_iterator(directory.path)) {
        EXPECT_NE(entry.path().filename().string().front(), '.') << entry.path();
    }

    ASSERT_TRUE(restore_access_server_binary(target));
    EXPECT_EQ(read_file(target), "old");
    EXPECT_EQ(stat_of(target).st_ino, old_inode);
    EXPECT_EQ(read_file(backup), "old");

    EXPECT_FALSE(install_access_server_binary(target, target));
    EXPECT_FALSE(install_access_server_binary(source, "relative/access-server"));
    EXPECT_FALSE(install_access_server_binary(directory.path + "/missing", target));
    EXPECT_EQ(read_file(target), "old");

    const std::string fresh = directory.path + "/fresh";
    ASSERT_TRUE(install_access_server_binary(source, fresh));
    EXPECT_EQ(read_file(fresh), "new");
    struct stat no_backup{};
    EXPECT_NE(::lstat(access_server_binary_backup_path(fresh).c_str(), &no_backup), 0);
    EXPECT_FALSE(restore_access_server_binary(fresh));
}

} // namespace
} // namespace fiber::access_server
