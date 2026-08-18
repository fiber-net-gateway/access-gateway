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
    EXPECT_NE(response.find(R"("schemaVersion":2)"), std::string::npos) << response;
    EXPECT_NE(response.find(R"("maxUpstreamTlsProfiles":256)"), std::string::npos) << response;
    EXPECT_NE(response.find(R"("maxUpstreamTlsCaPemBytes":524288)"), std::string::npos) << response;
    EXPECT_NE(response.find(R"("maxPayloadBytes":4194304)"), std::string::npos) << response;
    EXPECT_NE(response.find(R"("maxRoutes":5000)"), std::string::npos) << response;
    EXPECT_NE(response.find(R"("maxEstimatedSnapshotBytes":67108864)"), std::string::npos) << response;
    EXPECT_NE(response.find(R"("grayRules")"), std::string::npos) << response;
}

} // namespace
