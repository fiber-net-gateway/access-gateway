#ifndef FIBER_ACCESS_SERVER_ACCESS_SCRIPT_RUNTIME_H
#define FIBER_ACCESS_SERVER_ACCESS_SCRIPT_RUNTIME_H

#include "../execution/AccessRequestHandler.h"

#include <string>
#include <string_view>

namespace fiber::access_server {

// Stateless request-script execution adapter. Route compilation and its
// process-lifetime extension userdata are owned by AccessScriptCompiler.
class AccessScriptRuntime {
public:
    AccessScriptRuntime() = default;

    [[nodiscard]] AccessRequestScriptAdapter request_adapter() noexcept;

private:
    [[nodiscard]] static bool evaluate_condition(void *context, http_script::ScriptExchangeCtx &script_context,
                                                 const script::Script &program) noexcept;
    [[nodiscard]] static Result<void> evaluate_template(void *context, http_script::ScriptExchangeCtx &script_context,
                                                        const script::Script &program, std::string_view expression,
                                                        std::string &output) noexcept;
    [[nodiscard]] static async::Task<Result<void>> execute_route_script(void *context,
                                                                        http_script::ScriptExchangeCtx &script_context,
                                                                        script::Script &program) noexcept;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_ACCESS_SCRIPT_RUNTIME_H
