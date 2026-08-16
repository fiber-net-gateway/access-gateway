#include "AccessProviderTransaction.h"

#include "../execution/AccessResult.h"

#include <array>
#include <charconv>
#include <limits>

#include <fiber/cat/Status.h>

namespace fiber::access_server {
namespace {

template<typename T>
void add_integer(cat::Transaction &transaction, std::string_view key, T value) noexcept {
    std::array<char, std::numeric_limits<T>::digits10 + 3> buffer{};
    const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (converted.ec == std::errc{}) {
        (void) transaction.add_data(
                key, std::string_view(buffer.data(), static_cast<std::size_t>(converted.ptr - buffer.data())));
    }
}

} // namespace

AccessProviderTransaction::AccessProviderTransaction(AccessProviderTransaction &&other) noexcept :
    transaction_(std::move(other.transaction_)) {}

AccessProviderTransaction &AccessProviderTransaction::operator=(AccessProviderTransaction &&other) noexcept {
    if (this == &other) {
        return *this;
    }
    cancel_pending();
    transaction_ = std::move(other.transaction_);
    return *this;
}

AccessProviderTransaction::~AccessProviderTransaction() noexcept { cancel_pending(); }

bool AccessProviderTransaction::valid() const noexcept { return transaction_.valid(); }

void AccessProviderTransaction::add_upstream(std::string_view upstream, std::size_t attempt) noexcept {
    if (!valid()) {
        return;
    }
    (void) transaction_.add_data("upstream", upstream);
    add_integer(transaction_, "attempt", attempt);
}

void AccessProviderTransaction::add_connection_reuse(std::uint64_t reuse_count) noexcept {
    if (!valid()) {
        return;
    }
    add_integer(transaction_, "reuse_count", reuse_count);
}

void AccessProviderTransaction::fail(std::string_view phase, common::IoErr error) noexcept {
    if (!valid()) {
        return;
    }
    if (!phase.empty()) {
        (void) transaction_.add_data("phase", phase);
    }
    if (error != common::IoErr::None) {
        (void) transaction_.add_data("io_error", common::io_err_name(error));
    }
    (void) transaction_.complete(cat::status::Fail);
}

void AccessProviderTransaction::call_error(const Exception &exception, std::string_view phase,
                                           common::IoErr error) noexcept {
    if (!valid()) {
        return;
    }
    auto event = transaction_.start_event("CALL_ERROR", exception.name);
    if (event) {
        (void) event->add_data(exception.message);
        if (!phase.empty()) {
            (void) event->add_data("phase", phase);
        }
        if (error != common::IoErr::None) {
            (void) event->add_data("io_error", common::io_err_name(error));
        }
        (void) event->complete(cat::status::Error);
    }
    fail(phase, error);
}

void AccessProviderTransaction::complete(int status_code) noexcept {
    if (!valid()) {
        return;
    }
    add_integer(transaction_, "status", status_code);
    (void) transaction_.complete(status_code < 500 ? cat::status::Success : cat::status::Fail);
}

void AccessProviderTransaction::cancel_pending() noexcept {
    if (valid()) {
        fail("canceled", common::IoErr::Canceled);
    }
}

} // namespace fiber::access_server
