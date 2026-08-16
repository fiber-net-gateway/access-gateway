#include "TracePropagation.h"

#include "AccessProviderTransaction.h"
#include "ScriptExecutionContext.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <utility>

#include <openssl/rand.h>

#include <fiber/cat/Status.h>
#include <fiber/cat/Transaction.h>
#include <fiber/http/HttpExchange.h>
#include <fiber/http/HttpHeaderHash.h>
#include <fiber/http/HttpHeaders.h>
#include <fiber/http_script/ConstPackage.h>

namespace fiber::access_server {
namespace {

constexpr std::string_view kTraceIdHeader = "Hi-Trace-Id";
constexpr std::string_view kTraceIdLowcaseHeader = "hi-trace-id";
constexpr std::uint64_t kTraceIdHeaderHash = http::http_header_name_hash(kTraceIdLowcaseHeader);
constexpr std::string_view kParentSpanIdHeader = "HI-SPAN-ID-PARENT";
constexpr std::string_view kParentSpanIdLowcaseHeader = "hi-span-id-parent";
constexpr std::uint64_t kParentSpanIdHeaderHash = http::http_header_name_hash(kParentSpanIdLowcaseHeader);
constexpr std::string_view kSpanIdHeader = "HI-SPAN-ID";
constexpr std::string_view kSpanIdLowcaseHeader = "hi-span-id";
constexpr std::uint64_t kSpanIdHeaderHash = http::http_header_name_hash(kSpanIdLowcaseHeader);
constexpr std::string_view kTraceParentHeader = "traceparent";
constexpr std::uint64_t kTraceParentHeaderHash = http::http_header_name_hash(kTraceParentHeader);
constexpr std::string_view kTraceStateHeader = "tracestate";
constexpr std::uint64_t kTraceStateHeaderHash = http::http_header_name_hash(kTraceStateHeader);

bool all_zero(std::span<const unsigned char> value) noexcept {
    return std::all_of(value.begin(), value.end(), [](unsigned char byte) { return byte == 0; });
}

std::string_view generate_trace_parent(mem::BufPool &pool) noexcept {
    constexpr std::size_t kTraceParentSize = 55;
    constexpr char kHex[] = "0123456789abcdef";
    std::array<unsigned char, 24> random{};
    bool generated = false;
    for (std::uint8_t attempt = 0; attempt < 4; ++attempt) {
        if (RAND_bytes(random.data(), random.size()) != 1) {
            return {};
        }
        if (!all_zero(std::span<const unsigned char>(random.data(), 16)) &&
            !all_zero(std::span<const unsigned char>(random.data() + 16, 8))) {
            generated = true;
            break;
        }
    }
    if (!generated) {
        return {};
    }

    char *storage = pool.alloc<char>(kTraceParentSize);
    if (!storage) {
        return {};
    }
    char *cursor = storage;
    *cursor++ = '0';
    *cursor++ = '0';
    *cursor++ = '-';
    for (std::size_t i = 0; i < 16; ++i) {
        *cursor++ = kHex[random[i] >> 4U];
        *cursor++ = kHex[random[i] & 0x0FU];
    }
    *cursor++ = '-';
    for (std::size_t i = 16; i < random.size(); ++i) {
        *cursor++ = kHex[random[i] >> 4U];
        *cursor++ = kHex[random[i] & 0x0FU];
    }
    *cursor++ = '-';
    *cursor++ = '0';
    *cursor++ = '1';
    return {storage, kTraceParentSize};
}

} // namespace

TracePropagation::TracePropagation(mem::BufPool &pool) noexcept : trace_state_(pool) {}

void TracePropagation::initialize(http::HttpExchange &exchange) noexcept {
    const http::HttpHeaders &request_headers = exchange.request_headers();
    trace_parent_ = request_headers.get(kTraceParentHeader, kTraceParentHeaderHash);
    if (trace_parent_.empty()) {
        trace_parent_ = generate_trace_parent(exchange.pool());
    }
    trace_state_.parse(request_headers.get(kTraceStateHeader, kTraceStateHeaderHash));
}

void TracePropagation::attach_cat(cat::Transaction &root) noexcept {
    if (!root.valid()) {
        return;
    }
    auto propagation = root.message_trace().propagation_context();
    if (propagation) {
        cat_context_.emplace(std::move(*propagation));
    }
    cat::MessageTrace message_trace = root.message_trace();
    trace_state_.for_each_context([&message_trace](std::string_view key, std::string_view value) noexcept {
        if (!key.empty()) {
            (void) message_trace.put_context(key, value);
        }
        return true;
    });
}

common::IoResult<void> TracePropagation::bind_trace_context(ScriptExecutionContext &execution,
                                                            const http_script::ConstPackage &constants) noexcept {
    const_package_ = &constants;
    bool bound = true;
    trace_state_.for_each_context([&](std::string_view key, std::string_view value) noexcept {
        if (!execution.bind_context_constant(constants, key, value)) {
            bound = false;
            return false;
        }
        return true;
    });
    if (!bound) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (!trace_parent_.empty() && !execution.bind_header_constant(constants, kTraceParentHeader, trace_parent_)) {
        return std::unexpected(common::IoErr::Invalid);
    }
    return {};
}

common::IoResult<void> TracePropagation::put_trace_context(ScriptExecutionContext &execution, cat::Transaction &root,
                                                           std::string_view key, std::string_view value) noexcept {
    auto stored = trace_state_.put_context(key, value);
    if (!stored) {
        return stored;
    }
    if (root.valid() && !key.empty()) {
        cat::MessageTrace message_trace = root.message_trace();
        (void) message_trace.put_context(key, value);
    }
    if (const_package_ && !execution.bind_context_constant(*const_package_, key, value)) {
        return std::unexpected(common::IoErr::Invalid);
    }
    return {};
}

void TracePropagation::remove_trace_context(ScriptExecutionContext &execution, cat::Transaction &root,
                                            std::string_view key) noexcept {
    if (!trace_state_.remove_context(key)) {
        return;
    }
    if (root.valid() && !key.empty()) {
        cat::MessageTrace message_trace = root.message_trace();
        (void) message_trace.remove_context(key);
    }
    if (const_package_) {
        execution.clear_context_constant(*const_package_, key);
    }
}

bool TracePropagation::finalize_response_headers(http::HttpHeaders &headers) noexcept {
    const std::string_view id = trace_id();
    return id.empty() || headers.set_view(kTraceIdHeader, id, kTraceIdLowcaseHeader.data(), kTraceIdHeaderHash);
}

bool TracePropagation::inject_upstream_headers(http::HttpHeaders &headers, ScriptExecutionContext &execution,
                                               cat::Transaction &root, AccessProviderTransaction &provider) noexcept {
    if (trace_state_.should_override_upstream()) {
        auto trace_state = trace_state_.encode();
        if (!trace_state ||
            !headers.set_view(kTraceStateHeader, *trace_state, kTraceStateHeader.data(), kTraceStateHeaderHash)) {
            return false;
        }
    }
    if (!root.valid() || !cat_context_ || cat_context_->message_id.empty()) {
        return true;
    }
    cat::Transaction &parent = provider.valid() ? provider.transaction_ : root;
    auto remote = parent.message_trace().create_remote_context(execution.pool());
    if (!remote) {
        return true;
    }
    const std::string_view message_id = remote->message_id;
    const std::string_view root_id = remote->root_message_id.empty() ? message_id : remote->root_message_id;
    const std::string_view parent_id = remote->parent_message_id;
    if (message_id.empty() || root_id.empty() || parent_id.empty()) {
        return true;
    }
    if (!headers.set_view(kTraceIdHeader, root_id, kTraceIdLowcaseHeader.data(), kTraceIdHeaderHash) ||
        !headers.set_view(kParentSpanIdHeader, parent_id, kParentSpanIdLowcaseHeader.data(), kParentSpanIdHeaderHash) ||
        !headers.set_view(kSpanIdHeader, message_id, kSpanIdLowcaseHeader.data(), kSpanIdHeaderHash)) {
        return false;
    }
    auto event = parent.start_event("RemoteCall", "");
    if (event) {
        (void) event->add_data(message_id);
        (void) event->complete(cat::status::Success);
    }
    return true;
}

} // namespace fiber::access_server
