#ifndef FIBER_ACCESS_SERVER_ROUTE_SNAPSHOT_PUBLISHER_H
#define FIBER_ACCESS_SERVER_ROUTE_SNAPSHOT_PUBLISHER_H

#include "../routing/AccessRouteSnapshot.h"

#include <atomic>
#include <memory>

#include <fiber/common/NonCopyable.h>
#include <fiber/common/NonMovable.h>

namespace fiber::event {

class EventLoopGroup;

} // namespace fiber::event

namespace fiber::access_server {

// The only cross-thread boundary for immutable route snapshots. Standalone
// readers use the canonical snapshot; serving workers use independent wrapper
// control blocks so request pins do not contend across EventLoops. A bound
// worker group has a fixed size, outlives this publisher, and is drained before
// publisher destruction.
class RouteSnapshotPublisher final : public common::NonCopyable, public common::NonMovable {
public:
    RouteSnapshotPublisher();
    explicit RouteSnapshotPublisher(event::EventLoopGroup &workers);
    ~RouteSnapshotPublisher() noexcept;

    [[nodiscard]] std::shared_ptr<const AccessRouteSnapshot> pin() const noexcept;

    void publish(std::shared_ptr<const AccessRouteSnapshot> snapshot) noexcept;

private:
    struct WorkerState;

#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
    std::atomic<std::shared_ptr<const AccessRouteSnapshot>> published_;
#else
    std::shared_ptr<const AccessRouteSnapshot> published_;
#endif
    std::unique_ptr<WorkerState> worker_state_;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_ROUTE_SNAPSHOT_PUBLISHER_H
