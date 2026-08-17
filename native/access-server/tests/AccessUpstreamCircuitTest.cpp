#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "routing/AccessUpstreamCircuit.h"

namespace {

using namespace std::chrono_literals;

TEST(AccessUpstreamCircuitTest, SharesFailureThresholdAndAllowsOneRecoveryProbe) {
    fiber::access_server::AccessUpstreamCircuit circuit(1, 10s);
    const auto start = fiber::access_server::AccessUpstreamCircuit::TimePoint{};

    auto initial = circuit.acquire(start);
    ASSERT_TRUE(initial);
    circuit.report(*initial, false, start);
    EXPECT_EQ(circuit.failure_count(), 1U);
    EXPECT_FALSE(circuit.available(start + 9s));
    EXPECT_FALSE(circuit.acquire(start + 9s));
    EXPECT_FALSE(circuit.available(start + 10s));
    EXPECT_FALSE(circuit.acquire(start + 10s));

    auto recovery = circuit.acquire(start + 11s);
    ASSERT_TRUE(recovery);
    EXPECT_NE(recovery->recovery_epoch, 0U);
    EXPECT_FALSE(circuit.acquire(start + 11s));
    circuit.report(*recovery, true, start + 11s);
    EXPECT_EQ(circuit.failure_count(), 0U);
    EXPECT_TRUE(circuit.acquire(start + 11s));
}

TEST(AccessUpstreamCircuitTest, StaleRecoveryCannotEraseANewerFailure) {
    fiber::access_server::AccessUpstreamCircuit circuit(1, 10s);
    const auto start = fiber::access_server::AccessUpstreamCircuit::TimePoint{};

    circuit.report({}, false, start);
    auto recovery = circuit.acquire(start + 11s);
    ASSERT_TRUE(recovery);
    circuit.report({}, false, start + 12s);
    circuit.report(*recovery, true, start + 13s);

    EXPECT_EQ(circuit.failure_count(), 2U);
    EXPECT_FALSE(circuit.available(start + 21s));
    EXPECT_TRUE(circuit.available(start + 23s));
}

TEST(AccessUpstreamCircuitTest, SuccessfulCheckResetsFailuresBelowThreshold) {
    fiber::access_server::AccessUpstreamCircuit circuit(3, 10s);
    const auto start = fiber::access_server::AccessUpstreamCircuit::TimePoint{};

    circuit.report({}, false, start);
    EXPECT_TRUE(circuit.available(start + 1s));
    auto ordinary = circuit.acquire(start + 1s);
    ASSERT_TRUE(ordinary);
    EXPECT_EQ(ordinary->recovery_epoch, 0U);
    circuit.report(*ordinary, true, start + 1s);
    EXPECT_EQ(circuit.failure_count(), 1U);

    auto check = circuit.acquire(start + 11s);
    ASSERT_TRUE(check);
    EXPECT_NE(check->recovery_epoch, 0U);
    circuit.report(*check, true, start + 11s);
    EXPECT_EQ(circuit.failure_count(), 0U);
}

TEST(AccessUpstreamCircuitTest, AdmitsOnlyOneConcurrentRecoveryProbe) {
    fiber::access_server::AccessUpstreamCircuit circuit(1, 10s);
    const auto start = fiber::access_server::AccessUpstreamCircuit::TimePoint{};
    circuit.report({}, false, start);

    std::atomic<std::size_t> admitted{0};
    {
        std::vector<std::jthread> threads;
        threads.reserve(16);
        for (std::size_t index = 0; index < 16; ++index) {
            threads.emplace_back([&]() {
                if (circuit.acquire(start + 11s)) {
                    admitted.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
    }
    EXPECT_EQ(admitted.load(std::memory_order_relaxed), 1U);
}

} // namespace
