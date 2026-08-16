#include "AccessRuntimeMetrics.h"

namespace fiber::access_server {

AccessRuntimeMetrics::AccessRuntimeMetrics(event::EventLoop &nacos_owner) noexcept :
    config_(nacos_owner), discovery_(nacos_owner) {}

void AccessRuntimeMetrics::append_prometheus(std::string &output, std::chrono::steady_clock::time_point now) const {
    config_.append_prometheus(output, now);
    discovery_.append_prometheus(output);
}

} // namespace fiber::access_server
