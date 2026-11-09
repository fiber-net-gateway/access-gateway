#include "AccessEdgeFunctionTransaction.h"

#include "../config/AccessConfig.h"

#include <array>
#include <charconv>
#include <limits>
#include <system_error>
#include <type_traits>
#include <utility>

#include <fiber/cat/Status.h>

namespace fiber::access_server {
namespace {

template<typename T>
void add_integer(cat::Transaction &transaction, std::string_view key, T value) noexcept {
    static_assert(std::is_integral_v<T>);
    std::array<char, std::numeric_limits<T>::digits10 + 3> buffer{};
    const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (converted.ec == std::errc{}) {
        (void) transaction.add_data(
                key, std::string_view(buffer.data(), static_cast<std::size_t>(converted.ptr - buffer.data())));
    }
}

std::string_view phase_name(EdgeFunctionPhase phase) noexcept {
    switch (phase) {
        case EdgeFunctionPhase::RequestHead:
            return "head";
        case EdgeFunctionPhase::RequestBody:
            return "body";
        default:
            return {};
    }
}

} // namespace

AccessEdgeFunctionTransaction::AccessEdgeFunctionTransaction(AccessEdgeFunctionTransaction &&other) noexcept :
    transaction_(std::move(other.transaction_)) {}

AccessEdgeFunctionTransaction &
AccessEdgeFunctionTransaction::operator=(AccessEdgeFunctionTransaction &&other) noexcept {
    if (this == &other) {
        return *this;
    }
    cancel_pending();
    transaction_ = std::move(other.transaction_);
    return *this;
}

AccessEdgeFunctionTransaction::~AccessEdgeFunctionTransaction() noexcept { cancel_pending(); }

bool AccessEdgeFunctionTransaction::valid() const noexcept { return transaction_.valid(); }

void AccessEdgeFunctionTransaction::add_phase(EdgeFunctionPhase phase) noexcept {
    const std::string_view name = phase_name(phase);
    if (!valid() || name.empty()) {
        return;
    }
    (void) transaction_.add_data("phase", name);
}

void AccessEdgeFunctionTransaction::add_cost(const script::GcHeapStats &before,
                                             const script::GcHeapStats &after) noexcept {
    if (!valid()) {
        return;
    }
    add_integer(transaction_, "alloc_bytes", after.allocated_bytes_total - before.allocated_bytes_total);
    add_integer(transaction_, "heap_bytes",
                static_cast<std::int64_t>(after.current_bytes) - static_cast<std::int64_t>(before.current_bytes));
    const std::uint64_t gc_cycles = after.gc_count - before.gc_count;
    if (gc_cycles != 0) {
        add_integer(transaction_, "gc", gc_cycles);
        add_integer(transaction_, "gc_freed", after.gc_freed_bytes_total - before.gc_freed_bytes_total);
    }
}

void AccessEdgeFunctionTransaction::complete_error(std::string_view reason, std::string_view detail) noexcept {
    if (!valid()) {
        return;
    }
    if (!reason.empty()) {
        (void) transaction_.add_data("reason", reason);
    }
    if (!detail.empty()) {
        (void) transaction_.add_data("detail", detail);
    }
    (void) transaction_.complete(cat::status::Error);
}

void AccessEdgeFunctionTransaction::complete_success() noexcept {
    if (!valid()) {
        return;
    }
    (void) transaction_.complete(cat::status::Success);
}

void AccessEdgeFunctionTransaction::cancel_pending() noexcept {
    if (!valid()) {
        return;
    }
    (void) transaction_.add_data("reason", "canceled");
    (void) transaction_.complete(cat::status::Fail);
}

} // namespace fiber::access_server
