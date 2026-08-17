#include "AccessScriptCompiler.h"

#include <expected>
#include <utility>

#include <fiber/http_script/HttpScriptLib.h>
#include <fiber/http_script/RequestFuncs.h>
#include <fiber/script/ScriptCompiler.h>

namespace fiber::access_server {

AccessScriptCompiler::AccessScriptCompiler() {
    http_script::register_request_funcs(expression_library_);
    http_script::register_http_functions_to_lib(route_library_);
    expression_library_.add_ext_ops(&exchange_const_extension_, http_script::ExchangeConstExtension::ops());
    expression_library_.add_ext_ops(&route_extension_, http_script::RouteScriptExtension::ops());
    route_library_.add_ext_ops(&exchange_const_extension_, http_script::ExchangeConstExtension::ops());
    route_library_.add_ext_ops(&route_extension_, http_script::RouteScriptExtension::ops());
}

ScriptCompilerAdapter AccessScriptCompiler::adapter() noexcept {
    return ScriptCompilerAdapter{
            .context = this,
            .compile_expression = compile_expression,
            .compile_route_script = compile_route_script,
    };
}

ScriptCompilerAdapter::Result
AccessScriptCompiler::compile_route_script(void *context, http_script::ConstPackage::Builder &constants,
                                           std::string_view source, std::span<const std::string> path_variable_names) {
    auto &compiler = *static_cast<AccessScriptCompiler *>(context);
    http_script::RouteScriptExtension::CompileScope compile_scope(compiler.route_extension_, constants,
                                                                  path_variable_names, false);
    auto compiled = script::compile_script(compiler.route_library_, source, true);
    if (!compiled) {
        std::string message = "route script compile failed at script offset ";
        message.append(std::to_string(compiled.error().position));
        return std::unexpected(std::move(message));
    }
    return std::move(*compiled);
}

ScriptCompilerAdapter::Result
AccessScriptCompiler::compile_expression(void *context, http_script::ConstPackage::Builder &constants,
                                         std::string_view expression,
                                         std::span<const std::string> path_variable_names) {
    auto &compiler = *static_cast<AccessScriptCompiler *>(context);
    http_script::RouteScriptExtension::CompileScope compile_scope(compiler.route_extension_, constants,
                                                                  path_variable_names, false);

    std::string source;
    source.reserve(expression.size() + 8);
    source.append("return ");
    source.append(expression);
    source.push_back(';');
    auto compiled = script::compile_script(compiler.expression_library_, source, false);
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

} // namespace fiber::access_server
