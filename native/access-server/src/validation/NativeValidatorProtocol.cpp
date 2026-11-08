#include "NativeValidatorProtocol.h"

#include "config/AccessConfigCodec.h"
#include "routing/AccessScriptCompiler.h"
#include "routing/Cidr.h"
#include "routing/EdgeFunctionLibrary.h"
#include "routing/GrayMatchCompiler.h"
#include "routing/ProjectConfigCompiler.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include <fiber/common/json/JsonEncode.h>
#include <fiber/common/json/JsonParse.h>
#include <fiber/common/json/JsonParser.h>
#include <fiber/common/mem/BufPool.h>
#include <fiber/common/util/Base64.h>

namespace fiber::access_server {
namespace {

using json::Generator;
using json::JsonParser;
using json::ObjectFieldStatus;
using json::ParseStatus;
using mem::BufPool;

enum RequestField : std::uint8_t {
    ContractVersion = 1U << 0U,
    RequestId = 1U << 1U,
    Kind = 1U << 2U,
    Project = 1U << 3U,
    Payload = 1U << 4U,
    Phase = 1U << 5U,
};

struct ValidatorRequest {
    std::int64_t contract_version = 0;
    std::string_view request_id;
    std::string_view kind;
    std::string_view project;
    std::string_view payload_base64;
    // Optional, kind=edge_function_script only: compile the script in the
    // request_body phase so $req.body resolves (empty = request_head).
    std::string_view phase;
    std::uint8_t present = 0;
};

struct ValidationError {
    std::string code;
    std::string field;
    std::size_t offset = 0;
    std::string message;
};

struct ValidationSummary {
    std::optional<std::int32_t> project_version;
    std::optional<std::size_t> host_count;
    std::optional<std::size_t> route_count;
    std::optional<std::size_t> gray_rule_count;
    std::optional<std::size_t> estimated_snapshot_bytes;
    std::optional<std::size_t> static_response_bytes;
    std::optional<std::size_t> compiled_program_count;
    std::optional<std::size_t> function_count;
    std::optional<std::size_t> script_bytes;
    std::optional<std::size_t> cidr_directive_count;
};

struct ValidationResult {
    bool valid = false;
    ValidationSummary summary;
    std::vector<ValidationError> errors;
};

bool set_present(ValidatorRequest &out, RequestField field, JsonParser &parser) noexcept {
    const auto flag = static_cast<std::uint8_t>(field);
    if ((out.present & flag) != 0) {
        (void) parser.fail("duplicate validator request field");
        return false;
    }
    out.present |= flag;
    return true;
}

ObjectFieldStatus parse_request_field(std::string_view field, JsonParser &parser, BufPool &pool,
                                      ValidatorRequest &out) noexcept {
    if (field == "contractVersion") {
        if (!set_present(out, RequestField::ContractVersion, parser)) {
            return ObjectFieldStatus::Error;
        }
        return json::to_object_field_status(json::parse_integer(parser, pool, out.contract_version));
    }
    if (field == "requestId") {
        if (!set_present(out, RequestField::RequestId, parser)) {
            return ObjectFieldStatus::Error;
        }
        return json::to_object_field_status(json::parse_text(parser, pool, out.request_id));
    }
    if (field == "kind") {
        if (!set_present(out, RequestField::Kind, parser)) {
            return ObjectFieldStatus::Error;
        }
        return json::to_object_field_status(json::parse_text(parser, pool, out.kind));
    }
    if (field == "project") {
        if (!set_present(out, RequestField::Project, parser)) {
            return ObjectFieldStatus::Error;
        }
        return json::to_object_field_status(json::parse_text(parser, pool, out.project));
    }
    if (field == "payloadBase64") {
        if (!set_present(out, RequestField::Payload, parser)) {
            return ObjectFieldStatus::Error;
        }
        return json::to_object_field_status(json::parse_text(parser, pool, out.payload_base64));
    }
    if (field == "phase") {
        if (!set_present(out, RequestField::Phase, parser)) {
            return ObjectFieldStatus::Error;
        }
        return json::to_object_field_status(json::parse_text(parser, pool, out.phase));
    }
    (void) parser.fail("unknown validator request field");
    return ObjectFieldStatus::Error;
}

ParseStatus parse_request(JsonParser &parser, BufPool &pool, ValidatorRequest &out) noexcept {
    return json::parse_object_fields<parse_request_field>(parser, pool, out);
}

std::optional<ValidationError> decode_request(std::string_view input, ValidatorRequest &request, BufPool &pool) {
    JsonParser parser;
    if (!parser.feed(input.data(), input.size())) {
        const json::ParseError &error = parser.error();
        return ValidationError{
                .code = "invalid_request",
                .field = "request",
                .offset = error.offset,
                .message = error.message ? error.message : "invalid validator request JSON",
        };
    }
    parser.finish();
    if (json::parse_document<parse_request>(parser, pool, request) != ParseStatus::Done) {
        const json::ParseError &error = parser.error();
        return ValidationError{
                .code = "invalid_request",
                .field = "request",
                .offset = error.offset,
                .message = error.message ? error.message : "invalid validator request JSON",
        };
    }
    constexpr std::uint8_t required = RequestField::ContractVersion | RequestField::RequestId | RequestField::Kind |
                                      RequestField::Project | RequestField::Payload;
    if ((request.present & required) != required) {
        return ValidationError{
                .code = "invalid_request",
                .field = "request",
                .message = "validator request is missing a required field",
        };
    }
    if (request.contract_version != kNativeValidatorContractVersion) {
        return ValidationError{
                .code = "unsupported_contract",
                .field = "contractVersion",
                .message = "validator contract version is not supported",
        };
    }
    if (request.request_id.empty() || request.request_id.size() > 128) {
        return ValidationError{
                .code = "invalid_request",
                .field = "requestId",
                .message = "requestId must be 1-128 bytes",
        };
    }
    if (request.kind != "project_route" && request.kind != "gray_rules" && request.kind != "edge_function_index" &&
        request.kind != "edge_function_script") {
        return ValidationError{
                .code = "invalid_request",
                .field = "kind",
                .message = "validator kind is not supported",
        };
    }
    if (request.kind == "project_route" &&
        (request.project.empty() || request.project.size() > kAccessConfigLimits.project_list.max_project_name_bytes ||
         request.project.find(';') != std::string_view::npos)) {
        return ValidationError{
                .code = "invalid_request",
                .field = "project",
                .message = "project name is invalid",
        };
    }
    return std::nullopt;
}

std::string_view access_error_code(AccessConfigErrorCode code) noexcept {
    switch (code) {
        case AccessConfigErrorCode::InvalidJson:
            return "invalid_json";
        case AccessConfigErrorCode::InvalidRoot:
            return "invalid_root";
        case AccessConfigErrorCode::InvalidField:
            return "invalid_field";
        case AccessConfigErrorCode::OutOfRange:
            return "out_of_range";
        case AccessConfigErrorCode::InvalidCombination:
            return "invalid_combination";
        case AccessConfigErrorCode::Conflict:
            return "conflict";
        case AccessConfigErrorCode::LimitExceeded:
            return "limit_exceeded";
        case AccessConfigErrorCode::MissingDependency:
            return "missing_dependency";
    }
    return "invalid_configuration";
}

ValidationError to_validation_error(AccessConfigError error) {
    return ValidationError{
            .code = std::string(access_error_code(error.code)),
            .field = std::move(error.field),
            .offset = error.offset,
            .message = std::move(error.message),
    };
}

ValidationResult validate_project(std::string_view project, std::string_view payload) {
    auto parsed = parse_project_config(payload);
    if (!parsed) {
        return ValidationResult{.errors = {to_validation_error(std::move(parsed.error()))}};
    }
    if (!*parsed) {
        return ValidationResult{
                .valid = true,
                .summary =
                        {
                                .project_version = 0,
                                .host_count = 0,
                                .route_count = 0,
                        },
        };
    }

    AccessScriptCompiler scripts;
    auto compiled = compile_project_config(project, **parsed, scripts.adapter());
    if (!compiled) {
        return ValidationResult{.errors = {to_validation_error(std::move(compiled.error()))}};
    }
    const std::size_t host_count = *compiled ? (**compiled).hosts().size() : 0;
    const std::size_t route_count = *compiled ? (**compiled).routes().size() : 0;
    return ValidationResult{
            .valid = true,
            .summary =
                    {
                            .project_version = (**parsed).version,
                            .host_count = host_count,
                            .route_count = route_count,
                            .estimated_snapshot_bytes = *compiled ? (**compiled).estimated_memory_bytes() : 0,
                            .static_response_bytes = *compiled ? (**compiled).static_response_bytes() : 0,
                            .compiled_program_count = *compiled ? (**compiled).compiled_program_count() : 0,
                    },
    };
}

std::optional<ValidationError> validate_gray_entry(const GrayMatchConfigEntry &entry, std::size_t index,
                                                   std::unordered_set<std::string> &entries) {
    const std::string prefix = "rules[" + std::to_string(index) + ']';
    if (!recognized_gray_match_entry(entry.entry)) {
        return ValidationError{
                .code = "invalid_field",
                .field = prefix + ".entry",
                .message = "gray entry is not recognized",
        };
    }
    if (!entries.emplace(entry.entry).second) {
        return ValidationError{
                .code = "conflict",
                .field = prefix + ".entry",
                .message = "gray entry is duplicate",
        };
    }
    if (entry.ratio < 0 || entry.ratio > 10000) {
        return ValidationError{
                .code = "out_of_range",
                .field = prefix + ".ratio",
                .message = "gray ratio must be between 0 and 10000",
        };
    }
    for (std::size_t cidr_index = 0; cidr_index < entry.cidrs.size(); ++cidr_index) {
        const std::string field = prefix + ".cidrs[" + std::to_string(cidr_index) + ']';
        if (!entry.cidrs[cidr_index]) {
            return ValidationError{
                    .code = "invalid_field",
                    .field = field,
                    .message = "gray CIDR must not be null",
            };
        }
        auto cidr = Cidr::parse(*entry.cidrs[cidr_index], field);
        if (!cidr) {
            AccessConfigError error = std::move(cidr.error());
            error.field = field;
            return to_validation_error(std::move(error));
        }
    }
    return std::nullopt;
}

ValidationResult validate_gray(std::string_view payload) {
    auto parsed = parse_gray_match_config(payload);
    if (!parsed) {
        return ValidationResult{.errors = {to_validation_error(std::move(parsed.error()))}};
    }
    if (!*parsed) {
        return ValidationResult{
                .valid = true,
                .summary = {.gray_rule_count = 0},
        };
    }

    std::unordered_set<std::string> entries;
    for (std::size_t index = 0; index < (**parsed).size(); ++index) {
        if (auto error = validate_gray_entry((**parsed)[index], index, entries)) {
            return ValidationResult{.errors = {std::move(*error)}};
        }
    }
    auto compiled = compile_gray_match_config(**parsed);
    if (!compiled) {
        return ValidationResult{.errors = {to_validation_error(std::move(compiled.error()))}};
    }
    return ValidationResult{
            .valid = true,
            .summary = {.gray_rule_count = compiled->rule_count()},
    };
}

ValidationResult validate_edge_function_index(std::string_view payload) {
    auto parsed = parse_edge_function_index_config(payload);
    if (!parsed) {
        return ValidationResult{.errors = {to_validation_error(std::move(parsed.error()))}};
    }
    if (!parsed->has_value()) {
        // The watcher treats empty content as keep-current; a validation
        // request is always about a publishable candidate, and only JSON
        // null (or an empty functions array) publishes.
        return ValidationResult{
                .errors = {{
                        .code = "invalid_request",
                        .field = "payloadBase64",
                        .message = "empty payload is keep-current, not a publishable edge-function index",
                }},
        };
    }
    if (auto limited = validate_edge_function_index_limits(**parsed); !limited) {
        return ValidationResult{.errors = {to_validation_error(std::move(limited.error()))}};
    }
    return ValidationResult{
            .valid = true,
            .summary = {.function_count = (**parsed).functions.size()},
    };
}

ValidationResult validate_edge_function_script(std::string_view payload, std::string_view phase) {
    if (payload.size() > kAccessConfigLimits.edge_functions.max_script_payload_bytes) {
        return ValidationResult{
                .errors = {{
                        .code = "limit_exceeded",
                        .field = "payloadBase64",
                        .message = "edge-function script exceeds maxScriptPayloadBytes",
                }},
        };
    }
    const EdgeFunctionPhase compile_phase = phase.empty() || phase == "request_head" ? EdgeFunctionPhase::RequestHead
                                            : phase == "request_body"                ? EdgeFunctionPhase::RequestBody
                                                                                     : EdgeFunctionPhase::Count;
    if (compile_phase == EdgeFunctionPhase::Count) {
        return ValidationResult{
                .errors = {{
                        .code = "invalid_request",
                        .field = "phase",
                        .message = "phase must be request_head or request_body",
                }},
        };
    }
    EdgeFunctionLibrary library;
    auto program = library.compile(payload, compile_phase);
    if (!program) {
        return ValidationResult{
                .errors = {{
                        .code = "script_compile_failed",
                        .field = "payloadBase64",
                        .offset = program.error().position,
                        .message = std::move(program.error().message),
                }},
        };
    }
    return ValidationResult{
            .valid = true,
            .summary =
                    {
                            .compiled_program_count = 1,
                            .script_bytes = payload.size(),
                            .cidr_directive_count = program->cidr_directives.size(),
                    },
    };
}

bool generated(Generator::Result result) noexcept { return result == Generator::Result::OK; }

bool write_key(Generator &generator, std::string_view key) {
    return generated(generator.string(key.data(), key.size()));
}

bool write_string(Generator &generator, std::string_view value) {
    return generated(generator.string(value.data(), value.size()));
}

bool write_size(Generator &generator, std::string_view key, std::size_t value) {
    return write_key(generator, key) && generated(generator.integer(static_cast<std::int64_t>(value)));
}

std::string encode_config_limits() {
    std::string output;
    json::StringSink sink(output);
    Generator generator(sink);
    const ProjectListLimits &project_list = kAccessConfigLimits.project_list;
    const ProjectRouteLimits &project_route = kAccessConfigLimits.project_route;
    const GrayRuleLimits &gray_rules = kAccessConfigLimits.gray_rules;
    const EdgeFunctionLimits &edge_functions = kAccessConfigLimits.edge_functions;

    bool ok = generated(generator.map_open()) &&
              write_size(generator, "schemaVersion", kAccessConfigLimits.schema_version) &&
              write_key(generator, "projectList") && generated(generator.map_open()) &&
              write_size(generator, "maxPayloadBytes", project_list.max_payload_bytes) &&
              write_size(generator, "maxProjects", project_list.max_projects) &&
              write_size(generator, "maxProjectNameBytes", project_list.max_project_name_bytes) &&
              generated(generator.map_close()) && write_key(generator, "projectRoute") &&
              generated(generator.map_open()) &&
              write_size(generator, "maxPayloadBytes", project_route.max_payload_bytes) &&
              write_size(generator, "maxHosts", project_route.max_hosts) &&
              write_size(generator, "maxRoutes", project_route.max_routes) &&
              write_size(generator, "maxHostPatternBytes", project_route.max_host_pattern_bytes) &&
              write_size(generator, "maxPathBytes", project_route.max_path_bytes) &&
              write_size(generator, "maxMethodBytes", project_route.max_method_bytes) &&
              write_size(generator, "maxServiceBytes", project_route.max_service_bytes) &&
              write_size(generator, "maxClusterBytes", project_route.max_cluster_bytes) &&
              write_size(generator, "maxConditionBytes", project_route.max_condition_bytes) &&
              write_size(generator, "maxScriptBytes", project_route.max_script_bytes) &&
              write_size(generator, "maxTemplateBytes", project_route.max_template_bytes) &&
              write_size(generator, "maxHeaderEntries", project_route.max_header_entries) &&
              write_size(generator, "maxHeaderNameBytes", project_route.max_header_name_bytes) &&
              write_size(generator, "maxHeaderValueBytes", project_route.max_header_value_bytes) &&
              write_size(generator, "maxCidrsPerRoute", project_route.max_cidrs_per_route) &&
              write_size(generator, "maxCidrBytes", project_route.max_cidr_bytes) &&
              write_size(generator, "maxAddressesPerRoute", project_route.max_addresses_per_route) &&
              write_size(generator, "maxAddressBytes", project_route.max_address_bytes) &&
              write_size(generator, "maxUpstreamTlsProfiles", project_route.max_upstream_tls_profiles) &&
              write_size(generator, "maxUpstreamTlsCaPemBytes", project_route.max_upstream_tls_ca_pem_bytes) &&
              write_size(generator, "maxStaticResponseBodyBytes", project_route.max_static_response_body_bytes) &&
              write_size(generator, "maxStaticResponseBytes", project_route.max_static_response_bytes) &&
              write_size(generator, "maxPathVariables", project_route.max_path_variables) &&
              write_size(generator, "maxTemplateExpressions", project_route.max_template_expressions) &&
              write_size(generator, "maxCompiledPrograms", project_route.max_compiled_programs) &&
              write_size(generator, "maxEstimatedSnapshotBytes", project_route.max_estimated_snapshot_bytes) &&
              generated(generator.map_close()) && write_key(generator, "grayRules") &&
              generated(generator.map_open()) &&
              write_size(generator, "maxPayloadBytes", gray_rules.max_payload_bytes) &&
              write_size(generator, "maxRules", gray_rules.max_rules) &&
              write_size(generator, "maxEntryBytes", gray_rules.max_entry_bytes) &&
              write_size(generator, "maxCidrsPerRule", gray_rules.max_cidrs_per_rule) &&
              write_size(generator, "maxCidrBytes", gray_rules.max_cidr_bytes) && generated(generator.map_close()) &&
              write_key(generator, "edgeFunctions") && generated(generator.map_open()) &&
              write_size(generator, "maxPayloadBytes", edge_functions.max_payload_bytes) &&
              write_size(generator, "maxFunctions", edge_functions.max_functions) &&
              write_size(generator, "maxFunctionNameBytes", edge_functions.max_function_name_bytes) &&
              write_size(generator, "maxGlobsPerSelector", edge_functions.max_globs_per_selector) &&
              write_size(generator, "maxGlobBytes", edge_functions.max_glob_bytes) &&
              write_size(generator, "maxScriptPayloadBytes", edge_functions.max_script_payload_bytes) &&
              write_size(generator, "maxTotalScriptBytes", edge_functions.max_total_script_bytes) &&
              write_size(generator, "maxReportsPerExecution", edge_functions.max_reports_per_execution) &&
              write_size(generator, "maxCidrDirectivesPerScript", edge_functions.max_cidr_directives_per_script) &&
              write_size(generator, "maxCidrsPerDirective", edge_functions.max_cidrs_per_directive) &&
              write_size(generator, "maxCidrBytes", edge_functions.max_cidr_bytes) &&
              write_size(generator, "maxInputFieldBytes", edge_functions.max_input_field_bytes) &&
              write_size(generator, "maxBodyPhaseBytes", edge_functions.max_body_phase_bytes) &&
              generated(generator.map_close()) && generated(generator.map_close());
    return ok ? output : std::string(R"({"schemaVersion":6,"error":"encoding_failed"})");
}

std::string encode_response(const ValidationResult &result) {
    std::string output;
    json::StringSink sink(output);
    Generator generator(sink);
    generator.set_option(Generator::Option::ValidateUtf8);

    bool ok = generated(generator.map_open()) && write_key(generator, "contractVersion") &&
              generated(generator.integer(kNativeValidatorContractVersion)) && write_key(generator, "valid") &&
              generated(generator.bool_value(result.valid));
    if (ok && result.valid) {
        ok = write_key(generator, "normalized") && generated(generator.map_open());
        if (ok && result.summary.project_version) {
            ok = write_key(generator, "projectVersion") &&
                 generated(generator.integer(*result.summary.project_version));
        }
        if (ok && result.summary.host_count) {
            ok = write_key(generator, "hostCount") &&
                 generated(generator.integer(static_cast<std::int64_t>(*result.summary.host_count)));
        }
        if (ok && result.summary.route_count) {
            ok = write_key(generator, "routeCount") &&
                 generated(generator.integer(static_cast<std::int64_t>(*result.summary.route_count)));
        }
        if (ok && result.summary.gray_rule_count) {
            ok = write_key(generator, "grayRuleCount") &&
                 generated(generator.integer(static_cast<std::int64_t>(*result.summary.gray_rule_count)));
        }
        if (ok && result.summary.estimated_snapshot_bytes) {
            ok = write_size(generator, "estimatedSnapshotBytes", *result.summary.estimated_snapshot_bytes);
        }
        if (ok && result.summary.static_response_bytes) {
            ok = write_size(generator, "staticResponseBytes", *result.summary.static_response_bytes);
        }
        if (ok && result.summary.compiled_program_count) {
            ok = write_size(generator, "compiledProgramCount", *result.summary.compiled_program_count);
        }
        if (ok && result.summary.function_count) {
            ok = write_size(generator, "functionCount", *result.summary.function_count);
        }
        if (ok && result.summary.script_bytes) {
            ok = write_size(generator, "scriptBytes", *result.summary.script_bytes);
        }
        if (ok && result.summary.cidr_directive_count) {
            ok = write_size(generator, "cidrDirectiveCount", *result.summary.cidr_directive_count);
        }
        ok = ok && generated(generator.map_close());
    }
    ok = ok && write_key(generator, "errors") && generated(generator.array_open());
    for (const ValidationError &error: result.errors) {
        ok = ok && generated(generator.map_open()) && write_key(generator, "code") &&
             write_string(generator, error.code) && write_key(generator, "field") &&
             write_string(generator, error.field) && write_key(generator, "offset") &&
             generated(generator.integer(static_cast<std::int64_t>(error.offset))) && write_key(generator, "message") &&
             write_string(generator, error.message) && generated(generator.map_close());
        if (!ok) {
            break;
        }
    }
    ok = ok && generated(generator.array_close()) && generated(generator.map_close());
    if (!ok) {
        return R"({"contractVersion":1,"valid":false,"errors":[{"code":"encoding_failed","field":"response","offset":0,"message":"validator response encoding failed"}]})";
    }
    return output;
}

} // namespace

std::string process_native_validator_request(std::string_view input) {
    ValidatorRequest request;
    BufPool pool;
    if (auto error = decode_request(input, request, pool)) {
        return encode_response(ValidationResult{.errors = {std::move(*error)}});
    }

    std::string payload;
    bool base64_ok = false;
    const std::size_t decoded_bytes = util::base64_decoded_size(request.payload_base64, base64_ok);
    if (!base64_ok) {
        return encode_response(ValidationResult{
                .errors = {{
                        .code = "invalid_request",
                        .field = "payloadBase64",
                        .message = "payloadBase64 is not valid basic Base64",
                }},
        });
    }
    payload.resize(decoded_bytes);
    if (!util::base64_decode_into(request.payload_base64, reinterpret_cast<std::uint8_t *>(payload.data()))) {
        return encode_response(ValidationResult{
                .errors = {{
                        .code = "invalid_request",
                        .field = "payloadBase64",
                        .message = "payloadBase64 is not valid basic Base64",
                }},
        });
    }
    if (request.kind == "project_route") {
        return encode_response(validate_project(request.project, payload));
    }
    if (request.kind == "gray_rules") {
        return encode_response(validate_gray(payload));
    }
    if (request.kind == "edge_function_index") {
        return encode_response(validate_edge_function_index(payload));
    }
    return encode_response(validate_edge_function_script(payload, request.phase));
}

std::string native_validator_input_too_large_response() {
    return encode_response(ValidationResult{
            .errors = {{
                    .code = "input_too_large",
                    .field = "request",
                    .message = "validator request exceeds the protocol input limit",
            }},
    });
}

std::string native_validator_config_limits_response() { return encode_config_limits(); }

} // namespace fiber::access_server
