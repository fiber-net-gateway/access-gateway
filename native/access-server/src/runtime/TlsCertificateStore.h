#ifndef FIBER_ACCESS_SERVER_TLS_CERTIFICATE_STORE_H
#define FIBER_ACCESS_SERVER_TLS_CERTIFICATE_STORE_H

#include "../config/TlsCertificateConfig.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <fiber/async/Task.h>
#include <fiber/async/Watch.h>
#include <fiber/common/NonCopyable.h>
#include <fiber/common/NonMovable.h>
#include <fiber/event/EventLoop.h>
#include <fiber/event/EventLoopGroup.h>
#include <fiber/net/TlsOptions.h>

namespace fiber::access_server {

enum class TlsCertificateUpdateStatus : std::uint8_t {
    IgnoredOlderVersion,
    VersionUnchanged,
    Published,
};

using TlsCertificateContentDigest = std::array<std::uint8_t, 32>;

struct TlsCertificateVersionState {
    bool active = false;
    std::uint64_t version = 0;
    TlsCertificateContentDigest content_digest{};
};

using TlsCertificateClassification =
        std::optional<std::expected<TlsCertificateUpdateStatus, TlsCertificateConfigError>>;

class TlsBootstrapIdentity final : public common::NonCopyable, public common::NonMovable {
public:
    ~TlsBootstrapIdentity();

    [[nodiscard]] static std::expected<std::shared_ptr<TlsBootstrapIdentity>, TlsCertificateConfigError>
    create(std::string_view certificate_pem, std::string_view private_key_pem);
    [[nodiscard]] const std::string &certificate_path() const noexcept { return certificate_path_; }
    [[nodiscard]] const std::string &private_key_path() const noexcept { return private_key_path_; }
    void close() noexcept;

private:
    TlsBootstrapIdentity(int certificate_fd, int private_key_fd);

    std::atomic<int> certificate_fd_{-1};
    std::atomic<int> private_key_fd_{-1};
    std::string certificate_path_;
    std::string private_key_path_;
};

class TlsCertificateStore final : public common::NonCopyable, public common::NonMovable {
public:
    class Snapshot;
    class PreparedUpdate final {
    public:
        PreparedUpdate(const PreparedUpdate &) = delete;
        PreparedUpdate &operator=(const PreparedUpdate &) = delete;
        PreparedUpdate(PreparedUpdate &&other) noexcept;
        PreparedUpdate &operator=(PreparedUpdate &&other) noexcept;
        ~PreparedUpdate();

    private:
        friend class TlsCertificateStore;

        PreparedUpdate() noexcept = default;

        std::uint64_t version_ = 0;
        TlsCertificateContentDigest content_digest_{};
        std::unique_ptr<Snapshot> snapshot_;
        std::shared_ptr<TlsBootstrapIdentity> bootstrap_;
    };

    TlsCertificateStore(event::EventLoop &owner_loop, event::EventLoopGroup &workers, bool quic_enabled);
    ~TlsCertificateStore();

    [[nodiscard]] static TlsCertificateContentDigest content_digest(std::string_view wire_content) noexcept;
    [[nodiscard]] static std::expected<PreparedUpdate, TlsCertificateConfigError>
    prepare(const TlsCertificateSnapshotConfig &config, TlsCertificateContentDigest digest, bool quic_enabled,
            bool prepare_bootstrap);
    [[nodiscard]] std::expected<TlsCertificateUpdateStatus, TlsCertificateConfigError>
    apply(const TlsCertificateSnapshotConfig &config, std::string_view wire_content);
    [[nodiscard]] std::expected<TlsCertificateUpdateStatus, TlsCertificateConfigError> commit(PreparedUpdate prepared);
    [[nodiscard]] async::Task<void> shutdown() noexcept;

    [[nodiscard]] net::TlsIdentitySelectorOps selector_ops() noexcept;
    [[nodiscard]] std::shared_ptr<TlsBootstrapIdentity> bootstrap_identity() const noexcept { return bootstrap_; }
    [[nodiscard]] std::uint64_t version() const noexcept { return version_; }
    [[nodiscard]] std::size_t certificate_count() const noexcept;
    [[nodiscard]] bool quic_enabled() const noexcept { return quic_enabled_; }
    [[nodiscard]] TlsCertificateVersionState version_state() const noexcept;
    [[nodiscard]] TlsCertificateClassification classify(std::uint64_t version,
                                                        const TlsCertificateContentDigest &digest) const;

private:
    struct alignas(64) WorkerSlot {
        std::atomic<Snapshot *> hazard{nullptr};
        event::EventLoop::DeferEntry clear_entry;
        TlsCertificateStore *store = nullptr;
    };

    [[nodiscard]] static net::TlsContext *select_identity(void *context,
                                                          const net::TlsIdentitySelectInput &input) noexcept;
    static void clear_hazard(WorkerSlot *slot) noexcept;
    static void run_reaper(TlsCertificateStore *store) noexcept;
    void request_reclaim() noexcept;
    void reclaim_retired() noexcept;

    event::EventLoop *owner_loop_ = nullptr;
    event::EventLoopGroup *workers_ = nullptr;
    std::vector<std::unique_ptr<WorkerSlot>> worker_slots_;
    std::atomic<Snapshot *> current_{nullptr};
    std::unique_ptr<Snapshot> active_;
    std::vector<std::unique_ptr<Snapshot>> retired_;
    std::shared_ptr<TlsBootstrapIdentity> bootstrap_;
    TlsCertificateContentDigest content_digest_{};
    event::EventLoop::NotifyEntry reaper_entry_;
    std::atomic<bool> reaper_posted_{false};
    async::Watch<std::uint64_t> reclaim_epoch_{0};
    std::optional<async::Watch<std::uint64_t>::Publisher> reclaim_publisher_;
    std::uint64_t reclaim_epoch_value_ = 0;
    std::uint64_t version_ = 0;
    bool quic_enabled_ = false;
    bool shutting_down_ = false;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_TLS_CERTIFICATE_STORE_H
