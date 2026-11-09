#ifndef FIBER_ACCESS_SERVER_ACCESS_EDGE_FUNCTION_TRANSACTION_H
#define FIBER_ACCESS_SERVER_ACCESS_EDGE_FUNCTION_TRANSACTION_H

#include <cstdint>
#include <string_view>

#include <fiber/cat/Transaction.h>
#include <fiber/common/NonCopyable.h>
#include <fiber/script/JsGc.h>

namespace fiber::access_server {

enum class EdgeFunctionPhase : std::uint8_t;

// Move-only request-scope CAT transaction for one edge-function execution
// (type "Access.EdgeFunction", name = the function slug). Execution cost rides
// as data tags: the wall duration is computed by the CAT client at complete(),
// the memory cost from GcHeap lifetime-counter deltas around the execution
// (GcHeapStats measured in requested GC bytes — string-intern buckets, root
// pools, and allocator overhead are outside it). An unfinished transaction is
// failed on destruction, so cancellation cannot leave it pending.
class AccessEdgeFunctionTransaction final : public common::NonCopyable {
public:
    AccessEdgeFunctionTransaction() noexcept = default;
    AccessEdgeFunctionTransaction(AccessEdgeFunctionTransaction &&other) noexcept;
    AccessEdgeFunctionTransaction &operator=(AccessEdgeFunctionTransaction &&other) noexcept;
    ~AccessEdgeFunctionTransaction() noexcept;

    [[nodiscard]] bool valid() const noexcept;

    // Which mount chain the execution belongs to: the same function may run in
    // both phases for one request, which produces two transactions.
    void add_phase(EdgeFunctionPhase phase) noexcept;
    // Heap cost between two stats() snapshots taken around one execution.
    // alloc_bytes is this execution's gross requested-byte churn (the primary
    // cost signal); heap_bytes is the net retained delta, which can go
    // negative when a threshold collection inside the window reclaims an
    // earlier function's garbage. gc/gc_freed cover completed mark/sweep
    // cycles inside the window.
    void add_cost(const script::GcHeapStats &before, const script::GcHeapStats &after) noexcept;
    // Terminal error classification: the fixed reason vocabulary shared with
    // access_server_function_errors_total, plus the abort reason name.
    void complete_error(std::string_view reason, std::string_view detail = {}) noexcept;
    // Completed without an error: returned void/value, or wrote a response.
    void complete_success() noexcept;

private:
    friend class RequestObservability;

    explicit AccessEdgeFunctionTransaction(cat::Transaction transaction) noexcept :
        transaction_(std::move(transaction)) {}

    void cancel_pending() noexcept;

    cat::Transaction transaction_;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_ACCESS_EDGE_FUNCTION_TRANSACTION_H
