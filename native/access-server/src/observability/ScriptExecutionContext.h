#ifndef FIBER_ACCESS_SERVER_SCRIPT_EXECUTION_CONTEXT_H
#define FIBER_ACCESS_SERVER_SCRIPT_EXECUTION_CONTEXT_H

#include <array>
#include <string_view>

#include <fiber/http/HttpExchange.h>
#include <fiber/http/HttpHeaders.h>
#include <fiber/http_script/ConstPackage.h>
#include <fiber/http_script/ScriptExchangeCtx.h>
#include <fiber/script/JsGc.h>

namespace fiber::mem {
class BufPool;
}

namespace fiber::access_server {

// Owns all script/runtime state tied to one HttpExchange. It deliberately has
// no observability or tracing policy so scripts can be tested independently.
class ScriptExecutionContext final {
public:
    explicit ScriptExecutionContext(http::HttpExchange &exchange) noexcept;

    ScriptExecutionContext(const ScriptExecutionContext &) = delete;
    ScriptExecutionContext &operator=(const ScriptExecutionContext &) = delete;
    ScriptExecutionContext(ScriptExecutionContext &&) = delete;
    ScriptExecutionContext &operator=(ScriptExecutionContext &&) = delete;

    [[nodiscard]] http_script::ScriptExchangeCtx &script_context() noexcept { return script_context_; }
    [[nodiscard]] const http_script::ScriptExchangeCtx &script_context() const noexcept { return script_context_; }
    [[nodiscard]] http::HttpHeaders &response_headers() noexcept { return response_headers_; }
    [[nodiscard]] const http::HttpHeaders &response_headers() const noexcept { return response_headers_; }
    [[nodiscard]] http::HttpExchange &exchange() const noexcept { return script_context_.exchange(); }
    [[nodiscard]] mem::BufPool &pool() const noexcept { return exchange().pool(); }

    [[nodiscard]] std::string_view copy_to_request_pool(std::string_view value) noexcept;
    [[nodiscard]] bool bind_context_constant(const http_script::ConstPackage &constants, std::string_view key,
                                             std::string_view value) noexcept {
        const http_script::ConstIndex index = constants.find(http_script::ConstType::Context, key);
        return index == http_script::kInvalidConstIndex || script_context_.bind_constant(index, value);
    }
    [[nodiscard]] bool bind_header_constant(const http_script::ConstPackage &constants, std::string_view name,
                                            std::string_view value) noexcept {
        const http_script::ConstIndex index = constants.find(http_script::ConstType::Header, name);
        return index == http_script::kInvalidConstIndex || script_context_.bind_constant(index, value);
    }
    void clear_context_constant(const http_script::ConstPackage &constants, std::string_view key) noexcept {
        const std::array<http_script::ConstIndex, 1> indices{
                constants.find(http_script::ConstType::Context, key),
        };
        script_context_.clear_constants(indices);
    }

private:
    script::GcHeap script_heap_;
    http_script::ScriptExchangeCtx script_context_;
    http::HttpHeaders response_headers_;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_SCRIPT_EXECUTION_CONTEXT_H
