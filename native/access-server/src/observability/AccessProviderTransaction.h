#ifndef FIBER_ACCESS_SERVER_ACCESS_PROVIDER_TRANSACTION_H
#define FIBER_ACCESS_SERVER_ACCESS_PROVIDER_TRANSACTION_H

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
    void add_connection_reuse(std::uint64_t reuse_count) noexcept;
    void fail(std::string_view phase, common::IoErr error) noexcept;
    void call_error(const Exception &exception, std::string_view phase, common::IoErr error) noexcept;
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
