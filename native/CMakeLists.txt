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
option(ACCESS_SERVER_BUILD_RUNTIME "Build the online access-server runtime" ON)
option(ACCESS_SERVER_BUILD_BENCHMARKS "Build access-server microbenchmarks" OFF)
option(ACCESS_SERVER_BUILD_EXTERNAL_INTEROP_TESTS
    "Build explicit Docker-backed access-server interoperability tests" OFF)
set(ACCESS_SERVER_SANITIZER "none" CACHE STRING
    "Native sanitizer instrumentation: none, address (ASAN+UBSAN), or thread (TSAN)")
set_property(CACHE ACCESS_SERVER_SANITIZER PROPERTY STRINGS none address thread)

if (NOT ACCESS_SERVER_SANITIZER MATCHES "^(none|address|thread)$")
    message(FATAL_ERROR
        "ACCESS_SERVER_SANITIZER must be one of: none, address, thread")
endif()

if (NOT ACCESS_SERVER_SANITIZER STREQUAL "none")
    if (NOT CMAKE_CXX_COMPILER_ID MATCHES "^(Clang|GNU)$")
        message(FATAL_ERROR
            "ACCESS_SERVER_SANITIZER requires Clang or GCC")
    endif()

    # Sanitizer coverage must include the pinned Fiber static libraries as well
    # as access-server. IPO obscures diagnostics and is not part of this gate.
    set(FIBER_ENABLE_LTO OFF CACHE BOOL "Enable LTO/IPO for fiber targets" FORCE)
    if (ACCESS_SERVER_SANITIZER STREQUAL "address")
        set(ACCESS_SERVER_SANITIZER_FLAGS
            -fsanitize=address,undefined
            -fno-sanitize-recover=all
            -fno-omit-frame-pointer)
    else()
        set(ACCESS_SERVER_SANITIZER_FLAGS
            -fsanitize=thread
            -fno-sanitize-recover=all
            -fno-omit-frame-pointer)
    endif()
    add_compile_options(${ACCESS_SERVER_SANITIZER_FLAGS})
    add_link_options(${ACCESS_SERVER_SANITIZER_FLAGS})
endif()

# Keep all generated dependency state in the build tree, never in the submodule.
set(FIBER_DEPS_DIR "${CMAKE_BINARY_DIR}/_deps" CACHE PATH "Fiber dependency directory" FORCE)
set(FETCHCONTENT_BASE_DIR "${FIBER_DEPS_DIR}" CACHE PATH "FetchContent base directory" FORCE)

# The application is repository-owned below; only reusable upstream components are enabled.
set(FIBER_BUILD_APPS OFF CACHE BOOL "" FORCE)
set(FIBER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FIBER_BUILD_NACOS ${ACCESS_SERVER_BUILD_RUNTIME} CACHE BOOL "" FORCE)
set(FIBER_BUILD_CAT ${ACCESS_SERVER_BUILD_RUNTIME} CACHE BOOL "" FORCE)
set(FIBER_BUILD_PROMETHEUS ${ACCESS_SERVER_BUILD_RUNTIME} CACHE BOOL "" FORCE)
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
