// Regression: a generation used to die ~2s after its last channel exchange on
// an idle boss channel (deployed repro, 111 cold start). Mirrors the production
// shape: a STATUS exchange, then pure idle longer than the frame timeout, then
// one more exchange to prove the channel survived the idle gap.
#include "runtime/AccessBossChannel.h"

#include <gtest/gtest.h>

#include <array>
#include <optional>
#include <string>
#include <utility>

#include <sys/socket.h>
#include <unistd.h>

#include <fiber/async/Sleep.h>
#include <fiber/async/Spawn.h>
#include <fiber/event/EventLoop.h>

namespace fiber::access_server {
namespace {

struct IdleRuntime {
    AccessServerRuntimeState state = AccessServerRuntimeState::Running;

    AccessBossChildActions actions() noexcept {
        return {
                .context = this,
                .state = [](void *p) noexcept { return static_cast<IdleRuntime *>(p)->state; },
                .ready = [](void *) noexcept { return true; },
                .registration = [](void *) noexcept -> std::string_view { return "registered"; },
                .activate = &activate,
                .release = &release,
        };
    }
    static async::Task<common::IoResult<void>> activate(void *, bool) noexcept { co_return common::IoResult<void>{}; }
    static async::Task<common::IoResult<void>> release(void *) noexcept { co_return common::IoResult<void>{}; }
};

unsigned boss_lost_calls = 0;

TEST(AccessBossIdleReproTest, ChannelSurvivesTwoSecondIdleAfterExchanges) {
    event::EventLoop loop;
    IdleRuntime runtime;
    std::array<int, 2> pair{-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, pair.data()), 0);
    net::UnixStream boss(loop, pair[0]);
    std::optional<AccessBossChannel> channel;
    AccessBossChannelOptions options;
    options.compatibility = "repro";
    options.boss_lost = []() noexcept { ++boss_lost_calls; };
    channel.emplace(loop, pair[1], runtime.actions(), std::move(options));

    // Simulate the boss polling STATUS every 50ms during activation.
    auto status = [&]() -> async::Task<common::IoResult<AccessControlMessage>> {
        AccessControlMessage request;
        request.request_id = "boss";
        request.command = "STATUS";
        co_return co_await exchange_access_control(boss, std::move(request));
    };
    auto drive = [&](auto body) {
        async::spawn(loop, [&]() -> async::DetachedTask {
            channel->start();
            co_await body();
            co_await channel->quiesce();
            co_await channel->shutdown();
            boss.close();
            loop.stop();
        });
        loop.run();
    };

    bool ok = true;
    drive([&]() -> async::Task<void> {
        {
            auto reply = co_await status();
            if (!reply || reply->state != "serving") {
                ok = false;
                co_return;
            }
        }
        // Pure idle: nobody talks to the channel for longer than the 2s frame
        // deadline. A healthy channel keeps serving; the 111 deployment breaks
        // here and the generation exits with "boss channel lost".
        co_await async::sleep(std::chrono::milliseconds(2600));
        auto reply = co_await status();
        ok = reply.has_value() && reply->state == "serving";
    });
    EXPECT_TRUE(ok) << "channel broke during the idle window";
    EXPECT_EQ(boss_lost_calls, 0u);
    ::close(pair[0]);
    ::close(pair[1]);
}

} // namespace
} // namespace fiber::access_server
