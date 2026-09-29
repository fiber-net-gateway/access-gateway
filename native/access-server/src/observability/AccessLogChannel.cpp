#include "AccessLogChannel.h"

#include <atomic>
#include <cstdio>
#include <ctime>
#include <memory>
#include <utility>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>

#include <fiber/common/Assert.h>
#include <fiber/log/Log.h>

namespace fiber::access_server {
namespace {

// The access line rides the shared LogWorker pipeline. A dedicated logger
// with additivity off keeps the line out of the root (console) appenders, and
// the pinned Info level keeps any future diagnostic-level change from
// silencing the ledger.
DEFINE_LOGGER(LOG_ACCESS_LOG, "access_server.access");

std::atomic<AccessLogChannel *> g_access_log_channel{nullptr};

} // namespace

int local_utc_offset_minutes() noexcept {
    static const int offset = []() noexcept {
        const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::tm local{};
        if (::localtime_r(&now, &local) == nullptr) {
            return 0;
        }
        return static_cast<int>(local.tm_gmtoff) / 60;
    }();
    return offset;
}

std::string resolve_local_server_ip() noexcept {
    // Java NetUtil.findFirstNonLoopbackAddress: across up interfaces, the
    // lowest interface index wins; within an interface the last non-loopback
    // IPv4 address wins.
    ifaddrs *addresses = nullptr;
    if (::getifaddrs(&addresses) != 0) {
        return "127.0.0.1";
    }
    std::string selected;
    unsigned selected_index = 0;
    bool has_selection = false;
    for (const ifaddrs *entry = addresses; entry != nullptr; entry = entry->ifa_next) {
        if (entry->ifa_addr == nullptr || entry->ifa_addr->sa_family != AF_INET) {
            continue;
        }
        const auto *sin = reinterpret_cast<const sockaddr_in *>(entry->ifa_addr);
        const std::uint32_t address = sin->sin_addr.s_addr;
        if ((ntohl(address) & 0xff000000U) == 0x7f000000U) {
            continue;
        }
        char text[INET_ADDRSTRLEN];
        if (::inet_ntop(AF_INET, &sin->sin_addr, text, sizeof(text)) == nullptr) {
            continue;
        }
        const unsigned index = ::if_nametoindex(entry->ifa_name);
        if (!has_selection || (index != 0 && index < selected_index)) {
            selected = text;
            selected_index = index;
            has_selection = true;
        } else if (has_selection && index == selected_index) {
            selected = text;
        }
    }
    ::freeifaddrs(addresses);
    return has_selection ? selected : std::string("127.0.0.1");
}

AccessLogChannel::AccessLogChannel(std::string gateway_name) :
    gateway_name_(std::move(gateway_name)), server_ip_(resolve_local_server_ip()),
    utc_offset_minutes_(local_utc_offset_minutes()) {}

void AccessLogChannel::submit(std::string line) noexcept {
    if (!line.empty() && line.back() == '\n') {
        line.pop_back();
    }
    if (line.empty()) {
        return;
    }
    // Raw append without escaping: the tab-separated columns must reach the
    // MessageOnly appender byte-identical. The handle resolves to the compiled
    // logger after initialization and falls back to bootstrap stderr around it.
    (void) log::log_complete_message(LOG_ACCESS_LOG.get(), log::LogLevel::Info, __FILE__, __LINE__, __func__, line);
}

void install_access_log_channel(std::string gateway_name) noexcept {
    FIBER_ASSERT(g_access_log_channel.load(std::memory_order_acquire) == nullptr);
    try {
        auto channel = std::make_unique<AccessLogChannel>(std::move(gateway_name));
        g_access_log_channel.store(channel.release(), std::memory_order_release);
    } catch (...) {
        std::fprintf(stderr, "access-server failed to start the access log channel\n");
    }
}

AccessLogChannel *access_log_channel() noexcept { return g_access_log_channel.load(std::memory_order_acquire); }

void uninstall_access_log_channel() noexcept {
    AccessLogChannel *channel = g_access_log_channel.exchange(nullptr, std::memory_order_acq_rel);
    delete channel;
}

} // namespace fiber::access_server
