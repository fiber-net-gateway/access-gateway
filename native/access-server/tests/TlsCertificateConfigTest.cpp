#include <gtest/gtest.h>

#include "config/TlsCertificateConfig.h"
#include "runtime/AccessConfigCompiler.h"
#include "runtime/TlsCertificateStore.h"
#include "runtime/TlsCertificateWatcher.h"

#include <expected>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <openssl/bio.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/obj.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <fiber/async/Spawn.h>
#include <fiber/async/Watch.h>
#include <fiber/event/EventLoop.h>
#include <fiber/event/EventLoopGroup.h>
#include <fiber/nacos/ConfigService.h>
#include <fiber/nacos/Subscription.h>

#include "NacosSnapshotTestBuilder.h"
#include "NacosSubscriptionStub.h"

namespace fiber::access_server {
namespace {

class FakeTlsConfigService final : public nacos::ConfigService {
public:
    using Result = nacos::SubscriptionResult<nacos::ConfigData>;

    common::IoResult<void> start() noexcept override { return {}; }
    async::Task<void> shutdown() noexcept override { co_return; }

    async::Task<std::expected<std::shared_ptr<const nacos::ConfigData>, nacos::ConfigServiceError>>
    get_config(std::string, std::string) noexcept override {
        co_return tests::make_config_data(nacos::ConfigState::NotFound);
    }

    async::Task<std::expected<void, nacos::ConfigServiceError>>
    publish(std::string, std::string, std::string, nacos::ConfigType, std::optional<std::string>) noexcept override {
        co_return std::expected<void, nacos::ConfigServiceError>{};
    }

    async::Task<std::expected<void, nacos::ConfigServiceError>> remove_config(std::string,
                                                                              std::string) noexcept override {
        co_return std::expected<void, nacos::ConfigServiceError>{};
    }

    std::expected<nacos::Subscription<nacos::ConfigData>, nacos::ConfigServiceError>
    subscribe(std::string_view data_id, std::string_view group,
              nacos::Subscription<nacos::ConfigData>::NotifyCallback on_notify, void *context) override {
        EXPECT_EQ(data_id, kTlsCertificatesDataId);
        EXPECT_EQ(group, kTlsCertificatesGroup);
        return subscriptions_.subscribe(on_notify, context);
    }

    void push(std::string content, std::string md5) {
        subscriptions_.publish(Result{
                .kind = nacos::ResultKind::Success,
                .data = tests::make_config_data(nacos::ConfigState::Present, std::move(md5), std::move(content)),
        });
    }

private:
    tests::NacosSubscriptionStub<nacos::ConfigData> subscriptions_;
};

std::string json_string(std::string_view value) {
    std::string result;
    result.reserve(value.size() + 2);
    result.push_back('"');
    for (char ch: value) {
        switch (ch) {
            case '\\':
                result.append("\\\\");
                break;
            case '"':
                result.append("\\\"");
                break;
            case '\n':
                result.append("\\n");
                break;
            case '\r':
                result.append("\\r");
                break;
            case '\t':
                result.append("\\t");
                break;
            default:
                result.push_back(ch);
                break;
        }
    }
    result.push_back('"');
    return result;
}

std::string tls_snapshot(std::uint64_t version, std::string_view certificate_pem, std::string_view private_key_pem) {
    return std::string("{\"schemaVersion\":1,\"version\":") + std::to_string(version) +
           ",\"defaultCertificate\":\"default\",\"certificates\":[{\"id\":\"default\",\"certificatePem\":" +
           json_string(certificate_pem) + ",\"privateKeyPem\":" + json_string(private_key_pem) + "}]}";
}

async::Task<void> wait_for_bool(async::Watch<bool>::Subscriber &subscriber, async::Watch<bool>::Snapshot &snapshot,
                                bool expected) {
    snapshot = subscriber.current();
    while (!snapshot.value || *snapshot.value != expected) {
        snapshot = co_await subscriber.next(snapshot.version);
    }
}

struct DeferredTlsStoreStep {
    event::EventLoop *loop = nullptr;
    event::EventLoop::DeferEntry defer_entry;
    event::EventLoop::NotifyEntry notify_entry;
    std::function<void()> callback;

    void schedule() noexcept {
        loop->post_local<DeferredTlsStoreStep, &DeferredTlsStoreStep::defer_entry, &enqueue>(*this);
    }

    static void enqueue(DeferredTlsStoreStep *step) noexcept {
        step->loop->post<DeferredTlsStoreStep, &DeferredTlsStoreStep::notify_entry, &run>(*step);
    }

    static void run(DeferredTlsStoreStep *step) noexcept { step->callback(); }
};

std::pair<std::string, std::string> make_test_identity() {
    using BioPtr = std::unique_ptr<BIO, decltype(&BIO_free)>;
    using EcKeyPtr = std::unique_ptr<EC_KEY, decltype(&EC_KEY_free)>;
    using KeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
    using X509Ptr = std::unique_ptr<X509, decltype(&X509_free)>;

    EcKeyPtr ec_key(EC_KEY_new_by_curve_name(NID_X9_62_prime256v1), &EC_KEY_free);
    EXPECT_NE(ec_key, nullptr);
    EXPECT_EQ(EC_KEY_generate_key(ec_key.get()), 1);
    KeyPtr key(EVP_PKEY_new(), &EVP_PKEY_free);
    EXPECT_NE(key, nullptr);
    EXPECT_EQ(EVP_PKEY_assign_EC_KEY(key.get(), ec_key.release()), 1);

    X509Ptr certificate(X509_new(), &X509_free);
    EXPECT_NE(certificate, nullptr);
    EXPECT_EQ(X509_set_version(certificate.get(), X509_VERSION_3), 1);
    EXPECT_EQ(ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 1), 1);
    EXPECT_NE(X509_gmtime_adj(X509_get_notBefore(certificate.get()), -60), nullptr);
    EXPECT_NE(X509_gmtime_adj(X509_get_notAfter(certificate.get()), 3600), nullptr);
    EXPECT_EQ(X509_set_pubkey(certificate.get(), key.get()), 1);
    X509_NAME *subject = X509_get_subject_name(certificate.get());
    EXPECT_NE(subject, nullptr);
    EXPECT_EQ(X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC,
                                         reinterpret_cast<const unsigned char *>("api.example.com"), -1, -1, 0),
              1);
    EXPECT_EQ(X509_set_issuer_name(certificate.get(), subject), 1);
    std::unique_ptr<GENERAL_NAMES, decltype(&GENERAL_NAMES_free)> names(GENERAL_NAMES_new(), &GENERAL_NAMES_free);
    EXPECT_NE(names, nullptr);
    for (std::string_view dns_name: {std::string_view("api.example.com"), std::string_view("*.example.org")}) {
        GENERAL_NAME *name = GENERAL_NAME_new();
        EXPECT_NE(name, nullptr);
        name->type = GEN_DNS;
        name->d.dNSName = ASN1_IA5STRING_new();
        EXPECT_NE(name->d.dNSName, nullptr);
        EXPECT_EQ(ASN1_STRING_set(name->d.dNSName, dns_name.data(), static_cast<int>(dns_name.size())), 1);
        EXPECT_GT(sk_GENERAL_NAME_push(names.get(), name), 0);
    }
    EXPECT_EQ(X509_add1_ext_i2d(certificate.get(), NID_subject_alt_name, names.get(), 0, 0), 1);
    EXPECT_GT(X509_sign(certificate.get(), key.get(), EVP_sha256()), 0);

    BioPtr certificate_bio(BIO_new(BIO_s_mem()), &BIO_free);
    BioPtr key_bio(BIO_new(BIO_s_mem()), &BIO_free);
    EXPECT_EQ(PEM_write_bio_X509(certificate_bio.get(), certificate.get()), 1);
    EXPECT_EQ(PEM_write_bio_PrivateKey(key_bio.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr), 1);
    char *certificate_data = nullptr;
    char *key_data = nullptr;
    const long certificate_size = BIO_get_mem_data(certificate_bio.get(), &certificate_data);
    const long key_size = BIO_get_mem_data(key_bio.get(), &key_data);
    EXPECT_GT(certificate_size, 0);
    EXPECT_GT(key_size, 0);
    return {
            std::string(certificate_data, static_cast<std::size_t>(certificate_size)),
            std::string(key_data, static_cast<std::size_t>(key_size)),
    };
}

TEST(TlsCertificateConfigTest, ParsesCompleteVersionedSnapshotWithoutSniRules) {
    auto parsed = parse_tls_certificate_config(
            R"({"schemaVersion":1,"version":42,"defaultCertificate":"version-a","certificates":[)"
            R"({"id":"version-a","certificatePem":"CERT-A","privateKeyPem":"KEY-A"},)"
            R"({"id":"version-b","certificatePem":"CERT-B","privateKeyPem":"KEY-B"}]})");

    ASSERT_TRUE(parsed);
    ASSERT_TRUE(*parsed);
    EXPECT_EQ((*parsed)->version, 42u);
    EXPECT_EQ((*parsed)->default_certificate, "version-a");
    ASSERT_EQ((*parsed)->certificates.size(), 2u);
    EXPECT_EQ((*parsed)->certificates[1].id, "version-b");
    EXPECT_EQ((*parsed)->certificates[1].private_key_pem, "KEY-B");
}

TEST(TlsCertificateConfigTest, EmptyAndNullRetainTheCurrentSnapshot) {
    auto empty = parse_tls_certificate_config("");
    ASSERT_TRUE(empty);
    EXPECT_FALSE(*empty);

    auto null = parse_tls_certificate_config("null");
    ASSERT_TRUE(null);
    EXPECT_FALSE(*null);
}

TEST(TlsCertificateConfigTest, RejectsUnknownDuplicateAndOversizedFields) {
    auto unknown = parse_tls_certificate_config(
            R"({"schemaVersion":1,"version":1,"defaultCertificate":"a","certificates":[],"sniRules":[]})");
    ASSERT_FALSE(unknown);
    EXPECT_EQ(unknown.error().code, TlsCertificateConfigErrorCode::InvalidField);

    auto duplicate = parse_tls_certificate_config(
            R"({"schemaVersion":1,"version":1,"version":2,"defaultCertificate":"a","certificates":[]})");
    ASSERT_FALSE(duplicate);
    EXPECT_EQ(duplicate.error().code, TlsCertificateConfigErrorCode::DuplicateField);

    std::string content(kMaxTlsSnapshotBytes + 1, 'x');
    auto oversized = parse_tls_certificate_config(content);
    ASSERT_FALSE(oversized);
    EXPECT_EQ(oversized.error().code, TlsCertificateConfigErrorCode::LimitExceeded);
}

TEST(TlsCertificateStoreTest, PublishesOnlyIncreasingValidSnapshotsAndKeepsNoPemCopies) {
    auto [certificate_pem, private_key_pem] = make_test_identity();
    TlsCertificateSnapshotConfig config{
            .version = 7,
            .default_certificate = "version-a",
            .certificates = {{
                    .id = "version-a",
                    .certificate_pem = certificate_pem,
                    .private_key_pem = private_key_pem,
            }},
    };
    event::EventLoop owner_loop;
    event::EventLoopGroup workers(1);
    TlsCertificateStore store(owner_loop, workers, false);
    bool completed = false;

    async::spawn(owner_loop, [&]() -> async::DetachedTask {
        auto published = store.apply(config, "wire-v7");
        EXPECT_TRUE(published);
        if (published) {
            EXPECT_EQ(*published, TlsCertificateUpdateStatus::Published);
        }
        EXPECT_EQ(store.version(), 7u);
        EXPECT_EQ(store.certificate_count(), 1u);
        EXPECT_TRUE(store.bootstrap_identity());

        auto unchanged = store.apply(config, "wire-v7");
        EXPECT_TRUE(unchanged);
        if (unchanged) {
            EXPECT_EQ(*unchanged, TlsCertificateUpdateStatus::VersionUnchanged);
        }

        auto conflict = store.apply(config, "different-wire-v7");
        EXPECT_FALSE(conflict);
        if (!conflict) {
            EXPECT_EQ(conflict.error().code, TlsCertificateConfigErrorCode::VersionConflict);
        }
        EXPECT_EQ(store.version(), 7u);

        config.version = 8;
        config.certificates.front().private_key_pem = "not-a-private-key";
        auto invalid = store.apply(config, "wire-v8");
        EXPECT_FALSE(invalid);
        EXPECT_EQ(store.version(), 7u);
        EXPECT_EQ(store.certificate_count(), 1u);

        config.version = 6;
        config.certificates.front().private_key_pem = private_key_pem;
        auto older = store.apply(config, "wire-v6");
        EXPECT_TRUE(older);
        if (older) {
            EXPECT_EQ(*older, TlsCertificateUpdateStatus::IgnoredOlderVersion);
        }
        EXPECT_EQ(store.version(), 7u);

        co_await store.shutdown();
        completed = true;
        owner_loop.stop();
    });
    owner_loop.run();
    EXPECT_TRUE(completed);
}

TEST(TlsCertificateStoreTest, PostsReaperOnlyForSnapshotsStillHeldByHazards) {
    using namespace std::chrono_literals;

    auto [certificate_pem, private_key_pem] = make_test_identity();
    TlsCertificateSnapshotConfig config{
            .version = 1,
            .default_certificate = "default",
            .certificates = {{
                    .id = "default",
                    .certificate_pem = std::move(certificate_pem),
                    .private_key_pem = std::move(private_key_pem),
            }},
    };
    event::EventLoopGroup workers(1);
    event::EventLoop &loop = workers.at(0);
    AccessTlsMetrics metrics(loop);
    TlsCertificateStore store(loop, workers, false, metrics.observer());
    DeferredTlsStoreStep after_idle_clear{.loop = &loop};
    DeferredTlsStoreStep after_rotation_clear{.loop = &loop};
    std::promise<void> done_promise;
    auto done = done_promise.get_future();

    after_idle_clear.callback = [&]() {
        std::string idle_output;
        metrics.append_prometheus(idle_output, loop.now());
        EXPECT_NE(idle_output.find("access_server_tls_certificate_rotations_total 0"), std::string::npos);
        EXPECT_NE(idle_output.find("access_server_tls_certificate_reclaim_runs_total{trigger=\"hazard_clear\"} 0"),
                  std::string::npos);
        EXPECT_NE(idle_output.find("access_server_tls_certificate_retired_snapshots 0"), std::string::npos);

        const net::TlsIdentitySelectorOps selector = store.selector_ops();
        net::TlsContext *selected = selector.select(selector.ctx, net::TlsIdentitySelectInput{
                                                                          .server_name = "api.example.com",
                                                                          .transport = net::TlsTransportKind::Tcp,
                                                                  });
        EXPECT_NE(selected, nullptr);
        config.version = 2;
        auto rotated = store.apply(config, "wire-v2");
        EXPECT_TRUE(rotated);
        if (rotated) {
            EXPECT_EQ(*rotated, TlsCertificateUpdateStatus::Published);
        }

        std::string retained_output;
        metrics.append_prometheus(retained_output, loop.now());
        EXPECT_NE(retained_output.find("access_server_tls_certificate_rotations_total 1"), std::string::npos);
        EXPECT_NE(retained_output.find("access_server_tls_certificate_reclaim_runs_total{trigger=\"publish\"} 1"),
                  std::string::npos);
        EXPECT_NE(
                retained_output.find("access_server_tls_certificate_reclaimed_snapshots_total{trigger=\"publish\"} 0"),
                std::string::npos);
        EXPECT_NE(retained_output.find("access_server_tls_certificate_retired_snapshots 1"), std::string::npos);
        after_rotation_clear.schedule();
    };

    after_rotation_clear.callback = [&]() {
        std::string reclaimed_output;
        metrics.append_prometheus(reclaimed_output, loop.now());
        EXPECT_NE(reclaimed_output.find("access_server_tls_certificate_reclaim_runs_total{trigger=\"hazard_clear\"} 1"),
                  std::string::npos);
        EXPECT_NE(reclaimed_output.find(
                          "access_server_tls_certificate_reclaimed_snapshots_total{trigger=\"hazard_clear\"} 1"),
                  std::string::npos);
        EXPECT_NE(reclaimed_output.find("access_server_tls_certificate_retired_snapshots 0"), std::string::npos);

        config.version = 3;
        auto rotated = store.apply(config, "wire-v3");
        EXPECT_TRUE(rotated);
        if (rotated) {
            EXPECT_EQ(*rotated, TlsCertificateUpdateStatus::Published);
        }
        std::string direct_output;
        metrics.append_prometheus(direct_output, loop.now());
        EXPECT_NE(direct_output.find("access_server_tls_certificate_rotations_total 2"), std::string::npos);
        EXPECT_NE(direct_output.find("access_server_tls_certificate_reclaim_runs_total{trigger=\"publish\"} 2"),
                  std::string::npos);
        EXPECT_NE(direct_output.find("access_server_tls_certificate_reclaimed_snapshots_total{trigger=\"publish\"} 1"),
                  std::string::npos);
        EXPECT_NE(direct_output.find("access_server_tls_certificate_retired_snapshots 0"), std::string::npos);

        async::spawn(loop, [&]() -> async::DetachedTask {
            co_await store.shutdown();
            std::string shutdown_output;
            metrics.append_prometheus(shutdown_output, loop.now());
            EXPECT_NE(shutdown_output.find("access_server_tls_certificate_reclaim_runs_total{trigger=\"shutdown\"} 1"),
                      std::string::npos);
            EXPECT_NE(shutdown_output.find(
                              "access_server_tls_certificate_reclaimed_snapshots_total{trigger=\"shutdown\"} 1"),
                      std::string::npos);
            done_promise.set_value();
            loop.stop();
        });
    };

    workers.start();
    async::spawn(loop, [&]() -> async::DetachedTask {
        auto published = store.apply(config, "wire-v1");
        EXPECT_TRUE(published);
        if (published) {
            EXPECT_EQ(*published, TlsCertificateUpdateStatus::Published);
        }
        const net::TlsIdentitySelectorOps selector = store.selector_ops();
        net::TlsContext *selected = selector.select(selector.ctx, net::TlsIdentitySelectInput{
                                                                          .server_name = "api.example.com",
                                                                          .transport = net::TlsTransportKind::Tcp,
                                                                  });
        EXPECT_NE(selected, nullptr);
        after_idle_clear.schedule();
        co_return;
    });

    const std::future_status status = done.wait_for(5s);
    workers.stop();
    workers.join();
    EXPECT_EQ(status, std::future_status::ready);
}

TEST(TlsCertificateWatcherTest, CompilesOffLoopAndCoalescesLatestSnapshot) {
    auto [certificate_pem, private_key_pem] = make_test_identity();
    const std::string version_three = tls_snapshot(3, certificate_pem, private_key_pem);
    event::EventLoop owner_loop;
    event::EventLoopGroup compiler_group(1);
    event::EventLoopGroup http_workers(1);
    AccessConfigCompiler compiler(compiler_group.at(0));
    FakeTlsConfigService service;
    TlsCertificateStore store(owner_loop, http_workers, false);
    TlsCertificateWatcher watcher(owner_loop, compiler, service, store);
    bool owner_progressed = false;
    bool compiler_started = false;
    bool completed = false;

    async::spawn(owner_loop, [&]() -> async::DetachedTask {
        auto ready = watcher.subscribe_ready();
        auto ready_snapshot = ready.current();
        auto processing = watcher.subscribe_processing();
        auto processing_snapshot = processing.current();
        EXPECT_TRUE(watcher.start());

        service.push(tls_snapshot(1, certificate_pem, private_key_pem), "v1");
        service.push(tls_snapshot(2, certificate_pem, private_key_pem), "v2");
        service.push(version_three, "v3");
        processing_snapshot = processing.current();
        EXPECT_TRUE(processing_snapshot.value);
        if (processing_snapshot.value) {
            EXPECT_TRUE(*processing_snapshot.value);
        }
        EXPECT_EQ(store.version(), 0u);
        EXPECT_EQ(watcher.successful_updates(), 0u);
        owner_progressed = true;

        compiler_group.start();
        compiler_started = true;
        co_await wait_for_bool(processing, processing_snapshot, false);
        ready_snapshot = ready.current();
        EXPECT_TRUE(ready_snapshot.value);
        if (ready_snapshot.value) {
            EXPECT_TRUE(*ready_snapshot.value);
        }
        EXPECT_EQ(store.version(), 3u);
        EXPECT_EQ(store.certificate_count(), 1u);
        EXPECT_EQ(watcher.successful_updates(), 1u);
        EXPECT_EQ(watcher.failed_updates(), 0u);

        service.push(version_three, "same");
        co_await wait_for_bool(processing, processing_snapshot, false);
        EXPECT_EQ(watcher.successful_updates(), 1u);
        EXPECT_EQ(watcher.failed_updates(), 0u);

        service.push(version_three + "\n", "conflict");
        co_await wait_for_bool(processing, processing_snapshot, false);
        EXPECT_EQ(watcher.failed_updates(), 1u);
        EXPECT_TRUE(watcher.last_failure());
        if (watcher.last_failure()) {
            EXPECT_EQ(watcher.last_failure()->error.code, TlsCertificateConfigErrorCode::VersionConflict);
        }
        EXPECT_EQ(store.version(), 3u);

        service.push(tls_snapshot(2, certificate_pem, "not-a-private-key"), "older-invalid");
        co_await wait_for_bool(processing, processing_snapshot, false);
        EXPECT_EQ(watcher.failed_updates(), 1u);
        EXPECT_EQ(store.version(), 3u);

        service.push(tls_snapshot(4, certificate_pem, "not-a-private-key"), "new-invalid");
        co_await wait_for_bool(processing, processing_snapshot, false);
        EXPECT_EQ(watcher.failed_updates(), 2u);
        EXPECT_TRUE(watcher.last_failure());
        if (watcher.last_failure()) {
            EXPECT_EQ(watcher.last_failure()->error.code, TlsCertificateConfigErrorCode::InvalidPrivateKey);
        }
        EXPECT_EQ(store.version(), 3u);

        co_await watcher.shutdown();
        co_await store.shutdown();
        completed = true;
        owner_loop.stop();
    });

    owner_loop.run();
    if (compiler_started) {
        compiler_group.stop();
        compiler_group.join();
    }
    EXPECT_TRUE(owner_progressed);
    EXPECT_TRUE(completed);
}

} // namespace
} // namespace fiber::access_server
