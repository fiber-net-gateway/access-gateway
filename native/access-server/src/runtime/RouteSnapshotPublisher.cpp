#include "RouteSnapshotPublisher.h"

#include <utility>

#include <fiber/common/Assert.h>

namespace fiber::access_server {

RouteSnapshotPublisher::RouteSnapshotPublisher() {
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
    published_.store(std::make_shared<const AccessRouteSnapshot>(), std::memory_order_relaxed);
#else
    std::atomic_store_explicit(&published_, std::make_shared<const AccessRouteSnapshot>(), std::memory_order_relaxed);
#endif
}

void RouteSnapshotPublisher::publish(std::shared_ptr<const AccessRouteSnapshot> snapshot) noexcept {
    FIBER_ASSERT(snapshot);
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
    published_.store(std::move(snapshot), std::memory_order_release);
#else
    std::atomic_store_explicit(&published_, std::move(snapshot), std::memory_order_release);
#endif
}

} // namespace fiber::access_server
