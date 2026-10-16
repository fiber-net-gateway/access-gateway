#ifndef FIBER_ACCESS_SERVER_ACCESS_PROVIDER_TRANSACTION_H
#define FIBER_ACCESS_SERVER_ACCESS_PROVIDER_TRANSACTION_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>

#include <fiber/cat/Transaction.h>
#include <fiber/common/IoError.h>
#include <fiber/common/NonCopyable.h>

namespace fiber::access_server {

struct Exception;
class RequestObservability;
class TracePropagation;

// Move-only request-scope CAT provider transaction. An unfinished transaction
// is failed on destruction so connection cancellation cannot leave it pending.
class AccessProviderTransaction final : public common::NonCopyable {
public:
    AccessProviderTransaction() noexcept = default;
    AccessProviderTransaction(AccessProviderTransaction &&other) noexcept;
    AccessProviderTransaction &operator=(AccessProviderTransaction &&other) noexcept;
    ~AccessProviderTransaction() noexcept;

    [[nodiscard]] bool valid() const noexcept;

    void add_upstream(std::string_view upstream, std::size_t attempt) noexcept;
    void add_uri(std::string_view uri) noexcept;
    void add_connection_reuse(std::uint64_t reuse_count) noexcept;
    // Fresh-connection acquisition latency from the provider transaction's
    // start: pool wait, DNS, dialing, TLS handshake, and the ALPN check all
    // fall inside. Unlike the Java ConnRecorder connect tag, which fires at TCP
    // establishment while netty completes the handshake asynchronously, the
    // native connect() resolves only after the handshake. Also recorded on the
    // acquisition-failure path, before the transaction fails.
    void add_connect(std::chrono::microseconds duration) noexcept;
    // Body-transfer accounting: bytes pulled in for the phase and the wall time
    // its forwarding took. Recorded before completion only — data added after
    // the transaction completes is dropped by the CAT client.
    void add_request_body(std::uint64_t bytes, std::chrono::microseconds duration) noexcept;
    void add_response_body(std::uint64_t bytes, std::chrono::microseconds duration) noexcept;
    void fail(std::string_view phase, common::IoErr error) noexcept;
    void call_error(const Exception &exception, std::string_view phase, common::IoErr error) noexcept;
    // Stage-attributed terminal error event: the type carries the failure family
    // (CALL_ERROR / ReqBodyErr.Read|Write / RespBodyErr.Read|Write|Invalid), the
    // name the stage token or io-error cause (send_req, read_resp, write_resp,
    // client_abort, timed_out, ...). The event completes with Fail and the
    // transaction then finishes through fail(), which appends the same
    // phase/io_error data before its own completion.
    void fail_with_event(std::string_view type, std::string_view name, std::string_view phase,
                         common::IoErr error) noexcept;
    void complete(int status_code) noexcept;

private:
    friend class RequestObservability;
    friend class TracePropagation;

    explicit AccessProviderTransaction(cat::Transaction transaction) noexcept : transaction_(std::move(transaction)) {}
    void cancel_pending() noexcept;

    cat::Transaction transaction_;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_ACCESS_PROVIDER_TRANSACTION_H
