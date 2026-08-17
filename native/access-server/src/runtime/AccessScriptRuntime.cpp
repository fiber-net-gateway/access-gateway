#include "AccessScriptRuntime.h"

#include <expected>
#include <string>

#include <fiber/http_script/ScriptExchangeCtx.h>
#include <fiber/script/JsGc.h>
#include <fiber/script/JsValue.h>
#include <fiber/script/Script.h>
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

AccessRequestScriptAdapter AccessScriptRuntime::request_adapter() noexcept {
    return AccessRequestScriptAdapter{
            .context = this,
            .evaluate_condition = evaluate_condition,
            .evaluate_template = evaluate_template,
            .execute_route_script = execute_route_script,
    };
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
