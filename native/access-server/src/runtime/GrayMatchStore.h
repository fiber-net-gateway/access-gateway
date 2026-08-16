#ifndef FIBER_ACCESS_SERVER_GRAY_MATCH_STORE_H
#define FIBER_ACCESS_SERVER_GRAY_MATCH_STORE_H

#include "../config/AccessConfig.h"
#include "../config/AccessConfigError.h"
#include "../execution/ClientMetadata.h"
#include "../routing/Cidr.h"
#include "../routing/ProxyAddressSelector.h"

#include <atomic>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fiber::event {

class EventLoopGroup;

} // namespace fiber::event

namespace fiber::access_server {

enum class GrayMatchUpdateStatus : std::uint8_t {
    IgnoredEmpty,
    Published,
};

struct GrayMatchStoreOptions {
    std::uint64_t random_seed = 0;
};

class GrayMatchStore {
public:
    // Standalone mode supports apply(), explicit-sample matches(), and
    // validation diagnostics. Request-path adapter() requires worker mode.
    GrayMatchStore();
    explicit GrayMatchStore(event::EventLoopGroup &workers, GrayMatchStoreOptions options = {});

    // Single-writer control-plane update. Worker readers observe one complete
    // immutable generation through their independently published slot.
    [[nodiscard]] std::expected<GrayMatchUpdateStatus, AccessConfigError>
    apply(const std::optional<GrayMatchConfig> &config);

    [[nodiscard]] bool matches(std::string_view entry, const ClientMetadata &metadata,
                               std::uint32_t random_sample) const noexcept;
    [[nodiscard]] ProxyClusterMatcher adapter() noexcept;
    [[nodiscard]] std::size_t rule_count() const noexcept;
    [[nodiscard]] std::uint64_t generation() const noexcept;

private:
    static bool matches_request(void *context, std::string_view entry, const ClientMetadata &metadata) noexcept;
    struct Rule {
        std::string entry;
        std::int32_t ratio = 0;
        std::vector<Cidr> cidrs;
    };

    struct Snapshot {
        std::uint64_t generation = 0;
        std::vector<Rule> rules;
    };

    struct alignas(64) WorkerSnapshot {
        explicit WorkerSnapshot(std::shared_ptr<const Snapshot> value) noexcept : snapshot(std::move(value)) {}

        // The wrapper's control block is worker-local while the immutable rule
        // storage is shared without request-path reference-count changes.
        const std::shared_ptr<const Snapshot> snapshot;
    };

    struct alignas(64) WorkerSlot {
        WorkerSlot(std::shared_ptr<const WorkerSnapshot> initial, std::uint64_t sequence) noexcept;

        std::atomic<std::shared_ptr<const WorkerSnapshot>> published;
        std::uint64_t random_sequence = 0;
    };

    void initialize(GrayMatchStoreOptions options);
    [[nodiscard]] static bool recognized_entry(std::string_view entry) noexcept;
    [[nodiscard]] static bool matches_snapshot(const Snapshot &snapshot, std::string_view entry,
                                               const ClientMetadata &metadata, std::uint32_t random_sample) noexcept;
    [[nodiscard]] static std::uint32_t next_sample(WorkerSlot &slot) noexcept;
    [[nodiscard]] std::shared_ptr<const Snapshot> pin() const noexcept;

    std::atomic<std::shared_ptr<const Snapshot>> published_;
    event::EventLoopGroup *workers_ = nullptr;
    std::vector<std::unique_ptr<WorkerSlot>> worker_slots_;
    std::uint64_t next_generation_ = 0;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_GRAY_MATCH_STORE_H
