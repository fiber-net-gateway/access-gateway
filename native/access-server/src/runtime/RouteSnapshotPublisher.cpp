#include "RouteSnapshotPublisher.h"

#include <utility>
#include <vector>

#include <fiber/common/Assert.h>
#include <fiber/event/EventLoop.h>
#include <fiber/event/EventLoopGroup.h>

namespace fiber::access_server {

struct RouteSnapshotPublisher::WorkerState {
    struct alignas(64) WorkerSnapshot {
        explicit WorkerSnapshot(std::shared_ptr<const AccessRouteSnapshot> value) noexcept :
            snapshot(std::move(value)) {}

        const std::shared_ptr<const AccessRouteSnapshot> snapshot;
    };

    struct alignas(64) WorkerSlot {
        explicit WorkerSlot(std::shared_ptr<const WorkerSnapshot> initial) noexcept : published(std::move(initial)) {}

        std::atomic<std::shared_ptr<const WorkerSnapshot>> published;
    };

    WorkerState(event::EventLoopGroup &worker_group, const std::shared_ptr<const AccessRouteSnapshot> &initial) :
        workers(&worker_group) {
        slots.reserve(worker_group.size());
        for (std::size_t index = 0; index < worker_group.size(); ++index) {
            slots.push_back(std::make_unique<WorkerSlot>(std::make_shared<const WorkerSnapshot>(initial)));
        }
    }

    [[nodiscard]] std::shared_ptr<const AccessRouteSnapshot> pin(std::size_t index) const noexcept {
        FIBER_ASSERT(index < slots.size());
        std::shared_ptr<const WorkerSnapshot> worker_snapshot = slots[index]->published.load(std::memory_order_acquire);
        FIBER_ASSERT(worker_snapshot);
        FIBER_ASSERT(worker_snapshot->snapshot);
        const AccessRouteSnapshot *snapshot = worker_snapshot->snapshot.get();
        return std::shared_ptr<const AccessRouteSnapshot>(std::move(worker_snapshot), snapshot);
    }

    void publish(const std::shared_ptr<const AccessRouteSnapshot> &snapshot) {
        // Allocate the complete generation before making it visible to any
        // worker. Each wrapper supplies a worker-local ownership control block
        // while retaining the single canonical route snapshot.
        std::vector<std::shared_ptr<const WorkerSnapshot>> candidates;
        candidates.reserve(slots.size());
        for (std::size_t index = 0; index < slots.size(); ++index) {
            candidates.push_back(std::make_shared<const WorkerSnapshot>(snapshot));
        }
        for (std::size_t index = 0; index < slots.size(); ++index) {
            slots[index]->published.store(std::move(candidates[index]), std::memory_order_release);
        }
    }

    event::EventLoopGroup *workers = nullptr;
    std::vector<std::unique_ptr<WorkerSlot>> slots;
};

RouteSnapshotPublisher::RouteSnapshotPublisher() {
    auto initial = std::make_shared<const AccessRouteSnapshot>();
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
    published_.store(std::move(initial), std::memory_order_relaxed);
#else
    std::atomic_store_explicit(&published_, std::move(initial), std::memory_order_relaxed);
#endif
}

RouteSnapshotPublisher::RouteSnapshotPublisher(event::EventLoopGroup &workers) {
    FIBER_ASSERT(workers.size() > 0);
    auto initial = std::make_shared<const AccessRouteSnapshot>();
    worker_state_ = std::make_unique<WorkerState>(workers, initial);
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
    published_.store(std::move(initial), std::memory_order_relaxed);
#else
    std::atomic_store_explicit(&published_, std::move(initial), std::memory_order_relaxed);
#endif
}

RouteSnapshotPublisher::~RouteSnapshotPublisher() noexcept = default;

std::shared_ptr<const AccessRouteSnapshot> RouteSnapshotPublisher::pin() const noexcept {
    if (worker_state_) {
        event::EventLoop *loop = event::EventLoop::current_or_null();
        if (loop != nullptr && loop->has_group_index() && loop->group() == worker_state_->workers) {
            return worker_state_->pin(loop->group_index());
        }
    }
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
    return published_.load(std::memory_order_acquire);
#else
    return std::atomic_load_explicit(&published_, std::memory_order_acquire);
#endif
}

void RouteSnapshotPublisher::publish(std::shared_ptr<const AccessRouteSnapshot> snapshot) noexcept {
    FIBER_ASSERT(snapshot);
    if (worker_state_) {
        worker_state_->publish(snapshot);
    }
    // A caller observing the canonical generation after publish returns can
    // rely on every serving worker slot having already received it.
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
    published_.store(std::move(snapshot), std::memory_order_release);
#else
    std::atomic_store_explicit(&published_, std::move(snapshot), std::memory_order_release);
#endif
}

} // namespace fiber::access_server
