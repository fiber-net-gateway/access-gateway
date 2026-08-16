#include "ScriptExecutionContext.h"

#include <cstring>

#include <fiber/http/HttpExchange.h>

namespace fiber::access_server {

ScriptExecutionContext::ScriptExecutionContext(http::HttpExchange &exchange) noexcept :
    script_heap_(exchange.pool()), script_context_(exchange, script_heap_), response_headers_(exchange.pool()) {}

std::string_view ScriptExecutionContext::copy_to_request_pool(std::string_view value) noexcept {
    if (value.empty()) {
        return {};
    }
    char *copy = pool().alloc<char>(value.size());
    if (!copy) {
        return {};
    }
    std::memcpy(copy, value.data(), value.size());
    return {copy, value.size()};
}

} // namespace fiber::access_server
