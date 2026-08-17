#include "AccessRuntimeMetrics.h"

namespace fiber::access_server {

AccessRuntimeMetrics::AccessRuntimeMetrics(event::EventLoop &nacos_owner,
                                           AccessProcessMetricsSources process_sources) noexcept :
    config_(nacos_owner), discovery_(nacos_owner), tls_(nacos_owner), process_(process_sources) {}

void AccessRuntimeMetrics::append_prometheus(std::string &output, std::chrono::steady_clock::time_point now) const {
    config_.append_prometheus(output, now);
    dns_.append_prometheus(output);
    discovery_.append_prometheus(output);
    tls_.append_prometheus(output, now);
    process_.append_prometheus(output);
}

} // namespace fiber::access_server
