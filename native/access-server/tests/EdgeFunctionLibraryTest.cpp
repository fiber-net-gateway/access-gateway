#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "config/AccessConfigLimits.h"
#include "routing/EdgeFunctionLibrary.h"

#include <fiber/http/HttpExchange.h>
#include <fiber/http_script/ScriptExchangeCtx.h>
#include <fiber/net/IpAddress.h>
#include <fiber/net/SocketAddress.h>
#include <fiber/script/JsValue.h>
#include <fiber/script/ScriptResult.h>
#include <fiber/script/std/NodeText.h>

namespace {

using fiber::access_server::AccessConfigLimits;
using fiber::access_server::EdgeFunctionLibrary;
using fiber::access_server::EdgeFunctionProgram;

class TestEdgeContext final : public fiber::access_server::EdgeFunctionRequestContext {
public:
    void report(std::string_view event, std::string_view label) noexcept override {
        std::string entry(event);
        entry.push_back('#');
        entry.append(label);
        events.push_back(std::move(entry));
    }

    std::vector<std::string> events;
};

struct ExecOutcome {
    bool compiled = false;
    std::string compile_error;
    bool executed = false;
    bool is_value = false;
    bool is_exception = false;
    bool is_abort = false;
    bool is_boolean = false;
    bool boolean = false;
    bool is_undefined = false;
    std::string text;
};

std::string first_compile_error(std::string_view source, const AccessConfigLimits &limits = {}) {
    EdgeFunctionLibrary library(limits);
    auto program = library.compile(source);
    EXPECT_FALSE(program.has_value());
    return program ? std::string() : program.error().message;
}

class EdgeFunctionLibraryTest : public ::testing::Test {
protected:
    void SetUp() override {
        fiber::net::IpAddress remote;
        ASSERT_TRUE(fiber::net::IpAddress::parse("10.1.2.3", remote));
        exchange = std::make_unique<fiber::http::HttpExchange>(fiber::net::SocketAddress(remote, 44344));
        path = "/loan/list";
        query = "a=b&c=d";
        raw_uri = "/loan/list?a=b&c=d";
        exchange->uri().path = path;
        exchange->uri().query = query;
        exchange->uri().unparsed_uri = raw_uri;
    }

    ExecOutcome run(std::string_view source, const AccessConfigLimits &limits = {}, bool with_edge_context = true) {
        ExecOutcome outcome;
        EdgeFunctionLibrary library(limits);
        auto program = library.compile(source);
        if (!program) {
            outcome.compile_error = program.error().message;
            return outcome;
        }
        outcome.compiled = true;

        // Keep the Script alive while the result borrows its constants; the
        // directives ride in the same program.
        retained = std::make_unique<EdgeFunctionProgram>(std::move(*program));
        fiber::script::GcHeap heap;
        fiber::http_script::ScriptExchangeCtx script_context(*exchange, heap);
        TestEdgeContext edge;
        edge.project = "loan-web";
        edge.host = "loan.example.com";
        if (with_edge_context) {
            script_context.set_services(&edge);
        }
        fiber::script::JsValue root = fiber::script::JsValue::make_undefined();
        fiber::script::ScriptResult result = retained->program.exec_sync(root, &script_context, heap);

        outcome.executed = true;
        outcome.is_value = result.is_value();
        outcome.is_exception = result.is_exception();
        outcome.is_abort = result.is_abort();
        reports = edge.events;
        if (!result.is_value()) {
            return outcome;
        }
        const fiber::script::JsValue &value = result.value();
        const fiber::script::JsNodeType kind = fiber::script::js_value_type(value);
        outcome.is_boolean = kind == fiber::script::JsNodeType::Boolean;
        outcome.boolean = outcome.is_boolean && fiber::script::js_value_bool(value);
        outcome.is_undefined = kind == fiber::script::JsNodeType::Undefined;
        if (kind == fiber::script::JsNodeType::String) {
            std::string_view sv;
            if (!fiber::script::std_lib::string_utf8_view(value, sv)) {
                fiber::script::std_lib::node_as_text(value, outcome.text);
            } else {
                outcome.text.assign(sv);
            }
        }
        return outcome;
    }

    std::unique_ptr<fiber::http::HttpExchange> exchange;
    std::unique_ptr<EdgeFunctionProgram> retained;
    std::vector<std::string> reports;
    std::string path;
    std::string query;
    std::string raw_uri;
};

// ---- compile-time surface restrictions ----

TEST(EdgeFunctionLibrary, InvalidCidrLiteralFailsCompile) {
    EXPECT_FALSE(first_compile_error(R"(directive block = cidr "10.0.0.0/33"; true)").empty());
    EXPECT_FALSE(first_compile_error(R"(directive block = cidr "10.0.0/8"; true)").empty());
    EXPECT_FALSE(first_compile_error(R"(directive block = cidr "not-an-ip"; true)").empty());
}

TEST(EdgeFunctionLibrary, EmptyCidrLiteralFailsCompile) {
    // No networks at all: fail closed at compile time.
    EXPECT_FALSE(first_compile_error(R"(directive block = cidr ""; true)").empty());
    EXPECT_FALSE(first_compile_error(R"(directive block = cidr ";;"; true)").empty());
}

TEST(EdgeFunctionLibrary, NonTextCidrLiteralFailsCompile) {
    EXPECT_FALSE(first_compile_error("directive block = cidr 123; true").empty());
}

TEST(EdgeFunctionLibrary, BodyReadersAreCompileErrors) {
    EXPECT_FALSE(first_compile_error("req.readJson()").empty());
    EXPECT_FALSE(first_compile_error("req.readBinary()").empty());
    EXPECT_FALSE(first_compile_error("req.discardBody()").empty());
}

TEST(EdgeFunctionLibrary, RouteConstantNamespacesCompileExceptContextAndPath) {
    // $header/$query/$cookie resolve dynamically per request (RouteScriptExtension
    // with per-request memoization); $context is project-only and $path has no
    // route selected, so both stay compile errors.
    EdgeFunctionLibrary library;
    EXPECT_TRUE(library.compile("$header.foo == 'x'").has_value());
    EXPECT_TRUE(library.compile("$query.foo == 'x'").has_value());
    EXPECT_TRUE(library.compile("$cookie.foo == 'x'").has_value());
    EXPECT_FALSE(first_compile_error("$context.foo").empty());
    EXPECT_FALSE(first_compile_error("$path.foo").empty());
}

TEST_F(EdgeFunctionLibraryTest, HyphenatedKeyParsesAsArithmetic) {
    // Constant keys after "$ns." accept the identifier charset only: a hyphen is
    // a subtraction operator and the bare words are undefined globals, so the
    // expression compiles but raises TypeError at runtime. The normalized form
    // ($header.x_edge_token) is the supported spelling and matches hyphenated
    // header names through the const-name normalization.
    exchange->request_headers().add("x-edge-token", "s3cret");
    const ExecOutcome hyphen = run(R"(return $header.x-edge-token;)");
    ASSERT_TRUE(hyphen.compiled) << hyphen.compile_error;
    EXPECT_TRUE(hyphen.is_exception);

    const ExecOutcome normalized = run(R"(return $header.x_edge_token;)");
    ASSERT_TRUE(normalized.compiled) << normalized.compile_error;
    ASSERT_TRUE(normalized.is_value);
    EXPECT_EQ(normalized.text, "s3cret");
}

TEST(EdgeFunctionLibrary, UnknownFunctionIsCompileError) { EXPECT_FALSE(first_compile_error("nope()").empty()); }

TEST(EdgeFunctionLibrary, HttpDirectiveIsCompileError) {
    EXPECT_FALSE(first_compile_error(R"(directive up = http "http://127.0.0.1:1"; true)").empty());
}

TEST(EdgeFunctionLibrary, AssignmentIsCompileError) {
    // allow_assign=false: edge functions cannot mutate bindings.
    EXPECT_FALSE(first_compile_error("var x = 1; x = 2; x").empty());
    EXPECT_FALSE(first_compile_error("x = 1; x").empty());
}

TEST(EdgeFunctionLibrary, ContainsArityIsEnforced) {
    EXPECT_FALSE(first_compile_error(R"(directive c = cidr "10.0.0.0/8"; c.contains())").empty());
    EXPECT_FALSE(first_compile_error(R"(directive c = cidr "10.0.0.0/8"; c.contains("1.2.3.4", "5.6.7.8"))").empty());
}

TEST(EdgeFunctionLibrary, UnknownDirectiveMethodIsCompileError) {
    EXPECT_FALSE(first_compile_error(R"(directive c = cidr "10.0.0.0/8"; c.foo("1.2.3.4"))").empty());
}

TEST(EdgeFunctionLibrary, DirectiveBudgetEnforced) {
    AccessConfigLimits limits{};
    limits.edge_functions.max_cidr_directives_per_script = 1;
    EdgeFunctionLibrary library(limits);
    EXPECT_TRUE(library.compile(R"(directive a = cidr "10.0.0.0/8"; true)").has_value());
    auto two = library.compile(R"(directive a = cidr "10.0.0.0/8"; directive b = cidr "10.1.0.0/16"; true)");
    ASSERT_FALSE(two.has_value());
    EXPECT_FALSE(two.error().message.empty());
}

TEST(EdgeFunctionLibrary, CidrPerDirectiveBudgetEnforced) {
    AccessConfigLimits limits{};
    limits.edge_functions.max_cidrs_per_directive = 2;
    const std::string source = R"(directive c = cidr "10.0.0.0/8;10.1.0.0/16;10.2.0.0/16"; true)";
    EdgeFunctionLibrary library(limits);
    auto program = library.compile(source);
    ASSERT_FALSE(program.has_value());
}

TEST(EdgeFunctionLibrary, ScriptPayloadBudgetEnforced) {
    AccessConfigLimits limits{};
    limits.edge_functions.max_script_payload_bytes = 8;
    EdgeFunctionLibrary library(limits);
    auto program = library.compile("strings.match('loan', 'loan-*')");
    ASSERT_FALSE(program.has_value());
    EXPECT_NE(program.error().message.find("byte limit"), std::string::npos);
}

// ---- runtime execution ----

TEST_F(EdgeFunctionLibraryTest, CidrContainsMatchesInsideRange) {
    const ExecOutcome outcome = run(R"(directive block = cidr "10.0.0.0/8;192.168.1.5";
return block.contains("10.1.2.3");)");
    ASSERT_TRUE(outcome.compiled) << outcome.compile_error;
    ASSERT_TRUE(outcome.is_value);
    ASSERT_TRUE(outcome.is_boolean);
    EXPECT_TRUE(outcome.boolean);
}

TEST_F(EdgeFunctionLibraryTest, CidrContainsRejectsOutsideRange) {
    const ExecOutcome outcome = run(R"(directive block = cidr "10.0.0.0/8;192.168.1.5";
return block.contains("8.8.8.8");)");
    ASSERT_TRUE(outcome.compiled) << outcome.compile_error;
    ASSERT_TRUE(outcome.is_boolean);
    EXPECT_FALSE(outcome.boolean);
}

TEST_F(EdgeFunctionLibraryTest, CidrSingleHostMatchesExactly) {
    const ExecOutcome hit = run(R"(directive host = cidr "192.168.1.5"; return host.contains("192.168.1.5");)");
    ASSERT_TRUE(hit.is_boolean);
    EXPECT_TRUE(hit.boolean);

    const ExecOutcome miss = run(R"(directive host = cidr "192.168.1.5"; return host.contains("192.168.1.6");)");
    ASSERT_TRUE(miss.is_boolean);
    EXPECT_FALSE(miss.boolean);
}

TEST_F(EdgeFunctionLibraryTest, CidrMalformedArgumentsNeverMatch) {
    const ExecOutcome outcome = run(R"(directive block = cidr "10.0.0.0/8";
return block.contains("not-an-ip") || block.contains("") || block.contains(null);)");
    ASSERT_TRUE(outcome.compiled) << outcome.compile_error;
    ASSERT_TRUE(outcome.is_boolean);
    EXPECT_FALSE(outcome.boolean);
}

TEST_F(EdgeFunctionLibraryTest, CidrV6Directive) {
    const ExecOutcome inside = run(R"(directive v6 = cidr "2001:db8::/32"; return v6.contains("2001:db8::1");)");
    ASSERT_TRUE(inside.is_boolean);
    EXPECT_TRUE(inside.boolean);

    const ExecOutcome outside = run(R"(directive v6 = cidr "2001:db8::/32"; return v6.contains("2001:db9::1");)");
    ASSERT_TRUE(outside.is_boolean);
    EXPECT_FALSE(outside.boolean);
}

TEST_F(EdgeFunctionLibraryTest, CidrFamilyMismatchNeverMatches) {
    const ExecOutcome outcome = run(R"(directive v4 = cidr "10.0.0.0/8"; return v4.contains("::ffff:10.1.2.3");)");
    ASSERT_TRUE(outcome.is_boolean);
    EXPECT_FALSE(outcome.boolean);
}

TEST_F(EdgeFunctionLibraryTest, CidrEmptySegmentsAreIgnored) {
    const ExecOutcome outcome = run(R"(directive c = cidr "10.0.0.0/8;;10.9.0.0/16"; return c.contains("10.9.1.1");)");
    ASSERT_TRUE(outcome.is_boolean);
    EXPECT_TRUE(outcome.boolean);
}

TEST_F(EdgeFunctionLibraryTest, CidrMultipleLiteralsAccumulate) {
    const ExecOutcome outcome =
            run(R"(directive c = cidr "10.0.0.0/8" "172.16.0.0/12"; return c.contains("172.20.1.1");)");
    ASSERT_TRUE(outcome.is_boolean);
    EXPECT_TRUE(outcome.boolean);
}

TEST_F(EdgeFunctionLibraryTest, CidrHostBitsAreMaskedLikeGrayRules) {
    // parse_strict masks host bits ("10.0.0.1/8" == 10.0.0.0/8), matching the
    // gray-rule and route-allows CIDR semantics.
    const ExecOutcome outcome = run(R"(directive c = cidr "10.0.0.1/8"; return c.contains("10.200.0.1");)");
    ASSERT_TRUE(outcome.is_boolean);
    EXPECT_TRUE(outcome.boolean);
}

TEST_F(EdgeFunctionLibraryTest, HeaderReaderReturnsValue) {
    exchange->request_headers().add("x-waf-token", "secret123");
    const ExecOutcome outcome = run(R"(return req.getHeader("x-waf-token");)");
    ASSERT_TRUE(outcome.compiled) << outcome.compile_error;
    ASSERT_TRUE(outcome.is_value);
    EXPECT_EQ(outcome.text, "secret123");
}

TEST_F(EdgeFunctionLibraryTest, MissingHeaderIsUndefined) {
    const ExecOutcome outcome = run(R"(return req.getHeader("no-such-header");)");
    ASSERT_TRUE(outcome.compiled) << outcome.compile_error;
    ASSERT_TRUE(outcome.is_value);
    EXPECT_TRUE(outcome.is_undefined);
}

TEST_F(EdgeFunctionLibraryTest, PathAndQueryReaders) {
    const ExecOutcome path_outcome = run(R"(return req.getPath();)");
    ASSERT_TRUE(path_outcome.compiled) << path_outcome.compile_error;
    EXPECT_EQ(path_outcome.text, "/loan/list");

    const ExecOutcome query_outcome = run(R"(return req.getQuery("a");)");
    ASSERT_TRUE(query_outcome.compiled) << query_outcome.compile_error;
    EXPECT_EQ(query_outcome.text, "b");

    const ExecOutcome uri_outcome = run(R"(return req.getUri();)");
    ASSERT_TRUE(uri_outcome.compiled) << uri_outcome.compile_error;
    EXPECT_EQ(uri_outcome.text, "/loan/list?a=b&c=d");

    const ExecOutcome query_str_outcome = run(R"(return req.getQueryStr();)");
    ASSERT_TRUE(query_str_outcome.compiled) << query_str_outcome.compile_error;
    EXPECT_EQ(query_str_outcome.text, "a=b&c=d");

    const ExecOutcome req_path_outcome = run(R"(return $req.path;)");
    ASSERT_TRUE(req_path_outcome.compiled) << req_path_outcome.compile_error;
    EXPECT_EQ(req_path_outcome.text, "/loan/list");
}

TEST_F(EdgeFunctionLibraryTest, CookieReaderParsesCookieHeader) {
    exchange->request_headers().add("cookie", "sess=abc; other=1");
    const ExecOutcome outcome = run(R"(return req.getCookie("sess");)");
    ASSERT_TRUE(outcome.compiled) << outcome.compile_error;
    EXPECT_EQ(outcome.text, "abc");
}

TEST_F(EdgeFunctionLibraryTest, InputFieldsAreTruncated) {
    AccessConfigLimits limits{};
    limits.edge_functions.max_input_field_bytes = 4;
    exchange->request_headers().add("x-waf-token", "secret123");
    const ExecOutcome outcome = run(R"(return req.getHeader("x-waf-token");)", limits);
    ASSERT_TRUE(outcome.compiled) << outcome.compile_error;
    EXPECT_EQ(outcome.text, "secr");
}

TEST_F(EdgeFunctionLibraryTest, ConnectionConstantsAreAvailable) {
    const ExecOutcome outcome = run(R"(return $conn.remote_addr;)");
    ASSERT_TRUE(outcome.compiled) << outcome.compile_error;
    ASSERT_TRUE(outcome.is_value);
    EXPECT_EQ(outcome.text, "10.1.2.3");
}

TEST_F(EdgeFunctionLibraryTest, CtxConstantsComeFromEdgeContext) {
    const ExecOutcome project = run(R"(return $ctx.project;)");
    ASSERT_TRUE(project.compiled) << project.compile_error;
    EXPECT_EQ(project.text, "loan-web");

    const ExecOutcome host = run(R"(return $ctx.host;)");
    ASSERT_TRUE(host.compiled) << host.compile_error;
    EXPECT_EQ(host.text, "loan.example.com");
}

TEST_F(EdgeFunctionLibraryTest, CtxConstantWithoutEdgeContextAborts) {
    const ExecOutcome outcome = run(R"(return $ctx.project;)", {}, /*with_edge_context=*/false);
    ASSERT_TRUE(outcome.compiled) << outcome.compile_error;
    EXPECT_FALSE(outcome.is_value);
    EXPECT_TRUE(outcome.is_abort);
}

TEST_F(EdgeFunctionLibraryTest, CtxReportRecordsEvents) {
    const ExecOutcome outcome = run(R"(ctx.report("waf", "sqli");
ctx.report("waf", "xss");
return true;)");
    ASSERT_TRUE(outcome.compiled) << outcome.compile_error;
    ASSERT_TRUE(outcome.is_boolean);
    EXPECT_TRUE(outcome.boolean);
    ASSERT_EQ(reports.size(), 2U);
    EXPECT_EQ(reports[0], "waf#sqli");
    EXPECT_EQ(reports[1], "waf#xss");
}

TEST_F(EdgeFunctionLibraryTest, CtxReportWithoutEdgeContextAborts) {
    const ExecOutcome outcome = run(R"(ctx.report("waf", "sqli"); return true;)", {},
                                    /*with_edge_context=*/false);
    ASSERT_TRUE(outcome.compiled) << outcome.compile_error;
    EXPECT_FALSE(outcome.is_value);
    EXPECT_TRUE(outcome.is_abort);
}

TEST_F(EdgeFunctionLibraryTest, CtxReportRejectsNonStringArguments) {
    const ExecOutcome outcome = run(R"(ctx.report(1, "x"); return true;)");
    ASSERT_TRUE(outcome.compiled) << outcome.compile_error;
    EXPECT_TRUE(outcome.is_exception);
}

TEST_F(EdgeFunctionLibraryTest, StdLibraryBaseIsPresent) {
    const ExecOutcome outcome = run(R"(return strings.match("loan-web-1", "loan-*");)");
    ASSERT_TRUE(outcome.compiled) << outcome.compile_error;
    ASSERT_TRUE(outcome.is_boolean);
    EXPECT_TRUE(outcome.boolean);
}

TEST_F(EdgeFunctionLibraryTest, StdLibraryAndCidrCombine) {
    const ExecOutcome outcome = run(R"(directive office = cidr "10.0.0.0/8";
return strings.match($req.path, "/loan/*") && office.contains($conn.remote_addr);)");
    ASSERT_TRUE(outcome.compiled) << outcome.compile_error;
    ASSERT_TRUE(outcome.is_boolean);
    EXPECT_TRUE(outcome.boolean);
}

TEST_F(EdgeFunctionLibraryTest, RespSendCompilesAndContainsAsync) {
    EdgeFunctionLibrary library;
    auto program = library.compile(R"(resp.send(403, "blocked");)");
    ASSERT_TRUE(program.has_value());
    EXPECT_TRUE(program->program.contains_async());
}

TEST_F(EdgeFunctionLibraryTest, SyncScriptHasNoAsyncOpcodes) {
    EdgeFunctionLibrary library;
    auto program = library.compile(R"(directive c = cidr "10.0.0.0/8"; return c.contains("10.1.2.3");)");
    ASSERT_TRUE(program.has_value());
    EXPECT_FALSE(program->program.contains_async());
}

TEST(EdgeFunctionLibrary, ProgramsOwnTheirDirectivesAcrossCompiles) {
    // Two programs compiled from one library must keep independent directive
    // definitions (the pending pool is moved out per compile, never shared).
    EdgeFunctionLibrary library;
    auto first = library.compile(R"(directive c = cidr "10.0.0.0/8"; return c.contains("10.1.2.3");)");
    auto second = library.compile(R"(directive c = cidr "192.168.0.0/16"; return c.contains("10.1.2.3");)");
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());

    fiber::script::GcHeap heap;
    fiber::net::IpAddress remote;
    ASSERT_TRUE(fiber::net::IpAddress::parse("10.1.2.3", remote));
    fiber::http::HttpExchange exchange(fiber::net::SocketAddress(remote, 44344));
    fiber::http_script::ScriptExchangeCtx script_context(exchange, heap);
    fiber::script::JsValue root = fiber::script::JsValue::make_undefined();

    auto first_result = first->program.exec_sync(root, &script_context, heap);
    ASSERT_TRUE(first_result.is_value());
    EXPECT_TRUE(fiber::script::js_value_bool(first_result.value()));

    auto second_result = second->program.exec_sync(root, &script_context, heap);
    ASSERT_TRUE(second_result.is_value());
    EXPECT_FALSE(fiber::script::js_value_bool(second_result.value()));
}

} // namespace
