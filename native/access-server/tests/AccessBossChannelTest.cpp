#include "runtime/AccessBossChannel.h"
#include "runtime/AccessServerConfig.h"

#include <gtest/gtest.h>

#include <array>
#include <string>
#include <utility>

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <fiber/async/Spawn.h>
#include <fiber/async/Yield.h>

namespace fiber::access_server {
namespace {

struct Runtime {
    AccessServerRuntimeState state = AccessServerRuntimeState::Prepared;
    bool ready = true;
    bool fail_activation = false;
    bool fail_release = false;
    bool background = false;
    unsigned activations = 0;
    unsigned releases = 0;

    AccessBossChildActions actions() noexcept {
        return {
                .context = this,
                .state = [](void *p) noexcept { return static_cast<Runtime *>(p)->state; },
                .ready = [](void *p) noexcept { return static_cast<Runtime *>(p)->ready; },
                .registration = [](void *) noexcept -> std::string_view { return "pending"; },
                .activate = &activate,
                .release = &release,
        };
    }
    static async::Task<common::IoResult<void>> activate(void *context, bool background) noexcept {
        auto &self = *static_cast<Runtime *>(context);
        ++self.activations;
        self.background = background;
        self.state = AccessServerRuntimeState::Activating;
        co_await async::yield();
        if (self.fail_activation) {
            self.state = AccessServerRuntimeState::Stopped;
            co_return std::unexpected(common::IoErr::AddrInUse);
        }
        self.state = AccessServerRuntimeState::Running;
        co_return common::IoResult<void>{};
    }
    static async::Task<common::IoResult<void>> release(void *context) noexcept {
        auto &self = *static_cast<Runtime *>(context);
        ++self.releases;
        if (self.fail_release) {
            co_return std::unexpected(common::IoErr::Busy);
        }
        self.state = AccessServerRuntimeState::Draining;
        co_return common::IoResult<void>{};
    }
};

unsigned boss_lost_calls = 0;
void count_boss_lost() noexcept { ++boss_lost_calls; }

// The test plays the boss on one end of a socketpair; the channel serves the other.
struct Harness {
    event::EventLoop loop;
    Runtime runtime;
    std::array<int, 2> pair{-1, -1};
    std::unique_ptr<net::UnixStream> boss;
    std::unique_ptr<AccessBossChannel> channel;
    unsigned sequence = 0;

    Harness() {
        boss_lost_calls = 0;
        EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, pair.data()), 0);
        boss = std::make_unique<net::UnixStream>(loop, pair[0]);
        channel = std::make_unique<AccessBossChannel>(
                loop, pair[1], runtime.actions(),
                AccessBossChannelOptions{.compatibility = "digest", .http3 = true, .boss_lost = &count_boss_lost});
    }

    template<class Function>
    void run(Function function) {
        async::spawn(loop, [&]() -> async::DetachedTask {
            channel->start();
            co_await function();
            co_await channel->quiesce();
            co_await channel->shutdown();
            boss->close();
            loop.stop();
        });
        loop.run();
    }

    async::Task<common::IoResult<AccessControlMessage>> send(std::string command, bool background = false) {
        AccessControlMessage request;
        request.request_id = "r" + std::to_string(++sequence);
        request.command = std::move(command);
        request.background = background;
        co_return co_await exchange_access_control(*boss, std::move(request));
    }

    async::Task<AccessControlMessage> wait_phase(std::string_view phase) {
        for (unsigned i = 0; i < 1000; ++i) {
            auto status = co_await send("STATUS");
            if (status && status->phase == phase) {
                co_return *status;
            }
            co_await async::yield();
        }
        ADD_FAILURE() << "phase " << phase << " not reached";
        co_return AccessControlMessage{};
    }

    // Completes once the channel asked the process to stop.
    async::Task<bool> exit_requested() {
        bool done = false;
        async::spawn(loop, [&]() -> async::DetachedTask {
            co_await channel->wait_exit();
            done = true;
        });
        for (unsigned i = 0; i < 1000 && !done; ++i) {
            co_await async::yield();
        }
        co_return done;
    }
};

TEST(AccessBossChannelTest, StatusReportsReadinessSettingsAndPid) {
    Harness harness;
    harness.run([&]() -> async::Task<void> {
        auto status = co_await harness.send("STATUS");
        EXPECT_TRUE(status);
        if (!status) {
            co_return;
        }
        EXPECT_EQ(status->state, "prepared");
        EXPECT_TRUE(status->ready);
        EXPECT_EQ(status->phase, "idle");
        EXPECT_EQ(status->registration, "pending");
        EXPECT_EQ(status->compatibility, "digest");
        EXPECT_TRUE(status->http3);
        EXPECT_TRUE(status->fixed_ports);
        EXPECT_EQ(status->pid, ::getpid());
        EXPECT_TRUE(status->error.empty());

        harness.runtime.ready = false;
        status = co_await harness.send("STATUS");
        EXPECT_TRUE(status && !status->ready);
        status = co_await harness.send("UNKNOWN");
        EXPECT_TRUE(status && status->error == "UNKNOWN_COMMAND");
    });
}

TEST(AccessBossChannelTest, ActivatesOncePreparedAndReportsRegistrationMode) {
    Harness harness;
    harness.runtime.state = AccessServerRuntimeState::Starting;
    harness.run([&]() -> async::Task<void> {
        auto early = co_await harness.send("ACTIVATE", true);
        EXPECT_TRUE(early && early->error == "NOT_PREPARED");
        EXPECT_EQ(harness.runtime.activations, 0U);

        harness.runtime.state = AccessServerRuntimeState::Prepared;
        auto accepted = co_await harness.send("ACTIVATE", true);
        EXPECT_TRUE(accepted && accepted->error.empty() && accepted->phase == "activating");
        auto serving = co_await harness.wait_phase("serving");
        EXPECT_EQ(serving.state, "serving");
        EXPECT_EQ(harness.runtime.activations, 1U);
        EXPECT_TRUE(harness.runtime.background);

        auto again = co_await harness.send("ACTIVATE");
        EXPECT_TRUE(again && again->error == "NOT_PREPARED");
        EXPECT_EQ(harness.runtime.activations, 1U);
        EXPECT_FALSE(harness.channel->failed());
    });
}

TEST(AccessBossChannelTest, FailedActivationRequestsAFailedExit) {
    Harness harness;
    harness.runtime.fail_activation = true;
    harness.run([&]() -> async::Task<void> {
        auto accepted = co_await harness.send("ACTIVATE");
        EXPECT_TRUE(accepted && accepted->error.empty());
        auto failed = co_await harness.wait_phase("failed");
        EXPECT_EQ(failed.state, "stopped");
        EXPECT_FALSE(harness.runtime.background);
        EXPECT_TRUE(co_await harness.exit_requested());
        EXPECT_TRUE(harness.channel->failed());
        EXPECT_FALSE(harness.channel->boss_lost());
    });
}

TEST(AccessBossChannelTest, ReleaseRequiresServingRepliesBeforeExitAndIsIdempotent) {
    Harness harness;
    harness.run([&]() -> async::Task<void> {
        auto refused = co_await harness.send("RELEASE");
        EXPECT_TRUE(refused && refused->error == "NOT_SERVING");
        EXPECT_EQ(harness.runtime.releases, 0U);

        harness.runtime.state = AccessServerRuntimeState::Running;
        auto released = co_await harness.send("RELEASE");
        EXPECT_TRUE(released && released->error.empty() && released->phase == "released");
        EXPECT_TRUE(released && released->state == "draining");
        EXPECT_TRUE(co_await harness.exit_requested());

        // Draining generations stay queryable and a retried RELEASE is not repeated.
        auto retried = co_await harness.send("RELEASE");
        EXPECT_TRUE(retried && retried->error.empty() && retried->phase == "released");
        EXPECT_EQ(harness.runtime.releases, 1U);
        EXPECT_FALSE(harness.channel->failed());
    });
}

TEST(AccessBossChannelTest, RefusedReleaseKeepsServing) {
    Harness harness;
    harness.runtime.state = AccessServerRuntimeState::Running;
    harness.runtime.fail_release = true;
    harness.run([&]() -> async::Task<void> {
        auto refused = co_await harness.send("RELEASE");
        EXPECT_TRUE(refused && refused->error == "RELEASE_FAILED" && refused->state == "serving");
        EXPECT_FALSE(co_await harness.exit_requested());
    });
}

TEST(AccessBossChannelTest, BossEofStopsWithTheBoundedCallbackOnce) {
    Harness harness;
    harness.run([&]() -> async::Task<void> {
        harness.boss->close();
        EXPECT_TRUE(co_await harness.exit_requested());
        EXPECT_TRUE(harness.channel->boss_lost());
        EXPECT_EQ(boss_lost_calls, 1U);
    });
    EXPECT_EQ(boss_lost_calls, 1U);
}

TEST(AccessBossChannelTest, LocalShutdownIsNotBossLossAndQuiesceRejectsMutations) {
    Harness harness;
    harness.run([&]() -> async::Task<void> {
        co_await harness.channel->quiesce();
        auto refused = co_await harness.send("ACTIVATE");
        EXPECT_TRUE(refused && refused->error == "PROCESS_STOPPING" && !refused->ready);
        EXPECT_EQ(harness.runtime.activations, 0U);
    });
    EXPECT_FALSE(harness.channel->boss_lost());
    EXPECT_EQ(boss_lost_calls, 0U);
}

TEST(AccessBossChannelTest, LifetimeBindingRequiresTheParentAsPeer) {
    // A socketpair created by this process reports this process, not the parent.
    std::array<int, 2> pair{-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair.data()), 0);
    auto bound = bind_access_boss_lifetime(pair[1]);
    ASSERT_FALSE(bound);
    EXPECT_EQ(bound.error(), common::IoErr::Permission);
    ::close(pair[0]);
    ::close(pair[1]);

    std::array<int, 2> pipe{-1, -1};
    ASSERT_EQ(::pipe2(pipe.data(), O_CLOEXEC), 0);
    bound = bind_access_boss_lifetime(pipe[0]);
    ASSERT_FALSE(bound);
    EXPECT_EQ(bound.error(), common::IoErr::Invalid);
    ::close(pipe[0]);
    ::close(pipe[1]);
}

TEST(AccessBossChannelTest, CompatibilityDigestTracksSharedSettings) {
    const auto load = [](std::string_view port, std::string_view extra = {}) {
        auto config = AccessServerConfig::load_from_string("NACOS_SERVER_ADDRESSES=127.0.0.1\n"
                                                           "ACCESS_SERVER_TLS_ENABLED=false\n"
                                                           "ACCESS_SERVER_HTTP3_ENABLED=false\n"
                                                           "ACCESS_SERVER_PLAIN_LISTEN_PORT=" +
                                                           std::string(port) + "\n" + std::string(extra));
        EXPECT_TRUE(config) << (config ? std::string() : config.error().detail);
        return config;
    };
    auto base = load("8000");
    auto same = load("8000", "ACCESS_SERVER_ACCESS_LOG_DIR=/var/log/other\n");
    auto moved = load("8100");
    ASSERT_TRUE(base && same && moved);
    const auto options = access_boss_channel_options(*base);
    EXPECT_EQ(options.compatibility.size(), 64U);
    EXPECT_TRUE(options.fixed_ports);
    EXPECT_FALSE(options.http3);
    // The access-log directory is not part of the contract: generations may share it.
    EXPECT_EQ(access_boss_channel_options(*same).compatibility, options.compatibility);
    EXPECT_NE(access_boss_channel_options(*moved).compatibility, options.compatibility);
}

} // namespace
} // namespace fiber::access_server
