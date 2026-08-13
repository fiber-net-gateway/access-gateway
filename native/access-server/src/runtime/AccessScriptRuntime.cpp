#include "AccessScriptRuntime.h"

#include <utility>

#include <fiber/http_script/HttpScriptLib.h>
#include <fiber/http_script/RequestFuncs.h>
#include <fiber/http_script/ScriptExchangeCtx.h>
#include <fiber/script/JsGc.h>
#include <fiber/script/JsValue.h>
#include <fiber/script/Script.h>
#include <fiber/script/ScriptCompiler.h>
#include <fiber/script/run/Compares.h>
#include <fiber/script/std/NodeText.h>

namespace fiber::access_server {
namespace {

std::string script_failure_message(const script::ScriptResult &result) {
    if (result.is_abort()) {
        std::string message = "local script aborted: ";
        message.append(script::abort_reason_name(result.abort().reason));
        return message;
    }
    if (result.is_exception()) {
        return "local script raised an exception";
    }
    return "local script returned no value";
}

} // namespace

AccessScriptRuntime::AccessScriptRuntime() {
    http_script::register_request_funcs(expression_library_);
    http_script::register_http_functions_to_lib(route_library_);
    expression_library_.add_ext_ops(&exchange_const_extension_, http_script::ExchangeConstExtension::ops());
    expression_library_.add_ext_ops(&route_extension_, http_script::RouteScriptExtension::ops());
    route_library_.add_ext_ops(&exchange_const_extension_, http_script::ExchangeConstExtension::ops());
    route_library_.add_ext_ops(&route_extension_, http_script::RouteScriptExtension::ops());
}

ScriptCompilerAdapter AccessScriptRuntime::compiler_adapter() noexcept {
    return ScriptCompilerAdapter{
            .context = this,
            .compile_expression = compile_expression,
            .compile_route_script = compile_route_script,
    };
}

AccessRequestScriptAdapter AccessScriptRuntime::request_adapter() noexcept {
    return AccessRequestScriptAdapter{
            .context = this,
            .evaluate_condition = evaluate_condition,
            .evaluate_template = evaluate_template,
            .execute_route_script = execute_route_script,
    };
}

ScriptCompilerAdapter::Result
AccessScriptRuntime::compile_route_script(void *context, http_script::ConstPackage::Builder &constants,
                                          std::string_view source, std::span<const std::string> path_variable_names) {
    auto &runtime = *static_cast<AccessScriptRuntime *>(context);
    http_script::RouteScriptExtension::CompileScope compile_scope(runtime.route_extension_, constants,
                                                                  path_variable_names, false);
    auto compiled = script::compile_script(runtime.route_library_, source, true);
    if (!compiled) {
        std::string message = "route script compile failed at script offset ";
        message.append(std::to_string(compiled.error().position));
        return std::unexpected(std::move(message));
    }
    return std::move(*compiled);
}

ScriptCompilerAdapter::Result
AccessScriptRuntime::compile_expression(void *context, http_script::ConstPackage::Builder &constants,
                                        std::string_view expression, std::span<const std::string> path_variable_names) {
    auto &runtime = *static_cast<AccessScriptRuntime *>(context);
    http_script::RouteScriptExtension::CompileScope compile_scope(runtime.route_extension_, constants,
                                                                  path_variable_names, false);

    std::string source;
    source.reserve(expression.size() + 8);
    source.append("return ");
    source.append(expression);
    source.push_back(';');
    auto compiled = script::compile_script(runtime.expression_library_, source, false);
    if (!compiled) {
        std::string message = compiled.error().message;
        message.append(" at expression offset ");
        message.append(std::to_string(compiled.error().position));
        return std::unexpected(std::move(message));
    }
    if (compiled->contains_async()) {
        return std::unexpected("asynchronous route expressions are not supported");
    }
    return std::move(*compiled);
}

bool AccessScriptRuntime::evaluate_condition(void *, http_script::ScriptExchangeCtx &script_context,
                                             const script::Script &program) noexcept {
    auto result = program.exec_sync(script::JsValue::make_null(), &script_context, script_context.heap());
    if (!result.is_value()) {
        return false;
    }
    script::JsValue value = result.value();
    return script::run::Compares::logic(script::ConstValueHandle(&value));
}

Result<void> AccessScriptRuntime::evaluate_template(void *, http_script::ScriptExchangeCtx &script_context,
                                                    const script::Script &program, std::string_view,
                                                    std::string &output) noexcept {
    auto result = program.exec_sync(script::JsValue::make_null(), &script_context, script_context.heap());
    if (!result.is_value()) {
        auto exception =
                make_template_script_exception(script_context.exchange().pool(), script_failure_message(result));
        if (!exception) {
            return std::unexpected(Err::from_error(exception.error()));
        }
        return std::unexpected(Err::from_exception(*exception));
    }
    const script::JsNodeType type = script::js_value_type(result.value());
    if (type != script::JsNodeType::Null && type != script::JsNodeType::Undefined) {
        script::std_lib::node_as_text(result.value(), output);
    }
    return {};
}

async::Task<Result<void>> AccessScriptRuntime::execute_route_script(void *,
                                                                    http_script::ScriptExchangeCtx &script_context,
                                                                    script::Script &program) noexcept {
    script::JsValue root = script::JsValue::make_undefined();
    auto result = co_await program.exec_async(root, &script_context, script_context.heap());
    if (script_context.response_header_sent()) {
        co_return Result<void>{};
    }

    common::IoResult<void> written;
    switch (result.kind) {
        case script::ScriptResultKind::Void:
            written = co_await script_context.write_empty(204);
            break;
        case script::ScriptResultKind::Value:
            written = co_await script_context.write_json(200, result.value());
            break;
        case script::ScriptResultKind::Exception:
        case script::ScriptResultKind::Abort:
            co_return std::unexpected(Err::from_exception(Exception{
                    .name = "SCRIPT_EXECUTION",
                    .message = "route script execution failed",
                    .status = 500,
            }));
    }
    if (!written) {
        co_return std::unexpected(Err::from_error(written.error()));
    }
    co_return Result<void>{};
}

} // namespace fiber::access_server
