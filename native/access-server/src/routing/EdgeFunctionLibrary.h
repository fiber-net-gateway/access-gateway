#ifndef FIBER_ACCESS_SERVER_EDGE_FUNCTION_LIBRARY_H
#define FIBER_ACCESS_SERVER_EDGE_FUNCTION_LIBRARY_H

#include "../config/AccessConfigLimits.h"
#include "Cidr.h"

#include <chrono>
#include <cstddef>
#include <expected>
#include <memory>
#include <string_view>
#include <vector>

#include <fiber/async/Task.h>
#include <fiber/common/IoError.h>
#include <fiber/http_script/ConstPackage.h>
#include <fiber/http_script/ExchangeConstExtension.h>
#include <fiber/http_script/HttpScriptServices.h>
#include <fiber/http_script/RouteScriptExtension.h>
#include <fiber/http_script/ScriptExchangeCtx.h>
#include <fiber/net/IpAddress.h>
#include <fiber/net/SocketAddress.h>
#include <fiber/script/JsValue.h>
#include <fiber/script/Library.h>
#include <fiber/script/Script.h>
#include <fiber/script/ScriptResult.h>
#include <fiber/script/parse/ParseError.h>
#include <fiber/script/std/StdLibrary.h>

namespace fiber::access_server {

// `directive <name> = cidr "a;b" "c/d";` — every CIDR is parsed while the
// script compiles (an invalid literal fails the compile, so bad CIDRs can never
// reach a data plane). The only method is contains(ip): a pure C++ interval
// match with no VM work, no allocation, and no per-call parsing. Null, empty,
// and malformed IP arguments return false (docs/edge-functions-and-waf-design
//.md §3.4: rule authors must pick the safe direction for that).
class EdgeCidrDirectiveDef final : public script::Library::DirectiveDef {
public:
    script::Library::FunctionMatchResult resolve_func(std::string_view directive, std::string_view function,
                                                      const script::Library::FunctionMatchRequest &request,
                                                      const script::Library &library) const override;
    script::Library::FunctionMatchResult resolve_async_func(std::string_view directive, std::string_view function,
                                                            const script::Library::FunctionMatchRequest &request,
                                                            const script::Library &library) const override;

    [[nodiscard]] bool contains(const net::IpAddress &address) const noexcept;

private:
    friend class EdgeFunctionLibrary;

    explicit EdgeCidrDirectiveDef(std::vector<Cidr> ranges) noexcept;

    static script::AbiResult contains_fn(void *userdata, const script::Library::HostCallFrame &frame,
                                         script::Library::Arguments arguments) noexcept;

    std::vector<Cidr> ranges_;
    script::Library::HostCallable contains_callable_{};
};

// One compiled edge function. Owns the cidr directive definitions and the
// constant package whose HostCallable userdata the compiled program
// references, so they stay alive together and nothing is shared between
// functions.
struct EdgeFunctionProgram {
    script::Script program;
    std::vector<std::unique_ptr<EdgeCidrDirectiveDef>> cidr_directives;
    // Lifetime anchor for the compiled $header/$query/$cookie references; the
    // package must outlive every ScriptExchangeCtx the program runs against.
    std::shared_ptr<const http_script::ConstPackage> constants;
};

// Per-request side channel for edge functions, installed through
// ScriptExchangeCtx::set_services() for the duration of one function
// execution (the executor saves and restores the previous services pointer).
// Backs ctx.report(event, label) and the $ctx.project / $ctx.host runtime
// constants.
class EdgeFunctionRequestContext : public http_script::HttpScriptServices {
public:
    // Contract "no upstream access": the edge library never registers http
    // request functions, so acquire() is unreachable from a compiled edge
    // function. Fail closed instead of asserting in case of future surface
    // leakage.
    fiber::async::Task<fiber::common::IoResult<std::unique_ptr<http_script::HttpUpstreamConnection>>>
    acquire(const http_script::HttpTargetSpec &, std::chrono::milliseconds) noexcept override;

    // Counts one event. Implementations own the per-request report budget
    // (limits.edge_functions.max_reports_per_execution): excess calls are
    // dropped and counted, never propagated into the VM.
    virtual void report(std::string_view event, std::string_view label) noexcept = 0;

    // Set by the executor immediately before each program execution.
    std::string_view project;
    std::string_view host;
    std::string_view function;
};

// Restricted edge-function library (docs/edge-functions-and-waf-design.md
// §3.3). Registered surface:
//   - $req / $conn constants (ExchangeConstExtension; no prepare needed)
//   - $ctx.project / $ctx.host runtime constants (this library)
//   - $header / $query / $cookie constants (RouteScriptExtension, resolved
//     dynamically per request with per-request memoization; values are NOT
//     length-truncated unlike the req.* readers — matching stays linear-time)
//   - synchronous req.* readers (getHeader/getQuery/getUri/getPath/
//     getQueryStr/getMethod/getCookie) with input truncation — the async body
//     readers (req.readJson/readBinary/discardBody) are deliberately absent,
//     making them compile-time errors
//   - full resp.* response functions
//   - ctx.report(event, label)
//   - directive <name> = cidr "..." "..."
// Deliberately absent: $context and $path (no route is selected when edge
// functions run, and the trace extension is project-only) and the http
// directive (no upstream access; RouteScriptExtension compiles with
// http_directives_enabled=false).
//
// Compilation is serialized (the watcher owns one instance); compiled programs
// keep no reference to the library except through their own
// EdgeFunctionProgram userdata.
class EdgeFunctionLibrary final {
public:
    explicit EdgeFunctionLibrary(const AccessConfigLimits &limits = kAccessConfigLimits);

    EdgeFunctionLibrary(const EdgeFunctionLibrary &) = delete;
    EdgeFunctionLibrary &operator=(const EdgeFunctionLibrary &) = delete;

    // allow_assign=false; unresolved names are compile errors. Exceeding the
    // script payload limit fails before parsing.
    [[nodiscard]] std::expected<EdgeFunctionProgram, script::parse::ParseError> compile(std::string_view source);

    [[nodiscard]] const EdgeFunctionLimits &limits() const noexcept { return limits_; }

private:
    using ExtOps = script::std_lib::StdLibrary::ExtOps;

    [[nodiscard]] static const ExtOps &ops() noexcept;
    [[nodiscard]] static script::Library::DirectiveDef *
    resolve_directive_def_op(void *ctx, std::string_view type, std::string_view name,
                             const std::vector<script::JsValue> &literals);
    [[nodiscard]] static const script::Library::HostCallable *
    resolve_constant_op(void *ctx, std::string_view namespace_name, std::string_view key);
    static script::AbiResult ctx_constant_fn(void *userdata, const script::Library::HostCallFrame &frame) noexcept;

    [[nodiscard]] script::Library::DirectiveDef *resolve_directive_def(std::string_view type, std::string_view name,
                                                                       const std::vector<script::JsValue> &literals);
    [[nodiscard]] const script::Library::HostCallable *resolve_constant(std::string_view namespace_name,
                                                                        std::string_view key);

    void register_request_readers();
    void register_response_writers();

    EdgeFunctionLimits limits_;
    script::std_lib::StdLibrary library_;
    http_script::ExchangeConstExtension exchange_const_extension_;
    http_script::RouteScriptExtension route_extension_;
    // Directives created by the in-flight compile() call; moved into the
    // returned EdgeFunctionProgram on success.
    std::vector<std::unique_ptr<EdgeCidrDirectiveDef>> pending_directives_;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_EDGE_FUNCTION_LIBRARY_H
