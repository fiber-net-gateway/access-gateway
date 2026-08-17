#ifndef FIBER_ACCESS_SERVER_ACCESS_SCRIPT_COMPILER_H
#define FIBER_ACCESS_SERVER_ACCESS_SCRIPT_COMPILER_H

#include "ProjectConfigCompiler.h"

#include <span>
#include <string>
#include <string_view>

#include <fiber/http_script/ExchangeConstExtension.h>
#include <fiber/http_script/RouteScriptExtension.h>
#include <fiber/script/std/StdLibrary.h>

namespace fiber::access_server {

// Process-lifetime owner for the side-effect-free route compilation libraries
// and extension userdata referenced by compiled Project snapshots.
class AccessScriptCompiler final {
public:
    AccessScriptCompiler();

    [[nodiscard]] ScriptCompilerAdapter adapter() noexcept;

private:
    [[nodiscard]] static ScriptCompilerAdapter::Result
    compile_expression(void *context, http_script::ConstPackage::Builder &constants, std::string_view expression,
                       std::span<const std::string> path_variable_names);
    [[nodiscard]] static ScriptCompilerAdapter::Result
    compile_route_script(void *context, http_script::ConstPackage::Builder &constants, std::string_view source,
                         std::span<const std::string> path_variable_names);

    script::std_lib::StdLibrary expression_library_;
    script::std_lib::StdLibrary route_library_;
    http_script::ExchangeConstExtension exchange_const_extension_;
    http_script::RouteScriptExtension route_extension_;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_ACCESS_SCRIPT_COMPILER_H
