#include "TlsCertificateStore.h"

#include <algorithm>
#include <climits>
#include <memory>
#include <utility>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/mem.h>
#include <openssl/pem.h>
#include <openssl/sha.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <fiber/common/Assert.h>

namespace fiber::access_server {
namespace {

using BioPtr = std::unique_ptr<BIO, decltype(&BIO_free)>;
using X509Ptr = std::unique_ptr<X509, decltype(&X509_free)>;
using GeneralNamesPtr = std::unique_ptr<GENERAL_NAMES, decltype(&GENERAL_NAMES_free)>;

TlsCertificateConfigError config_error(TlsCertificateConfigErrorCode code, std::string field, std::string message) {
    return TlsCertificateConfigError{
            .code = code,
            .field = std::move(field),
            .message = std::move(message),
    };
}

std::string ascii_lower(std::string_view value) {
    std::string result(value);
    for (char &ch: result) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch + ('a' - 'A'));
        }
    }
    return result;
}

bool valid_dns_name(std::string_view name, bool wildcard) noexcept {
    if (name.empty() || name.size() > 253 || name.front() == '.' || name.back() == '.') {
        return false;
    }
    if (wildcard) {
        if (!name.starts_with("*.") || name.size() < 5 || name.substr(2).find('.') == std::string_view::npos) {
            return false;
        }
        name.remove_prefix(2);
    } else if (name.find('*') != std::string_view::npos) {
        return false;
    }
    std::size_t label_size = 0;
    for (char ch: name) {
        if (ch == '.') {
            if (label_size == 0 || label_size > 63) {
                return false;
            }
            label_size = 0;
            continue;
        }
        const bool valid =
                (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-';
        if (!valid) {
            return false;
        }
        ++label_size;
    }
    return label_size > 0 && label_size <= 63;
}

std::expected<std::vector<std::string>, TlsCertificateConfigError> certificate_dns_names(std::string_view pem,
                                                                                         std::string_view field) {
    BioPtr bio(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())), &BIO_free);
    if (!bio) {
        return std::unexpected(config_error(TlsCertificateConfigErrorCode::InvalidCertificate, std::string(field),
                                            "failed to allocate certificate parser"));
    }
    X509Ptr leaf(PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr), &X509_free);
    if (!leaf) {
        ERR_clear_error();
        return std::unexpected(config_error(TlsCertificateConfigErrorCode::InvalidCertificate, std::string(field),
                                            "certificate PEM does not contain a certificate"));
    }
    if (X509_cmp_current_time(X509_get0_notBefore(leaf.get())) > 0 ||
        X509_cmp_current_time(X509_get0_notAfter(leaf.get())) <= 0) {
        return std::unexpected(config_error(TlsCertificateConfigErrorCode::InvalidCertificate, std::string(field),
                                            "leaf certificate is not currently valid"));
    }

    GeneralNamesPtr names(
            static_cast<GENERAL_NAMES *>(X509_get_ext_d2i(leaf.get(), NID_subject_alt_name, nullptr, nullptr)),
            &GENERAL_NAMES_free);
    std::vector<std::string> result;
    if (!names) {
        ERR_clear_error();
        return result;
    }
    const int count = sk_GENERAL_NAME_num(names.get());
    if (count < 0 || static_cast<std::size_t>(count) > kMaxTlsDnsNamesPerCertificate) {
        return std::unexpected(config_error(TlsCertificateConfigErrorCode::LimitExceeded, std::string(field),
                                            "leaf certificate has more than 64 subjectAltName entries"));
    }
    result.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        const GENERAL_NAME *name = sk_GENERAL_NAME_value(names.get(), i);
        if (!name || name->type != GEN_DNS) {
            continue;
        }
        const ASN1_STRING *dns = name->d.dNSName;
        const int length = ASN1_STRING_length(dns);
        const auto *data = ASN1_STRING_get0_data(dns);
        if (length <= 0 || !data || std::find(data, data + length, 0) != data + length) {
            return std::unexpected(config_error(TlsCertificateConfigErrorCode::InvalidDnsName, std::string(field),
                                                "leaf certificate contains an invalid DNS SAN"));
        }
        std::string_view value(reinterpret_cast<const char *>(data), static_cast<std::size_t>(length));
        const bool wildcard = value.starts_with("*.");
        if (!valid_dns_name(value, wildcard)) {
            return std::unexpected(config_error(TlsCertificateConfigErrorCode::InvalidDnsName, std::string(field),
                                                "leaf certificate contains an unsupported DNS SAN"));
        }
        result.push_back(ascii_lower(value));
    }
    return result;
}

int compare_ascii_case_insensitive(std::string_view left, std::string_view right) noexcept {
    const std::size_t common = std::min(left.size(), right.size());
    for (std::size_t i = 0; i < common; ++i) {
        unsigned char l = static_cast<unsigned char>(left[i]);
        unsigned char r = static_cast<unsigned char>(right[i]);
        if (l >= 'A' && l <= 'Z') {
            l = static_cast<unsigned char>(l + ('a' - 'A'));
        }
        if (r >= 'A' && r <= 'Z') {
            r = static_cast<unsigned char>(r + ('a' - 'A'));
        }
        if (l != r) {
            return l < r ? -1 : 1;
        }
    }
    return left.size() == right.size() ? 0 : (left.size() < right.size() ? -1 : 1);
}

} // namespace

class TlsCertificateStore::Snapshot {
public:
    struct Identity {
        std::string id;
        std::shared_ptr<const UpstreamTlsClientIdentity> client;
    };

    struct NameEntry {
        std::string name;
        Identity *identity = nullptr;
    };

    [[nodiscard]] const net::TlsCredential *select(std::string_view server_name) noexcept {
        Identity *identity = nullptr;
        if (!server_name.empty()) {
            identity = find(exact_names, server_name);
            if (!identity) {
                const std::size_t dot = server_name.find('.');
                if (dot != std::string_view::npos && dot + 1 < server_name.size()) {
                    identity = find(wildcard_suffixes, server_name.substr(dot + 1));
                }
            }
        }
        if (!identity) {
            identity = default_identity;
        }
        return &identity->client->credential();
    }

    static Identity *find(const std::vector<NameEntry> &entries, std::string_view name) noexcept {
        std::size_t first = 0;
        std::size_t last = entries.size();
        while (first < last) {
            const std::size_t middle = first + (last - first) / 2;
            if (compare_ascii_case_insensitive(entries[middle].name, name) < 0) {
                first = middle + 1;
            } else {
                last = middle;
            }
        }
        if (first < entries.size() && compare_ascii_case_insensitive(entries[first].name, name) == 0) {
            return entries[first].identity;
        }
        return nullptr;
    }

    [[nodiscard]] const Identity *find_client(std::string_view id) const noexcept {
        const auto found =
                std::lower_bound(client_index.begin(), client_index.end(), id,
                                 [](const Identity *entry, std::string_view value) { return entry->id < value; });
        return found != client_index.end() && (*found)->id == id ? *found : nullptr;
    }

    std::uint64_t version = 0;
    std::vector<Identity> identities;
    std::vector<NameEntry> exact_names;
    std::vector<NameEntry> wildcard_suffixes;
    std::vector<Identity *> client_index;
    Identity *default_identity = nullptr;
};

TlsCertificateStore::PreparedUpdate::PreparedUpdate(PreparedUpdate &&other) noexcept = default;

TlsCertificateStore::PreparedUpdate &
TlsCertificateStore::PreparedUpdate::operator=(PreparedUpdate &&other) noexcept = default;

TlsCertificateStore::PreparedUpdate::~PreparedUpdate() = default;

namespace {

std::expected<std::unique_ptr<TlsCertificateStore::Snapshot>, TlsCertificateConfigError>
compile_snapshot(const TlsCertificateSnapshotConfig &config, bool quic_enabled) {
    (void) quic_enabled;
    auto snapshot = std::make_unique<TlsCertificateStore::Snapshot>();
    snapshot->version = config.version;
    snapshot->identities.reserve(config.certificates.size());
    std::vector<std::vector<std::string>> dns_names;
    dns_names.reserve(config.certificates.size());
    std::size_t total_dns_names = 0;
    for (std::size_t i = 0; i < config.certificates.size(); ++i) {
        const auto &source = config.certificates[i];
        const std::string field = "certificates[" + std::to_string(i) + "]";
        auto names = certificate_dns_names(source.certificate_pem, field + ".certificatePem");
        if (!names) {
            return std::unexpected(std::move(names.error()));
        }
        total_dns_names += names->size();
        if (total_dns_names > kMaxTlsDnsNames) {
            return std::unexpected(config_error(TlsCertificateConfigErrorCode::LimitExceeded, "certificates",
                                                "snapshot has more than 8192 DNS SAN entries"));
        }
        auto client = UpstreamTlsClientIdentity::create(source.certificate_pem, source.private_key_pem);
        if (!client) {
            return std::unexpected(config_error(TlsCertificateConfigErrorCode::InvalidPrivateKey, field,
                                                "private key is invalid or does not match the leaf certificate"));
        }
        snapshot->identities.push_back(TlsCertificateStore::Snapshot::Identity{
                .id = source.id,
                .client = std::move(*client),
        });
        dns_names.push_back(std::move(*names));
    }
    snapshot->client_index.reserve(snapshot->identities.size());
    for (auto &identity: snapshot->identities) {
        snapshot->client_index.push_back(&identity);
    }
    std::sort(snapshot->client_index.begin(), snapshot->client_index.end(),
              [](const auto *left, const auto *right) { return left->id < right->id; });
    for (std::size_t i = 0; i < snapshot->identities.size(); ++i) {
        auto &identity = snapshot->identities[i];
        if (identity.id == config.default_certificate) {
            snapshot->default_identity = &identity;
        }
        for (std::string &name: dns_names[i]) {
            if (name.starts_with("*.")) {
                snapshot->wildcard_suffixes.push_back({.name = name.substr(2), .identity = &identity});
            } else {
                snapshot->exact_names.push_back({.name = std::move(name), .identity = &identity});
            }
        }
    }
    if (!snapshot->default_identity) {
        return std::unexpected(config_error(TlsCertificateConfigErrorCode::DefaultCertificateNotFound,
                                            "defaultCertificate", "default certificate id is not present"));
    }
    const auto sort_and_validate = [](std::vector<TlsCertificateStore::Snapshot::NameEntry> &entries,
                                      std::string_view field) -> std::optional<TlsCertificateConfigError> {
        std::sort(entries.begin(), entries.end(),
                  [](const auto &left, const auto &right) { return left.name < right.name; });
        for (std::size_t i = 1; i < entries.size(); ++i) {
            if (entries[i - 1].name == entries[i].name) {
                return config_error(TlsCertificateConfigErrorCode::DuplicateDnsName, std::string(field),
                                    "DNS SAN is claimed by more than one certificate");
            }
        }
        return std::nullopt;
    };
    if (auto error = sort_and_validate(snapshot->exact_names, "certificates.certificatePem")) {
        return std::unexpected(std::move(*error));
    }
    if (auto error = sort_and_validate(snapshot->wildcard_suffixes, "certificates.certificatePem")) {
        return std::unexpected(std::move(*error));
    }
    return snapshot;
}

} // namespace

TlsBootstrapIdentity::~TlsBootstrapIdentity() = default;

std::shared_ptr<TlsBootstrapIdentity> TlsBootstrapIdentity::create() {
    return std::shared_ptr<TlsBootstrapIdentity>(new TlsBootstrapIdentity());
}

TlsCertificateContentDigest TlsCertificateStore::content_digest(std::string_view wire_content) noexcept {
    TlsCertificateContentDigest digest{};
    SHA256(reinterpret_cast<const std::uint8_t *>(wire_content.data()), wire_content.size(), digest.data());
    return digest;
}

std::expected<TlsCertificateStore::PreparedUpdate, TlsCertificateConfigError>
TlsCertificateStore::prepare(const TlsCertificateSnapshotConfig &config, TlsCertificateContentDigest digest,
                             bool quic_enabled, bool prepare_bootstrap) {
    auto candidate = compile_snapshot(config, quic_enabled);
    if (!candidate) {
        return std::unexpected(std::move(candidate.error()));
    }

    PreparedUpdate prepared;
    prepared.version_ = config.version;
    prepared.content_digest_ = digest;
    prepared.snapshot_ = std::move(*candidate);
    if (prepare_bootstrap) {
        prepared.bootstrap_ = TlsBootstrapIdentity::create();
    }
    return prepared;
}

void TlsBootstrapIdentity::close() noexcept {}

TlsCertificateStore::TlsCertificateStore(event::EventLoop &owner_loop, event::EventLoopGroup &workers,
                                         bool quic_enabled, AccessTlsMetricsObserver metrics_observer,
                                         TlsCertificateIdentityObserver observer) :
    owner_loop_(&owner_loop), workers_(&workers), metrics_observer_(metrics_observer), identity_observer_(observer),
    quic_enabled_(quic_enabled) {
    worker_slots_.reserve(workers.size());
    for (std::size_t i = 0; i < workers.size(); ++i) {
        auto slot = std::make_unique<WorkerSlot>();
        slot->store = this;
        worker_slots_.push_back(std::move(slot));
    }
    reclaim_publisher_ = reclaim_epoch_.acquire_publisher();
    FIBER_ASSERT(reclaim_publisher_.has_value());
}

TlsCertificateStore::~TlsCertificateStore() {
    FIBER_ASSERT(current_.load(std::memory_order_relaxed) == nullptr);
    FIBER_ASSERT(!active_);
    FIBER_ASSERT(retired_.empty());
    FIBER_ASSERT(!retirement_pending_.load(std::memory_order_relaxed));
    FIBER_ASSERT(!reaper_posted_.load(std::memory_order_relaxed));
}

std::expected<TlsCertificateUpdateStatus, TlsCertificateConfigError>
TlsCertificateStore::apply(const TlsCertificateSnapshotConfig &config, std::string_view wire_content) {
    FIBER_ASSERT(owner_loop_->in_loop());
    const TlsCertificateContentDigest digest = content_digest(wire_content);
    if (TlsCertificateClassification classified = classify(config.version, digest)) {
        return std::move(*classified);
    }
    auto prepared = prepare(config, digest, quic_enabled_, !bootstrap_);
    if (!prepared) {
        return std::unexpected(std::move(prepared.error()));
    }
    return commit(std::move(*prepared));
}

TlsCertificateVersionState TlsCertificateStore::version_state() const noexcept {
    FIBER_ASSERT(owner_loop_->in_loop());
    return TlsCertificateVersionState{
            .active = active_ != nullptr,
            .version = version_,
            .content_digest = content_digest_,
    };
}

TlsCertificateClassification TlsCertificateStore::classify(std::uint64_t version,
                                                           const TlsCertificateContentDigest &digest) const {
    FIBER_ASSERT(owner_loop_->in_loop());
    if (!active_) {
        return std::nullopt;
    }
    if (version < version_) {
        return TlsCertificateClassification(std::in_place, TlsCertificateUpdateStatus::IgnoredOlderVersion);
    }
    if (version == version_) {
        if (digest == content_digest_) {
            return TlsCertificateClassification(std::in_place, TlsCertificateUpdateStatus::VersionUnchanged);
        }
        return TlsCertificateClassification(
                std::in_place, std::unexpected(config_error(TlsCertificateConfigErrorCode::VersionConflict, "version",
                                                            "same TLS snapshot version has different content")));
    }
    return std::nullopt;
}

std::expected<TlsCertificateUpdateStatus, TlsCertificateConfigError>
TlsCertificateStore::commit(PreparedUpdate prepared) {
    FIBER_ASSERT(owner_loop_->in_loop());
    if (TlsCertificateClassification classified = classify(prepared.version_, prepared.content_digest_)) {
        return std::move(*classified);
    }
    if (!prepared.snapshot_) {
        return std::unexpected(config_error(TlsCertificateConfigErrorCode::InvalidField, "snapshot",
                                            "prepared TLS snapshot is empty"));
    }
    if (active_) {
        for (const Snapshot::Identity *candidate: prepared.snapshot_->client_index) {
            const Snapshot::Identity *current = active_->find_client(candidate->id);
            if (current && current->client->digest() != candidate->client->digest()) {
                return std::unexpected(config_error(TlsCertificateConfigErrorCode::VersionConflict, "certificates.id",
                                                    "immutable TLS certificate id has different content"));
            }
        }
    }
    if (!bootstrap_) {
        if (!prepared.bootstrap_) {
            return std::unexpected(config_error(TlsCertificateConfigErrorCode::InvalidField, "defaultCertificate",
                                                "prepared TLS snapshot has no bootstrap identity"));
        }
        bootstrap_ = std::move(prepared.bootstrap_);
    }

    Snapshot *old = current_.exchange(prepared.snapshot_.get(), std::memory_order_acq_rel);
    if (active_) {
        FIBER_ASSERT(old == active_.get());
        retired_.push_back(RetiredSnapshot{
                .snapshot = std::move(active_),
                .retired_at = event::EventLoop::current().now(),
        });
        // Publish pending before the scan. A hazard cleared before the scan is
        // observed as null; one cleared after a retained hazard was observed
        // sees pending and schedules the next scan.
        retirement_pending_.store(true, std::memory_order_release);
        metrics_observer_.record_rotation();
    }
    active_ = std::move(prepared.snapshot_);
    version_ = prepared.version_;
    content_digest_ = prepared.content_digest_;
    if (!retired_.empty()) {
        reclaim_retired(AccessTlsReclaimTrigger::Publication);
    }
    if (identity_observer_.on_update) {
        identity_observer_.on_update(identity_observer_.context);
    }
    return TlsCertificateUpdateStatus::Published;
}

net::TlsServerParam TlsCertificateStore::tls_server_param() noexcept {
    net::TlsServerParam param;
    param.configure_callback = &configure_handshake;
    param.configure_ctx = this;
    return param;
}

UpstreamTlsClientIdentityResolver TlsCertificateStore::client_identity_resolver() noexcept {
    return UpstreamTlsClientIdentityResolver{
            .context = this,
            .find = &find_client_identity,
    };
}

std::shared_ptr<const UpstreamTlsClientIdentity>
TlsCertificateStore::find_client_identity(void *context, std::string_view id) noexcept {
    auto &store = *static_cast<TlsCertificateStore *>(context);
    FIBER_ASSERT(store.owner_loop_->in_loop());
    if (!store.active_) {
        return {};
    }
    const Snapshot::Identity *identity = store.active_->find_client(id);
    return identity ? identity->client : std::shared_ptr<const UpstreamTlsClientIdentity>{};
}

std::size_t TlsCertificateStore::certificate_count() const noexcept { return active_ ? active_->identities.size() : 0; }

common::IoErr TlsCertificateStore::configure_handshake(void *context, net::TlsServerHandshakeConfig &config,
                                                       const net::TlsClientHelloView &client_hello) noexcept {
    auto &store = *static_cast<TlsCertificateStore *>(context);
    const net::TlsCredential *selected = store.select_credential(client_hello.server_name);
    return selected ? config.add_credential(*selected) : common::IoErr::Invalid;
}

const net::TlsCredential *TlsCertificateStore::select_credential(std::string_view server_name) noexcept {
    event::EventLoop *loop = event::EventLoop::current_or_null();
    if (!loop || !loop->has_group_index() || loop->group() != workers_) {
        return nullptr;
    }
    WorkerSlot &slot = *worker_slots_[loop->group_index()];
    Snapshot *snapshot;
    do {
        snapshot = current_.load(std::memory_order_acquire);
        if (!snapshot) {
            return nullptr;
        }
        slot.hazard.store(snapshot, std::memory_order_seq_cst);
    } while (snapshot != current_.load(std::memory_order_seq_cst));
    const net::TlsCredential *selected = snapshot->select(server_name);
    loop->post_local<WorkerSlot, &WorkerSlot::clear_entry, &clear_hazard>(slot);
    return selected;
}

void TlsCertificateStore::clear_hazard(WorkerSlot *slot) noexcept {
    slot->hazard.store(nullptr, std::memory_order_seq_cst);
    slot->store->request_reclaim();
}

void TlsCertificateStore::request_reclaim() noexcept {
    if (!retirement_pending_.load(std::memory_order_acquire)) {
        return;
    }
    if (!reaper_posted_.exchange(true, std::memory_order_acq_rel)) {
        owner_loop_->post<TlsCertificateStore, &TlsCertificateStore::reaper_entry_, &run_reaper>(*this);
    }
}

void TlsCertificateStore::run_reaper(TlsCertificateStore *store) noexcept {
    store->reaper_posted_.store(false, std::memory_order_release);
    store->reclaim_retired(AccessTlsReclaimTrigger::HazardClear);
}

void TlsCertificateStore::reclaim_retired(AccessTlsReclaimTrigger trigger) noexcept {
    FIBER_ASSERT(owner_loop_->in_loop());
    const auto now = event::EventLoop::current().now();
    std::chrono::nanoseconds max_reclaimed_retention{0};
    const std::size_t before = retired_.size();
    retired_.erase(std::remove_if(retired_.begin(), retired_.end(),
                                  [&](const RetiredSnapshot &retired) {
                                      for (const auto &slot: worker_slots_) {
                                          if (slot->hazard.load(std::memory_order_seq_cst) == retired.snapshot.get()) {
                                              return false;
                                          }
                                      }
                                      if (now > retired.retired_at) {
                                          max_reclaimed_retention =
                                                  std::max(max_reclaimed_retention,
                                                           std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                                   now - retired.retired_at));
                                      }
                                      return true;
                                  }),
                   retired_.end());
    retirement_pending_.store(!retired_.empty(), std::memory_order_release);
    metrics_observer_.record_reclaim(AccessTlsReclaimObservation{
            .trigger = trigger,
            .reclaimed_snapshots = before - retired_.size(),
            .retired_snapshots = retired_.size(),
            .oldest_retired_at =
                    retired_.empty() ? std::chrono::steady_clock::time_point{} : retired_.front().retired_at,
            .max_reclaimed_retention = max_reclaimed_retention,
    });
    ++reclaim_epoch_value_;
    reclaim_publisher_->publish(reclaim_epoch_value_);
}

async::Task<void> TlsCertificateStore::shutdown() noexcept {
    FIBER_ASSERT(owner_loop_->in_loop());
    if (shutting_down_) {
        co_return;
    }
    shutting_down_ = true;
    Snapshot *old = current_.exchange(nullptr, std::memory_order_acq_rel);
    if (active_) {
        FIBER_ASSERT(old == active_.get());
        retired_.push_back(RetiredSnapshot{
                .snapshot = std::move(active_),
                .retired_at = event::EventLoop::current().now(),
        });
        retirement_pending_.store(true, std::memory_order_release);
    }
    auto subscriber = reclaim_epoch_.subscribe();
    auto epoch = subscriber.current();
    reclaim_retired(AccessTlsReclaimTrigger::Shutdown);
    while (!retired_.empty() || reaper_posted_.load(std::memory_order_acquire)) {
        epoch = co_await subscriber.next(epoch.version);
    }
    bootstrap_.reset();
}

} // namespace fiber::access_server
