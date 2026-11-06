#include "validation/NativeValidatorProtocol.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include <fiber/common/util/Base64.h>

namespace {

std::string request(std::string_view kind, std::string_view project, std::string_view payload) {
    const std::string encoded =
            fiber::util::base64_encode(reinterpret_cast<const std::uint8_t *>(payload.data()), payload.size());
    return R"({"contractVersion":1,"requestId":"test-request","kind":")" + std::string(kind) + R"(","project":")" +
           std::string(project) + R"(","payloadBase64":")" + encoded + R"("})";
}

TEST(NativeValidatorProtocolTest, CompilesProjectPayloadWithConfigAndValidationComponents) {
    constexpr std::string_view payload = R"({
        "version": 3,
        "host": {"example.com": {"https": "S_NOT_MUST"}},
        "routes": [{
            "path": "/health",
            "type": "RESPONSE",
            "status": 200,
            "body": {"type": "TEXT", "content": "ok"},
            "gzip": true
        }]
    })";

    const std::string response =
            fiber::access_server::process_native_validator_request(request("project_route", "example", payload));
    EXPECT_NE(response.find(R"("valid":true)"), std::string::npos) << response;
    EXPECT_NE(response.find(R"("projectVersion":3)"), std::string::npos) << response;
    EXPECT_NE(response.find(R"("hostCount":1)"), std::string::npos) << response;
    EXPECT_NE(response.find(R"("routeCount":1)"), std::string::npos) << response;
    EXPECT_NE(response.find(R"("estimatedSnapshotBytes":)"), std::string::npos) << response;
    EXPECT_NE(response.find(R"("staticResponseBytes":)"), std::string::npos) << response;
}

TEST(NativeValidatorProtocolTest, CompilesDynamicResponseGzipCombination) {
    constexpr std::string_view payload = R"({
        "version": 4,
        "host": {"example.com": {}},
        "routes": [{
            "path": "/dynamic",
            "type": "RESPONSE",
            "status": 200,
            "body": {"type": "TEMPLATE", "content": "dynamic"},
            "gzip": true
        }]
    })";

    const std::string response =
            fiber::access_server::process_native_validator_request(request("project_route", "example", payload));
    EXPECT_NE(response.find(R"("valid":true)"), std::string::npos) << response;
    EXPECT_NE(response.find(R"("routeCount":1)"), std::string::npos) << response;
}

TEST(NativeValidatorProtocolTest, ReturnsStructuredCompiledModelErrors) {
    constexpr std::string_view payload = R"({
        "version": 3,
        "host": {"example.com": {}},
        "routes": [{"path": "/health", "type": "RESPONSE", "status": 42}]
    })";

    const std::string response =
            fiber::access_server::process_native_validator_request(request("project_route", "example", payload));
    EXPECT_NE(response.find(R"("valid":false)"), std::string::npos) << response;
    EXPECT_NE(response.find(R"("code":"out_of_range")"), std::string::npos) << response;
    EXPECT_NE(response.find(R"("field":"routes[0].status")"), std::string::npos) << response;
}

TEST(NativeValidatorProtocolTest, CompilesMixedMethodAndJavaScriptRoutes) {
    constexpr std::string_view payload = R"({
        "version": 4,
        "host": {"example.com": {"https": "S_NOT_MUST"}},
        "routes": [
            {"path": "/items", "method": "GET", "type": "RESPONSE", "status": 200},
            {"path": "/items/:id", "method": "POST", "type": "SCRIPT",
             "script": "return {id: $path.id};"}
        ]
    })";

    const std::string response =
            fiber::access_server::process_native_validator_request(request("project_route", "example", payload));
    EXPECT_NE(response.find(R"("valid":true)"), std::string::npos) << response;
    EXPECT_NE(response.find(R"("routeCount":2)"), std::string::npos) << response;
}

TEST(NativeValidatorProtocolTest, CompilesHttpDirectiveJavaScriptRoute) {
    constexpr std::string_view payload = R"({
        "version": 5,
        "host": {"example.com": {}},
        "routes": [{
            "path": "/google",
            "type": "SCRIPT",
            "script": "directive google = http \"https://www.google.com\"; return google.request({path: \"/\"});"
        }]
    })";

    const std::string response =
            fiber::access_server::process_native_validator_request(request("project_route", "example", payload));
    EXPECT_NE(response.find(R"("valid":true)"), std::string::npos) << response;
    EXPECT_NE(response.find(R"("routeCount":1)"), std::string::npos) << response;
}

TEST(NativeValidatorProtocolTest, ValidatesClientIdentityReferenceWithoutClaimingRuntimeResolution) {
    constexpr std::string_view payload = R"({
        "version": 5,
        "host": {"example.com": {}},
        "routes": [{
            "path": "/secure",
            "type": "PROXY",
            "addresses": ["https://upstream.example"],
            "upstream_tls": {
                "generation": 1,
                "client_identity_ref": "123e4567-e89b-42d3-a456-426614174000"
            }
        }]
    })";
    const std::string accepted =
            fiber::access_server::process_native_validator_request(request("project_route", "example", payload));
    EXPECT_NE(accepted.find(R"("valid":true)"), std::string::npos) << accepted;

    constexpr std::string_view invalid_marker = "private-identity-marker";
    const std::string invalid_payload =
            R"({"host":{"example.com":{}},"routes":[{"path":"/","type":"PROXY","addresses":["https://upstream.example"],"upstream_tls":{"generation":1,"client_identity_ref":")" +
            std::string(invalid_marker) + R"("}}]})";
    const std::string rejected = fiber::access_server::process_native_validator_request(
            request("project_route", "example", invalid_payload));
    EXPECT_NE(rejected.find(R"("valid":false)"), std::string::npos) << rejected;
    EXPECT_NE(rejected.find(R"("field":"routes[0].upstream_tls.client_identity_ref")"), std::string::npos) << rejected;
    EXPECT_EQ(rejected.find(invalid_marker), std::string::npos) << rejected;
}

TEST(NativeValidatorProtocolTest, ReportsJavaScriptCompileErrorsWithoutEchoingSource) {
    constexpr std::string_view payload = R"({
        "host": {"example.com": {}},
        "routes": [{"path": "/script", "type": "SCRIPT", "script": "secret_marker + ;"}]
    })";

    const std::string response =
            fiber::access_server::process_native_validator_request(request("project_route", "example", payload));
    EXPECT_NE(response.find(R"("valid":false)"), std::string::npos) << response;
    EXPECT_NE(response.find(R"("field":"routes[0].script")"), std::string::npos) << response;
    EXPECT_EQ(response.find("secret_marker"), std::string::npos) << response;
}

TEST(NativeValidatorProtocolTest, RejectsInvalidProtocolAndBase64WithoutEchoingPayload) {
    const std::string malformed = fiber::access_server::process_native_validator_request("not-json");
    EXPECT_NE(malformed.find(R"("code":"invalid_request")"), std::string::npos) << malformed;

    const std::string invalid_base64 = fiber::access_server::process_native_validator_request(
            R"({"contractVersion":1,"requestId":"test","kind":"project_route","project":"secret-project","payloadBase64":"***"})");
    EXPECT_NE(invalid_base64.find(R"("field":"payloadBase64")"), std::string::npos) << invalid_base64;
    EXPECT_EQ(invalid_base64.find("secret-project"), std::string::npos) << invalid_base64;
}

TEST(NativeValidatorProtocolTest, AppliesStrictConsoleGrayValidation) {
    constexpr std::string_view payload = R"({"vdi":{"ratio":10001,"whites":[]}})";
    const std::string response =
            fiber::access_server::process_native_validator_request(request("gray_rules", "", payload));
    EXPECT_NE(response.find(R"("valid":false)"), std::string::npos) << response;
    EXPECT_NE(response.find(R"("code":"out_of_range")"), std::string::npos) << response;
}

TEST(NativeValidatorProtocolTest, ReportsStableLimitErrorsWithoutEchoingValues) {
    const std::string marker = "private-limit-marker";
    const std::string path = "/" + marker + std::string(2048, 'x');
    const std::string payload =
            R"({"host":{"example.com":{}},"routes":[{"path":")" + path + R"(","type":"RESPONSE","status":200}]})";
    const std::string response =
            fiber::access_server::process_native_validator_request(request("project_route", "example", payload));
    EXPECT_NE(response.find(R"("code":"limit_exceeded")"), std::string::npos) << response;
    EXPECT_NE(response.find(R"("field":"routes[0].path")"), std::string::npos) << response;
    EXPECT_EQ(response.find(marker), std::string::npos) << response;
}

TEST(NativeValidatorProtocolTest, DescribesTheRuntimeConfigLimits) {
    const std::string response = fiber::access_server::native_validator_config_limits_response();
    EXPECT_NE(response.find(R"("schemaVersion":4)"), std::string::npos) << response;
    EXPECT_NE(response.find(R"("maxUpstreamTlsProfiles":256)"), std::string::npos) << response;
    EXPECT_NE(response.find(R"("maxUpstreamTlsCaPemBytes":524288)"), std::string::npos) << response;
    EXPECT_NE(response.find(R"("maxPayloadBytes":4194304)"), std::string::npos) << response;
    EXPECT_NE(response.find(R"("maxRoutes":5000)"), std::string::npos) << response;
    EXPECT_NE(response.find(R"("maxEstimatedSnapshotBytes":67108864)"), std::string::npos) << response;
    EXPECT_NE(response.find(R"("grayRules")"), std::string::npos) << response;
    EXPECT_NE(response.find(R"("edgeFunctions":{"maxPayloadBytes":262144,"maxFunctions":64)"), std::string::npos)
            << response;
    EXPECT_NE(response.find(R"("maxFunctionNameBytes":64)"), std::string::npos) << response;
    EXPECT_NE(response.find(R"("maxScriptPayloadBytes":1048576,"maxTotalScriptBytes":8388608)"), std::string::npos)
            << response;
    EXPECT_NE(response.find(R"("maxCidrDirectivesPerScript":16,"maxCidrsPerDirective":256)"), std::string::npos)
            << response;
}

TEST(NativeValidatorProtocolTest, ValidatesEdgeFunctionIndexCandidates) {
    constexpr std::string_view payload = R"({
        "version": 3,
        "functions": [
            {"name": "waf",
             "sha256": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
             "mounts": [{"projects": {"include": ["*"]}}]},
            {"name": "audit",
             "sha256": "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210",
             "mounts": [{"projects": {"include": ["loan-*"]}, "priority": 10}]}
        ]
    })";

    const std::string response =
            fiber::access_server::process_native_validator_request(request("edge_function_index", "", payload));
    EXPECT_NE(response.find(R"("valid":true)"), std::string::npos) << response;
    EXPECT_NE(response.find(R"("functionCount":2)"), std::string::npos) << response;

    // JSON null is the explicit unmount candidate and validates as empty.
    const std::string unmount =
            fiber::access_server::process_native_validator_request(request("edge_function_index", "", "null"));
    EXPECT_NE(unmount.find(R"("valid":true)"), std::string::npos) << unmount;
    EXPECT_NE(unmount.find(R"("functionCount":0)"), std::string::npos) << unmount;

    // Empty content is keep-current for the watcher, never a publishable
    // candidate.
    const std::string empty =
            fiber::access_server::process_native_validator_request(request("edge_function_index", "", ""));
    EXPECT_NE(empty.find(R"("valid":false)"), std::string::npos) << empty;
    EXPECT_NE(empty.find(R"("code":"invalid_request")"), std::string::npos) << empty;
    EXPECT_NE(empty.find(R"("field":"payloadBase64")"), std::string::npos) << empty;

    // Codec violations surface with their structured field path: duplicate
    // slug names conflict.
    const std::string invalid = fiber::access_server::process_native_validator_request(
            request("edge_function_index", "",
                    R"({"version":1,"functions":[{"name":"waf",)"
                    R"("sha256":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",)"
                    R"("mounts":[{"projects":{"include":["*"]}}]},)"
                    R"({"name":"waf","sha256":"fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210",)"
                    R"("mounts":[{"projects":{"include":["*"]}}]}]})"));
    EXPECT_NE(invalid.find(R"("valid":false)"), std::string::npos) << invalid;
    EXPECT_NE(invalid.find(R"("code":"conflict")"), std::string::npos) << invalid;
    EXPECT_NE(invalid.find("functions[1]"), std::string::npos) << invalid;
}

TEST(NativeValidatorProtocolTest, CompilesEdgeFunctionScriptsWithCidrDirective) {
    constexpr std::string_view payload =
            R"(directive office = cidr "172.28.0.0/16"; return office.contains($conn.remote_addr);)";

    const std::string response =
            fiber::access_server::process_native_validator_request(request("edge_function_script", "", payload));
    EXPECT_NE(response.find(R"("valid":true)"), std::string::npos) << response;
    EXPECT_NE(response.find(R"("compiledProgramCount":1)"), std::string::npos) << response;
    EXPECT_NE(response.find(R"("scriptBytes":)" + std::to_string(payload.size()) + ","), std::string::npos) << response;
    EXPECT_NE(response.find(R"("cidrDirectiveCount":1)"), std::string::npos) << response;

    // Malformed CIDR literals fail the compile (never reach a data plane).
    const std::string bad_cidr = fiber::access_server::process_native_validator_request(
            request("edge_function_script", "", R"(directive office = cidr "172.28.0.0/33"; true)"));
    EXPECT_NE(bad_cidr.find(R"("valid":false)"), std::string::npos) << bad_cidr;
    EXPECT_NE(bad_cidr.find(R"("code":"script_compile_failed")"), std::string::npos) << bad_cidr;
}

TEST(NativeValidatorProtocolTest, ReportsEdgeScriptCompileErrorsWithoutEchoingSource) {
    const std::string response = fiber::access_server::process_native_validator_request(
            request("edge_function_script", "", "$context.secret_marker == 'x'"));
    EXPECT_NE(response.find(R"("valid":false)"), std::string::npos) << response;
    EXPECT_NE(response.find(R"("code":"script_compile_failed")"), std::string::npos) << response;
    EXPECT_NE(response.find(R"("field":"payloadBase64")"), std::string::npos) << response;
    EXPECT_NE(response.find(R"("offset":)"), std::string::npos) << response;
    EXPECT_EQ(response.find("secret_marker"), std::string::npos) << response;
}

TEST(NativeValidatorProtocolTest, RejectsOversizedEdgeScriptPayload) {
    const std::string oversized(1024U * 1024U + 1U, 'x');
    const std::string response =
            fiber::access_server::process_native_validator_request(request("edge_function_script", "", oversized));
    EXPECT_NE(response.find(R"("valid":false)"), std::string::npos) << response;
    EXPECT_NE(response.find(R"("code":"limit_exceeded")"), std::string::npos) << response;
    EXPECT_NE(response.find(R"("field":"payloadBase64")"), std::string::npos) << response;
    EXPECT_EQ(response.find("xxxxxxxx"), std::string::npos) << response;
}

} // namespace
