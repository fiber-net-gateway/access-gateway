cmake_minimum_required(VERSION 4.1)

set(ACCESS_GATEWAY_FIBER_SOURCE_DIR
    "${CMAKE_CURRENT_LIST_DIR}/../third_party/fiber-gateway-cpp")
if (NOT EXISTS "${ACCESS_GATEWAY_FIBER_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "fiber-gateway-cpp submodule is not initialized. Run: "
        "git submodule update --init --recursive")
endif()

# Reuse Fiber's compiler selection before the top-level project() enables C/C++.
include("${ACCESS_GATEWAY_FIBER_SOURCE_DIR}/cmake/SelectToolchain.cmake")

project(access_gateway_native LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(FIBER_BUILD_TESTS "Build Fiber and access-server tests" ON)
option(FIBER_FETCH_DEPS "Allow CMake to download third-party dependencies" ON)
option(ACCESS_SERVER_BUILD_BENCHMARKS "Build access-server microbenchmarks" OFF)

# Keep all generated dependency state in the build tree, never in the submodule.
set(FIBER_DEPS_DIR "${CMAKE_BINARY_DIR}/_deps" CACHE PATH "Fiber dependency directory" FORCE)
set(FETCHCONTENT_BASE_DIR "${FIBER_DEPS_DIR}" CACHE PATH "FetchContent base directory" FORCE)

# The application is repository-owned below; only reusable upstream components are enabled.
set(FIBER_BUILD_APPS OFF CACHE BOOL "" FORCE)
set(FIBER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FIBER_BUILD_NACOS ON CACHE BOOL "" FORCE)
set(FIBER_BUILD_CAT ON CACHE BOOL "" FORCE)
set(FIBER_BUILD_PROMETHEUS ON CACHE BOOL "" FORCE)
set(FIBER_BUILD_CAT_DEMO OFF CACHE BOOL "" FORCE)
set(FIBER_BUILD_PROMETHEUS_BENCHMARK OFF CACHE BOOL "" FORCE)

if (FIBER_BUILD_TESTS)
    enable_testing()
endif()

add_subdirectory("${ACCESS_GATEWAY_FIBER_SOURCE_DIR}"
    "${CMAKE_BINARY_DIR}/fiber-gateway-cpp")

# Fiber pins and populates zlib but deliberately leaves it out of its target
# graph until an in-tree consumer needs gzip. Reuse that exact source checkout
# for the repository-owned access-server instead of resolving a system or
# second downloaded copy.
set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SKIP_INSTALL_ALL ON CACHE BOOL "" FORCE)
if (NOT TARGET zlibstatic)
    add_subdirectory("${FIBER_ZLIB_SOURCE_DIR}"
        "${CMAKE_BINARY_DIR}/zlib-runtime" EXCLUDE_FROM_ALL)
endif()

# Fiber detects IPO inside its subdirectory. Promote the target result into this directory so the
# repository-owned application and test targets use compatible compile and link settings.
get_target_property(ACCESS_GATEWAY_FIBER_IPO fiber_lib INTERPROCEDURAL_OPTIMIZATION)
if (ACCESS_GATEWAY_FIBER_IPO)
    set(FIBER_IPO_SUPPORTED TRUE)
else()
    set(FIBER_IPO_SUPPORTED FALSE)
endif()

add_subdirectory(access-server)
