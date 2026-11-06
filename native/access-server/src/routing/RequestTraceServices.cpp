#include "RequestTraceServices.h"

namespace fiber::access_server {

fiber::async::Task<fiber::common::IoResult<std::unique_ptr<http_script::HttpUpstreamConnection>>>
RequestTraceServices::acquire(const http_script::HttpTargetSpec &target,
                              std::chrono::milliseconds connect_timeout) noexcept {
    if (upstream_ == nullptr) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    co_return co_await upstream_->acquire(target, connect_timeout);
}

} // namespace fiber::access_server
