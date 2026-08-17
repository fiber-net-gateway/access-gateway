#include "BenchmarkSupport.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <future>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

#include <fiber/async/Spawn.h>
#include <fiber/async/Yield.h>
#include <fiber/common/IoError.h>
#include <fiber/dns/DnsClient.h>
#include <fiber/dns/DnsProtocol.h>
#include <fiber/event/EventLoopGroup.h>
#include <fiber/net/IpAddress.h>
#include <fiber/net/SocketAddress.h>
#include <fiber/net/TcpConnector.h>
#include <fiber/net/TcpListener.h>
#include <fiber/net/UdpSocket.h>

namespace {

using namespace std::chrono_literals;

using fiber::access_server::benchmark::Distribution;

constexpr std::uint64_t kDefaultDnsQueries = 101;
constexpr std::uint64_t kDefaultConnects = 101;
constexpr std::string_view kDnsName = "benchmark.example";

enum class DnsMode : std::uint8_t {
    ServerFailure,
    Timeout,
};

fiber::common::IoResult<std::uint16_t> bound_port(int fd) {
    sockaddr_storage storage{};
    socklen_t length = sizeof(storage);
    if (::getsockname(fd, reinterpret_cast<sockaddr *>(&storage), &length) != 0) {
        return std::unexpected(fiber::common::io_err_from_errno(errno));
    }
    fiber::net::SocketAddress address;
    if (!fiber::net::SocketAddress::from_sockaddr(reinterpret_cast<sockaddr *>(&storage), length, address)) {
        return std::unexpected(fiber::common::IoErr::NotSupported);
    }
    return address.port();
}

void push_be16(std::vector<std::uint8_t> &output, std::uint16_t value) {
    output.push_back(static_cast<std::uint8_t>(value >> 8U));
    output.push_back(static_cast<std::uint8_t>(value & 0xffU));
}

void push_be32(std::vector<std::uint8_t> &output, std::uint32_t value) {
    output.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xffU));
    output.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xffU));
    output.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
    output.push_back(static_cast<std::uint8_t>(value & 0xffU));
}

void append_dns_name(std::vector<std::uint8_t> &output, std::string_view name) {
    std::size_t offset = 0;
    while (offset < name.size()) {
        std::size_t dot = name.find('.', offset);
        if (dot == std::string_view::npos) {
            dot = name.size();
        }
        output.push_back(static_cast<std::uint8_t>(dot - offset));
        output.insert(output.end(), name.begin() + static_cast<std::ptrdiff_t>(offset),
                      name.begin() + static_cast<std::ptrdiff_t>(dot));
        offset = dot + 1;
    }
    output.push_back(0);
}

std::uint16_t read_be16(const std::uint8_t *input) noexcept {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(input[0]) << 8U) | input[1]);
}

std::vector<std::uint8_t> dns_response(std::uint16_t id, bool success) {
    std::vector<std::uint8_t> packet;
    packet.reserve(64);
    push_be16(packet, id);
    push_be16(packet, success ? 0x8180U : 0x8182U);
    push_be16(packet, 1);
    push_be16(packet, success ? 1 : 0);
    push_be16(packet, 0);
    push_be16(packet, 0);
    append_dns_name(packet, kDnsName);
    push_be16(packet, static_cast<std::uint16_t>(fiber::dns::RecordType::A));
    push_be16(packet, static_cast<std::uint16_t>(fiber::dns::RecordClass::IN));
    if (success) {
        push_be16(packet, 0xc00cU);
        push_be16(packet, static_cast<std::uint16_t>(fiber::dns::RecordType::A));
        push_be16(packet, static_cast<std::uint16_t>(fiber::dns::RecordClass::IN));
        push_be32(packet, 60);
        push_be16(packet, 4);
        packet.insert(packet.end(), {127, 0, 0, 1});
    }
    return packet;
}

struct DnsResponderState {
    bool done = false;
    bool success = true;
};

fiber::async::DetachedTask respond_dns(std::shared_ptr<fiber::net::UdpSocket> first,
                                       std::shared_ptr<fiber::net::UdpSocket> second, DnsMode mode,
                                       std::uint64_t query_count, std::shared_ptr<DnsResponderState> state) {
    std::array<std::uint8_t, 512> packet{};
    for (std::uint64_t query = 0; query < query_count; ++query) {
        auto first_query = co_await first->recv_from(packet.data(), packet.size(), 5s);
        if (!first_query || first_query->size < 2) {
            state->success = false;
            break;
        }
        if (mode == DnsMode::ServerFailure) {
            const std::vector<std::uint8_t> response = dns_response(read_be16(packet.data()), false);
            auto sent = co_await first->send_to(response.data(), response.size(), first_query->peer, 5s);
            if (!sent) {
                state->success = false;
                break;
            }
        }

        auto second_query = co_await second->recv_from(packet.data(), packet.size(), 5s);
        if (!second_query || second_query->size < 2) {
            state->success = false;
            break;
        }
        const std::vector<std::uint8_t> response = dns_response(read_be16(packet.data()), true);
        auto sent = co_await second->send_to(response.data(), response.size(), second_query->peer, 5s);
        if (!sent) {
            state->success = false;
            break;
        }
    }
    state->done = true;
}

struct DnsCaseOutcome {
    Distribution distribution;
    bool success = false;
};

fiber::async::DetachedTask run_dns_case(fiber::event::EventLoop *loop, DnsMode mode, std::uint64_t query_count,
                                        std::promise<DnsCaseOutcome> *done) {
    auto first = std::make_shared<fiber::net::UdpSocket>(*loop);
    auto second = std::make_shared<fiber::net::UdpSocket>(*loop);
    const auto first_bound = first->bind(fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), 0), {});
    const auto second_bound = second->bind(fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), 0), {});
    const auto first_port = first_bound ? bound_port(first->fd())
                                        : fiber::common::IoResult<std::uint16_t>(std::unexpected(first_bound.error()));
    const auto second_port = second_bound
                                     ? bound_port(second->fd())
                                     : fiber::common::IoResult<std::uint16_t>(std::unexpected(second_bound.error()));
    if (!first_port || !second_port) {
        done->set_value({});
        co_return;
    }

    auto responder = std::make_shared<DnsResponderState>();
    fiber::async::spawn(*loop, [first, second, mode, query_count, responder]() {
        return respond_dns(first, second, mode, query_count, responder);
    });
    fiber::dns::DnsClient client;
    fiber::dns::DnsClient::Options options;
    (void) options.nameservers.add(fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), *first_port));
    (void) options.nameservers.add(fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), *second_port));
    options.timeout = 2ms;
    options.attempts = 1;
    options.enable_0x20 = false;
    if (!client.init(*loop, options)) {
        first->close();
        second->close();
        done->set_value({});
        co_return;
    }

    std::vector<std::uint64_t> elapsed;
    elapsed.reserve(static_cast<std::size_t>(query_count));
    fiber::dns::QuestionSpec question{
            .name = kDnsName,
            .type = static_cast<std::uint16_t>(fiber::dns::RecordType::A),
            .dns_class = static_cast<std::uint16_t>(fiber::dns::RecordClass::IN),
    };
    for (std::uint64_t query = 0; query < query_count; ++query) {
        std::array<std::uint8_t, 512> response{};
        const auto started = std::chrono::steady_clock::now();
        auto result = co_await client.query_raw(question, response.data(), response.size());
        elapsed.push_back(static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started)
                        .count()));
        if (!result) {
            client.close();
            client.release();
            first->close();
            second->close();
            done->set_value({});
            co_return;
        }
    }
    while (!responder->done) {
        co_await fiber::async::yield();
    }
    client.close();
    client.release();
    first->close();
    second->close();
    if (!responder->success) {
        done->set_value({});
        co_return;
    }
    done->set_value({
            .distribution = fiber::access_server::benchmark::summarize(std::move(elapsed), 1),
            .success = true,
    });
}

struct V6Guard {
    int fd = -1;

    ~V6Guard() {
        if (fd >= 0) {
            ::close(fd);
        }
    }
};

bool bind_v6_guard(std::uint16_t port, V6Guard &guard) noexcept {
    guard.fd = ::socket(AF_INET6, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (guard.fd < 0) {
        return false;
    }
    fiber::net::SocketAddress address(fiber::net::IpAddress::loopback_v6(), port);
    sockaddr_storage storage{};
    socklen_t length = 0;
    return address.to_sockaddr(storage, length) &&
           ::bind(guard.fd, reinterpret_cast<sockaddr *>(&storage), length) == 0;
}

struct ConnectorCaseOutcome {
    Distribution distribution;
    bool ipv6_supported = true;
    bool success = false;
};

fiber::async::DetachedTask run_connector_case(fiber::event::EventLoop *loop, bool fallback, std::uint64_t connect_count,
                                              std::promise<ConnectorCaseOutcome> *done) {
    fiber::net::TcpListener listener(*loop);
    auto bound = listener.bind(fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), 0), {});
    auto port =
            bound ? bound_port(listener.fd()) : fiber::common::IoResult<std::uint16_t>(std::unexpected(bound.error()));
    if (!port) {
        done->set_value({});
        co_return;
    }
    V6Guard guard;
    if (fallback && !bind_v6_guard(*port, guard)) {
        listener.close();
        done->set_value({.ipv6_supported = false, .success = true});
        co_return;
    }

    const fiber::net::SocketAddress v4(fiber::net::IpAddress::loopback_v4(), *port);
    const fiber::net::SocketAddress v6(fiber::net::IpAddress::loopback_v6(), *port);
    const std::array<fiber::net::SocketAddress, 2> fallback_addresses{v6, v4};
    const std::array<fiber::net::SocketAddress, 1> direct_addresses{v4};
    fiber::net::HappyEyeballsOptions options;
    options.total_timeout = 1s;
    options.connection_attempt_delay = fiber::net::kHappyEyeballsMinimumConnectionAttemptDelay;
    std::vector<std::uint64_t> elapsed;
    elapsed.reserve(static_cast<std::size_t>(connect_count));
    for (std::uint64_t connect = 0; connect < connect_count; ++connect) {
        const auto addresses = fallback ? std::span<const fiber::net::SocketAddress>(fallback_addresses)
                                        : std::span<const fiber::net::SocketAddress>(direct_addresses);
        const auto started = std::chrono::steady_clock::now();
        auto connected = co_await fiber::net::TcpConnector::connect(*loop, addresses, options);
        elapsed.push_back(static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started)
                        .count()));
        if (!connected) {
            listener.close();
            done->set_value({});
            co_return;
        }
        auto accepted = co_await listener.accept();
        if (!accepted) {
            const int connected_fd = connected->release_fd();
            ::close(connected_fd);
            listener.close();
            done->set_value({});
            co_return;
        }
        const int connected_fd = connected->release_fd();
        const int accepted_fd = accepted->release_fd();
        ::close(connected_fd);
        ::close(accepted_fd);
    }
    listener.close();
    done->set_value({
            .distribution = fiber::access_server::benchmark::summarize(std::move(elapsed), 1),
            .success = true,
    });
}

void print_case(std::string_view name, std::uint64_t operations, const Distribution &distribution) {
    std::printf("%.*s,%llu,%.2f,%.2f,%.2f,%.0f\n", static_cast<int>(name.size()), name.data(),
                static_cast<unsigned long long>(operations), distribution.p50_ns_per_operation,
                distribution.p95_ns_per_operation, distribution.p99_ns_per_operation,
                distribution.operations_per_second);
}

std::optional<DnsCaseOutcome> execute_dns_case(DnsMode mode, std::uint64_t queries) {
    fiber::event::EventLoopGroup group(1);
    std::promise<DnsCaseOutcome> promise;
    auto future = promise.get_future();
    group.start();
    fiber::async::spawn(group.at(0), [&]() { return run_dns_case(&group.at(0), mode, queries, &promise); });
    const bool completed = future.wait_for(std::chrono::minutes(1)) == std::future_status::ready;
    std::optional<DnsCaseOutcome> outcome;
    if (completed) {
        outcome = future.get();
    }
    group.stop();
    group.join();
    return outcome;
}

std::optional<ConnectorCaseOutcome> execute_connector_case(bool fallback, std::uint64_t connects) {
    fiber::event::EventLoopGroup group(1);
    std::promise<ConnectorCaseOutcome> promise;
    auto future = promise.get_future();
    group.start();
    fiber::async::spawn(group.at(0), [&]() { return run_connector_case(&group.at(0), fallback, connects, &promise); });
    const bool completed = future.wait_for(std::chrono::minutes(1)) == std::future_status::ready;
    std::optional<ConnectorCaseOutcome> outcome;
    if (completed) {
        outcome = future.get();
    }
    group.stop();
    group.join();
    return outcome;
}

} // namespace

int main(int argc, char **argv) {
    std::uint64_t dns_queries = kDefaultDnsQueries;
    std::uint64_t connects = kDefaultConnects;
    if ((argc >= 2 && !fiber::access_server::benchmark::parse_positive(argv[1], dns_queries)) ||
        (argc >= 3 && !fiber::access_server::benchmark::parse_positive(argv[2], connects)) || argc > 3) {
        std::fprintf(stderr, "usage: %s [dns-queries-per-case] [connects-per-case]\n", argv[0]);
        return 2;
    }

    const auto dns_server_failure = execute_dns_case(DnsMode::ServerFailure, dns_queries);
    const auto dns_timeout = execute_dns_case(DnsMode::Timeout, dns_queries);
    const auto connector_v4 = execute_connector_case(false, connects);
    const auto connector_fallback = execute_connector_case(true, connects);
    if (!dns_server_failure || !dns_server_failure->success || !dns_timeout || !dns_timeout->success || !connector_v4 ||
        !connector_v4->success || !connector_fallback || !connector_fallback->success) {
        std::fprintf(stderr,
                     "network integration benchmark failed dns_servfail=%d dns_timeout=%d connector_v4=%d "
                     "connector_fallback=%d\n",
                     dns_server_failure && dns_server_failure->success, dns_timeout && dns_timeout->success,
                     connector_v4 && connector_v4->success, connector_fallback && connector_fallback->success);
        return 1;
    }

    std::printf("case,operations,p50_ns_per_operation,p95_ns_per_operation,p99_ns_per_operation,"
                "operations_per_second\n");
    print_case("dns_servfail_then_success", dns_queries, dns_server_failure->distribution);
    print_case("dns_timeout_then_success", dns_queries, dns_timeout->distribution);
    print_case("connector_v4", connects, connector_v4->distribution);
    if (connector_fallback->ipv6_supported) {
        print_case("connector_v6_failure_then_v4", connects, connector_fallback->distribution);
    } else {
        std::fprintf(stderr, "connector_v6_failure_then_v4=SKIP reason=ipv6_loopback_unavailable\n");
    }
    return 0;
}
