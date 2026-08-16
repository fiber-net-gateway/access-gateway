#include "GrayMatchStore.h"

#include <utility>

namespace fiber::access_server {

GrayMatchStore::GrayMatchStore() {
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
    published_.store(std::make_shared<const Snapshot>(), std::memory_order_relaxed);
#else
    std::atomic_store_explicit(&published_, std::make_shared<const Snapshot>(), std::memory_order_relaxed);
#endif
}

std::expected<GrayMatchUpdateStatus, AccessConfigError>
GrayMatchStore::apply(const std::optional<GrayMatchConfig> &config) {
    if (!config) {
        return GrayMatchUpdateStatus::IgnoredEmpty;
    }

    auto candidate = std::make_shared<Snapshot>();
    candidate->rules.reserve(config->size());
    for (const GrayMatchConfigEntry &entry: *config) {
        if (!recognized_entry(entry.entry) || entry.ratio < 0 || (entry.ratio == 0 && entry.cidrs.empty())) {
            continue;
        }

        Rule rule{
                .entry = entry.entry,
                .ratio = entry.ratio,
        };
        rule.cidrs.reserve(entry.cidrs.size());
        for (const std::optional<std::string> &text: entry.cidrs) {
            if (!text) {
                continue;
            }
            auto cidr = Cidr::parse(*text, entry.entry);
            if (cidr) {
                rule.cidrs.push_back(std::move(*cidr));
            }
        }
        candidate->rules.push_back(std::move(rule));
    }
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
    published_.store(std::move(candidate), std::memory_order_release);
#else
    std::shared_ptr<const Snapshot> published = std::move(candidate);
    std::atomic_store_explicit(&published_, std::move(published), std::memory_order_release);
#endif
    return GrayMatchUpdateStatus::Published;
}

ProxyClusterMatcher GrayMatchStore::adapter() noexcept {
    return ProxyClusterMatcher{
            .context = this,
            .matches = &GrayMatchStore::matches_request,
    };
}

bool GrayMatchStore::matches_request(void *context, std::string_view entry, const ClientMetadata &metadata) noexcept {
    auto &store = *static_cast<GrayMatchStore *>(context);
    return store.matches(entry, metadata, store.next_sample());
}

bool GrayMatchStore::matches(std::string_view entry, const ClientMetadata &metadata,
                             std::uint32_t random_sample) const noexcept {
    std::shared_ptr<const Snapshot> snapshot = pin();
    const Rule *matched = nullptr;
    for (const Rule &rule: snapshot->rules) {
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

std::size_t GrayMatchStore::rule_count() const noexcept { return pin()->rules.size(); }

std::shared_ptr<const GrayMatchStore::Snapshot> GrayMatchStore::pin() const noexcept {
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
    return published_.load(std::memory_order_acquire);
#else
    return std::atomic_load_explicit(&published_, std::memory_order_acquire);
#endif
}

bool GrayMatchStore::recognized_entry(std::string_view entry) noexcept {
    return entry == "vdi" || entry == "desktop" || entry == "internet" || entry == "custom";
}

std::uint32_t GrayMatchStore::next_sample() const noexcept {
    std::uint64_t value = random_sequence_.fetch_add(1, std::memory_order_relaxed) + 1;
    value ^= value >> 30U;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27U;
    value *= 0x94d049bb133111ebULL;
    value ^= value >> 31U;
    return static_cast<std::uint32_t>(value % 10000U);
}

} // namespace fiber::access_server
