#include "ResponsePlan.h"

#include <algorithm>

#include <fiber/http/HttpHeaderHash.h>
#include <fiber/http/HttpHeaders.h>

namespace fiber::access_server {
namespace {

std::string_view trim_ows(std::string_view value) noexcept {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
        value.remove_suffix(1);
    }
    return value;
}

bool ascii_iequals(std::string_view left, std::string_view right) noexcept {
    return http::http_header_name_equals_ci(left, right);
}

std::optional<int> parse_quality(std::string_view value) noexcept {
    value = trim_ows(value);
    if (value.empty()) {
        return std::nullopt;
    }
    if (value.front() == '1') {
        value.remove_prefix(1);
        if (value.empty()) {
            return 1000;
        }
        if (value.front() != '.') {
            return std::nullopt;
        }
        value.remove_prefix(1);
        if (value.size() > 3) {
            return std::nullopt;
        }
        for (const char ch: value) {
            if (ch != '0') {
                return std::nullopt;
            }
        }
        return 1000;
    }
    if (value.front() != '0') {
        return std::nullopt;
    }
    value.remove_prefix(1);
    if (value.empty()) {
        return 0;
    }
    if (value.front() != '.') {
        return std::nullopt;
    }
    value.remove_prefix(1);
    if (value.size() > 3) {
        return std::nullopt;
    }
    int quality = 0;
    int scale = 100;
    for (const char ch: value) {
        if (ch < '0' || ch > '9') {
            return std::nullopt;
        }
        quality += (ch - '0') * scale;
        scale /= 10;
    }
    return quality;
}

struct EncodingPreferences {
    int gzip = -1;
    int identity = -1;
    int wildcard = -1;
    bool header_present = false;
};

void update_quality(int &current, int quality) noexcept { current = std::max(current, quality); }

void parse_encoding_item(std::string_view item, EncodingPreferences &preferences) noexcept {
    item = trim_ows(item);
    if (item.empty()) {
        return;
    }

    const std::size_t semicolon = item.find(';');
    const std::string_view coding = trim_ows(item.substr(0, semicolon));
    int quality = 1000;
    bool valid = !coding.empty();
    bool quality_seen = false;
    std::string_view parameters = semicolon == std::string_view::npos ? std::string_view{} : item.substr(semicolon + 1);
    while (!parameters.empty()) {
        const std::size_t next = parameters.find(';');
        const std::string_view parameter = trim_ows(parameters.substr(0, next));
        parameters = next == std::string_view::npos ? std::string_view{} : parameters.substr(next + 1);
        const std::size_t equals = parameter.find('=');
        if (equals == std::string_view::npos || !ascii_iequals(trim_ows(parameter.substr(0, equals)), "q") ||
            quality_seen) {
            valid = false;
            continue;
        }
        quality_seen = true;
        const auto parsed = parse_quality(parameter.substr(equals + 1));
        if (!parsed) {
            valid = false;
            continue;
        }
        quality = *parsed;
    }
    if (!valid) {
        quality = 0;
    }

    if (ascii_iequals(coding, "gzip")) {
        update_quality(preferences.gzip, quality);
    } else if (ascii_iequals(coding, "identity")) {
        update_quality(preferences.identity, quality);
    } else if (coding == "*") {
        update_quality(preferences.wildcard, quality);
    }
}

void parse_accept_encoding(std::string_view value, EncodingPreferences &preferences) noexcept {
    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t comma = value.find(',', start);
        parse_encoding_item(
                value.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start),
                preferences);
        if (comma == std::string_view::npos) {
            return;
        }
        start = comma + 1;
    }
}

bool has_vary_token(std::string_view value, std::string_view expected) noexcept {
    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t comma = value.find(',', start);
        const std::string_view token =
                trim_ows(value.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start));
        if (token == "*" || ascii_iequals(token, expected)) {
            return true;
        }
        if (comma == std::string_view::npos) {
            return false;
        }
        start = comma + 1;
    }
    return false;
}

EvaluatedHeader *find_header(std::vector<EvaluatedHeader> &headers, std::string_view name) noexcept {
    EvaluatedHeader *matched = nullptr;
    for (EvaluatedHeader &header: headers) {
        if (ascii_iequals(header.name, name)) {
            matched = &header;
        }
    }
    // HttpHeaders::set applies configured response headers in source order, so
    // the last ASCII-case-insensitive duplicate is the effective value.
    return matched;
}

void merge_vary_accept_encoding(std::vector<EvaluatedHeader> &headers) {
    EvaluatedHeader *vary = find_header(headers, "Vary");
    if (!vary) {
        constexpr std::string_view kName = "Vary";
        constexpr std::string_view kLowcaseName = "vary";
        headers.push_back({
                .name = kName,
                .lowcase_name = kLowcaseName,
                .name_hash = http::http_header_name_hash(kLowcaseName),
                .value = EvaluatedTemplate::borrowed("Accept-Encoding"),
        });
        return;
    }
    if (has_vary_token(vary->value.view(), "Accept-Encoding")) {
        return;
    }
    std::string &value = vary->value.materialize();
    if (!value.empty()) {
        value.append(", ");
    }
    value.append("Accept-Encoding");
}

void weaken_strong_etag(std::vector<EvaluatedHeader> &headers) {
    EvaluatedHeader *etag = find_header(headers, "ETag");
    if (etag && !etag->value.view().empty() && etag->value.view().front() == '"') {
        etag->value.materialize().insert(0, "W/");
    }
}

} // namespace

bool is_valid_http_header_name(std::string_view name) noexcept {
    if (name.empty()) {
        return false;
    }
    for (const unsigned char ch: name) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')) {
            continue;
        }
        switch (ch) {
            case '!':
            case '#':
            case '$':
            case '%':
            case '&':
            case '\'':
            case '*':
            case '+':
            case '-':
            case '.':
            case '^':
            case '_':
            case '`':
            case '|':
            case '~':
                continue;
            default:
                return false;
        }
    }
    return true;
}

bool is_valid_http_header_value(std::string_view value) noexcept {
    for (const unsigned char ch: value) {
        if (ch == '\0' || ch == '\v' || ch == '\f' || ch == '\r' || ch == '\n' || ch == 0x7fU) {
            return false;
        }
    }
    return true;
}

bool is_java_filtered_response_header(std::string_view name) noexcept {
    return http::http_header_name_equals_ci(name, "connection") ||
           http::http_header_name_equals_ci(name, "content-length") ||
           http::http_header_name_equals_ci(name, "proxy-connection") ||
           http::http_header_name_equals_ci(name, "keep-alive") ||
           http::http_header_name_equals_ci(name, "proxy-authenticate") ||
           http::http_header_name_equals_ci(name, "proxy-authorization") ||
           http::http_header_name_equals_ci(name, "te") || http::http_header_name_equals_ci(name, "trailer") ||
           http::http_header_name_equals_ci(name, "transfer-encoding") ||
           http::http_header_name_equals_ci(name, "upgrade");
}

PreparedResponseResult prepare_response(const CompiledResponseRoute &response, TemplateEvaluator evaluator) {
    PreparedResponse prepared;
    prepared.status = response.status;

    prepared.headers.reserve(response.response_headers.size() + (response.gzip_level ? 2U : 0U));
    for (const CompiledResponseHeaderTemplate &header: response.response_headers) {
        auto value = evaluate_template(header.value, evaluator);
        if (!value) {
            return std::unexpected(value.error());
        }
        prepared.headers.push_back(EvaluatedHeader{
                .name = header.name,
                .lowcase_name = header.lowcase_name,
                .name_hash = header.name_hash,
                .value = std::move(*value),
        });
    }

    std::size_t committed = 0;
    for (std::size_t index = 0; index < prepared.headers.size(); ++index) {
        EvaluatedHeader &header = prepared.headers[index];
        if (is_java_filtered_response_header(header.name)) {
            continue;
        }
        if (!is_valid_http_header_name(header.name) || !is_valid_http_header_value(header.value.view())) {
            return std::unexpected(Err::from_exception(Exception::unknown("invalid response header")));
        }
        if (committed != index) {
            prepared.headers[committed] = std::move(header);
        }
        ++committed;
    }
    prepared.headers.resize(committed);

    if (response.body_kind == ResponseBodyKind::Template) {
        if (!response.body_template) {
            return std::unexpected(Err::from_exception(Exception{
                    .name = "TEMPLATE_SCRIPT",
                    .message = "error exec for template expression: invalid compiled template",
                    .status = 500,
            }));
        }
        auto body = evaluate_template(*response.body_template, evaluator);
        if (!body) {
            return std::unexpected(body.error());
        }
        prepared.body = std::move(*body);
    } else {
        prepared.body = EvaluatedTemplate::borrowed(response.body);
    }
    return prepared;
}

ResponseContentCoding select_response_content_coding(const http::HttpHeaders &request_headers) noexcept {
    constexpr std::string_view kAcceptEncoding = "accept-encoding";
    constexpr std::uint64_t kAcceptEncodingHash = http::http_header_name_hash(kAcceptEncoding);
    EncodingPreferences preferences;
    for (const http::HttpHeaders::HeaderField &field: request_headers.get_all(kAcceptEncoding, kAcceptEncodingHash)) {
        preferences.header_present = true;
        parse_accept_encoding(field.value_view(), preferences);
    }
    if (!preferences.header_present) {
        return ResponseContentCoding::Identity;
    }

    const int gzip = preferences.gzip >= 0 ? preferences.gzip : std::max(preferences.wildcard, 0);
    const int identity = preferences.identity >= 0 ? preferences.identity : (preferences.wildcard == 0 ? 0 : 1000);
    if (gzip == 0 && identity == 0) {
        return ResponseContentCoding::NotAcceptable;
    }
    return gzip > 0 && gzip >= identity ? ResponseContentCoding::Gzip : ResponseContentCoding::Identity;
}

Result<ResponseContentCoding> apply_response_gzip(const CompiledResponseRoute &response,
                                                  const http::HttpHeaders &request_headers,
                                                  PreparedResponse &prepared) {
    if (!response.gzip_level) {
        return ResponseContentCoding::Identity;
    }
    if (response.gzip_body.empty()) {
        return std::unexpected(Err::from_exception(Exception::unknown("compiled gzip body is missing")));
    }
    if (find_header(prepared.headers, "Content-Encoding")) {
        return std::unexpected(Err::from_exception(Exception::unknown("gzip response has Content-Encoding")));
    }

    const ResponseContentCoding coding = select_response_content_coding(request_headers);
    if (coding == ResponseContentCoding::NotAcceptable) {
        return std::unexpected(Err::from_exception(Exception::not_acceptable()));
    }

    merge_vary_accept_encoding(prepared.headers);
    weaken_strong_etag(prepared.headers);
    if (coding == ResponseContentCoding::Gzip) {
        constexpr std::string_view kName = "Content-Encoding";
        constexpr std::string_view kLowcaseName = "content-encoding";
        prepared.headers.push_back({
                .name = kName,
                .lowcase_name = kLowcaseName,
                .name_hash = http::http_header_name_hash(kLowcaseName),
                .value = EvaluatedTemplate::borrowed("gzip"),
        });
        prepared.body = EvaluatedTemplate::borrowed(response.gzip_body);
    }
    return coding;
}

} // namespace fiber::access_server
