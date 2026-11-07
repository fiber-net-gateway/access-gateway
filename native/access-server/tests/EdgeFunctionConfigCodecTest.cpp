#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "config/AccessConfigCodec.h"
#include "config/AccessConfigError.h"
#include "config/AccessConfigLimits.h"

namespace {

using fiber::access_server::AccessConfigError;
using fiber::access_server::AccessConfigErrorCode;
using fiber::access_server::AccessConfigLimits;
using fiber::access_server::EdgeFunctionIndexConfig;
using fiber::access_server::EdgeFunctionIndexEntry;
using fiber::access_server::EdgeFunctionMountConfig;
using fiber::access_server::parse_edge_function_index_config;
using fiber::access_server::validate_edge_function_index_limits;

const char *kValidSha256 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
const char *kAllProjects = R"({"projects": {"include": ["*"]}})";

// mounts_json is a single mount object; the wire wraps it in an array. The
// script Data ID is derived from the name and never appears in the JSON.
std::string function_json(const std::string &name, std::string sha256 = kValidSha256,
                          std::string mounts_json = kAllProjects) {
    return "{\"name\": \"" + name + "\", \"sha256\": \"" + sha256 + "\", \"mounts\": [" + mounts_json + "]}";
}

std::string index_json(std::string_view version, std::string functions) {
    std::string content = "{\"version\": ";
    content.append(version.empty() ? "1" : version);
    content.append(", \"functions\": [");
    content.append(std::move(functions));
    content.append("]}");
    return content;
}

AccessConfigError parse_error(std::string content) {
    auto parsed = parse_edge_function_index_config(content);
    EXPECT_FALSE(parsed.has_value());
    return parsed.error();
}

TEST(EdgeFunctionConfigCodec, EmptyContentIsKeepCurrent) {
    auto parsed = parse_edge_function_index_config("");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_FALSE(parsed->has_value());
}

TEST(EdgeFunctionConfigCodec, NullContentUnmountsAll) {
    auto parsed = parse_edge_function_index_config("null");
    ASSERT_TRUE(parsed.has_value());
    ASSERT_TRUE(parsed->has_value());
    EXPECT_TRUE((*parsed)->functions.empty());
    EXPECT_EQ((*parsed)->version, 0);
}

TEST(EdgeFunctionConfigCodec, ParsesFullIndex) {
    const std::string content = R"({
        "version": 7,
        "functions": [
            {
                "name": "waf-generated",
                "sha256": ")" + std::string(kValidSha256) +
                                R"(",
                "mounts": [
                    {"projects": {"include": ["loan-*", "trade-web"], "exclude": ["loan-admin-legacy"]},
                     "priority": 100, "ratio": 1000},
                    {"projects": {"include": ["*"]}}
                ]
            },
            {
                "name": "audit",
                "sha256": ")" + std::string(kValidSha256) +
                                R"(",
                "mounts": [{"projects": {"include": ["trade-web"]}, "priority": 0}]
            }
        ]
    })";
    auto parsed = parse_edge_function_index_config(content);
    ASSERT_TRUE(parsed.has_value());
    ASSERT_TRUE(parsed->has_value());
    const EdgeFunctionIndexConfig &config = **parsed;
    EXPECT_EQ(config.version, 7);
    ASSERT_EQ(config.functions.size(), 2U);

    const EdgeFunctionIndexEntry &first = config.functions[0];
    EXPECT_EQ(first.name, "waf-generated");
    EXPECT_EQ(first.sha256_hex, kValidSha256);
    ASSERT_EQ(first.mounts.size(), 2U);
    EXPECT_EQ(first.mounts[0].priority, 100);
    EXPECT_EQ(first.mounts[0].ratio, 1000);
    ASSERT_EQ(first.mounts[0].projects.include.size(), 2U);
    EXPECT_EQ(first.mounts[0].projects.include[0], "loan-*");
    EXPECT_EQ(first.mounts[0].projects.include[1], "trade-web");
    ASSERT_EQ(first.mounts[0].projects.exclude.size(), 1U);
    EXPECT_EQ(first.mounts[0].projects.exclude[0], "loan-admin-legacy");
    EXPECT_EQ(first.mounts[1].priority, 0);
    EXPECT_EQ(first.mounts[1].ratio, fiber::access_server::kEdgeFunctionFullRatio);
    ASSERT_EQ(first.mounts[1].projects.include.size(), 1U);
    EXPECT_EQ(first.mounts[1].projects.include[0], "*");
    EXPECT_TRUE(first.mounts[1].projects.exclude.empty());

    EXPECT_EQ(config.functions[1].name, "audit");
    EXPECT_EQ(config.functions[1].mounts[0].ratio, fiber::access_server::kEdgeFunctionFullRatio);
}

TEST(EdgeFunctionConfigCodec, PhaseDefaultsToRequestHead) {
    auto parsed = parse_edge_function_index_config(index_json("1", function_json("audit")));
    ASSERT_TRUE(parsed.has_value());
    ASSERT_TRUE(parsed->has_value());
    ASSERT_EQ((*parsed)->functions.size(), 1U);
    EXPECT_EQ((*parsed)->functions[0].phase, fiber::access_server::EdgeFunctionPhase::RequestHead);
}

TEST(EdgeFunctionConfigCodec, PhaseRequestBodyAccepted) {
    auto parsed = parse_edge_function_index_config(
            R"({"version": 3, "functions": [)"
            R"({"name": "body-scan", "sha256": ")" +
            std::string(kValidSha256) +
            R"(", "phase": "request_body", "mounts": [{"projects": {"include": ["loan-*"]}}]}]})");
    ASSERT_TRUE(parsed.has_value());
    ASSERT_TRUE(parsed->has_value());
    ASSERT_EQ((*parsed)->functions.size(), 1U);
    EXPECT_EQ((*parsed)->functions[0].phase, fiber::access_server::EdgeFunctionPhase::RequestBody);
}

TEST(EdgeFunctionConfigCodec, UnknownPhaseRejected) {
    const AccessConfigError error =
            parse_error(R"({"version": 3, "functions": [)"
                        R"({"name": "body-scan", "sha256": ")" +
                        std::string(kValidSha256) +
                        R"(", "phase": "response", "mounts": [{"projects": {"include": ["loan-*"]}}]}]})");
    EXPECT_EQ(error.code, AccessConfigErrorCode::InvalidField);
    EXPECT_EQ(error.field, "functions[0].phase");
}

TEST(EdgeFunctionConfigCodec, PhaseMustBeText) {
    const AccessConfigError error = parse_error(
            R"({"version": 3, "functions": [)"
            R"({"name": "body-scan", "sha256": ")" +
            std::string(kValidSha256) + R"(", "phase": 2, "mounts": [{"projects": {"include": ["loan-*"]}}]}]})");
    EXPECT_EQ(error.code, AccessConfigErrorCode::InvalidField);
    EXPECT_EQ(error.field, "functions[0].phase");
}

TEST(EdgeFunctionConfigCodec, EmptyFunctionsListIsAccepted) {
    auto parsed = parse_edge_function_index_config(R"({"version": 2})");
    ASSERT_TRUE(parsed.has_value());
    ASSERT_TRUE(parsed->has_value());
    EXPECT_TRUE((*parsed)->functions.empty());
    EXPECT_EQ((*parsed)->version, 2);
}

TEST(EdgeFunctionConfigCodec, MissingVersionRejected) {
    const AccessConfigError error = parse_error(R"({"functions": []})");
    EXPECT_EQ(error.code, AccessConfigErrorCode::InvalidField);
    EXPECT_EQ(error.field, "version");
}

TEST(EdgeFunctionConfigCodec, NegativeVersionRejected) {
    EXPECT_EQ(parse_error(R"({"version": -1})").code, AccessConfigErrorCode::OutOfRange);
}

TEST(EdgeFunctionConfigCodec, NonIntegerVersionRejected) {
    EXPECT_EQ(parse_error(R"({"version": 3.5})").code, AccessConfigErrorCode::InvalidField);
    EXPECT_EQ(parse_error(R"({"version": "1"})").code, AccessConfigErrorCode::InvalidField);
}

TEST(EdgeFunctionConfigCodec, RootMustBeObjectOrNull) {
    EXPECT_EQ(parse_error(R"([1, 2])").code, AccessConfigErrorCode::InvalidRoot);
    EXPECT_EQ(parse_error(R"("x")").code, AccessConfigErrorCode::InvalidRoot);
    EXPECT_EQ(parse_error(R"(42)").code, AccessConfigErrorCode::InvalidRoot);
    EXPECT_EQ(parse_error("{").code, AccessConfigErrorCode::InvalidJson);
}

TEST(EdgeFunctionConfigCodec, UnknownFieldsRejected) {
    EXPECT_EQ(parse_error(R"({"version": 1, "extra": 0})").field, "index.extra");
    const AccessConfigError entry_error =
            parse_error(index_json("1", function_json("waf") + ",{\"unexpected\": true}"));
    EXPECT_EQ(entry_error.code, AccessConfigErrorCode::InvalidField);
    // The pre-revision identity fields are gone from the wire: carrying them
    // again is an unknown-field failure, not a conflict.
    EXPECT_EQ(parse_error(index_json("1", "{\"id\": \"waf\", \"sha256\": \"" + std::string(kValidSha256) +
                                                  "\", \"mounts\": [" + kAllProjects + "]}"))
                      .field,
              "functions[0].id");
    EXPECT_EQ(
            parse_error(index_json("1", "{\"dataId\": \"ploto.unified-access.edge-function.waf\", \"sha256\": \"" +
                                                std::string(kValidSha256) + "\", \"mounts\": [" + kAllProjects + "]}"))
                    .field,
            "functions[0].dataId");
}

TEST(EdgeFunctionConfigCodec, InvalidFunctionNamesRejected) {
    for (const std::string name: {std::string("WAF"), std::string("-waf"), std::string("waf-"), std::string("wa f"),
                                  std::string("waf_x"), std::string(65, 'a'), std::string("中文")}) {
        const AccessConfigError error = parse_error(index_json("1", function_json(name)));
        EXPECT_EQ(error.code, AccessConfigErrorCode::InvalidField) << "name: " << name;
        EXPECT_EQ(error.field, "functions[0].name") << "name: " << name;
    }
}

TEST(EdgeFunctionConfigCodec, Sha256MustBeLowercaseHex64) {
    for (const std::string hash:
         {std::string(64, 'A'), std::string(63, 'a'), std::string(64, 'z'), std::string(65, 'a')}) {
        const AccessConfigError error = parse_error(index_json("1", function_json("waf", hash)));
        EXPECT_EQ(error.code, AccessConfigErrorCode::InvalidField) << "hash: " << hash;
        EXPECT_EQ(error.field, "functions[0].sha256") << "hash: " << hash;
    }
}

TEST(EdgeFunctionConfigCodec, DuplicateFunctionNameRejected) {
    const std::string entry = function_json("dup");
    const AccessConfigError error = parse_error(index_json("1", entry + ", " + entry));
    EXPECT_EQ(error.code, AccessConfigErrorCode::Conflict);
    EXPECT_EQ(error.field, "functions[1]");
}

TEST(EdgeFunctionConfigCodec, MissingEntryFieldsRejected) {
    const AccessConfigError no_name = parse_error(
            index_json("1", "{\"sha256\": \"" + std::string(kValidSha256) + "\", \"mounts\": [" + kAllProjects + "]}"));
    EXPECT_EQ(no_name.field, "functions[0].name");

    const AccessConfigError no_sha256 =
            parse_error(index_json("1", "{\"name\": \"waf\", \"mounts\": [" + std::string(kAllProjects) + "]}"));
    EXPECT_EQ(no_sha256.field, "functions[0].sha256");

    const AccessConfigError no_mounts =
            parse_error(index_json("1", "{\"name\": \"waf\", \"sha256\": \"" + std::string(kValidSha256) + "\"}"));
    EXPECT_EQ(no_mounts.field, "functions[0].mounts");

    const AccessConfigError empty_mounts = parse_error(index_json("1", function_json("waf", kValidSha256, "")));
    EXPECT_EQ(empty_mounts.field, "functions[0].mounts");
}

TEST(EdgeFunctionConfigCodec, SelectorMustIncludeAtLeastOneGlob) {
    const AccessConfigError error =
            parse_error(index_json("1", function_json("waf", kValidSha256, R"({"projects": {"include": []}})")));
    EXPECT_EQ(error.code, AccessConfigErrorCode::InvalidField);
    EXPECT_EQ(error.field, "functions[0].mounts[0].projects");
}

TEST(EdgeFunctionConfigCodec, SelectorWithoutProjectsRejected) {
    const AccessConfigError error =
            parse_error(index_json("1", function_json("waf", kValidSha256, R"({"priority": 5})")));
    EXPECT_EQ(error.field, "functions[0].mounts[0]");
}

TEST(EdgeFunctionConfigCodec, PriorityRangeEnforced) {
    for (const std::string priority: {"-1", "1000001", R"("5")"}) {
        const AccessConfigError error = parse_error(
                index_json("1", function_json("waf", kValidSha256,
                                              R"({"projects": {"include": ["*"]}, "priority": )" + priority + "}")));
        if (priority == R"("5")") {
            EXPECT_EQ(error.code, AccessConfigErrorCode::InvalidField) << "priority: " << priority;
        } else {
            EXPECT_EQ(error.code, AccessConfigErrorCode::OutOfRange) << "priority: " << priority;
        }
        EXPECT_EQ(error.field, "functions[0].mounts[0].priority") << "priority: " << priority;
    }
}

TEST(EdgeFunctionConfigCodec, RatioRangeEnforced) {
    for (const std::string ratio: {"-1", "10001", R"("5")"}) {
        const AccessConfigError error = parse_error(
                index_json("1", function_json("waf", kValidSha256,
                                              R"({"projects": {"include": ["*"]}, "ratio": )" + ratio + "}")));
        if (ratio == R"("5")") {
            EXPECT_EQ(error.code, AccessConfigErrorCode::InvalidField) << "ratio: " << ratio;
        } else {
            EXPECT_EQ(error.code, AccessConfigErrorCode::OutOfRange) << "ratio: " << ratio;
        }
        EXPECT_EQ(error.field, "functions[0].mounts[0].ratio") << "ratio: " << ratio;
    }

    // Both endpoints of the closed domain are legal: 10000 = must-run,
    // 0 = mounted-but-disabled.
    for (const std::string ratio: {"0", "10000"}) {
        auto parsed = parse_edge_function_index_config(
                index_json("1", function_json("waf", kValidSha256,
                                              R"({"projects": {"include": ["*"]}, "ratio": )" + ratio + "}")));
        ASSERT_TRUE(parsed.has_value() && parsed->has_value()) << "ratio: " << ratio;
        EXPECT_EQ((*parsed)->functions[0].mounts[0].ratio, std::stoi(ratio)) << "ratio: " << ratio;
    }
}

TEST(EdgeFunctionConfigCodec, GlobCharsetEnforced) {
    for (const std::string glob: {std::string("a;b"), std::string("")}) {
        const AccessConfigError error = parse_error(index_json(
                "1", function_json("waf", kValidSha256, R"({"projects": {"include": [")" + glob + R"("]}})")));
        EXPECT_EQ(error.code, AccessConfigErrorCode::InvalidField) << "glob: '" << glob << "'";
        EXPECT_EQ(error.field, "functions[0].mounts[0].projects.include[0]") << "glob: '" << glob << "'";
    }
    // A raw newline inside a JSON string never reaches the glob check; the
    // JSON parser rejects control characters first.
    const AccessConfigError json_error = parse_error(
            index_json("1", function_json("waf", kValidSha256, "{\"projects\": {\"include\": [\"a\nb\"]}}")));
    EXPECT_EQ(json_error.code, AccessConfigErrorCode::InvalidJson);
}

TEST(EdgeFunctionConfigCodec, LimitsEnforced) {
    const std::string content =
            index_json("1", function_json("waf", kValidSha256, R"({"projects": {"include": ["*", "loan-*"]}})"));

    AccessConfigLimits small{};
    small.edge_functions.max_functions = 0;
    auto error = parse_edge_function_index_config(content, small);
    ASSERT_FALSE(error.has_value());
    EXPECT_EQ(error.error().code, AccessConfigErrorCode::LimitExceeded);
    EXPECT_EQ(error.error().field, "functions");

    AccessConfigLimits tiny_glob{};
    tiny_glob.edge_functions.max_globs_per_selector = 1;
    error = parse_edge_function_index_config(content, tiny_glob);
    ASSERT_FALSE(error.has_value());
    EXPECT_EQ(error.error().field, "functions[0].mounts[0].projects.include");

    AccessConfigLimits tiny_payload{};
    tiny_payload.edge_functions.max_payload_bytes = 8;
    error = parse_edge_function_index_config(content, tiny_payload);
    ASSERT_FALSE(error.has_value());
    EXPECT_EQ(error.error().field, "payload");
}

TEST(EdgeFunctionConfigLimits, StandaloneValidatorChecksConstructedConfigs) {
    EdgeFunctionIndexConfig config;
    config.version = 1;
    EdgeFunctionIndexEntry entry;
    entry.name = "waf";
    entry.sha256_hex = kValidSha256;
    EdgeFunctionMountConfig mount;
    mount.projects.include.push_back("*");
    entry.mounts.push_back(mount);
    config.functions.push_back(entry);

    EXPECT_TRUE(validate_edge_function_index_limits(config).has_value());

    AccessConfigLimits small{};
    small.edge_functions.max_functions = 0;
    auto error = validate_edge_function_index_limits(config, small);
    ASSERT_FALSE(error.has_value());
    EXPECT_EQ(error.error().field, "functions");

    AccessConfigLimits small_name{};
    small_name.edge_functions.max_function_name_bytes = 0;
    error = validate_edge_function_index_limits(config, small_name);
    ASSERT_FALSE(error.has_value());
    EXPECT_EQ(error.error().field, "functions[0].name");

    AccessConfigLimits small_glob{};
    small_glob.edge_functions.max_glob_bytes = 0;
    error = validate_edge_function_index_limits(config, small_glob);
    ASSERT_FALSE(error.has_value());
    EXPECT_EQ(error.error().field, "functions[0].mounts[0].projects.include[0]");
}

} // namespace
