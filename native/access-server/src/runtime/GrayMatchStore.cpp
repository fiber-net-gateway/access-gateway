#include "GrayMatchStore.h"

#include <limits>
#include <utility>

#include <fiber/common/Assert.h>
#include <fiber/event/EventLoop.h>
#include <fiber/event/EventLoopGroup.h>

namespace fiber::access_server {
namespace {

constexpr std::uint64_t kRandomSequenceIncrement = 0x9e3779b97f4a7c15ULL;

std::uint64_t mix_random(std::uint64_t value) noexcept {
    value ^= value >> 30U;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27U;
    value *= 0x94d049bb133111ebULL;
    value ^= value >> 31U;
    return value;
}

std::uint64_t worker_random_sequence(std::uint64_t seed, std::size_t worker_index) noexcept {
    return mix_random(seed ^ (kRandomSequenceIncrement * (worker_index + 1)));
}

} // namespace

GrayMatchStore::WorkerSlot::WorkerSlot(std::shared_ptr<const WorkerSnapshot> initial, std::uint64_t sequence) noexcept :
    published(std::move(initial)), random_sequence(sequence) {}

GrayMatchStore::GrayMatchStore() { initialize({}); }

GrayMatchStore::GrayMatchStore(event::EventLoopGroup &workers, GrayMatchStoreOptions options) : workers_(&workers) {
    FIBER_ASSERT(workers.size() > 0);
    initialize(options);
}

void GrayMatchStore::initialize(GrayMatchStoreOptions options) {
    auto initial = std::make_shared<const Snapshot>();
    published_.store(initial, std::memory_order_relaxed);
    if (workers_ == nullptr) {
        return;
    }

    worker_slots_.reserve(workers_->size());
    for (std::size_t index = 0; index < workers_->size(); ++index) {
        auto worker_snapshot = std::make_shared<const WorkerSnapshot>(initial);
        worker_slots_.push_back(std::make_unique<WorkerSlot>(std::move(worker_snapshot),
                                                             worker_random_sequence(options.random_seed, index)));
    }
}

std::expected<GrayMatchUpdateStatus, AccessConfigError>
GrayMatchStore::apply(const std::optional<GrayMatchConfig> &config) {
    if (!config) {
        return GrayMatchUpdateStatus::IgnoredEmpty;
    }
    auto compiled = compile_gray_match_config(*config);
    if (!compiled) {
        return std::unexpected(std::move(compiled.error()));
    }

    FIBER_ASSERT(next_generation_ != std::numeric_limits<std::uint64_t>::max());
    auto candidate = std::make_shared<Snapshot>();
    candidate->generation = next_generation_ + 1;
    candidate->config = std::move(*compiled);

    std::shared_ptr<const Snapshot> published = std::move(candidate);
    std::vector<std::shared_ptr<const WorkerSnapshot>> worker_snapshots;
    worker_snapshots.reserve(worker_slots_.size());
    for (std::size_t index = 0; index < worker_slots_.size(); ++index) {
        worker_snapshots.push_back(std::make_shared<const WorkerSnapshot>(published));
    }
    for (std::size_t index = 0; index < worker_slots_.size(); ++index) {
        worker_slots_[index]->published.store(std::move(worker_snapshots[index]), std::memory_order_release);
    }
    published_.store(std::move(published), std::memory_order_release);
    ++next_generation_;
    return GrayMatchUpdateStatus::Published;
}

ProxyClusterMatcher GrayMatchStore::adapter() noexcept {
    FIBER_ASSERT(workers_ != nullptr);
    FIBER_ASSERT(worker_slots_.size() == workers_->size());
    return ProxyClusterMatcher{
            .context = this,
            .matches = &GrayMatchStore::matches_request,
    };
}

bool GrayMatchStore::matches_request(void *context, std::string_view entry, const ClientMetadata &metadata) noexcept {
    auto &store = *static_cast<GrayMatchStore *>(context);
    event::EventLoop *loop = event::EventLoop::current_or_null();
    if (loop == nullptr || !loop->has_group_index() || loop->group() != store.workers_) {
        return false;
    }
    FIBER_ASSERT(loop->group_index() < store.worker_slots_.size());
    WorkerSlot &slot = *store.worker_slots_[loop->group_index()];
    const std::shared_ptr<const WorkerSnapshot> worker_snapshot = slot.published.load(std::memory_order_acquire);
    FIBER_ASSERT(worker_snapshot != nullptr);
    FIBER_ASSERT(worker_snapshot->snapshot != nullptr);
    return matches_snapshot(*worker_snapshot->snapshot, entry, metadata, next_sample(slot));
}

bool GrayMatchStore::matches(std::string_view entry, const ClientMetadata &metadata,
                             std::uint32_t random_sample) const noexcept {
    return matches_snapshot(*pin(), entry, metadata, random_sample);
}

bool GrayMatchStore::matches_snapshot(const Snapshot &snapshot, std::string_view entry, const ClientMetadata &metadata,
                                      std::uint32_t random_sample) noexcept {
    const CompiledGrayMatchRule *matched = nullptr;
    for (const CompiledGrayMatchRule &rule: snapshot.config.rules()) {
        if (rule.entry == entry) {
            matched = &rule;
            break;
        }
    }
    if (!matched) {
        return false;
    }
    if (metadata.gray_target) {
        for (const Cidr &cidr: matched->cidrs) {
            if (cidr.contains(*metadata.gray_target)) {
                return true;
            }
        }
    }
    return random_sample % 10000U < static_cast<std::uint32_t>(matched->ratio);
}

std::size_t GrayMatchStore::rule_count() const noexcept { return pin()->config.rule_count(); }

std::uint64_t GrayMatchStore::generation() const noexcept { return pin()->generation; }

std::shared_ptr<const GrayMatchStore::Snapshot> GrayMatchStore::pin() const noexcept {
    return published_.load(std::memory_order_acquire);
}

std::uint32_t GrayMatchStore::next_sample(WorkerSlot &slot) noexcept {
    slot.random_sequence += kRandomSequenceIncrement;
    return static_cast<std::uint32_t>(mix_random(slot.random_sequence) % 10000U);
}

} // namespace fiber::access_server
