#include "TraceContextConstExtension.h"

#include "RequestTraceServices.h"

#include <cstddef>
#include <utility>

#include <fiber/http_script/ConstPackage.h>
#include <fiber/http_script/ScriptExchangeCtx.h>
#include <fiber/script/JsValue.h>
#include <fiber/script/ScriptResult.h>

namespace fiber::access_server {
namespace {

// Java-compatible constant-name normalization (the rule documented on
// const_name_hash in ConstPackage.h): ASCII lowercase plus '-' -> '_'. The
// normalized form has the same length as the input. Returns false when the
// input exceeds the fiber constant-name cap; such a name can never match a
// stored key, so the caller reports it as an unknown constant (compile error).
[[nodiscard]] bool normalize_const_name(std::string_view key, char *buffer, std::size_t &length) noexcept {
    if (key.size() > fiber::http_script::kMaxConstNameLength) {
        return false;
    }
    length = key.size();
    for (std::size_t i = 0; i < key.size(); ++i) {
        const char c = key[i];
        if (c >= 'A' && c <= 'Z') {
            buffer[i] = static_cast<char>(c - 'A' + 'a');
        } else if (c == '-') {
            buffer[i] = '_';
        } else {
            buffer[i] = c;
        }
    }
    return true;
}

[[nodiscard]] fiber::script::AbiResult native_string(std::string_view value) noexcept {
    if (value.empty()) {
        // Normalize to a non-null data pointer for make_native_string.
        // NOLINTNEXTLINE(bugprone-string-constructor)
        value = std::string_view("", 0);
    }
    return fiber::script::AbiResult::success(fiber::script::JsValue::make_native_string(value.data(), value.size()));
}

[[nodiscard]] const TraceConstSource *trace_source_of(fiber::http_script::ScriptExchangeCtx &context) noexcept {
    fiber::http_script::HttpScriptServices *services = context.services();
    if (services == nullptr) {
        return nullptr;
    }
    // Sound by the RequestTraceServices installation invariant: project
    // programs never execute while the edge services pointer is installed.
    return static_cast<RequestTraceServices *>(services)->trace_source();
}

} // namespace

TraceContextConstExtension::TraceContextConstExtension() noexcept {
    traceparent_callable_.kind = HostCallable::Kind::SyncConstant;
    traceparent_callable_.constant = &TraceContextConstExtension::traceparent_constant_fn;
    traceparent_callable_.debug_name = "$header.traceparent";
}

const fiber::script::std_lib::StdLibrary::ExtOps &TraceContextConstExtension::ops() noexcept {
    static const fiber::script::std_lib::StdLibrary::ExtOps kOps{
            .resolve_constant = &TraceContextConstExtension::resolve_constant_op,
    };
    return kOps;
}

const TraceContextConstExtension::HostCallable *
TraceContextConstExtension::resolve_constant_op(void *ctx, std::string_view namespace_name,
                                                std::string_view key) noexcept {
    return static_cast<TraceContextConstExtension *>(ctx)->resolve_constant(namespace_name, key);
}

const TraceContextConstExtension::HostCallable *
TraceContextConstExtension::resolve_constant(std::string_view namespace_name, std::string_view key) {
    char buffer[fiber::http_script::kMaxConstNameLength + 1];
    std::size_t length = 0;
    if (namespace_name == "$context") {
        if (!normalize_const_name(key, buffer, length)) {
            return nullptr;
        }
        const auto [entry, inserted] = context_callables_.try_emplace(std::string(buffer, length));
        if (inserted) {
            entry->second.kind = HostCallable::Kind::SyncConstant;
            entry->second.userdata = const_cast<std::string *>(&entry->first);
            entry->second.constant = &TraceContextConstExtension::context_constant_fn;
            entry->second.debug_name = "$context";
        }
        return &entry->second;
    }
    if (namespace_name == "$header" && normalize_const_name(key, buffer, length) &&
        std::string_view(buffer, length) == "traceparent") {
        return &traceparent_callable_;
    }
    return nullptr;
}

fiber::script::AbiResult TraceContextConstExtension::context_constant_fn(void *userdata,
                                                                         const HostCallFrame &frame) noexcept {
    const auto &key = *static_cast<const std::string *>(userdata);
    auto *context = static_cast<fiber::http_script::ScriptExchangeCtx *>(frame.attach);
    if (context == nullptr) {
        return fiber::script::AbiResult::abort(fiber::script::ScriptAbortReason::InvalidState);
    }
    const TraceConstSource *source = trace_source_of(*context);
    if (source == nullptr) {
        // No trace wiring on this execution context: unbound, like the old
        // prepare-time behavior for absent keys.
        return fiber::script::AbiResult::success(fiber::script::JsValue::make_null());
    }
    const std::optional<std::string_view> value = source->resolve_trace_context(key);
    if (!value) {
        return fiber::script::AbiResult::success(fiber::script::JsValue::make_null());
    }
    return native_string(*value);
}

fiber::script::AbiResult TraceContextConstExtension::traceparent_constant_fn(void *userdata,
                                                                             const HostCallFrame &frame) noexcept {
    (void) userdata;
    auto *context = static_cast<fiber::http_script::ScriptExchangeCtx *>(frame.attach);
    if (context == nullptr) {
        return fiber::script::AbiResult::abort(fiber::script::ScriptAbortReason::InvalidState);
    }
    const TraceConstSource *source = trace_source_of(*context);
    if (source == nullptr) {
        return fiber::script::AbiResult::success(fiber::script::JsValue::make_null());
    }
    return native_string(source->trace_parent());
}

} // namespace fiber::access_server
