#include <gtest/gtest.h>

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fiber/common/mem/BufPool.h>
#include <fiber/http/HttpHeaders.h>
#include <zlib.h>
#include "execution/AccessResult.h"
#include "execution/ErrorResponder.h"
#include "execution/GzipEncoder.h"
#include "execution/ProxyResponsePlan.h"
#include "execution/ResponsePlan.h"
#include "execution/TemplateEvaluator.h"

namespace {

using fiber::access_server::apply_response_gzip;
using fiber::access_server::CompiledHeaderTemplates;
using fiber::access_server::CompiledResponseRoute;
using fiber::access_server::CompiledTemplate;
using fiber::access_server::CompiledTemplateEntry;
using fiber::access_server::Err;
using fiber::access_server::ErrorResponder;
using fiber::access_server::evaluate_template;
using fiber::access_server::Exception;
using fiber::access_server::gzip_encode;
using fiber::access_server::is_java_filtered_response_header;
using fiber::access_server::make_url_not_matched_exception;
using fiber::access_server::parse_template;
using fiber::access_server::prepare_proxy_response_headers;
using fiber::access_server::prepare_response;
using fiber::access_server::ResponseBodyKind;
using fiber::access_server::ResponseContentCoding;
using fiber::access_server::Result;
using fiber::access_server::rewrite_java_proxy_location;
using fiber::access_server::rewrite_java_proxy_refresh;
using fiber::access_server::select_response_content_coding;
using fiber::access_server::TemplateEvaluator;

struct EvaluatorState {
    std::vector<std::string> expressions;
    std::optional<std::string> failing_expression;
};

Result<void> evaluate_expression(void *context, const fiber::script::Script &, std::string_view expression,
                                 std::string &output) noexcept {
    auto &state = *static_cast<EvaluatorState *>(context);
    state.expressions.emplace_back(expression);
    if (state.failing_expression && expression == *state.failing_expression) {
        return std::unexpected(Err::from_exception(Exception{
                .name = "TEMPLATE_SCRIPT",
                .message = "error exec for template expression: fixture failure",
                .status = 500,
        }));
    }
    if (expression == "$path.id") {
        output = "42";
    } else if (expression == "$request.method") {
        output = "POST";
    } else {
        output.clear();
    }
    return {};
}

TemplateEvaluator evaluator(EvaluatorState &state) {
    return TemplateEvaluator{
            .context = &state,
            .evaluate = evaluate_expression,
    };
}

CompiledTemplate compiled_template(std::string_view source) {
    auto compiled = parse_template(source);
    EXPECT_TRUE(compiled);
    return compiled ? std::move(*compiled) : CompiledTemplate{};
}

CompiledTemplateEntry template_entry(std::string name, std::string_view source) {
    return CompiledTemplateEntry{
            .name = std::move(name),
            .value = compiled_template(source),
    };
}

CompiledHeaderTemplates compiled_headers(std::vector<CompiledTemplateEntry> entries) {
    CompiledHeaderTemplates::Builder builder(entries.size());
    for (CompiledTemplateEntry &entry: entries) {
        EXPECT_TRUE(builder.insert(std::move(entry.name), std::move(entry.value)));
    }
    return std::move(builder).build();
}

std::optional<std::string> gunzip(std::string_view input) {
    z_stream stream{};
    if (inflateInit2(&stream, MAX_WBITS + 16) != Z_OK) {
        return std::nullopt;
    }

    stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(input.data()));
    stream.avail_in = static_cast<uInt>(input.size());
    std::array<unsigned char, 4096> buffer{};
    std::string output;
    int result = Z_OK;
    do {
        stream.next_out = buffer.data();
        stream.avail_out = static_cast<uInt>(buffer.size());
        result = inflate(&stream, Z_NO_FLUSH);
        if (result != Z_OK && result != Z_STREAM_END) {
            (void) inflateEnd(&stream);
            return std::nullopt;
        }
        output.append(reinterpret_cast<const char *>(buffer.data()), buffer.size() - stream.avail_out);
    } while (result != Z_STREAM_END);

    if (inflateEnd(&stream) != Z_OK) {
        return std::nullopt;
    }
    return output;
}

std::string_view find_header(const fiber::access_server::PreparedResponse &response, std::string_view name) {
    std::string_view matched;
    for (const auto &header: response.headers) {
        if (fiber::http::http_header_name_equals_ci(header.name, name)) {
            matched = header.value;
        }
    }
    return matched;
}

TEST(AccessResultTest, UsesJavaCompatibleStableExceptions) {
    const Exception router = Exception::router_not_found();
    EXPECT_EQ(router.status, 404);
    EXPECT_EQ(router.name, "ROUTER_NOT_FOUND");
    EXPECT_EQ(router.message, "error find router");

    const Exception bad_request = Exception::bad_request();
    EXPECT_EQ(bad_request.status, 400);
    EXPECT_EQ(bad_request.name, "BAD_REQUEST");
    EXPECT_EQ(bad_request.message, "error find router");

    fiber::mem::BufPool pool;
    const auto path_result = make_url_not_matched_exception(pool, "orders");
    ASSERT_TRUE(path_result);
    const Exception path = *path_result;
    EXPECT_EQ(path.status, 404);
    EXPECT_EQ(path.name, "URL_NOT_MATCHED");
    EXPECT_EQ(path.message, "url not matched is project:orders");

    const Exception entry = Exception::entry_error();
    EXPECT_EQ(entry.status, 403);
    EXPECT_EQ(entry.name, "ENTRY_ERROR");
    EXPECT_EQ(entry.message, "entry error");

    const Exception ip = Exception::source_ip_not_allowed();
    EXPECT_EQ(ip.status, 403);
    EXPECT_EQ(ip.name, "NOT_ALLOW_IP");
    EXPECT_EQ(ip.message, "source ip is not allowed");

    const Exception body = Exception::request_body_too_large();
    EXPECT_EQ(body.status, 413);
    EXPECT_EQ(body.name, "REQ_BODY_TOO_LARGE");
    EXPECT_EQ(body.message, "request body is too large");

    const Exception encoding = Exception::not_acceptable();
    EXPECT_EQ(encoding.status, 406);
    EXPECT_EQ(encoding.name, "NOT_ACCEPTABLE");
    EXPECT_EQ(encoding.message, "no acceptable response content coding");
}

TEST(AccessResultTest, DistinguishesUpstreamExceptionsFromLocalExceptions) {
    const Exception exception = Exception::unknown("upstream failed");
    const Err upstream = Err::from_upstream_exception(exception);

    EXPECT_EQ(upstream.kind, Err::Kind::UpstreamException);
    EXPECT_EQ(upstream.exception.name, exception.name);
    EXPECT_EQ(upstream.exception.message, exception.message);
}

TEST(TemplateEvaluatorTest, EvaluatesSegmentsAndJavaEscapes) {
    EvaluatorState state;
    const CompiledTemplate value = compiled_template(R"(id=${$path.id};method=${$request.method};literal=\$\{\}\\)");
    auto result = evaluate_template(value, evaluator(state));

    ASSERT_TRUE(result);
    EXPECT_EQ(*result, "id=42;method=POST;literal=${}\\");
    EXPECT_EQ(state.expressions, (std::vector<std::string>{"$path.id", "$request.method"}));
}

TEST(TemplateEvaluatorTest, FailsClosedWithoutAdapter) {
    const CompiledTemplate value = compiled_template("${$path.id}");
    auto result = evaluate_template(value, {});

    ASSERT_FALSE(result);
    ASSERT_EQ(result.error().kind, Err::Kind::Exception);
    EXPECT_EQ(result.error().exception.status, 500U);
    EXPECT_EQ(result.error().exception.name, "TEMPLATE_SCRIPT");
    EXPECT_EQ(result.error().exception.message,
              "error exec for template expression: template evaluator is not configured");
}

TEST(ResponsePlanTest, EvaluatesEveryHeaderBeforeApplyingJavaHopHeaderFilter) {
    CompiledResponseRoute response{
            .status = 201,
            .body_kind = ResponseBodyKind::Text,
            .body = "created",
            .response_headers =
                    {
                            template_entry("X-Request", "${$request.method}"),
                            template_entry("Content-Length", "${$path.id}"),
                            template_entry("Host", "public.example.com"),
                    },
    };
    EvaluatorState state;

    auto result = prepare_response(response, evaluator(state));

    ASSERT_TRUE(result);
    EXPECT_EQ(result->status, 201);
    EXPECT_EQ(result->body_view(), "created");
    ASSERT_EQ(result->headers.size(), 2U);
    EXPECT_EQ(result->headers[0].name, "X-Request");
    EXPECT_EQ(result->headers[0].value, "POST");
    EXPECT_EQ(result->headers[1].name, "Host");
    EXPECT_EQ(result->headers[1].value, "public.example.com");
    EXPECT_EQ(state.expressions, (std::vector<std::string>{"$request.method", "$path.id"}));
}

TEST(ResponsePlanTest, PreservesEmptyAndPrecompiledStaticBodyBytes) {
    for (const auto &[kind, body]: {
                 std::pair{ResponseBodyKind::Empty, std::string("")},
                 std::pair{ResponseBodyKind::Text, std::string("plain")},
                 std::pair{ResponseBodyKind::Base64, std::string("binary\0body", 11)},
         }) {
        const CompiledResponseRoute response{
                .status = 206,
                .body_kind = kind,
                .body = body,
        };

        auto result = prepare_response(response, {});

        ASSERT_TRUE(result);
        EXPECT_EQ(result->status, 206);
        EXPECT_EQ(result->body_view(), body);
    }
}

TEST(ResponsePlanTest, EvaluatesTemplateBodyAfterHeaders) {
    const CompiledResponseRoute response{
            .status = 200,
            .body_kind = ResponseBodyKind::Template,
            .body_template = compiled_template("item-${$path.id}"),
            .response_headers =
                    {
                            template_entry("X-Method", "${$request.method}"),
                    },
    };
    EvaluatorState state;

    auto result = prepare_response(response, evaluator(state));

    ASSERT_TRUE(result);
    EXPECT_EQ(result->body_view(), "item-42");
    EXPECT_EQ(state.expressions, (std::vector<std::string>{"$request.method", "$path.id"}));
}

TEST(GzipEncoderTest, ProducesDeterministicRoundTrippableRfc1952Bytes) {
    constexpr char kBody[] = "plain\0binary payload repeated repeated repeated";
    const std::string body(kBody, sizeof(kBody) - 1);

    auto first = gzip_encode(body, 6);
    auto second = gzip_encode(body, 6);

    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_EQ(*first, *second);
    ASSERT_GE(first->size(), 10U);
    EXPECT_EQ(static_cast<unsigned char>((*first)[0]), 0x1fU);
    EXPECT_EQ(static_cast<unsigned char>((*first)[1]), 0x8bU);
    EXPECT_EQ(static_cast<unsigned char>((*first)[9]), 0xffU);
    EXPECT_EQ(gunzip(*first), body);
    EXPECT_FALSE(gzip_encode(body, 0));
    EXPECT_FALSE(gzip_encode(body, 10));
}

TEST(ResponsePlanTest, NegotiatesGzipUsingRfcQualityAndWildcardRules) {
    struct Case {
        std::string_view value;
        ResponseContentCoding expected;
    };
    const std::array cases{
            Case{"gzip", ResponseContentCoding::Gzip},
            Case{"GZIP, br", ResponseContentCoding::Gzip},
            Case{"gzip;q=0", ResponseContentCoding::Identity},
            Case{"gzip;q=0.5", ResponseContentCoding::Identity},
            Case{"gzip;q=0.5, identity;q=0.1", ResponseContentCoding::Gzip},
            Case{"*", ResponseContentCoding::Gzip},
            Case{"gzip;q=0, *;q=1", ResponseContentCoding::Identity},
            Case{"*;q=0", ResponseContentCoding::NotAcceptable},
            Case{"gzip;q=0, identity;q=0", ResponseContentCoding::NotAcceptable},
            Case{"gzip;q=bogus", ResponseContentCoding::Identity},
    };

    for (const Case &test_case: cases) {
        SCOPED_TRACE(test_case.value);
        fiber::mem::BufPool pool;
        fiber::http::HttpHeaders headers(pool);
        ASSERT_NE(headers.add("Accept-Encoding", test_case.value), nullptr);
        EXPECT_EQ(select_response_content_coding(headers), test_case.expected);
    }

    fiber::mem::BufPool pool;
    fiber::http::HttpHeaders absent(pool);
    EXPECT_EQ(select_response_content_coding(absent), ResponseContentCoding::Identity);
}

TEST(ResponsePlanTest, AppliesSelectedGzipVariantAndRepresentationHeaders) {
    const std::string body = "compressible response response response response";
    auto encoded = gzip_encode(body, 6);
    ASSERT_TRUE(encoded);
    const CompiledResponseRoute response{
            .status = 200,
            .body_kind = ResponseBodyKind::Text,
            .body = body,
            .gzip_level = 6,
            .gzip_body = *encoded,
            .response_headers =
                    {
                            template_entry("Vary", "Origin"),
                            template_entry("vary", "Accept-Language"),
                            template_entry("ETag", R"("version-1")"),
                    },
    };
    auto prepared = prepare_response(response, {});
    ASSERT_TRUE(prepared);
    fiber::mem::BufPool pool;
    fiber::http::HttpHeaders headers(pool);
    ASSERT_NE(headers.add("Accept-Encoding", "gzip, identity;q=0.5"), nullptr);

    auto selected = apply_response_gzip(response, headers, *prepared);

    ASSERT_TRUE(selected);
    EXPECT_EQ(*selected, ResponseContentCoding::Gzip);
    EXPECT_EQ(find_header(*prepared, "Content-Encoding"), "gzip");
    EXPECT_EQ(find_header(*prepared, "Vary"), "Accept-Language, Accept-Encoding");
    EXPECT_EQ(find_header(*prepared, "ETag"), R"(W/"version-1")");
    EXPECT_EQ(gunzip(prepared->body_view()), body);
}

TEST(ResponsePlanTest, KeepsIdentityVariantAndRejectsWhenNoCodingIsAcceptable) {
    const std::string body = "identity response";
    auto encoded = gzip_encode(body, 1);
    ASSERT_TRUE(encoded);
    const CompiledResponseRoute response{
            .status = 200,
            .body_kind = ResponseBodyKind::Text,
            .body = body,
            .gzip_level = 1,
            .gzip_body = *encoded,
    };
    auto prepared = prepare_response(response, {});
    ASSERT_TRUE(prepared);
    fiber::mem::BufPool identity_pool;
    fiber::http::HttpHeaders identity_headers(identity_pool);

    auto identity = apply_response_gzip(response, identity_headers, *prepared);

    ASSERT_TRUE(identity);
    EXPECT_EQ(*identity, ResponseContentCoding::Identity);
    EXPECT_EQ(prepared->body_view(), body);
    EXPECT_EQ(find_header(*prepared, "Vary"), "Accept-Encoding");
    EXPECT_TRUE(find_header(*prepared, "Content-Encoding").empty());

    fiber::mem::BufPool rejected_pool;
    fiber::http::HttpHeaders rejected_headers(rejected_pool);
    ASSERT_NE(rejected_headers.add("Accept-Encoding", "gzip;q=0, identity;q=0"), nullptr);
    auto rejected_prepared = prepare_response(response, {});
    ASSERT_TRUE(rejected_prepared);

    auto rejected = apply_response_gzip(response, rejected_headers, *rejected_prepared);

    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().exception.status, 406U);
    EXPECT_EQ(rejected.error().exception.name, "NOT_ACCEPTABLE");
}

TEST(ResponsePlanTest, DiscardsAllConfiguredHeadersWhenAHeaderTemplateFails) {
    CompiledResponseRoute response{
            .status = 200,
            .body_kind = ResponseBodyKind::Text,
            .body = "unreached",
            .response_headers =
                    {
                            template_entry("X-First", "${$path.id}"),
                            template_entry("X-Fail", "${broken}"),
                    },
    };
    EvaluatorState state{.failing_expression = "broken"};

    auto result = prepare_response(response, evaluator(state));

    ASSERT_FALSE(result);
    ASSERT_EQ(result.error().kind, Err::Kind::Exception);
    EXPECT_EQ(result.error().exception.name, "TEMPLATE_SCRIPT");
}

TEST(ResponsePlanTest, DiscardsAllConfiguredHeadersWhenBodyTemplateFails) {
    CompiledResponseRoute response{
            .status = 200,
            .body_kind = ResponseBodyKind::Template,
            .body_template = compiled_template("${broken}"),
            .response_headers =
                    {
                            template_entry("X-First", "${$path.id}"),
                            template_entry("Content-Type", "application/custom"),
                    },
    };
    EvaluatorState state{.failing_expression = "broken"};

    auto result = prepare_response(response, evaluator(state));

    ASSERT_FALSE(result);
    ASSERT_EQ(result.error().kind, Err::Kind::Exception);
    EXPECT_EQ(result.error().exception.name, "TEMPLATE_SCRIPT");
}

TEST(ResponsePlanTest, DiscardsAllHeadersWhenOneHeaderIsInvalid) {
    CompiledResponseRoute response{
            .status = 200,
            .body_kind = ResponseBodyKind::Text,
            .body = "unreached",
            .response_headers =
                    {
                            template_entry("X-First", "one"),
                            template_entry("Bad Header", "two"),
                            template_entry("X-Last", "three"),
                    },
    };

    auto result = prepare_response(response, {});

    ASSERT_FALSE(result);
    ASSERT_EQ(result.error().kind, Err::Kind::Exception);
    EXPECT_EQ(result.error().exception.status, 500U);
    EXPECT_EQ(result.error().exception.name, "ACCESS_UNKNOWN_ERROR");
}

TEST(ResponsePlanTest, FiltersTheSameProtectedResponseHeadersAsJava) {
    for (const std::string_view header: {
                 "Connection",
                 "content-length",
                 "Proxy-Connection",
                 "Keep-Alive",
                 "Proxy-Authenticate",
                 "Proxy-Authorization",
                 "TE",
                 "Trailer",
                 "Transfer-Encoding",
                 "Upgrade",
         }) {
        EXPECT_TRUE(is_java_filtered_response_header(header)) << header;
    }
    EXPECT_FALSE(is_java_filtered_response_header("Host"));
    EXPECT_FALSE(is_java_filtered_response_header("Content-Type"));
}

TEST(ProxyResponsePlanTest, EvaluatesAllHeadersBeforeFilteringEmptyAndProtectedValues) {
    const CompiledHeaderTemplates headers = compiled_headers({
            template_entry("X-First", "${$path.id}"),
            template_entry("Content-Length", "${$request.method}"),
            template_entry("X-Empty", "${empty}"),
            template_entry("X-Last", "static"),
    });
    EvaluatorState state;

    auto result = prepare_proxy_response_headers(headers, evaluator(state));

    ASSERT_TRUE(result);
    ASSERT_EQ(result->size(), 2U);
    EXPECT_EQ((*result)[0].name, "X-First");
    EXPECT_EQ((*result)[0].value, "42");
    EXPECT_EQ((*result)[1].name, "X-Last");
    EXPECT_EQ((*result)[1].value, "static");
    EXPECT_EQ(state.expressions, (std::vector<std::string>{"$path.id", "$request.method", "empty"}));
    EXPECT_TRUE(headers.contains("content-length"));
    EXPECT_TRUE(headers.contains("x-empty"));
    EXPECT_FALSE(headers.contains("Location"));
}

TEST(ProxyResponsePlanTest, RewritesLocationWithJavaAuthorityPrefixSemantics) {
    auto rewritten = rewrite_java_proxy_location("http://backend/next?q=1", "backend:8080", "https", "api.example.com");

    ASSERT_TRUE(rewritten);
    EXPECT_EQ(*rewritten, "https://api.example.com/next?q=1");
    EXPECT_FALSE(rewrite_java_proxy_location("http://other/next", "backend:8080", "https", "api.example.com"));

    auto default_scheme = rewrite_java_proxy_location("custom://backend", "backend:8080", "", "api.example.com");
    ASSERT_TRUE(default_scheme);
    EXPECT_EQ(*default_scheme, "http://api.example.com");
}

TEST(ProxyResponsePlanTest, RewritesOnlyJavaStyleRefreshUrlsAfterAPrefix) {
    auto rewritten =
            rewrite_java_proxy_refresh("5;url=http://backend/next", "backend:8080", "https", "api.example.com");

    ASSERT_TRUE(rewritten);
    EXPECT_EQ(*rewritten, "5;url=https://api.example.com/next");
    EXPECT_FALSE(rewrite_java_proxy_refresh("url=http://backend/next", "backend:8080", "https", "api.example.com"));
    EXPECT_FALSE(rewrite_java_proxy_refresh("5;URL=http://backend/next", "backend:8080", "https", "api.example.com"));
}

TEST(ErrorResponderTest, NegotiatesHtmlOnlyFromTheAcceptPrefix) {
    EXPECT_TRUE(ErrorResponder::wants_html("text/html"));
    EXPECT_TRUE(ErrorResponder::wants_html("TEXT/HTML;q=0.8"));

    EXPECT_FALSE(ErrorResponder::wants_html(""));
    EXPECT_FALSE(ErrorResponder::wants_html("application/json, text/html"));
    EXPECT_FALSE(ErrorResponder::wants_html(" text/html"));
    EXPECT_FALSE(ErrorResponder::wants_html("application/xhtml+xml"));
}

TEST(ErrorResponderTest, RendersExactJavaJsonErrorShape) {
    const auto rendered = ErrorResponder::render(Exception::entry_error(), "", "trace-1");

    EXPECT_EQ(rendered.status, 403);
    EXPECT_EQ(rendered.content_type, "application/json; charset=utf-8");
    EXPECT_EQ(rendered.body, R"({"name":"ENTRY_ERROR","message":"entry error","meta":null})");
}

TEST(ErrorResponderTest, RendersExactJavaHtmlErrorPage) {
    const auto rendered =
            ErrorResponder::render(Exception::source_ip_not_allowed(), "text/html,application/json", "trace-1");

    EXPECT_EQ(rendered.status, 403);
    EXPECT_EQ(rendered.content_type, "text/html");
    EXPECT_EQ(rendered.body, "<!DOCTYPE html>\n"
                             "<html lang=\"en\">\n"
                             "<head>\n"
                             "    <meta charset=\"UTF-8\">\n"
                             "    <title>Ploto-Access-Server</title>\n"
                             "</head>\n"
                             "<body>\n"
                             "<div style=\"text-align: center\">\n"
                             "    <h3>NOT_ALLOW_IP:&nbsp; <span style=\"color: #ff5544\">403</span></h3>\n"
                             "    <p>source ip is not allowed</p>\n"
                             "    <h5>traceID: trace-1</h5>\n"
                             "    <pre></pre>\n"
                             "</div>\n"
                             "</body>\n"
                             "</html>");
}

} // namespace
