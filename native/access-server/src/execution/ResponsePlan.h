#ifndef FIBER_ACCESS_SERVER_RESPONSE_PLAN_H
#define FIBER_ACCESS_SERVER_RESPONSE_PLAN_H

#include "../routing/ProjectRouteSnapshot.h"
#include "AccessResult.h"
#include "TemplateEvaluator.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fiber::http {
class HttpHeaders;
}

namespace fiber::access_server {

struct EvaluatedHeader {
    std::string_view name;
    std::string_view lowcase_name;
    std::uint64_t name_hash = 0;
    EvaluatedTemplate value;
};

struct PreparedResponse {
    int status = 0;
    std::vector<EvaluatedHeader> headers;
    EvaluatedTemplate body;

    [[nodiscard]] std::string_view body_view() const noexcept { return body.view(); }
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
