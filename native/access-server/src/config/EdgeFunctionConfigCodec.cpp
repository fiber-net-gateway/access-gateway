#include "AccessConfigCodec.h"

#include "AccessConfigErrorBuilder.h"
#include "AccessConfigJson.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fiber/common/json/JsonParser.h>
#include <fiber/common/mem/BufPool.h>

namespace fiber::access_server {
namespace {

using config_detail::AccessJsonValue;
using config_detail::DecodeFailure;
using config_detail::DecodeResult;
using config_detail::index_path;
using config_detail::invalid_field;
using config_detail::limit_exceeded;
using config_detail::make_error;
using config_detail::out_of_range;
using config_detail::parse_access_json;

constexpr std::int32_t kMaxPriority = 1'000'000;

std::string child(std::string_view parent, std::string_view key) {
    std::string path(parent);
    path.push_back('.');
    path.append(key);
    return path;
}

DecodeFailure config_error(AccessConfigErrorCode code, std::string field, std::string message) {
    return std::unexpected(make_error(code, std::move(field), std::move(message)));
}

DecodeFailure unknown_key(const std::string &field) { return invalid_field(field, "unknown field"); }

bool is_lower_alnum(char value) noexcept { return (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9'); }

// Function names are slug identities: they travel into Data IDs, metric
// labels, event logs, and evidence. Restrict to [a-z0-9-], 1..64 bytes,
// edges alphanumeric. Display names live in the console DB, not the wire.
bool valid_function_name(std::string_view name) noexcept {
    if (name.empty() || name.size() > 64 || !is_lower_alnum(name.front()) || !is_lower_alnum(name.back())) {
        return false;
    }
    for (const char value: name) {
        if (!is_lower_alnum(value) && value != '-') {
            return false;
        }
    }
    return true;
}

bool valid_sha256_hex(std::string_view value) noexcept {
    if (value.size() != 64) {
        return false;
    }
    for (const char digit: value) {
        const bool ok = (digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f');
        if (!ok) {
            return false;
        }
    }
    return true;
}

// Glob bytes: printable, no ';'. Project names cannot contain ';' (the
// projects-list wire splits on it); high bytes pass through for UTF-8 names.
bool valid_glob(std::string_view glob) noexcept {
    if (glob.empty()) {
        return false;
    }
    for (const unsigned char value: glob) {
        if (value < 0x20U || value == 0x7FU || value == ';') {
            return false;
        }
    }
    return true;
}

DecodeResult<std::int32_t> strict_int32(const AccessJsonValue &value, const std::string &field) {
    if (!value.is_integer()) {
        return std::unexpected(invalid_field(field, "expected an integer"));
    }
    const std::int64_t parsed = value.as_integer();
    if (parsed < INT32_MIN || parsed > INT32_MAX) {
        return std::unexpected(out_of_range(field, "integer out of range"));
    }
    return static_cast<std::int32_t>(parsed);
}

DecodeResult<std::vector<std::string>> glob_list(const AccessJsonValue &value, const std::string &field,
                                                 const EdgeFunctionLimits &limits) {
    if (!value.is_array()) {
        return std::unexpected(invalid_field(field, "expected an array of project name globs"));
    }
    const auto &array = value.as_array();
    if (array.size() > limits.max_globs_per_selector) {
        return std::unexpected(limit_exceeded(field, "glob count exceeds the configured limit"));
    }
    std::vector<std::string> result;
    result.reserve(array.size());
    for (std::size_t index = 0; index < array.size(); ++index) {
        const std::string path = index_path(field, index);
        const AccessJsonValue &item = array[index];
        if (!item.is_text()) {
            return std::unexpected(invalid_field(path, "expected a project name glob string"));
        }
        const std::string_view glob = item.as_text();
        if (!valid_glob(glob)) {
            return std::unexpected(invalid_field(path, "glob must be non-empty printable text without ';'"));
        }
        if (glob.size() > limits.max_glob_bytes) {
            return std::unexpected(limit_exceeded(path, "glob bytes exceed the configured limit"));
        }
        result.emplace_back(glob);
    }
    return result;
}

DecodeResult<EdgeFunctionProjectsSelector> projects_selector(const AccessJsonValue &value, const std::string &field,
                                                             const EdgeFunctionLimits &limits) {
    if (!value.is_object()) {
        return std::unexpected(invalid_field(field, "expected a projects selector"));
    }
    EdgeFunctionProjectsSelector projects;
    for (const auto &selector: value.as_object()) {
        const std::string selector_path = child(field, selector.key);
        if (selector.key == "include") {
            auto decoded = glob_list(selector.value, selector_path, limits);
            if (!decoded) {
                return std::unexpected(std::move(decoded.error()));
            }
            projects.include = std::move(*decoded);
        } else if (selector.key == "exclude") {
            auto decoded = glob_list(selector.value, selector_path, limits);
            if (!decoded) {
                return std::unexpected(std::move(decoded.error()));
            }
            projects.exclude = std::move(*decoded);
        } else {
            return unknown_key(selector_path);
        }
    }
    if (projects.include.empty()) {
        return std::unexpected(invalid_field(field, "selector must declare at least one include glob"));
    }
    return projects;
}

DecodeResult<EdgeFunctionIndexEntry> index_entry(const AccessJsonValue &value, const std::string &field,
                                                 const EdgeFunctionLimits &limits) {
    if (!value.is_object()) {
        return std::unexpected(invalid_field(field, "expected a function object"));
    }
    EdgeFunctionIndexEntry entry;
    bool have_projects = false;
    for (const auto &property: value.as_object()) {
        const std::string path = child(field, property.key);
        if (property.key == "name") {
            if (!property.value.is_text() || !valid_function_name(property.value.as_text())) {
                return std::unexpected(
                        invalid_field(path, "function name must be 1-64 bytes of [a-z0-9-] with alphanumeric edges"));
            }
            if (property.value.as_text().size() > limits.max_function_name_bytes) {
                return std::unexpected(limit_exceeded(path, "function name bytes exceed the configured limit"));
            }
            entry.name.assign(property.value.as_text());
        } else if (property.key == "sha256") {
            if (!property.value.is_text() || !valid_sha256_hex(property.value.as_text())) {
                return std::unexpected(invalid_field(path, "expected 64 lowercase hex sha256 characters"));
            }
            entry.sha256_hex.assign(property.value.as_text());
        } else if (property.key == "phase") {
            if (!property.value.is_text()) {
                return std::unexpected(invalid_field(path, "expected a phase string"));
            }
            const std::string_view phase = property.value.as_text();
            if (phase == "request_head") {
                entry.phase = EdgeFunctionPhase::RequestHead;
            } else if (phase == "request_body") {
                entry.phase = EdgeFunctionPhase::RequestBody;
            } else {
                return std::unexpected(invalid_field(path, "phase must be request_head or request_body"));
            }
        } else if (property.key == "projects") {
            have_projects = true;
            auto decoded = projects_selector(property.value, path, limits);
            if (!decoded) {
                return std::unexpected(std::move(decoded.error()));
            }
            entry.projects = std::move(*decoded);
        } else if (property.key == "priority") {
            auto decoded = strict_int32(property.value, path);
            if (!decoded) {
                return std::unexpected(std::move(decoded.error()));
            }
            if (*decoded < 0 || *decoded > kMaxPriority) {
                return std::unexpected(out_of_range(path, "priority must be within [0, 1000000]"));
            }
            entry.priority = *decoded;
        } else if (property.key == "ratio") {
            auto decoded = strict_int32(property.value, path);
            if (!decoded) {
                return std::unexpected(std::move(decoded.error()));
            }
            if (*decoded < 0 || *decoded > kEdgeFunctionFullRatio) {
                return std::unexpected(out_of_range(path, "ratio must be within [0, 10000]"));
            }
            entry.ratio = *decoded;
        } else if (property.key == "replaces") {
            if (!property.value.is_text() || !valid_function_name(property.value.as_text())) {
                return std::unexpected(invalid_field(path, "replaces must name a function slug"));
            }
            entry.replaces.assign(property.value.as_text());
        } else {
            return unknown_key(path);
        }
    }
    if (entry.name.empty()) {
        return std::unexpected(invalid_field(child(field, "name"), "function is missing the name field"));
    }
    if (entry.sha256_hex.empty()) {
        return std::unexpected(invalid_field(child(field, "sha256"), "function is missing the sha256 field"));
    }
    if (!have_projects) {
        return std::unexpected(invalid_field(child(field, "projects"), "function is missing the projects selector"));
    }
    return entry;
}

// `replaces` relationships need the whole index, so they run after every entry
// decoded. The rules keep one project's bucket windows disjoint: a chain or a
// second canary of one target would leave two functions claiming a bucket. The
// index is capped at max_functions, so the quadratic scan stays small.
DecodeResult<void> validate_replaces(const EdgeFunctionIndexConfig &config) {
    const auto &functions = config.functions;
    for (std::size_t index = 0; index < functions.size(); ++index) {
        const EdgeFunctionIndexEntry &function = functions[index];
        if (function.replaces.empty()) {
            continue;
        }
        const std::string field = child(index_path("functions", index), "replaces");
        if (function.replaces == function.name) {
            return std::unexpected(invalid_field(field, "function cannot replace itself"));
        }
        const auto target = std::ranges::find(functions, function.replaces, &EdgeFunctionIndexEntry::name);
        if (target == functions.end()) {
            return std::unexpected(invalid_field(field, "replaces target is not in the index"));
        }
        if (target->phase != function.phase) {
            return config_error(AccessConfigErrorCode::Conflict, field, "replaces target runs in a different phase");
        }
        if (!target->replaces.empty()) {
            return config_error(AccessConfigErrorCode::Conflict, field, "replaces target must not replace another");
        }
        for (std::size_t other = 0; other < index; ++other) {
            if (functions[other].replaces == function.replaces) {
                return config_error(AccessConfigErrorCode::Conflict, field,
                                    "replaces target is already replaced by " + functions[other].name);
            }
        }
    }
    return {};
}

} // namespace

EdgeFunctionIndexResult parse_edge_function_index_config(std::string_view content, const AccessConfigLimits &limits) {
    const EdgeFunctionLimits &edge_limits = limits.edge_functions;
    if (content.empty()) {
        return std::optional<EdgeFunctionIndexConfig>{};
    }
    if (content.size() > edge_limits.max_payload_bytes) {
        return config_error(AccessConfigErrorCode::LimitExceeded, "payload",
                            "edge-function index payload exceeds the configured byte limit");
    }

    mem::BufPool pool;
    json::JsonParser parser;
    auto root = parse_access_json(content, pool, parser);
    if (!root) {
        return std::unexpected(std::move(root.error()));
    }
    if (root->is_null()) {
        return std::optional<EdgeFunctionIndexConfig>(EdgeFunctionIndexConfig{});
    }
    if (!root->is_object()) {
        return config_error(AccessConfigErrorCode::InvalidRoot, {}, "edge-function index must be an object or null");
    }

    EdgeFunctionIndexConfig config;
    bool have_version = false;
    for (const auto &property: root->as_object()) {
        if (property.key == "version") {
            have_version = true;
            auto decoded = strict_int32(property.value, "version");
            if (!decoded) {
                return std::unexpected(std::move(decoded.error()));
            }
            if (*decoded < 0) {
                return std::unexpected(out_of_range("version", "version must be non-negative"));
            }
            config.version = *decoded;
        } else if (property.key == "functions") {
            if (!property.value.is_array()) {
                return std::unexpected(invalid_field("functions", "expected an array of functions"));
            }
            if (property.value.as_array().size() > edge_limits.max_functions) {
                return std::unexpected(limit_exceeded("functions", "function count exceeds the configured limit"));
            }
            const auto &array = property.value.as_array();
            config.functions.reserve(array.size());
            for (std::size_t index = 0; index < array.size(); ++index) {
                auto decoded = index_entry(array[index], index_path("functions", index), edge_limits);
                if (!decoded) {
                    return std::unexpected(std::move(decoded.error()));
                }
                for (const EdgeFunctionIndexEntry &existing: config.functions) {
                    if (existing.name == decoded->name) {
                        return config_error(AccessConfigErrorCode::Conflict, index_path("functions", index),
                                            "duplicate function name: " + decoded->name);
                    }
                }
                config.functions.push_back(std::move(*decoded));
            }
        } else {
            return unknown_key(child("index", property.key));
        }
    }
    if (!have_version) {
        return std::unexpected(invalid_field("version", "index is missing the version field"));
    }
    if (auto within_limits = validate_edge_function_index_limits(config, limits); !within_limits) {
        return std::unexpected(std::move(within_limits.error()));
    }
    if (auto valid = validate_replaces(config); !valid) {
        return std::unexpected(std::move(valid.error()));
    }
    return std::optional<EdgeFunctionIndexConfig>(std::move(config));
}

} // namespace fiber::access_server
