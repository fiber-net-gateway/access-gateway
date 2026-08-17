#!/usr/bin/env bash

set -euo pipefail

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repository_root=$(CDPATH= cd -- "${script_dir}/../../.." && pwd)
build_dir=${ACCESS_SERVER_BENCHMARK_BUILD_DIR:-${repository_root}/native/build-benchmarks}
result_root=${ACCESS_SERVER_BENCHMARK_RESULT_DIR:-${build_dir}/results}
build_jobs=${NATIVE_BUILD_JOBS:-2}
max_workers=${ACCESS_SERVER_BENCHMARK_MAX_WORKERS:-4}
quick=${ACCESS_SERVER_BENCHMARK_QUICK:-0}
cpu_set=${ACCESS_SERVER_BENCHMARK_CPUSET:-}

for required_command in cmake git sha256sum sort timeout xargs; do
    if ! command -v "${required_command}" >/dev/null 2>&1; then
        echo "required command is unavailable: ${required_command}" >&2
        exit 2
    fi
done
if [[ ! -x /usr/bin/time ]]; then
    echo "required command is unavailable: /usr/bin/time" >&2
    exit 2
fi

validate_positive() {
    local name=$1
    local value=$2
    case "${value}" in
        ''|*[!0-9]*|0)
            echo "${name} must be a positive integer" >&2
            exit 2
            ;;
    esac
}

validate_positive NATIVE_BUILD_JOBS "${build_jobs}"
validate_positive ACCESS_SERVER_BENCHMARK_MAX_WORKERS "${max_workers}"
case "${quick}" in
    0|1) ;;
    *)
        echo "ACCESS_SERVER_BENCHMARK_QUICK must be 0 or 1" >&2
        exit 2
        ;;
esac

if [[ -n "${cpu_set}" ]] && ! command -v taskset >/dev/null 2>&1; then
    echo "ACCESS_SERVER_BENCHMARK_CPUSET requires taskset" >&2
    exit 2
fi
if [[ -n "${cpu_set}" ]] && ! taskset -c "${cpu_set}" true; then
    echo "ACCESS_SERVER_BENCHMARK_CPUSET is not permitted: ${cpu_set}" >&2
    exit 2
fi

if [[ "${quick}" == 1 ]]; then
    selection_operations=10000
    template_operations=1000
    lookup_operations=10000
    gray_operations=10000
    tls_operations=10000
    tls_rotations=7
    proxy_requests=100
    websocket_sessions=21
    log_operations=10000
    cat_operations=500
    dns_queries=21
    connects=21
else
    selection_operations=100000
    template_operations=10000
    lookup_operations=100000
    gray_operations=100000
    tls_operations=100000
    tls_rotations=21
    proxy_requests=1000
    websocket_sessions=101
    log_operations=100000
    cat_operations=1000
    dns_queries=101
    connects=101
fi

cmake -S "${repository_root}/native" -B "${build_dir}" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DFIBER_BUILD_TESTS=OFF \
    -DFIBER_ENABLE_LTO=ON \
    -DACCESS_SERVER_BUILD_BENCHMARKS=ON

targets=(
    fiber_access_service_selection_benchmark
    fiber_access_template_header_benchmark
    fiber_access_route_publication_benchmark
    fiber_access_host_matcher_benchmark
    fiber_access_route_lookup_benchmark
    fiber_access_gray_match_benchmark
    fiber_access_tls_identity_benchmark
    fiber_access_proxy_loopback_benchmark
    fiber_access_observability_benchmark
    fiber_access_network_integration_benchmark
)
cmake --build "${build_dir}" --target "${targets[@]}" --parallel "${build_jobs}"

timestamp=$(date -u +%Y%m%dT%H%M%SZ)
result_dir="${result_root}/${timestamp}"
mkdir -p "${result_dir}"

revision=$(git -C "${repository_root}" rev-parse HEAD)
dirty=false
if [[ -n "$(git -C "${repository_root}" status --porcelain --untracked-files=normal)" ]]; then
    dirty=true
fi
compiler_path=$(sed -n 's/^CMAKE_CXX_COMPILER:[^=]*=//p' "${build_dir}/CMakeCache.txt" | head -n 1)
if [[ -z "${compiler_path}" ]] || [[ ! -x "${compiler_path}" ]]; then
    echo "unable to resolve CMAKE_CXX_COMPILER from ${build_dir}/CMakeCache.txt" >&2
    exit 2
fi
benchmark_source_sha256=$(
    (
        cd "${repository_root}/native/access-server"
        {
            printf '%s\0' CMakeLists.txt scripts/run_benchmarks.sh
            find benchmarks -maxdepth 1 -type f \( -name '*.cpp' -o -name '*.h' \) -print0
        } | sort -z | xargs -0 sha256sum
    ) | sha256sum | awk '{print $1}'
)
native_source_sha256=$(
    (
        cd "${repository_root}/native/access-server"
        {
            printf '%s\0' ../CMakeLists.txt CMakeLists.txt scripts/run_benchmarks.sh
            find src benchmarks -type f \( -name '*.cpp' -o -name '*.h' \) -print0
        } | sort -z | xargs -0 sha256sum
    ) | sha256sum | awk '{print $1}'
)
fiber_revision=$(git -C "${repository_root}/third_party/fiber-gateway-cpp" rev-parse HEAD)

{
    echo "timestamp_utc=${timestamp}"
    echo "revision=${revision}"
    echo "dirty=${dirty}"
    echo "benchmark_source_sha256=${benchmark_source_sha256}"
    echo "native_source_sha256=${native_source_sha256}"
    echo "fiber_revision=${fiber_revision}"
    echo "build_type=Release"
    echo "lto=ON"
    echo "quick=${quick}"
    echo "max_workers=${max_workers}"
    echo "cpu_set=${cpu_set:-unrestricted}"
    echo "kernel=$(uname -srmo)"
    echo "compiler=$(${compiler_path} --version | head -n 1)"
    echo "compiler_path=${compiler_path}"
    if command -v lscpu >/dev/null 2>&1; then
        echo "cpu_model=$(lscpu | awk -F: '/^Model name:/ {sub(/^[[:space:]]+/, "", $2); print $2; exit}')"
        echo "logical_cpus=$(lscpu | awk -F: '/^CPU\(s\):/ {sub(/^[[:space:]]+/, "", $2); print $2; exit}')"
    fi
} >"${result_dir}/metadata.txt"

run_benchmark() {
    local target=$1
    shift
    local binary="${build_dir}/access-server/${target}"
    local stdout_file="${result_dir}/${target}.csv"
    local stderr_file="${result_dir}/${target}.stderr"
    local resource_file="${result_dir}/${target}.resources"
    local -a command=("${binary}" "$@")
    if [[ -n "${cpu_set}" ]]; then
        command=(taskset -c "${cpu_set}" "${command[@]}")
    fi
    echo "running ${target}" >&2
    timeout 20m /usr/bin/time -f \
        'user_seconds=%U\nsystem_seconds=%S\ncpu_percent=%P\nmax_rss_kb=%M\nelapsed_seconds=%e' \
        -o "${resource_file}" "${command[@]}" > >(tee "${stdout_file}") 2> >(tee "${stderr_file}" >&2)
}

run_benchmark fiber_access_service_selection_benchmark "${selection_operations}" "${max_workers}"
run_benchmark fiber_access_template_header_benchmark "${template_operations}"
run_benchmark fiber_access_route_publication_benchmark
run_benchmark fiber_access_host_matcher_benchmark
run_benchmark fiber_access_route_lookup_benchmark "${lookup_operations}" "${max_workers}"
run_benchmark fiber_access_gray_match_benchmark "${gray_operations}" "${max_workers}"
run_benchmark fiber_access_tls_identity_benchmark "${tls_operations}" "${tls_rotations}"
run_benchmark fiber_access_proxy_loopback_benchmark "${proxy_requests}" "${websocket_sessions}"
run_benchmark fiber_access_observability_benchmark "${log_operations}" "${cat_operations}"
run_benchmark fiber_access_network_integration_benchmark "${dns_queries}" "${connects}"

(
    cd "${result_dir}"
    sha256sum -- *.csv *.resources *.stderr metadata.txt >manifest.sha256
)
echo "benchmark results: ${result_dir}" >&2
