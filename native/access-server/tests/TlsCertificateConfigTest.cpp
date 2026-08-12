#include <gtest/gtest.h>

#include "config/TlsCertificateConfig.h"
#include "runtime/TlsCertificateStore.h"

#include <memory>
#include <string>
#include <utility>

#include <openssl/bio.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/obj.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <fiber/async/Spawn.h>
#include <fiber/event/EventLoop.h>
#include <fiber/event/EventLoopGroup.h>

namespace fiber::access_server {
namespace {

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

} // namespace
} // namespace fiber::access_server
