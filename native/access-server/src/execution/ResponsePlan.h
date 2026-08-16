#ifndef FIBER_ACCESS_SERVER_RESPONSE_PLAN_H
#define FIBER_ACCESS_SERVER_RESPONSE_PLAN_H

#include "../routing/ProjectRouteSnapshot.h"
#include "AccessResult.h"
#include "TemplateEvaluator.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace fiber::http {
class HttpHeaders;
}

namespace fiber::access_server {

struct EvaluatedHeader {
    std::string name;
    std::string value;
};

struct PreparedResponse {
    int status = 0;
    std::vector<EvaluatedHeader> headers;
    std::variant<std::string_view, std::string> body{std::string_view{}};

    [[nodiscard]] std::string_view body_view() const noexcept {
        if (const auto *view = std::get_if<std::string_view>(&body)) {
            return *view;
        }
        return std::get<std::string>(body);
    }
};

enum class ResponseContentCoding : std::uint8_t {
    Identity,
    Gzip,
    NotAcceptable,
};

using PreparedResponseResult = Result<PreparedResponse>;

[[nodiscard]] bool is_java_filtered_response_header(std::string_view name) noexcept;
[[nodiscard]] bool is_valid_http_header_name(std::string_view name) noexcept;
[[nodiscard]] bool is_valid_http_header_value(std::string_view value) noexcept;
[[nodiscard]] PreparedResponseResult prepare_response(const CompiledResponseRoute &response,
                                                      TemplateEvaluator evaluator);
[[nodiscard]] ResponseContentCoding select_response_content_coding(const http::HttpHeaders &request_headers) noexcept;
[[nodiscard]] Result<ResponseContentCoding> apply_response_gzip(const CompiledResponseRoute &response,
                                                                const http::HttpHeaders &request_headers,
                                                                PreparedResponse &prepared);

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_RESPONSE_PLAN_H
