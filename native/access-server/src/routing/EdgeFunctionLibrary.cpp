#include "EdgeFunctionLibrary.h"

#include <fiber/async/Task.h>
#include <fiber/common/IoError.h>
#include <fiber/common/util/CookieCodec.h>
#include <fiber/http/HttpExchange.h>
#include <fiber/script/AsyncTask.h>
#include <fiber/script/JsGc.h>
#include <fiber/script/ScriptCompiler.h>
#include <fiber/script/ScriptResult.h>
#include <fiber/script/gc/GcInternal.h>
#include <fiber/script/std/NodeText.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace fiber::access_server {
namespace {

using fiber::script::AbiResult;
using fiber::script::AsyncTask;
using fiber::script::ConstValueHandle;
using fiber::script::GcHeap;
using fiber::script::js_value_bool;
using fiber::script::js_value_int64;
using fiber::script::js_value_type;
using fiber::script::JsNodeType;
using fiber::script::JsValue;
using fiber::script::Library;
using fiber::script::ScriptAbortReason;
using fiber::script::ValueHandle;
using fiber::script::std_lib::binary_bytes;
using fiber::script::std_lib::node_as_text;
using fiber::script::std_lib::string_utf8_view;

http_script::ScriptExchangeCtx *ctx_of(const Library::HostCallFrame &frame) noexcept {
    return static_cast<http_script::ScriptExchangeCtx *>(frame.attach);
}

// Same lookup path fiber's response functions use: the per-request edge
// context rides in the ScriptExchangeCtx services slot while one edge
// function executes.
EdgeFunctionRequestContext *edge_context_of(const Library::HostCallFrame &frame) noexcept {
    auto *exchange = ctx_of(frame);
    if (exchange == nullptr || exchange->services() == nullptr) {
        return nullptr;
    }
    return dynamic_cast<EdgeFunctionRequestContext *>(exchange->services());
}

// Same exception construction as fiber's RequestFuncs/ResponseFuncs (Java
// ScriptExecException(name, message)); OOM while building it degrades to an
// OutOfMemory abort.
AbiResult error_exn(GcHeap &heap, std::string_view message) noexcept {
    static constexpr char kErrorName[] = "Error";
    GcHeap::LocalMark mark(heap);
    ValueHandle ex = heap.local_value();
    if (!ex) {
        return AbiResult::abort(ScriptAbortReason::OutOfMemory);
    }
    if (!fiber::script::gc_make_exception(&heap, ex, -1, kErrorName, sizeof(kErrorName) - 1, message.data(),
                                          message.size())) {
        return AbiResult::abort(ScriptAbortReason::OutOfMemory);
    }
    return AbiResult::exception(*ex);
}

AbiResult invalid_state() noexcept { return AbiResult::abort(ScriptAbortReason::InvalidState); }

// Every attacker-controlled text field fed to the VM is clamped to
// limits.max_input_field_bytes before a heap string is made from it.
std::string_view truncated(const EdgeFunctionLimits &limits, std::string_view value) noexcept {
    return value.size() > limits.max_input_field_bytes ? value.substr(0, limits.max_input_field_bytes) : value;
}

// Reads a named field off an Object as a JsValue (Undefined when absent or
// non-object). gc_object_get_key is a pure hash read (no allocation, no GC), so
// the returned heap ref stays valid for the duration of the sync caller
// without rooting.
JsValue get_field(GcHeap &heap, JsValue object, const char *key, std::size_t key_len) noexcept {
    if (js_value_type(object) != JsNodeType::Object) {
        return JsValue::make_undefined();
    }
    JsValue out = JsValue::make_undefined();
    fiber::script::gc_object_get_key(&heap, ConstValueHandle(&object), key, key_len, ValueHandle(&out));
    return out;
}

std::string_view field_string(GcHeap &heap, const JsValue &object, const char *key, std::size_t key_len) noexcept {
    JsValue value = get_field(heap, object, key, key_len);
    std::string_view sv;
    return string_utf8_view(value, sv) ? sv : std::string_view{};
}

int as_int(const JsValue &value, int fallback) noexcept {
    return js_value_type(value) == JsNodeType::Integer ? static_cast<int>(js_value_int64(value)) : fallback;
}

AbiResult native_string(std::string_view value) noexcept {
    if (value.empty()) {
        // Normalize to a non-null data pointer for make_native_string.
        // NOLINTNEXTLINE(bugprone-string-constructor)
        value = std::string_view("", 0);
    }
    return AbiResult::success(JsValue::make_native_string(value.data(), value.size()));
}

// ---- synchronous req.* readers (contract: no body consumption) ----

AbiResult get_header_all_fn(void * /*userdata*/, const Library::HostCallFrame &frame,
                            Library::Arguments /*args*/) noexcept {
    auto *ctx = ctx_of(frame);
    if (ctx == nullptr) {
        return invalid_state();
    }
    return AbiResult::success(ctx->headers());
}

AbiResult get_header_one_fn(void *userdata, const Library::HostCallFrame &frame, Library::Arguments args) noexcept {
    auto *ctx = ctx_of(frame);
    if (ctx == nullptr || args.args == nullptr || args.argc < 1) {
        return invalid_state();
    }
    const auto &limits = *static_cast<const EdgeFunctionLimits *>(userdata);
    const JsValue &name = args.args[0];
    std::string_view name_sv;
    if (!string_utf8_view(name, name_sv) || name_sv.empty()) {
        return AbiResult::success(JsValue::make_null()); // Java: empty/non-text name -> NullNode
    }
    if (!ctx->exchange().request_headers().contains(name_sv)) {
        return AbiResult::success(JsValue::make_undefined()); // Java MissingNode
    }
    const std::string_view value = truncated(limits, ctx->exchange().header(name_sv));
    JsValue result = JsValue::make_string(frame.runtime, value.data(), value.size());
    if (js_value_type(result) != JsNodeType::String) {
        return AbiResult::abort(ScriptAbortReason::OutOfMemory);
    }
    return AbiResult::success(result);
}

AbiResult get_query_all_fn(void * /*userdata*/, const Library::HostCallFrame &frame,
                           Library::Arguments /*args*/) noexcept {
    auto *ctx = ctx_of(frame);
    if (ctx == nullptr) {
        return invalid_state();
    }
    return AbiResult::success(ctx->query());
}

AbiResult get_query_one_fn(void * /*userdata*/, const Library::HostCallFrame &frame, Library::Arguments args) noexcept {
    auto *ctx = ctx_of(frame);
    if (ctx == nullptr || args.args == nullptr || args.argc < 1) {
        return invalid_state();
    }
    const JsValue &name = args.args[0];
    std::string_view name_sv;
    if (!string_utf8_view(name, name_sv) || name_sv.empty()) {
        return AbiResult::success(JsValue::make_undefined()); // Java: empty name -> null
    }
    return http_script::ScriptExchangeCtx::lookup_property(frame.runtime, ctx->query(), name_sv);
}

AbiResult make_text(const Library::HostCallFrame &frame, std::string_view text) noexcept {
    JsValue result = JsValue::make_string(frame.runtime, text.data(), text.size());
    if (js_value_type(result) != JsNodeType::String) {
        return AbiResult::abort(ScriptAbortReason::OutOfMemory);
    }
    return AbiResult::success(result);
}

AbiResult get_uri_fn(void *userdata, const Library::HostCallFrame &frame, Library::Arguments /*args*/) noexcept {
    auto *ctx = ctx_of(frame);
    if (ctx == nullptr) {
        return invalid_state();
    }
    const auto &limits = *static_cast<const EdgeFunctionLimits *>(userdata);
    // unparsed_uri is the raw request-target (path[?query]).
    return make_text(frame, truncated(limits, ctx->exchange().uri().unparsed_uri));
}

AbiResult get_path_fn(void *userdata, const Library::HostCallFrame &frame, Library::Arguments /*args*/) noexcept {
    auto *ctx = ctx_of(frame);
    if (ctx == nullptr) {
        return invalid_state();
    }
    const auto &limits = *static_cast<const EdgeFunctionLimits *>(userdata);
    return make_text(frame, truncated(limits, ctx->exchange().uri().path));
}

AbiResult get_query_str_fn(void *userdata, const Library::HostCallFrame &frame, Library::Arguments /*args*/) noexcept {
    auto *ctx = ctx_of(frame);
    if (ctx == nullptr) {
        return invalid_state();
    }
    const auto &limits = *static_cast<const EdgeFunctionLimits *>(userdata);
    return make_text(frame, truncated(limits, ctx->exchange().uri().query));
}

AbiResult get_method_fn(void *userdata, const Library::HostCallFrame &frame, Library::Arguments /*args*/) noexcept {
    auto *ctx = ctx_of(frame);
    if (ctx == nullptr) {
        return invalid_state();
    }
    const auto &limits = *static_cast<const EdgeFunctionLimits *>(userdata);
    return make_text(frame, truncated(limits, ctx->exchange().method_view()));
}

AbiResult get_cookie_all_fn(void * /*userdata*/, const Library::HostCallFrame &frame,
                            Library::Arguments /*args*/) noexcept {
    auto *ctx = ctx_of(frame);
    if (ctx == nullptr) {
        return invalid_state();
    }
    return AbiResult::success(ctx->cookies());
}

AbiResult get_cookie_one_fn(void * /*userdata*/, const Library::HostCallFrame &frame,
                            Library::Arguments args) noexcept {
    auto *ctx = ctx_of(frame);
    if (ctx == nullptr || args.args == nullptr || args.argc < 1) {
        return invalid_state();
    }
    const JsValue &name = args.args[0];
    std::string_view name_sv;
    if (!string_utf8_view(name, name_sv) || name_sv.empty()) {
        return AbiResult::success(JsValue::make_undefined());
    }
    return http_script::ScriptExchangeCtx::lookup_property(frame.runtime, ctx->cookies(), name_sv);
}

// ---- resp.* (adapted from fiber ResponseFuncs; semantics unchanged) ----

AbiResult set_header_fn(void * /*userdata*/, const Library::HostCallFrame &frame, Library::Arguments args) noexcept {
    auto *ctx = ctx_of(frame);
    if (ctx == nullptr || args.args == nullptr || args.argc < 2) {
        return invalid_state();
    }
    std::string_view name;
    if (!string_utf8_view(args.args[0], name)) {
        return error_exn(frame.runtime, "set header require string key value");
    }
    std::string value_buf;
    node_as_text(args.args[1], value_buf);
    if (name.empty() || value_buf.empty()) {
        return error_exn(frame.runtime, "set header require string key value");
    }
    ctx->set_response_header(name, value_buf);
    return AbiResult::success(JsValue::make_null());
}

AbiResult add_header_fn(void * /*userdata*/, const Library::HostCallFrame &frame, Library::Arguments args) noexcept {
    auto *ctx = ctx_of(frame);
    if (ctx == nullptr || args.args == nullptr || args.argc < 2) {
        return invalid_state();
    }
    std::string_view name;
    if (!string_utf8_view(args.args[0], name)) {
        return error_exn(frame.runtime, "add header require string key value");
    }
    std::string value_buf;
    node_as_text(args.args[1], value_buf);
    if (name.empty() || value_buf.empty()) {
        return error_exn(frame.runtime, "add header require string key value");
    }
    ctx->add_response_header(name, value_buf);
    return AbiResult::success(JsValue::make_null());
}

AsyncTask send_json_fn(void * /*userdata*/, const Library::HostCallFrame &frame, Library::Arguments args) noexcept {
    auto *ctx = ctx_of(frame);
    GcHeap *heap = &frame.runtime;
    if (ctx == nullptr || heap == nullptr || args.args == nullptr || args.argc < 2) {
        co_return invalid_state();
    }
    const int status = as_int(args.args[0], 200);
    const JsValue &body = args.args[1];
    auto result = co_await ctx->write_json(status, body);
    if (!result) {
        co_return error_exn(*heap, "error send json");
    }
    co_return AbiResult::success(JsValue::make_null());
}

AsyncTask send_one_fn(void * /*userdata*/, const Library::HostCallFrame &frame, Library::Arguments args) noexcept {
    auto *ctx = ctx_of(frame);
    GcHeap *heap = &frame.runtime;
    if (ctx == nullptr || heap == nullptr || args.args == nullptr || args.argc < 1) {
        co_return invalid_state();
    }
    const int status = as_int(args.args[0], 200);
    auto result = co_await ctx->write_empty(status);
    if (!result) {
        co_return error_exn(*heap, "error send");
    }
    co_return AbiResult::success(JsValue::make_null());
}

AsyncTask send_two_fn(void * /*userdata*/, const Library::HostCallFrame &frame, Library::Arguments args) noexcept {
    auto *ctx = ctx_of(frame);
    GcHeap *heap = &frame.runtime;
    if (ctx == nullptr || heap == nullptr || args.args == nullptr || args.argc < 2) {
        co_return invalid_state();
    }
    const int status = as_int(args.args[0], 200);
    const JsValue &body = args.args[1];
    const JsNodeType kind = js_value_type(body);

    if (kind == JsNodeType::Binary) {
        const std::uint8_t *data = nullptr;
        std::size_t len = 0;
        if (!binary_bytes(body, data, len)) {
            co_return error_exn(*heap, "error write binary response");
        }
        auto result = co_await ctx->write_raw_bytes(status, data, len);
        if (!result) {
            co_return error_exn(*heap, "error write binary response");
        }
        co_return AbiResult::success(JsValue::make_null());
    }

    if (kind == JsNodeType::String) {
        std::string text;
        std::string_view sv;
        if (string_utf8_view(body, sv)) {
            // zero-copy into the rooted heap string
        } else {
            node_as_text(body, text); // best-effort for strings without a strict UTF-8 view
            sv = text;
        }
        ctx->set_response_header("Content-Type", "text/plain;charset=utf-8");
        auto result =
                co_await ctx->write_raw_bytes(status, reinterpret_cast<const std::uint8_t *>(sv.data()), sv.size());
        if (!result) {
            co_return error_exn(*heap, "error textual response");
        }
        co_return AbiResult::success(JsValue::make_null());
    }

    // Numbers / objects / arrays / null / boolean -> JSON.
    auto result = co_await ctx->write_json(status, body);
    if (!result) {
        co_return error_exn(*heap, "error send json");
    }
    co_return AbiResult::success(JsValue::make_null());
}

AbiResult add_cookie_fn(void * /*userdata*/, const Library::HostCallFrame &frame, Library::Arguments args) noexcept {
    auto *ctx = ctx_of(frame);
    GcHeap *heap = &frame.runtime;
    if (ctx == nullptr || heap == nullptr || args.args == nullptr || args.argc < 1) {
        return invalid_state();
    }
    const JsValue &node = args.args[0];
    if (js_value_type(node) != JsNodeType::Object) {
        return AbiResult::success(JsValue::make_boolean(false));
    }

    const std::string_view name = field_string(*heap, node, "name", 4);
    if (name.empty()) {
        return AbiResult::success(JsValue::make_boolean(false));
    }
    std::string value_buf;
    node_as_text(get_field(*heap, node, "value", 5), value_buf);
    const std::string_view domain = field_string(*heap, node, "domain", 6);
    const std::string_view path = field_string(*heap, node, "path", 4);
    const JsValue max_age_v = get_field(*heap, node, "maxAge", 7);
    const std::int64_t max_age = js_value_type(max_age_v) == JsNodeType::Integer ? js_value_int64(max_age_v) : -1;
    const JsValue secure_v = get_field(*heap, node, "secure", 6);
    const bool secure = js_value_type(secure_v) == JsNodeType::Boolean && js_value_bool(secure_v);
    const JsValue http_only_v = get_field(*heap, node, "httpOnly", 8);
    const bool http_only = js_value_type(http_only_v) == JsNodeType::Boolean && js_value_bool(http_only_v);
    const std::string_view same_site = field_string(*heap, node, "sameSite", 8);

    fiber::util::Cookie cookie{};
    cookie.name = name;
    cookie.value = value_buf;
    cookie.domain = domain;
    cookie.path = path;
    cookie.max_age = max_age;
    cookie.secure = secure;
    cookie.http_only = http_only;
    if (same_site == "Lax") {
        cookie.same_site = fiber::util::CookieSameSite::Lax;
    } else if (same_site == "Strict") {
        cookie.same_site = fiber::util::CookieSameSite::Strict;
    } else if (same_site == "None") {
        cookie.same_site = fiber::util::CookieSameSite::None;
    } else {
        cookie.same_site = fiber::util::CookieSameSite::Unset;
    }

    // Mirrors upstream's add_cookie_fn: encode into a stack buffer bounded by
    // the RFC 6265 6.1 cookie size limit; an empty view means the cookie was
    // refused (invalid name or oversize) and the script sees false.
    char buffer[4096];
    const std::string_view encoded =
            fiber::util::encode_set_cookie(cookie, [&buffer](std::size_t size) noexcept -> char * {
                return size <= sizeof(buffer) ? buffer : nullptr;
            });
    if (encoded.empty()) {
        return AbiResult::success(JsValue::make_boolean(false));
    }
    ctx->add_response_header("Set-Cookie", encoded);
    return AbiResult::success(JsValue::make_boolean(true));
}

// ---- ctx.report(event, label) ----

AbiResult report_fn(void * /*userdata*/, const Library::HostCallFrame &frame, Library::Arguments args) noexcept {
    auto *edge = edge_context_of(frame);
    if (edge == nullptr || args.args == nullptr || args.argc < 2) {
        return invalid_state();
    }
    std::string_view event;
    std::string_view label;
    if (!string_utf8_view(args.args[0], event) || !string_utf8_view(args.args[1], label)) {
        return error_exn(frame.runtime, "ctx.report requires string arguments");
    }
    edge->report(event, label);
    return AbiResult::success(JsValue::make_null());
}

} // namespace

// ---- EdgeCidrDirectiveDef ----

EdgeCidrDirectiveDef::EdgeCidrDirectiveDef(std::vector<Cidr> ranges) noexcept : ranges_(std::move(ranges)) {
    contains_callable_.kind = Library::HostCallable::Kind::SyncFunction;
    contains_callable_.userdata = this;
    contains_callable_.function = &EdgeCidrDirectiveDef::contains_fn;
    contains_callable_.debug_name = "cidr.contains";
}

Library::FunctionMatchResult EdgeCidrDirectiveDef::resolve_func(std::string_view /*directive*/,
                                                                std::string_view function,
                                                                const Library::FunctionMatchRequest &request,
                                                                const Library & /*library*/) const {
    if (function != "contains") {
        return Library::FunctionMatchResult::not_found();
    }
    if (request.has_spread || request.spread_argc_unknown || request.known_argc != 1) {
        return Library::FunctionMatchResult::arity_mismatch();
    }
    Library::FunctionSignature signature{};
    signature.required_argc = 1;
    signature.fixed_argc = 1;
    signature.variadic = false;
    return Library::FunctionMatchResult::found(&contains_callable_, signature, nullptr, 0);
}

Library::FunctionMatchResult EdgeCidrDirectiveDef::resolve_async_func(std::string_view /*directive*/,
                                                                      std::string_view /*function*/,
                                                                      const Library::FunctionMatchRequest & /*request*/,
                                                                      const Library & /*library*/) const {
    return Library::FunctionMatchResult::not_found();
}

bool EdgeCidrDirectiveDef::contains(const net::IpAddress &address) const noexcept {
    for (const Cidr &range: ranges_) {
        if (range.matches(address)) {
            return true;
        }
    }
    return false;
}

AbiResult EdgeCidrDirectiveDef::contains_fn(void *userdata, const Library::HostCallFrame & /*frame*/,
                                            Library::Arguments args) noexcept {
    const auto *def = static_cast<const EdgeCidrDirectiveDef *>(userdata);
    if (def == nullptr || args.args == nullptr || args.argc < 1) {
        return invalid_state();
    }
    std::string_view text;
    if (!string_utf8_view(args.args[0], text) || text.empty()) {
        // Null / empty / malformed input never matches; rule authors pick the
        // safe direction explicitly (design doc §3.4).
        return AbiResult::success(JsValue::make_boolean(false));
    }
    net::IpAddress address;
    if (!net::IpAddress::parse(text, address)) {
        return AbiResult::success(JsValue::make_boolean(false));
    }
    return AbiResult::success(JsValue::make_boolean(def->contains(address)));
}

// ---- EdgeFunctionRequestContext ----

fiber::async::Task<fiber::common::IoResult<std::unique_ptr<http_script::HttpUpstreamConnection>>>
EdgeFunctionRequestContext::acquire(const http_script::HttpTargetSpec & /*target*/,
                                    std::chrono::milliseconds /*connect_timeout*/) noexcept {
    // Unreachable from a compiled edge function (the library registers no
    // upstream functions); fail closed if the surface ever leaks.
    co_return std::unexpected(fiber::common::IoErr::Invalid);
}

// ---- EdgeFunctionLibrary ----

// Shared userdata payload for the two $ctx HostCallables.
namespace edge_ctx_detail {

inline constexpr std::uint8_t kCtxFieldProject = 0;
inline constexpr std::uint8_t kCtxFieldHost = 1;

struct FieldRef {
    std::uint8_t field = kCtxFieldProject;
};

} // namespace edge_ctx_detail

EdgeFunctionLibrary::EdgeFunctionLibrary(const AccessConfigLimits &limits) : limits_(limits.edge_functions) {
    register_request_readers();
    register_response_writers();
    library_.register_func("ctx.report",
                           Library::FunctionSignature{
                                   .required_argc = 2,
                                   .fixed_argc = 2,
                                   .variadic = false,
                           },
                           &report_fn, nullptr, "ctx.report");
    library_.add_ext_ops(&exchange_const_extension_, http_script::ExchangeConstExtension::ops());
    library_.add_ext_ops(this, ops());
    // After this library's own ops so $ctx and the cidr directive keep
    // priority; RouteScriptExtension contributes the $header/$query/$cookie
    // constants. Its http directive stays disabled per compile (see compile()).
    library_.add_ext_ops(&route_extension_, http_script::RouteScriptExtension::ops());
}

void EdgeFunctionLibrary::register_request_readers() {
    using Sig = Library::FunctionSignature;
    void *limits = const_cast<EdgeFunctionLimits *>(&limits_);
    // The async body readers (req.readJson/readBinary/discardBody) are
    // deliberately NOT registered: referencing them is a compile error.
    library_.register_func("req.getHeader", Sig{.required_argc = 0, .fixed_argc = 0, .variadic = false},
                           &get_header_all_fn, nullptr, "req.getHeader");
    library_.register_func("req.getHeader", Sig{.required_argc = 1, .fixed_argc = 1, .variadic = false},
                           &get_header_one_fn, limits, "req.getHeader");
    library_.register_func("req.getQuery", Sig{.required_argc = 0, .fixed_argc = 0, .variadic = false},
                           &get_query_all_fn, nullptr, "req.getQuery");
    library_.register_func("req.getQuery", Sig{.required_argc = 1, .fixed_argc = 1, .variadic = false},
                           &get_query_one_fn, nullptr, "req.getQuery");
    library_.register_func("req.getUri", Sig{.required_argc = 0, .fixed_argc = 0, .variadic = false}, &get_uri_fn,
                           limits, "req.getUri");
    library_.register_func("req.getPath", Sig{.required_argc = 0, .fixed_argc = 0, .variadic = false}, &get_path_fn,
                           limits, "req.getPath");
    library_.register_func("req.getQueryStr", Sig{.required_argc = 0, .fixed_argc = 0, .variadic = false},
                           &get_query_str_fn, limits, "req.getQueryStr");
    library_.register_func("req.getMethod", Sig{.required_argc = 0, .fixed_argc = 0, .variadic = false}, &get_method_fn,
                           limits, "req.getMethod");
    library_.register_func("req.getCookie", Sig{.required_argc = 0, .fixed_argc = 0, .variadic = false},
                           &get_cookie_all_fn, nullptr, "req.getCookie");
    library_.register_func("req.getCookie", Sig{.required_argc = 1, .fixed_argc = 1, .variadic = false},
                           &get_cookie_one_fn, nullptr, "req.getCookie");
}

void EdgeFunctionLibrary::register_response_writers() {
    using Sig = Library::FunctionSignature;
    library_.register_func("resp.setHeader", Sig{.required_argc = 2, .fixed_argc = 2, .variadic = false},
                           &set_header_fn, nullptr, "resp.setHeader");
    library_.register_func("resp.addHeader", Sig{.required_argc = 2, .fixed_argc = 2, .variadic = false},
                           &add_header_fn, nullptr, "resp.addHeader");
    library_.register_async_func("resp.sendJson", Sig{.required_argc = 2, .fixed_argc = 2, .variadic = false},
                                 &send_json_fn, nullptr, "resp.sendJson");
    library_.register_async_func("resp.send", Sig{.required_argc = 1, .fixed_argc = 1, .variadic = false}, &send_one_fn,
                                 nullptr, "resp.send");
    library_.register_async_func("resp.send", Sig{.required_argc = 2, .fixed_argc = 2, .variadic = false}, &send_two_fn,
                                 nullptr, "resp.send");
    library_.register_func("resp.addCookie", Sig{.required_argc = 1, .fixed_argc = 1, .variadic = false},
                           &add_cookie_fn, nullptr, "resp.addCookie");
}

const EdgeFunctionLibrary::ExtOps &EdgeFunctionLibrary::ops() noexcept {
    static const ExtOps kOps{
            .resolve_constant = &EdgeFunctionLibrary::resolve_constant_op,
            .resolve_directive_def = &EdgeFunctionLibrary::resolve_directive_def_op,
    };
    return kOps;
}

script::Library::DirectiveDef *
EdgeFunctionLibrary::resolve_directive_def_op(void *ctx, std::string_view type, std::string_view name,
                                              const std::vector<script::JsValue> &literals) {
    return static_cast<EdgeFunctionLibrary *>(ctx)->resolve_directive_def(type, name, literals);
}

const script::Library::HostCallable *
EdgeFunctionLibrary::resolve_constant_op(void *ctx, std::string_view namespace_name, std::string_view key) {
    return static_cast<EdgeFunctionLibrary *>(ctx)->resolve_constant(namespace_name, key);
}

script::Library::DirectiveDef *
EdgeFunctionLibrary::resolve_directive_def(std::string_view type, std::string_view,
                                           const std::vector<script::JsValue> &literals) {
    if (type != "cidr" || literals.empty()) {
        return nullptr;
    }
    if (pending_directives_.size() >= limits_.max_cidr_directives_per_script) {
        return nullptr;
    }
    std::vector<Cidr> ranges;
    for (const script::JsValue &literal: literals) {
        std::string_view text;
        if (!string_utf8_view(literal, text)) {
            return nullptr;
        }
        // Multiple networks per directive via ';'-separated literals; empty
        // segments are ignored ("a;;b" == "a;b").
        std::size_t start = 0;
        while (start <= text.size()) {
            const std::size_t separator = text.find(';', start);
            const std::size_t end = separator == std::string_view::npos ? text.size() : separator;
            if (end > start) {
                const std::string_view cidr_text = text.substr(start, end - start);
                if (cidr_text.size() > limits_.max_cidr_bytes) {
                    return nullptr;
                }
                auto parsed = Cidr::parse_strict(cidr_text, "cidr directive");
                if (!parsed) {
                    return nullptr; // invalid CIDR fails the compile: fail closed
                }
                if (ranges.size() >= limits_.max_cidrs_per_directive) {
                    return nullptr;
                }
                ranges.push_back(std::move(*parsed));
            }
            if (separator == std::string_view::npos) {
                break;
            }
            start = separator + 1;
        }
    }
    if (ranges.empty()) {
        return nullptr; // a cidr directive with no networks is a configuration bug
    }
    pending_directives_.push_back(std::unique_ptr<EdgeCidrDirectiveDef>(new EdgeCidrDirectiveDef(std::move(ranges))));
    return pending_directives_.back().get();
}

const script::Library::HostCallable *EdgeFunctionLibrary::resolve_constant(std::string_view namespace_name,
                                                                           std::string_view key) {
    // $req.body is the only body-phase surface (design §3.3): a native-string
    // view over the handler-prefetched bytes. It resolves only while a
    // request_body-phase script compiles — head-phase scripts that reference
    // it fail to compile.
    if (namespace_name == "$req" && key == "body" && body_phase_compile_) {
        static const script::Library::HostCallable kBodyConstant{
                .kind = script::Library::HostCallable::Kind::SyncConstant,
                .userdata = nullptr,
                .constant = &EdgeFunctionLibrary::body_constant_fn,
                .debug_name = "$req.body",
        };
        return &kBodyConstant;
    }
    if (namespace_name != "$ctx") {
        return nullptr;
    }
    // Mirrors ExchangeConstExtension's table; local so the private
    // ctx_constant_fn address is reachable.
    struct CtxTable {
        edge_ctx_detail::FieldRef project{edge_ctx_detail::kCtxFieldProject};
        edge_ctx_detail::FieldRef host{edge_ctx_detail::kCtxFieldHost};
        script::Library::HostCallable callables[2]{};

        CtxTable() noexcept {
            callables[0].kind = script::Library::HostCallable::Kind::SyncConstant;
            callables[0].userdata = &project;
            callables[0].constant = &EdgeFunctionLibrary::ctx_constant_fn;
            callables[0].debug_name = "$ctx.project";
            callables[1].kind = script::Library::HostCallable::Kind::SyncConstant;
            callables[1].userdata = &host;
            callables[1].constant = &EdgeFunctionLibrary::ctx_constant_fn;
            callables[1].debug_name = "$ctx.host";
        }
    };
    static const CtxTable kTable;
    if (key == "project") {
        return &kTable.callables[0];
    }
    if (key == "host") {
        return &kTable.callables[1];
    }
    return nullptr;
}

script::AbiResult EdgeFunctionLibrary::ctx_constant_fn(void *userdata,
                                                       const script::Library::HostCallFrame &frame) noexcept {
    const auto *ref = static_cast<const edge_ctx_detail::FieldRef *>(userdata);
    auto *edge = edge_context_of(frame);
    if (ref == nullptr || edge == nullptr) {
        return invalid_state();
    }
    switch (ref->field) {
        case edge_ctx_detail::kCtxFieldProject:
            return native_string(edge->project);
        case edge_ctx_detail::kCtxFieldHost:
            return native_string(edge->host);
        default:
            return invalid_state();
    }
}

script::AbiResult EdgeFunctionLibrary::body_constant_fn(void * /*userdata*/,
                                                        const script::Library::HostCallFrame &frame) noexcept {
    auto *edge = edge_context_of(frame);
    if (edge == nullptr) {
        return invalid_state();
    }
    return native_string(edge->request_body);
}

std::expected<EdgeFunctionProgram, script::parse::ParseError> EdgeFunctionLibrary::compile(std::string_view source,
                                                                                           EdgeFunctionPhase phase) {
    if (source.size() > limits_.max_script_payload_bytes) {
        return std::unexpected(script::parse::ParseError{
                .message = "edge-function script exceeds the configured byte limit",
                .position = 0,
        });
    }
    pending_directives_.clear();
    body_phase_compile_ = phase == EdgeFunctionPhase::RequestBody;
    http_script::ConstPackage::Builder constants;
    // Empty path-variable set: $path.* is a compile error for edge functions,
    // and the http directive stays disabled (contract: no upstream access).
    http_script::RouteScriptExtension::CompileScope compile_scope(route_extension_, constants, {},
                                                                  /*http_directives_enabled=*/false);
    auto compiled = script::compile_script(library_, source, /*allow_assign=*/false);
    if (!compiled) {
        body_phase_compile_ = false;
        return std::unexpected(std::move(compiled.error()));
    }
    auto package = constants.build();
    if (!package) {
        body_phase_compile_ = false;
        return std::unexpected(script::parse::ParseError{
                .message = "failed to finalize edge-function constant package",
                .position = 0,
        });
    }
    EdgeFunctionProgram program{
            .program = std::move(*compiled),
            .cidr_directives = std::move(pending_directives_),
            .constants = std::move(package),
    };
    pending_directives_.clear();
    body_phase_compile_ = false;
    return program;
}

} // namespace fiber::access_server
