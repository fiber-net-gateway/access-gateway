#ifndef FIBER_ACCESS_SERVER_TRACE_PROPAGATION_H
#define FIBER_ACCESS_SERVER_TRACE_PROPAGATION_H

#include "AccessTraceState.h"

#include "../routing/TraceConstSource.h"

#include <optional>
#include <string_view>

#include <fiber/cat/MessageTrace.h>
#include <fiber/common/IoError.h>

namespace fiber::cat {
class Transaction;
}

namespace fiber::http {
class HttpExchange;
class HttpHeaders;
} // namespace fiber::http

namespace fiber::mem {
class BufPool;
}

namespace fiber::access_server {

class AccessProviderTransaction;
class ScriptExecutionContext;

// Request-local W3C and CAT propagation state. CAT objects are passed into
// operations explicitly so this component never owns or outlives a transaction.
//
// TraceConstSource exposes this state to script constants ($context.<key> and
// $header.traceparent) through RequestTraceServices; values are read live, so
// mid-request put/remove calls are visible to the next script access without
// any rebinding.
class TracePropagation final : public TraceConstSource {
public:
    explicit TracePropagation(mem::BufPool &pool) noexcept;

    TracePropagation(const TracePropagation &) = delete;
    TracePropagation &operator=(const TracePropagation &) = delete;
    TracePropagation(TracePropagation &&) = delete;
    TracePropagation &operator=(TracePropagation &&) = delete;

    void attach_cat(cat::Transaction &root) noexcept;

    [[nodiscard]] std::string_view trace_id() const noexcept {
        if (!cat_context_) {
            return {};
        }
        return cat_context_->root_message_id.empty() ? cat_context_->message_id : cat_context_->root_message_id;
    }
    [[nodiscard]] std::string_view trace_parent() const noexcept override { return trace_parent_; }
    [[nodiscard]] std::optional<std::string_view> trace_context(std::string_view key) const noexcept {
        return trace_state_.get_context(key);
    }

    // TraceConstSource: normalized-key lookup for the $context constants.
    [[nodiscard]] std::optional<std::string_view>
    resolve_trace_context(std::string_view normalized_key) const noexcept override;

    [[nodiscard]] common::IoResult<void> put_trace_context(cat::Transaction &root, std::string_view key,
                                                           std::string_view value) noexcept;
    void remove_trace_context(cat::Transaction &root, std::string_view key) noexcept;
    [[nodiscard]] bool finalize_response_headers(http::HttpHeaders &headers) noexcept;
    [[nodiscard]] bool inject_upstream_headers(http::HttpHeaders &headers, ScriptExecutionContext &execution,
                                               cat::Transaction &root, AccessProviderTransaction &provider) noexcept;

private:
    friend class RequestObservability;

    void initialize(http::HttpExchange &exchange) noexcept;

    AccessTraceState trace_state_;
    std::optional<cat::MessageTraceContext> cat_context_;
    std::string_view trace_parent_;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_TRACE_PROPAGATION_H
