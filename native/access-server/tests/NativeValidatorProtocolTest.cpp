#include "validation/NativeValidatorProtocol.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include <fiber/common/util/Base64.h>

namespace {

std::string request(std::string_view kind, std::string_view project,
                    std::string_view payload) {
  const std::string encoded = fiber::util::base64_encode(
      reinterpret_cast<const std::uint8_t *>(payload.data()), payload.size());
  return R"({"contractVersion":1,"requestId":"test-request","kind":")" +
         std::string(kind) + R"(","project":")" + std::string(project) +
         R"(","payloadBase64":")" + encoded + R"("})";
}

TEST(NativeValidatorProtocolTest, CompilesProjectPayloadWithAccessServerCore) {
  constexpr std::string_view payload = R"({
        "version": 3,
        "host": {"example.com": {"https": "S_NOT_MUST"}},
        "routes": [{
            "path": "/health",
            "type": "RESPONSE",
            "status": 200,
            "body": {"type": "TEXT", "content": "ok"}
        }]
    })";

  const std::string response =
      fiber::access_server::process_native_validator_request(
          request("project_route", "example", payload));
  EXPECT_NE(response.find(R"("valid":true)"), std::string::npos) << response;
  EXPECT_NE(response.find(R"("projectVersion":3)"), std::string::npos)
      << response;
  EXPECT_NE(response.find(R"("hostCount":1)"), std::string::npos) << response;
  EXPECT_NE(response.find(R"("routeCount":1)"), std::string::npos) << response;
}

TEST(NativeValidatorProtocolTest, ReturnsStructuredCompiledModelErrors) {
  constexpr std::string_view payload = R"({
        "version": 3,
        "host": {"example.com": {}},
        "routes": [{"path": "/health", "type": "RESPONSE", "status": 42}]
    })";

  const std::string response =
      fiber::access_server::process_native_validator_request(
          request("project_route", "example", payload));
  EXPECT_NE(response.find(R"("valid":false)"), std::string::npos) << response;
  EXPECT_NE(response.find(R"("code":"out_of_range")"), std::string::npos)
      << response;
  EXPECT_NE(response.find(R"("field":"routes[0].status")"), std::string::npos)
      << response;
}

TEST(NativeValidatorProtocolTest,
     RejectsInvalidProtocolAndBase64WithoutEchoingPayload) {
  const std::string malformed =
      fiber::access_server::process_native_validator_request("not-json");
  EXPECT_NE(malformed.find(R"("code":"invalid_request")"), std::string::npos)
      << malformed;

  const std::string invalid_base64 =
      fiber::access_server::process_native_validator_request(
          R"({"contractVersion":1,"requestId":"test","kind":"project_route","project":"secret-project","payloadBase64":"***"})");
  EXPECT_NE(invalid_base64.find(R"("field":"payloadBase64")"),
            std::string::npos)
      << invalid_base64;
  EXPECT_EQ(invalid_base64.find("secret-project"), std::string::npos)
      << invalid_base64;
}

TEST(NativeValidatorProtocolTest, AppliesStrictConsoleGrayValidation) {
  constexpr std::string_view payload = R"({"vdi":{"ratio":10001,"whites":[]}})";
  const std::string response =
      fiber::access_server::process_native_validator_request(
          request("gray_rules", "", payload));
  EXPECT_NE(response.find(R"("valid":false)"), std::string::npos) << response;
  EXPECT_NE(response.find(R"("code":"out_of_range")"), std::string::npos)
      << response;
}

} // namespace
