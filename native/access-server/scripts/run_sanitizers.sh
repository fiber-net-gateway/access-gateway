#!/usr/bin/env bash

set -euo pipefail

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repository_root=$(CDPATH= cd -- "${script_dir}/../../.." && pwd)
mode=${1:-all}
build_jobs=${NATIVE_BUILD_JOBS:-2}
repeat_override=${ACCESS_SERVER_SANITIZER_REPEAT:-}

case "${mode}" in
    address|thread|all) ;;
    *)
        echo "usage: $0 [address|thread|all]" >&2
        exit 2
        ;;
esac

case "${build_jobs}" in
    ''|*[!0-9]*|0)
        echo "NATIVE_BUILD_JOBS must be a positive integer" >&2
        exit 2
        ;;
esac

if [[ -n "${repeat_override}" ]]; then
    case "${repeat_override}" in
        *[!0-9]*|0)
            echo "ACCESS_SERVER_SANITIZER_REPEAT must be a positive integer" >&2
            exit 2
            ;;
    esac
fi

address_filter='AccessRuntimeCoordinatorTest.*:AccessDnsServiceTest.*:AccessConfigWatcherTest.*:NacosStatusMonitorTest.*:StartupStages/*:RouteSnapshotComponentsTest.PublisherSupportsConcurrentWorkerPinsAndHotUpdates:TlsCertificateStoreTest.*:ProxyUpstreamConnectionTest.*:ProxyExecutorTest.*:ClientMetadataTest.*:AccessUpstreamCircuitTest.*:AccessServiceStateTest.*'
thread_filter='AccessConfigMetricsTest.ReadersNeverObserveTornReadinessSamples:AccessDiscoveryMetricsTest.ReadersNeverObserveTornStatusAggregates:AccessUpstreamCircuitTest.*:AccessServiceStateTest.*:AccessRuntimeCoordinatorTest.*:AccessDnsServiceTest.ShutdownDoesNotBlockTheCallingEventLoop:AccessConfigWatcherTest.KeepsOwnerLoopResponsiveAndCoalescesQueuedGenerations:AccessConfigWatcherTest.ClosedProjectSubscriptionCancelsStaleCompileAndRecoversAfterReconcile:NacosStatusMonitorTest.*:RouteSnapshotComponentsTest.PublisherSupportsConcurrentWorkerPinsAndHotUpdates:TlsCertificateStoreTest.KeepsSelectedIdentityAliveWhenRotationInterleavesWithHandshake:StartupStages/*'

run_mode() {
    local sanitizer=$1
    local filter=$2
    local default_repeat=$3
    local build_dir="${repository_root}/native/build-${sanitizer}"
    local repeat=${repeat_override:-${default_repeat}}

    cmake -S "${repository_root}/native" -B "${build_dir}" -G Ninja \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DFIBER_BUILD_TESTS=ON \
        -DFIBER_ENABLE_LTO=OFF \
        -DACCESS_SERVER_SANITIZER="${sanitizer}"
    cmake --build "${build_dir}" --target fiber_access_server_tests --parallel "${build_jobs}"

    local test_binary="${build_dir}/access-server/fiber_access_server_tests"
    if [[ "${sanitizer}" == address ]]; then
        local -a options=(
            'ASAN_OPTIONS=detect_leaks=1:check_initialization_order=1:strict_init_order=1:halt_on_error=1'
            'UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1'
        )
        if [[ -x /usr/lib/llvm-22/bin/llvm-symbolizer ]]; then
            options+=('ASAN_SYMBOLIZER_PATH=/usr/lib/llvm-22/bin/llvm-symbolizer')
        fi
        timeout 20m env "${options[@]}" "${test_binary}" \
            --gtest_filter="${filter}" --gtest_repeat="${repeat}" --gtest_break_on_failure
    else
        local -a options=(
            'TSAN_OPTIONS=halt_on_error=1:history_size=7:second_deadlock_stack=1'
        )
        if [[ -x /usr/lib/llvm-22/bin/llvm-symbolizer ]]; then
            options+=('TSAN_SYMBOLIZER_PATH=/usr/lib/llvm-22/bin/llvm-symbolizer')
        fi
        timeout 20m env "${options[@]}" "${test_binary}" \
            --gtest_filter="${filter}" --gtest_repeat="${repeat}" --gtest_break_on_failure
    fi
}

if [[ "${mode}" == address || "${mode}" == all ]]; then
    run_mode address "${address_filter}" 5
fi
if [[ "${mode}" == thread || "${mode}" == all ]]; then
    run_mode thread "${thread_filter}" 10
fi
