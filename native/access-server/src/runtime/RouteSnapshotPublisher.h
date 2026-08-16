#ifndef FIBER_ACCESS_SERVER_ROUTE_SNAPSHOT_PUBLISHER_H
#define FIBER_ACCESS_SERVER_ROUTE_SNAPSHOT_PUBLISHER_H

#include "../routing/AccessRouteSnapshot.h"

#include <atomic>
#include <memory>

#include <fiber/common/NonCopyable.h>
#include <fiber/common/NonMovable.h>

namespace fiber::access_server {

// The only cross-thread boundary for immutable global route snapshots.
class RouteSnapshotPublisher final : public common::NonCopyable, public common::NonMovable {
public:
    RouteSnapshotPublisher();

    [[nodiscard]] std::shared_ptr<const AccessRouteSnapshot> pin() const noexcept {
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
        return published_.load(std::memory_order_acquire);
#else
        return std::atomic_load_explicit(&published_, std::memory_order_acquire);
#endif
    }

    void publish(std::shared_ptr<const AccessRouteSnapshot> snapshot) noexcept;

private:
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
    std::atomic<std::shared_ptr<const AccessRouteSnapshot>> published_;
#else
    std::shared_ptr<const AccessRouteSnapshot> published_;
#endif
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_ROUTE_SNAPSHOT_PUBLISHER_H
