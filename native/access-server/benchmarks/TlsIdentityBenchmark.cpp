#include "BenchmarkSupport.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <openssl/bio.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/obj.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <fiber/async/Spawn.h>
#include <fiber/async/Yield.h>
#include <fiber/event/EventLoopGroup.h>
#include <fiber/net/TlsContext.h>

#include "runtime/TlsCertificateStore.h"

namespace {

using fiber::access_server::AccessTlsMetricsObserver;
using fiber::access_server::AccessTlsReclaimObservation;
using fiber::access_server::AccessTlsReclaimTrigger;
using fiber::access_server::TlsCertificateConfig;
using fiber::access_server::TlsCertificateSnapshotConfig;
using fiber::access_server::TlsCertificateStore;
using fiber::access_server::TlsCertificateUpdateStatus;
using fiber::access_server::benchmark::Distribution;

constexpr std::uint64_t kDefaultSelectionOperations = 100'000;
constexpr std::uint64_t kDefaultRotations = 21;

struct IdentityPem {
    std::string certificate;
    std::string private_key;
};

template<typename T, auto Free>
using OpenSslPtr = std::unique_ptr<T, decltype(Free)>;

bool add_dns_name(GENERAL_NAMES &names, std::string_view dns_name) {
    OpenSslPtr<GENERAL_NAME, GENERAL_NAME_free> name(GENERAL_NAME_new(), &GENERAL_NAME_free);
    if (!name) {
        return false;
    }
    name->type = GEN_DNS;
    name->d.dNSName = ASN1_IA5STRING_new();
    if (name->d.dNSName == nullptr ||
        ASN1_STRING_set(name->d.dNSName, dns_name.data(), static_cast<int>(dns_name.size())) != 1 ||
        sk_GENERAL_NAME_push(&names, name.get()) <= 0) {
        return false;
    }
    (void) name.release();
    return true;
}

std::optional<IdentityPem> make_identity() {
    OpenSslPtr<EVP_PKEY_CTX, EVP_PKEY_CTX_free> key_context(EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr),
                                                            &EVP_PKEY_CTX_free);
    EVP_PKEY *raw_key = nullptr;
    if (!key_context || EVP_PKEY_keygen_init(key_context.get()) <= 0 ||
        EVP_PKEY_CTX_set_ec_paramgen_curve_nid(key_context.get(), NID_X9_62_prime256v1) <= 0 ||
        EVP_PKEY_keygen(key_context.get(), &raw_key) <= 0) {
        return std::nullopt;
    }
    OpenSslPtr<EVP_PKEY, EVP_PKEY_free> key(raw_key, &EVP_PKEY_free);
    OpenSslPtr<X509, X509_free> certificate(X509_new(), &X509_free);
    if (!certificate || X509_set_version(certificate.get(), X509_VERSION_3) != 1 ||
        ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 1) != 1 ||
        X509_gmtime_adj(X509_get_notBefore(certificate.get()), -60) == nullptr ||
        X509_gmtime_adj(X509_get_notAfter(certificate.get()), 3600) == nullptr ||
        X509_set_pubkey(certificate.get(), key.get()) != 1) {
        return std::nullopt;
    }

    constexpr std::string_view kCommonName = "api.example.com";
    X509_NAME *subject = X509_get_subject_name(certificate.get());
    if (subject == nullptr ||
        X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC,
                                   reinterpret_cast<const unsigned char *>(kCommonName.data()),
                                   static_cast<int>(kCommonName.size()), -1, 0) != 1 ||
        X509_set_issuer_name(certificate.get(), subject) != 1) {
        return std::nullopt;
    }

    OpenSslPtr<GENERAL_NAMES, GENERAL_NAMES_free> names(GENERAL_NAMES_new(), &GENERAL_NAMES_free);
    if (!names || !add_dns_name(*names, "api.example.com") || !add_dns_name(*names, "*.example.org") ||
        X509_add1_ext_i2d(certificate.get(), NID_subject_alt_name, names.get(), 0, 0) != 1 ||
        X509_sign(certificate.get(), key.get(), EVP_sha256()) <= 0) {
        return std::nullopt;
    }

    OpenSslPtr<BIO, BIO_free> certificate_bio(BIO_new(BIO_s_mem()), &BIO_free);
    OpenSslPtr<BIO, BIO_free> key_bio(BIO_new(BIO_s_mem()), &BIO_free);
    if (!certificate_bio || !key_bio || PEM_write_bio_X509(certificate_bio.get(), certificate.get()) != 1 ||
        PEM_write_bio_PrivateKey(key_bio.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr) != 1) {
        return std::nullopt;
    }
    char *certificate_data = nullptr;
    char *key_data = nullptr;
    const long certificate_size = BIO_get_mem_data(certificate_bio.get(), &certificate_data);
    const long key_size = BIO_get_mem_data(key_bio.get(), &key_data);
    if (certificate_size <= 0 || key_size <= 0) {
        return std::nullopt;
    }
    return IdentityPem{
            .certificate = std::string(certificate_data, static_cast<std::size_t>(certificate_size)),
            .private_key = std::string(key_data, static_cast<std::size_t>(key_size)),
    };
}

TlsCertificateSnapshotConfig make_config(std::uint64_t version, const IdentityPem &identity) {
    return {
            .version = version,
            .default_certificate = "default",
            .certificates = {TlsCertificateConfig{
                    .id = "default",
                    .certificate_pem = identity.certificate,
                    .private_key_pem = identity.private_key,
            }},
    };
}

struct ReclaimRecorder {
    [[nodiscard]] AccessTlsMetricsObserver observer() noexcept {
        return {
                .context = this,
                .on_rotation = &record_rotation,
                .on_reclaim = &record_reclaim,
        };
    }

    static void record_rotation(void *context) noexcept { ++static_cast<ReclaimRecorder *>(context)->rotations; }

    static void record_reclaim(void *context, const AccessTlsReclaimObservation &observation) noexcept {
        auto &recorder = *static_cast<ReclaimRecorder *>(context);
        ++recorder.reclaim_runs;
        recorder.reclaimed_snapshots += observation.reclaimed_snapshots;
        recorder.max_retention_ns = std::max(recorder.max_retention_ns,
                                             static_cast<std::uint64_t>(observation.max_reclaimed_retention.count()));
    }

    std::uint64_t rotations = 0;
    std::uint64_t reclaim_runs = 0;
    std::uint64_t reclaimed_snapshots = 0;
    std::uint64_t max_retention_ns = 0;
};

struct BenchmarkOutcome {
    std::array<Distribution, 3> selection;
    Distribution rotation;
    std::uint64_t checksum = 0;
    bool success = false;
};

fiber::async::DetachedTask run_benchmark(TlsCertificateStore *store, const TlsCertificateSnapshotConfig *initial,
                                         std::vector<TlsCertificateStore::PreparedUpdate> *prepared,
                                         std::uint64_t selection_operations, BenchmarkOutcome *outcome,
                                         std::promise<void> *done) {
    auto published = store->apply(*initial, "tls-benchmark-v1");
    if (!published || *published != TlsCertificateUpdateStatus::Published) {
        done->set_value();
        co_return;
    }

    const fiber::net::TlsIdentitySelectorOps selector = store->selector_ops();
    constexpr std::array<std::string_view, 3> kServerNames{
            "api.example.com",
            "tenant.example.org",
            "unmatched.example.net",
    };
    for (std::size_t case_index = 0; case_index < kServerNames.size(); ++case_index) {
        std::vector<std::uint64_t> elapsed;
        elapsed.reserve(fiber::access_server::benchmark::kDefaultSamples);
        for (std::size_t sample = 0; sample < fiber::access_server::benchmark::kDefaultSamples; ++sample) {
            std::uint64_t checksum = 0;
            const auto started = std::chrono::steady_clock::now();
            for (std::uint64_t operation = 0; operation < selection_operations; ++operation) {
                fiber::net::TlsContext *selected =
                        selector.select(selector.ctx, fiber::net::TlsIdentitySelectInput{
                                                              .server_name = kServerNames[case_index],
                                                              .transport = fiber::net::TlsTransportKind::Tcp,
                                                      });
                checksum += reinterpret_cast<std::uintptr_t>(selected) != 0 ? 1U : 0U;
            }
            elapsed.push_back(static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started)
                            .count()));
            outcome->checksum ^= checksum + sample;
        }
        outcome->selection[case_index] =
                fiber::access_server::benchmark::summarize(std::move(elapsed), selection_operations);
        co_await fiber::async::yield();
    }

    std::vector<std::uint64_t> rotation_elapsed;
    rotation_elapsed.reserve(prepared->size());
    for (auto &candidate: *prepared) {
        const auto started = std::chrono::steady_clock::now();
        fiber::net::TlsContext *selected =
                selector.select(selector.ctx, fiber::net::TlsIdentitySelectInput{
                                                      .server_name = "api.example.com",
                                                      .transport = fiber::net::TlsTransportKind::Tcp,
                                              });
        auto committed = store->commit(std::move(candidate));
        if (selected == nullptr || !committed || *committed != TlsCertificateUpdateStatus::Published) {
            co_await store->shutdown();
            done->set_value();
            co_return;
        }
        co_await fiber::async::yield();
        co_await fiber::async::yield();
        co_await fiber::async::yield();
        rotation_elapsed.push_back(static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started)
                        .count()));
    }
    outcome->rotation = fiber::access_server::benchmark::summarize(std::move(rotation_elapsed), 1);
    outcome->success = true;
    co_await store->shutdown();
    done->set_value();
}

void print_distribution(std::string_view name, std::uint64_t operations, const Distribution &distribution) {
    std::printf("%.*s,%llu,%.2f,%.2f,%.2f,%.0f\n", static_cast<int>(name.size()), name.data(),
                static_cast<unsigned long long>(operations), distribution.p50_ns_per_operation,
                distribution.p95_ns_per_operation, distribution.p99_ns_per_operation,
                distribution.operations_per_second);
}

} // namespace

int main(int argc, char **argv) {
    std::uint64_t selection_operations = kDefaultSelectionOperations;
    std::uint64_t rotations = kDefaultRotations;
    if ((argc >= 2 && !fiber::access_server::benchmark::parse_positive(argv[1], selection_operations)) ||
        (argc >= 3 && !fiber::access_server::benchmark::parse_positive(argv[2], rotations)) || argc > 3 ||
        rotations > std::numeric_limits<std::size_t>::max()) {
        std::fprintf(stderr, "usage: %s [selection-operations-per-sample] [rotations]\n", argv[0]);
        return 2;
    }

    const auto identity = make_identity();
    if (!identity) {
        std::fprintf(stderr, "failed to generate runtime TLS benchmark identity\n");
        return 1;
    }
    const TlsCertificateSnapshotConfig initial = make_config(1, *identity);

    std::vector<TlsCertificateStore::PreparedUpdate> prepared;
    prepared.reserve(static_cast<std::size_t>(rotations));
    std::vector<std::uint64_t> prepare_elapsed;
    prepare_elapsed.reserve(static_cast<std::size_t>(rotations));
    for (std::uint64_t rotation = 0; rotation < rotations; ++rotation) {
        TlsCertificateSnapshotConfig config = make_config(rotation + 2, *identity);
        const std::string wire = "tls-benchmark-v" + std::to_string(rotation + 2);
        const auto started = std::chrono::steady_clock::now();
        auto candidate = TlsCertificateStore::prepare(config, TlsCertificateStore::content_digest(wire), false, false);
        prepare_elapsed.push_back(static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started)
                        .count()));
        if (!candidate) {
            std::fprintf(stderr, "failed to prepare TLS rotation fixture\n");
            return 1;
        }
        prepared.push_back(std::move(*candidate));
    }
    const Distribution prepare = fiber::access_server::benchmark::summarize(std::move(prepare_elapsed), 1);

    fiber::event::EventLoopGroup workers(1);
    ReclaimRecorder recorder;
    TlsCertificateStore store(workers.at(0), workers, false, recorder.observer());
    BenchmarkOutcome outcome;
    std::promise<void> done_promise;
    auto done = done_promise.get_future();
    workers.start();
    fiber::async::spawn(workers.at(0), [&]() {
        return run_benchmark(&store, &initial, &prepared, selection_operations, &outcome, &done_promise);
    });
    if (done.wait_for(std::chrono::minutes(2)) != std::future_status::ready) {
        std::fprintf(stderr, "TLS benchmark timed out\n");
        workers.stop();
        workers.join();
        return 1;
    }
    workers.stop();
    workers.join();
    if (!outcome.success) {
        std::fprintf(stderr, "TLS benchmark failed\n");
        return 1;
    }

    std::printf("case,operations,p50_ns_per_operation,p95_ns_per_operation,p99_ns_per_operation,"
                "operations_per_second\n");
    print_distribution("select_exact", selection_operations, outcome.selection[0]);
    print_distribution("select_wildcard", selection_operations, outcome.selection[1]);
    print_distribution("select_default", selection_operations, outcome.selection[2]);
    print_distribution("prepare_snapshot", rotations, prepare);
    print_distribution("commit_and_reclaim", rotations, outcome.rotation);
    std::fprintf(stderr,
                 "samples=%zu rotations=%llu observed_rotations=%llu reclaim_runs=%llu reclaimed_snapshots=%llu "
                 "max_retention_ns=%llu checksum=%llu\n",
                 fiber::access_server::benchmark::kDefaultSamples, static_cast<unsigned long long>(rotations),
                 static_cast<unsigned long long>(recorder.rotations),
                 static_cast<unsigned long long>(recorder.reclaim_runs),
                 static_cast<unsigned long long>(recorder.reclaimed_snapshots),
                 static_cast<unsigned long long>(recorder.max_retention_ns),
                 static_cast<unsigned long long>(outcome.checksum));
    return recorder.rotations == rotations && recorder.reclaimed_snapshots >= rotations ? 0 : 1;
}
