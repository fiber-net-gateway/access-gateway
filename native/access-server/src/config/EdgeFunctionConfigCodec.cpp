#include "AccessConfigCodec.h"

#include "AccessConfigErrorBuilder.h"
#include "AccessConfigJson.h"

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

// Function ids travel into Data IDs and metric labels: restrict to
// [a-z0-9-], 1..64 bytes, edges must be alphanumeric.
bool valid_function_id(std::string_view id) noexcept {
    if (id.empty() || id.size() > 64 || !is_lower_alnum(id.front()) || !is_lower_alnum(id.back())) {
        return false;
    }
    for (const char value: id) {
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

DecodeResult<EdgeFunctionMountConfig> mount_config(const AccessJsonValue &value, const std::string &field,
                                                   const EdgeFunctionLimits &limits) {
    if (!value.is_object()) {
        return std::unexpected(invalid_field(field, "expected a mount object"));
    }
    EdgeFunctionMountConfig mount;
    bool have_projects = false;
    for (const auto &property: value.as_object()) {
        if (property.key == "projects") {
            have_projects = true;
            const std::string projects_field = child(field, "projects");
            if (!property.value.is_object()) {
                return std::unexpected(invalid_field(projects_field, "expected a projects selector"));
            }
            for (const auto &selector: property.value.as_object()) {
                const std::string selector_path = child(projects_field, selector.key);
                if (selector.key == "include") {
                    auto decoded = glob_list(selector.value, selector_path, limits);
                    if (!decoded) {
                        return std::unexpected(std::move(decoded.error()));
                    }
                    mount.projects.include = std::move(*decoded);
                } else if (selector.key == "exclude") {
                    auto decoded = glob_list(selector.value, selector_path, limits);
                    if (!decoded) {
                        return std::unexpected(std::move(decoded.error()));
                    }
                    mount.projects.exclude = std::move(*decoded);
                } else {
                    return unknown_key(selector_path);
                }
            }
            if (mount.projects.include.empty()) {
                return std::unexpected(
                        invalid_field(projects_field, "selector must declare at least one include glob"));
            }
        } else if (property.key == "priority") {
            const std::string priority_field = child(field, "priority");
            auto decoded = strict_int32(property.value, priority_field);
            if (!decoded) {
                return std::unexpected(std::move(decoded.error()));
            }
            if (*decoded < 0 || *decoded > kMaxPriority) {
                return std::unexpected(out_of_range(priority_field, "priority must be within [0, 1000000]"));
            }
            mount.priority = *decoded;
        } else {
            return unknown_key(child(field, property.key));
        }
    }
    if (!have_projects) {
        return std::unexpected(invalid_field(field, "mount is missing the projects selector"));
    }
    return mount;
}

DecodeResult<EdgeFunctionIndexEntry> index_entry(const AccessJsonValue &value, const std::string &field,
                                                 const EdgeFunctionLimits &limits) {
    if (!value.is_object()) {
        return std::unexpected(invalid_field(field, "expected a function object"));
    }
    EdgeFunctionIndexEntry entry;
    bool have_mounts = false;
    for (const auto &property: value.as_object()) {
        const std::string path = child(field, property.key);
        if (property.key == "id") {
            if (!property.value.is_text() || !valid_function_id(property.value.as_text())) {
                return std::unexpected(
                        invalid_field(path, "function id must be 1-64 bytes of [a-z0-9-] with alphanumeric edges"));
            }
            entry.id.assign(property.value.as_text());
        } else if (property.key == "name") {
            if (!property.value.is_null() && !property.value.is_text()) {
                return std::unexpected(invalid_field(path, "expected a display name string or null"));
            }
            if (property.value.is_text()) {
                const std::string_view name = property.value.as_text();
                if (name.size() > limits.max_function_name_bytes) {
                    return std::unexpected(limit_exceeded(path, "function name bytes exceed the configured limit"));
                }
                entry.name.emplace(name);
            }
        } else if (property.key == "dataId") {
            if (!property.value.is_text()) {
                return std::unexpected(invalid_field(path, "expected the script Data ID string"));
            }
            entry.data_id.assign(property.value.as_text());
        } else if (property.key == "sha256") {
            if (!property.value.is_text() || !valid_sha256_hex(property.value.as_text())) {
                return std::unexpected(invalid_field(path, "expected 64 lowercase hex sha256 characters"));
            }
            entry.sha256_hex.assign(property.value.as_text());
        } else if (property.key == "mounts") {
            have_mounts = true;
            if (!property.value.is_array() || property.value.as_array().empty()) {
                return std::unexpected(invalid_field(path, "expected a non-empty mounts array"));
            }
            if (property.value.as_array().size() > limits.max_mounts_per_function) {
                return std::unexpected(limit_exceeded(path, "mount count exceeds the configured limit"));
            }
            const auto &array = property.value.as_array();
            entry.mounts.reserve(array.size());
            for (std::size_t index = 0; index < array.size(); ++index) {
                auto decoded = mount_config(array[index], index_path(path, index), limits);
                if (!decoded) {
                    return std::unexpected(std::move(decoded.error()));
                }
                entry.mounts.push_back(std::move(*decoded));
            }
        } else {
            return unknown_key(path);
        }
    }
    if (entry.id.empty()) {
        return std::unexpected(invalid_field(child(field, "id"), "function is missing the id field"));
    }
    if (entry.data_id.empty()) {
        return std::unexpected(invalid_field(child(field, "dataId"), "function is missing the dataId field"));
    }
    if (entry.sha256_hex.empty()) {
        return std::unexpected(invalid_field(child(field, "sha256"), "function is missing the sha256 field"));
    }
    if (!have_mounts) {
        return std::unexpected(invalid_field(child(field, "mounts"), "function is missing the mounts field"));
    }
    const std::string expected_data_id = std::string(kEdgeFunctionDataIdPrefix) + entry.id;
    if (entry.data_id != expected_data_id) {
        return config_error(AccessConfigErrorCode::Conflict, child(field, "dataId"),
                            "dataId must equal " + expected_data_id);
    }
    return entry;
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
                    if (existing.id == decoded->id) {
                        return config_error(AccessConfigErrorCode::Conflict, index_path("functions", index),
                                            "duplicate function id: " + decoded->id);
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
    return std::optional<EdgeFunctionIndexConfig>(std::move(config));
}

} // namespace fiber::access_server
