#include "ProjectConfigCompiler.h"
#include "GzipEncoder.h"

#include <fiber/common/util/Base64.h>

#include <bit>
#include <charconv>
#include <limits>
#include <unordered_set>
#include <utility>

namespace fiber::access_server {
namespace {

constexpr std::string_view kDefaultServiceCluster = "default";
constexpr std::string_view kCallSourceSuffix = ".unifiedAccess";

std::string route_path(std::size_t route_index, std::string_view field) {
    std::string path = "routes[";
    path.append(std::to_string(route_index));
    path.push_back(']');
    if (!field.empty()) {
        path.push_back('.');
        path.append(field);
    }
    return path;
}

AccessConfigError route_error(AccessConfigErrorCode code, std::size_t route_index, std::string_view field,
                              std::string_view message) {
    return AccessConfigError{
            .code = code,
            .field = route_path(route_index, field),
            .message = std::string(message),
    };
}

AccessConfigError selector_factory_error(std::size_t route_index, ProxyAddressSelectorFactory::Error error) {
    return AccessConfigError{
            .code = AccessConfigErrorCode::InvalidCombination,
            .field = error.field.empty() ? route_path(route_index, "service") : std::move(error.field),
            .message = std::move(error.message),
    };
}

AccessConfigError project_error(std::string_view field, std::string_view message) {
    return AccessConfigError{
            .code = AccessConfigErrorCode::InvalidCombination,
            .field = std::string(field),
            .message = std::string(message),
    };
}

bool is_nonempty(const std::optional<std::string> &value) noexcept { return value && !value->empty(); }

bool has_non_whitespace(std::string_view value) noexcept {
    for (const unsigned char ch: value) {
        if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n' && ch != '\f' && ch != '\v') {
            return true;
        }
    }
    return false;
}

class ProjectCompileBudget {
public:
    explicit ProjectCompileBudget(const ProjectRouteLimits &limits) noexcept : limits_(&limits) {}

    [[nodiscard]] std::expected<void, AccessConfigError> account_config(std::string_view project,
                                                                        const ProjectConfig &config) {
        auto base = add_bytes(sizeof(ProjectRouteSnapshot) + project.size() * 2U + kCallSourceSuffix.size(), "project");
        if (!base) {
            return base;
        }
        if (config.hosts) {
            auto hosts = add_scaled(config.hosts->size(), sizeof(CompiledHost) + sizeof(HostPattern) + 64U, "host");
            if (!hosts) {
                return hosts;
            }
            for (std::size_t index = 0; index < config.hosts->size(); ++index) {
                auto pattern = add_scaled((*config.hosts)[index].pattern.size(), 2U,
                                          "host[" + std::to_string(index) + "].pattern");
                if (!pattern) {
                    return pattern;
                }
            }
        }
        if (!config.routes) {
            return {};
        }
        auto routes = add_scaled(config.routes->size(), sizeof(CompiledRoute) + 512U, "routes");
        if (!routes) {
            return routes;
        }
        for (std::size_t index = 0; index < config.routes->size(); ++index) {
            if (!(*config.routes)[index]) {
                continue;
            }
            const RouteConfig &route = *(*config.routes)[index];
            const auto add_optional = [&](const std::optional<std::string> &value, std::string_view field,
                                          std::size_t multiplier = 2U) {
                return value ? add_scaled(value->size(), multiplier, route_path(index, field))
                             : std::expected<void, AccessConfigError>{};
            };
            for (auto result: {add_optional(route.path, "path", 3U), add_optional(route.method, "method"),
                               add_optional(route.service, "service"), add_optional(route.cluster, "cluster"),
                               add_optional(route.condition, "condition", 4U),
                               add_optional(route.rewrite, "rewrite", 3U), add_optional(route.script, "script", 4U)}) {
                if (!result) {
                    return result;
                }
            }
            auto addresses = add_scaled(route.addresses.size(), sizeof(AccessUpstreamInstance) + 64U,
                                        route_path(index, "addresses"));
            if (!addresses) {
                return addresses;
            }
            for (const std::optional<std::string> &address: route.addresses) {
                if (address) {
                    auto result = add_scaled(address->size(), 2U, route_path(index, "addresses"));
                    if (!result) {
                        return result;
                    }
                }
            }
            auto cidrs = add_scaled(route.allows.size(), sizeof(Cidr) + 32U, route_path(index, "allows"));
            if (!cidrs) {
                return cidrs;
            }
            for (const auto &[entries, field]:
                 {std::pair<const StringConfigMap *, std::string_view>{&route.proxy_headers, "proxy_headers"},
                  {&route.response_headers, "response_headers"},
                  {&route.context, "context"}}) {
                auto structures =
                        add_scaled(entries->size(), sizeof(CompiledTemplateEntry) + 128U, route_path(index, field));
                if (!structures) {
                    return structures;
                }
                for (const StringConfigEntry &entry: *entries) {
                    auto name = add_scaled(entry.name.size(), 2U, route_path(index, field));
                    if (!name) {
                        return name;
                    }
                    if (entry.value) {
                        auto value = add_scaled(entry.value->size(), 3U, route_path(index, field));
                        if (!value) {
                            return value;
                        }
                    }
                }
            }
            if (route.body && route.body->content) {
                auto body = add_scaled(route.body->content->size(), 2U, route_path(index, "body.content"));
                if (!body) {
                    return body;
                }
            }
            if (route.upstream_tls) {
                auto profile =
                        add_scaled(1U, sizeof(UpstreamTlsTransportProfile) + 128U, route_path(index, "upstream_tls"));
                if (!profile) {
                    return profile;
                }
                for (auto result:
                     {add_optional(route.upstream_tls->ca_pem, "upstream_tls.ca_pem", 1U),
                      add_optional(route.upstream_tls->server_name, "upstream_tls.server_name"),
                      add_optional(route.upstream_tls->verify_name, "upstream_tls.verify_name"),
                      add_optional(route.upstream_tls->client_identity_ref, "upstream_tls.client_identity_ref")}) {
                    if (!result) {
                        return result;
                    }
                }
            }
        }
        return {};
    }

    [[nodiscard]] std::expected<void, AccessConfigError>
    account_template(const CompiledTemplate &value, std::size_t route_index, std::string_view field) {
        auto programs = reserve_programs(value.expressions.size(), route_index, field);
        if (!programs) {
            return programs;
        }
        return add_scaled(value.expressions.size(), sizeof(CompiledTemplateExpression), route_path(route_index, field));
    }

    [[nodiscard]] std::expected<void, AccessConfigError> reserve_programs(std::size_t count, std::size_t route_index,
                                                                          std::string_view field) {
        if (count > limits_->max_compiled_programs - compiled_programs_) {
            return std::unexpected(route_error(AccessConfigErrorCode::LimitExceeded, route_index, field,
                                               "compiled program count exceeds the configured limit"));
        }
        compiled_programs_ += count;
        return add_scaled(count, 1024U, route_path(route_index, field));
    }

    [[nodiscard]] std::expected<void, AccessConfigError> add_static_response(std::size_t bytes,
                                                                             std::size_t route_index) {
        if (bytes > limits_->max_static_response_bytes - static_response_bytes_) {
            return std::unexpected(route_error(AccessConfigErrorCode::LimitExceeded, route_index, "body",
                                               "project static response bytes exceed the configured limit"));
        }
        static_response_bytes_ += bytes;
        return add_bytes(bytes, route_path(route_index, "body"));
    }

    [[nodiscard]] std::size_t estimated_bytes() const noexcept { return estimated_bytes_; }
    [[nodiscard]] std::size_t static_response_bytes() const noexcept { return static_response_bytes_; }
    [[nodiscard]] std::size_t compiled_programs() const noexcept { return compiled_programs_; }
    [[nodiscard]] const ProjectRouteLimits &limits() const noexcept { return *limits_; }

private:
    [[nodiscard]] std::expected<void, AccessConfigError> add_scaled(std::size_t count, std::size_t bytes,
                                                                    std::string field) {
        if (count != 0 && bytes > limits_->max_estimated_snapshot_bytes / count) {
            return memory_error(std::move(field));
        }
        return add_bytes(count * bytes, std::move(field));
    }

    [[nodiscard]] std::expected<void, AccessConfigError> add_bytes(std::size_t bytes, std::string field) {
        if (bytes > limits_->max_estimated_snapshot_bytes - estimated_bytes_) {
            return memory_error(std::move(field));
        }
        estimated_bytes_ += bytes;
        return {};
    }

    [[nodiscard]] std::expected<void, AccessConfigError> memory_error(std::string field) const {
        return std::unexpected(AccessConfigError{
                .code = AccessConfigErrorCode::LimitExceeded,
                .field = std::move(field),
                .message = "estimated compiled snapshot bytes exceed the configured limit",
        });
    }

    const ProjectRouteLimits *limits_;
    std::size_t estimated_bytes_ = 0;
    std::size_t static_response_bytes_ = 0;
    std::size_t compiled_programs_ = 0;
};

std::expected<CompiledTemplate, AccessConfigError> compile_template(std::string_view source, std::size_t route_index,
                                                                    std::string_view field,
                                                                    ProjectCompileBudget &budget) {
    auto parsed = parse_template(source, budget.limits().max_template_expressions);
    if (!parsed) {
        const AccessConfigErrorCode code = parsed.error() == TemplateParseError::TooManyExpressions
                                                   ? AccessConfigErrorCode::LimitExceeded
                                                   : AccessConfigErrorCode::InvalidField;
        const std::string_view message = parsed.error() == TemplateParseError::TooManyExpressions
                                                 ? "template expression count exceeds the configured limit"
                                                 : "invalid template";
        return std::unexpected(route_error(code, route_index, field, message));
    }
    auto accounted = budget.account_template(*parsed, route_index, field);
    if (!accounted) {
        return std::unexpected(std::move(accounted.error()));
    }
    return std::move(*parsed);
}

std::int32_t java_int32_narrow(std::int64_t value) noexcept {
    const auto bits = static_cast<std::uint32_t>(static_cast<std::uint64_t>(value) & 0xFFFF'FFFFULL);
    return std::bit_cast<std::int32_t>(bits);
}

std::string conditional_route_key(std::string_view path, std::string_view condition) {
    std::uint32_t crc = std::numeric_limits<std::uint32_t>::max();
    for (const unsigned char byte: condition) {
        crc ^= byte;
        for (unsigned i = 0; i < 8; ++i) {
            const std::uint32_t low_bit_mask = 0U - (crc & 1U);
            crc = (crc >> 1U) ^ (0x82F63B78U & low_bit_mask);
        }
    }
    crc = ~crc;

    constexpr char kHex[] = "0123456789abcdef";
    std::string key;
    key.reserve(path.size() + 9);
    key.append(path);
    key.push_back('@');
    for (unsigned i = 0; i < 8; ++i) {
        key.push_back(kHex[(crc >> (i * 4U)) & 0xFU]);
    }
    return key;
}

std::optional<std::string_view> validate_path_pattern(std::string_view path) noexcept {
    if (path.empty()) {
        return "path is empty";
    }

    std::size_t segment_start = 0;
    bool wildcard = false;
    for (std::size_t i = 0; i <= path.size(); ++i) {
        const auto ch = i < path.size() ? static_cast<unsigned char>(path[i]) : 0U;
        if ((ch & 0x80U) != 0) {
            return "path must use ASCII bytes";
        }
        if (ch == 0 || ch == '/') {
            if (wildcard && ch != 0) {
                return "wildcard segment must be the last path segment";
            }
            segment_start = i + 1;
            wildcard = false;
        } else if (i == segment_start && ch == '*') {
            wildcard = true;
        }
    }
    return std::nullopt;
}

std::expected<std::vector<CompiledTemplateEntry>, AccessConfigError> compile_templates(const StringConfigMap &input,
                                                                                       std::size_t route_index,
                                                                                       std::string_view field,
                                                                                       ProjectCompileBudget &budget) {
    std::vector<CompiledTemplateEntry> result;
    result.reserve(input.size());
    for (const StringConfigEntry &entry: input) {
        if (!entry.value) {
            return std::unexpected(
                    route_error(AccessConfigErrorCode::InvalidField, route_index, field, "invalid template"));
        }
        auto value = compile_template(*entry.value, route_index, field, budget);
        if (!value) {
            return std::unexpected(std::move(value.error()));
        }
        result.push_back(CompiledTemplateEntry{
                .name = entry.name,
                .value = std::move(*value),
        });
    }
    return result;
}

std::expected<std::vector<CompiledResponseHeaderTemplate>, AccessConfigError>
compile_response_header_templates(const StringConfigMap &input, std::size_t route_index, std::string_view field,
                                  ProjectCompileBudget &budget) {
    std::vector<CompiledResponseHeaderTemplate> result;
    result.reserve(input.size());
    for (const StringConfigEntry &entry: input) {
        if (!entry.value) {
            return std::unexpected(
                    route_error(AccessConfigErrorCode::InvalidField, route_index, field, "invalid template"));
        }
        auto value = compile_template(*entry.value, route_index, field, budget);
        if (!value) {
            return std::unexpected(std::move(value.error()));
        }
        result.emplace_back(entry.name, std::move(*value));
    }
    return result;
}

std::expected<CompiledHeaderTemplates::Builder, AccessConfigError>
compile_header_templates(const StringConfigMap &input, std::size_t route_index, std::string_view field,
                         ProjectCompileBudget &budget) {
    auto entries = compile_templates(input, route_index, field, budget);
    if (!entries) {
        return std::unexpected(std::move(entries.error()));
    }

    CompiledHeaderTemplates::Builder result(entries->size());
    for (CompiledTemplateEntry &entry: *entries) {
        auto inserted = result.insert(std::move(entry.name), std::move(entry.value));
        if (!inserted) {
            if (inserted.error() == CompiledHeaderTemplates::InsertError::DuplicateName) {
                return std::unexpected(route_error(AccessConfigErrorCode::Conflict, route_index, field,
                                                   "header name is duplicate ignoring ASCII case"));
            }
            return std::unexpected(route_error(AccessConfigErrorCode::OutOfRange, route_index, field,
                                               "header collection is too large"));
        }
    }
    return result;
}

struct PendingRouteCompile {
    std::optional<std::string> condition;
    std::optional<std::string> script;
    bool has_predicate = false;
    CompiledHeaderTemplates::Builder proxy_headers;
    CompiledHeaderTemplates::Builder response_headers;
};

std::expected<void, AccessConfigError> compile_route_scripts(CompiledRoute &route, std::size_t route_index,
                                                             PendingRouteCompile &pending,
                                                             ScriptCompilerAdapter compiler,
                                                             http_script::ConstPackage::Builder &constants,
                                                             ProjectCompileBudget &budget) {
    auto compile_template = [&](CompiledTemplate &value,
                                std::string_view field) -> std::expected<void, AccessConfigError> {
        for (CompiledTemplateExpression &expression: value.expressions) {
            if (!compiler.compile_expression) {
                return std::unexpected(route_error(AccessConfigErrorCode::InvalidField, route_index, field,
                                                   "script compiler is not configured"));
            }
            auto program = compiler.compile_expression(compiler.context, constants, expression.source,
                                                       route.path_variable_names);
            if (!program) {
                return std::unexpected(
                        route_error(AccessConfigErrorCode::InvalidField, route_index, field, program.error()));
            }
            if (!program->valid()) {
                return std::unexpected(route_error(AccessConfigErrorCode::InvalidField, route_index, field,
                                                   "script compiler returned an invalid program"));
            }
            expression.program = std::move(*program);
        }
        return {};
    };
    auto compile_entries = [&]<typename Entry>(std::span<Entry> entries,
                                               std::string_view field) -> std::expected<void, AccessConfigError> {
        for (Entry &entry: entries) {
            auto compiled = compile_template(entry.value, field);
            if (!compiled) {
                return compiled;
            }
        }
        return {};
    };

    if (pending.condition) {
        auto reserved = budget.reserve_programs(1, route_index, "condition");
        if (!reserved) {
            return reserved;
        }
        if (!compiler.compile_expression) {
            return std::unexpected(route_error(AccessConfigErrorCode::InvalidField, route_index, "condition",
                                               "script compiler is not configured"));
        }
        auto program =
                compiler.compile_expression(compiler.context, constants, *pending.condition, route.path_variable_names);
        if (!program) {
            return std::unexpected(
                    route_error(AccessConfigErrorCode::InvalidField, route_index, "condition", program.error()));
        }
        if (!program->valid()) {
            return std::unexpected(route_error(AccessConfigErrorCode::InvalidField, route_index, "condition",
                                               "script compiler returned an invalid program"));
        }
        route.condition_program.emplace(std::move(*program));
    }

    if (pending.script) {
        auto reserved = budget.reserve_programs(1, route_index, "script");
        if (!reserved) {
            return reserved;
        }
        if (!compiler.compile_route_script) {
            return std::unexpected(route_error(AccessConfigErrorCode::InvalidField, route_index, "script",
                                               "route script compiler is not configured"));
        }
        auto program =
                compiler.compile_route_script(compiler.context, constants, *pending.script, route.path_variable_names);
        if (!program) {
            return std::unexpected(
                    route_error(AccessConfigErrorCode::InvalidField, route_index, "script", program.error()));
        }
        if (!program->valid()) {
            return std::unexpected(route_error(AccessConfigErrorCode::InvalidField, route_index, "script",
                                               "script compiler returned an invalid program"));
        }
        route.script_program = std::make_shared<script::Script>(std::move(*program));
        return {};
    }

    if (route.response) {
        if (route.response->body_kind == ResponseBodyKind::Template) {
            if (!route.response->body_template) {
                return std::unexpected(route_error(AccessConfigErrorCode::InvalidField, route_index, "body",
                                                   "invalid compiled template"));
            }
            auto compiled = compile_template(*route.response->body_template, "body");
            if (!compiled) {
                return compiled;
            }
        }
        return compile_entries(std::span(route.response->response_headers), "response_headers");
    }

    auto proxy_headers = compile_entries(pending.proxy_headers.entries(), "proxy_headers");
    if (!proxy_headers) {
        return proxy_headers;
    }
    auto response_headers = compile_entries(pending.response_headers.entries(), "response_headers");
    if (!response_headers) {
        return response_headers;
    }
    auto context = compile_entries(std::span(route.proxy->context), "context");
    if (!context) {
        return context;
    }
    if (route.proxy->rewrite) {
        return compile_template(*route.proxy->rewrite, "rewrite");
    }
    return {};
}

bool is_http_token(std::string_view value) noexcept {
    if (value.empty() || value.size() > 64) {
        return false;
    }
    for (const unsigned char ch: value) {
        const bool alpha_numeric = (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
        const bool punctuation = ch == '!' || ch == '#' || ch == '$' || ch == '%' || ch == '&' || ch == '\'' ||
                                 ch == '*' || ch == '+' || ch == '-' || ch == '.' || ch == '^' || ch == '_' ||
                                 ch == '`' || ch == '|' || ch == '~';
        if (!alpha_numeric && !punctuation) {
            return false;
        }
    }
    return true;
}

bool has_script_only_field_conflict(const RouteConfig &source) noexcept {
    return source.service.has_value() || source.cluster.has_value() || !source.addresses.empty() ||
           source.condition.has_value() || !source.proxy_headers.empty() || !source.response_headers.empty() ||
           !source.context.empty() || source.rewrite.has_value() || source.status != 0 || source.body.has_value() ||
           source.timeout_millis.has_value() || source.max_client_body_size.has_value() ||
           source.max_proxy_body_size.has_value() || source.websocket_timeout_millis.has_value() ||
           source.flush.has_value() || source.gzip.has_value();
}

std::string method_route_key(std::string_view path, std::string_view method,
                             const std::optional<std::string> &condition) {
    std::string signature;
    signature.reserve(method.size() + (condition ? condition->size() : 0) + 48);
    signature.append("method:");
    signature.append(std::to_string(method.size()));
    signature.push_back(':');
    signature.append(method);
    signature.append(";condition:");
    if (condition) {
        signature.append(std::to_string(condition->size()));
        signature.push_back(':');
        signature.append(*condition);
    } else {
        signature.append("none");
    }
    return conditional_route_key(path, signature);
}

bool ascii_iequals(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.size(); ++i) {
        const auto left_byte = static_cast<unsigned char>(left[i]);
        const auto right_byte = static_cast<unsigned char>(right[i]);
        const auto left_fold = left_byte >= 'A' && left_byte <= 'Z' ? left_byte | 0x20U : left_byte;
        const auto right_fold = right_byte >= 'A' && right_byte <= 'Z' ? right_byte | 0x20U : right_byte;
        if (left_fold != right_fold) {
            return false;
        }
    }
    return true;
}

bool contains_header(const StringConfigMap &headers, std::string_view name) noexcept {
    for (const StringConfigEntry &header: headers) {
        if (ascii_iequals(header.name, name)) {
            return true;
        }
    }
    return false;
}

bool response_status_has_no_content(std::int32_t status) noexcept {
    return (status >= 100 && status < 200) || status == 204 || status == 205 || status == 304;
}

std::expected<std::vector<CompiledTemplateEntry>, AccessConfigError>
compile_context(const StringConfigMap &input, std::size_t route_index, ProjectCompileBudget &budget) {
    auto compiled = compile_templates(input, route_index, "context", budget);
    if (!compiled) {
        return compiled;
    }

    constexpr std::string_view kTraceCluster = "HI-TRACE-CLUSTER";
    std::vector<CompiledTemplateEntry> result;
    result.reserve(compiled->size());
    for (CompiledTemplateEntry &entry: *compiled) {
        if (ascii_iequals(entry.name, "cluster") || ascii_iequals(entry.name, kTraceCluster)) {
            entry.name = kTraceCluster;
        }

        bool replaced = false;
        for (CompiledTemplateEntry &existing: result) {
            if (existing.name == entry.name) {
                existing.value = std::move(entry.value);
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            result.push_back(std::move(entry));
        }
    }
    return result;
}

std::optional<std::int32_t> parse_java_port(std::string_view value) noexcept {
    if (value.empty()) {
        return std::nullopt;
    }
    bool positive_sign = false;
    if (value.front() == '+') {
        positive_sign = true;
        value.remove_prefix(1);
        if (value.empty()) {
            return std::nullopt;
        }
    }
    std::int32_t port = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), port);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || (positive_sign && port < 0)) {
        return std::nullopt;
    }
    return port;
}

std::optional<AccessUpstreamInstance> compile_java_http_host(std::string_view value) {
    if (value.empty()) {
        return std::nullopt;
    }
    std::string_view host = value;
    std::string_view scheme_name;
    const std::size_t scheme_offset = host.find("://");
    if (scheme_offset != std::string_view::npos && scheme_offset > 0) {
        scheme_name = host.substr(0, scheme_offset);
        host.remove_prefix(scheme_offset + 3);
    }

    std::int32_t configured_port = -1;
    const std::size_t colon = host.rfind(':');
    if (colon > 0 && colon != std::string_view::npos) {
        const auto port = parse_java_port(host.substr(colon + 1));
        if (!port) {
            return std::nullopt;
        }
        configured_port = *port;
        host = host.substr(0, colon);
    }
    if (host.empty()) {
        return std::nullopt;
    }

    const bool https = scheme_name.empty() ? configured_port == 443 : ascii_iequals(scheme_name, "https");
    const std::uint16_t default_port = https ? 443 : 80;
    const std::int64_t real_port = configured_port <= 0 ? default_port : configured_port;
    if (real_port > std::numeric_limits<std::uint16_t>::max()) {
        return std::nullopt;
    }

    const std::uint16_t port = static_cast<std::uint16_t>(real_port);
    std::string authority(host);
    if (port != default_port) {
        authority.push_back(':');
        authority.append(std::to_string(port));
    }
    const auto scheme =
            https ? http::Http1ConnectionGroupKey::Scheme::Https : http::Http1ConnectionGroupKey::Scheme::Http;
    net::IpAddress ip;
    if (net::IpAddress::parse(host, ip)) {
        return AccessUpstreamInstance{
                .connection_key = http::Http1ConnectionGroupKey::from_ip(ip, port, scheme),
                .authority = std::move(authority),
        };
    }
    auto key = http::Http1ConnectionGroupKey::from_name(host, port, scheme);
    if (!key) {
        return std::nullopt;
    }
    return AccessUpstreamInstance{
            .connection_key = std::move(*key),
            .authority = std::move(authority),
    };
}

std::expected<std::vector<Cidr>, AccessConfigError> compile_cidr_list(const std::vector<std::string_view> &items,
                                                                      std::size_t route_index) {
    auto result = Cidr::parse_list(items, "allows");
    if (!result) {
        AccessConfigError error = std::move(result.error());
        error.field = "routes[" + std::to_string(route_index) + "].allows";
        return std::unexpected(std::move(error));
    }
    return result;
}

std::expected<CompiledRoute, AccessConfigError> compile_route(const RouteConfig &source, std::size_t route_index,
                                                              PendingRouteCompile &pending,
                                                              ProjectCompileBudget &budget) {
    if (!source.path || source.path->empty()) {
        return std::unexpected(route_error(AccessConfigErrorCode::InvalidField, route_index, "path", "path is empty"));
    }
    if (const auto invalid = validate_path_pattern(*source.path)) {
        return std::unexpected(route_error(AccessConfigErrorCode::InvalidField, route_index, "path", *invalid));
    }
    if (!source.type) {
        return std::unexpected(
                route_error(AccessConfigErrorCode::InvalidField, route_index, "type", "route type is null"));
    }
    if (source.upstream_tls && *source.type != RouteType::Proxy) {
        return std::unexpected(route_error(AccessConfigErrorCode::InvalidCombination, route_index, "upstream_tls",
                                           "upstream_tls is only valid for PROXY routes"));
    }

    CompiledRoute route;
    route.path = *source.path;
    route.type = *source.type;
    if (source.method) {
        if (!is_http_token(*source.method)) {
            return std::unexpected(route_error(AccessConfigErrorCode::InvalidField, route_index, "method",
                                               "method must be a non-empty HTTP token"));
        }
        route.method = *source.method;
    }
    if (is_nonempty(source.condition)) {
        pending.condition = *source.condition;
    }
    pending.has_predicate = route.method.has_value() || pending.condition.has_value();
    if (route.method) {
        route.key = method_route_key(route.path, *route.method, pending.condition);
    } else if (pending.condition) {
        route.key = conditional_route_key(route.path, *pending.condition);
    } else {
        route.key = route.path;
    }

    if (route.type == RouteType::Script) {
        if (!source.script || !has_non_whitespace(*source.script)) {
            return std::unexpected(
                    route_error(AccessConfigErrorCode::InvalidField, route_index, "script", "route script is empty"));
        }
        if (has_script_only_field_conflict(source)) {
            return std::unexpected(route_error(AccessConfigErrorCode::InvalidCombination, route_index, "script",
                                               "script route only supports path, method, script, and allows"));
        }
        pending.script = *source.script;
    } else if (source.script.has_value()) {
        return std::unexpected(route_error(AccessConfigErrorCode::InvalidCombination, route_index, "script",
                                           "script is only valid for SCRIPT routes"));
    } else if (route.type == RouteType::Response) {
        route.max_client_body_size = source.max_client_body_size;
        if (source.status < 100 || source.status >= 1000) {
            return std::unexpected(
                    route_error(AccessConfigErrorCode::OutOfRange, route_index, "status", "invalid status code"));
        }

        CompiledResponseRoute response;
        response.status = source.status;
        if (source.body) {
            const RouteBodyConfig &body = *source.body;
            if (body.type == BodyType::Base64) {
                if (!body.content || !util::base64_decode(*body.content, response.body)) {
                    return std::unexpected(route_error(AccessConfigErrorCode::InvalidField, route_index, "body",
                                                       "invalid base64 response body"));
                }
                if (response.body.size() > budget.limits().max_static_response_body_bytes) {
                    return std::unexpected(route_error(AccessConfigErrorCode::LimitExceeded, route_index, "body",
                                                       "response body bytes exceed the configured limit"));
                }
                response.body_kind = ResponseBodyKind::Base64;
            } else if (body.type == BodyType::Text) {
                if (!body.content || body.content->empty()) {
                    return std::unexpected(route_error(AccessConfigErrorCode::InvalidField, route_index, "body",
                                                       "text body content is empty"));
                }
                response.body_kind = ResponseBodyKind::Text;
                response.body = *body.content;
            } else {
                if (!body.content) {
                    return std::unexpected(route_error(AccessConfigErrorCode::InvalidField, route_index, "body",
                                                       "invalid template response body"));
                }
                auto compiled_body = compile_template(*body.content, route_index, "body", budget);
                if (!compiled_body) {
                    return std::unexpected(std::move(compiled_body.error()));
                }
                response.body_kind = ResponseBodyKind::Template;
                response.body_template.emplace(std::move(*compiled_body));
            }
        }
        if (response.body_kind == ResponseBodyKind::Text || response.body_kind == ResponseBodyKind::Base64) {
            if (response.body.size() > budget.limits().max_static_response_body_bytes) {
                return std::unexpected(route_error(AccessConfigErrorCode::LimitExceeded, route_index, "body",
                                                   "response body bytes exceed the configured limit"));
            }
            auto accounted = budget.add_static_response(response.body.size(), route_index);
            if (!accounted) {
                return std::unexpected(std::move(accounted.error()));
            }
        }

        auto headers =
                compile_response_header_templates(source.response_headers, route_index, "response_headers", budget);
        if (!headers) {
            return std::unexpected(std::move(headers.error()));
        }
        response.response_headers = std::move(*headers);

        if (source.gzip && source.gzip->enabled) {
            if (response.body_kind == ResponseBodyKind::Template) {
                return std::unexpected(route_error(AccessConfigErrorCode::InvalidCombination, route_index, "gzip",
                                                   "gzip is not supported for TEMPLATE response bodies"));
            }
            if (!source.body || response.body.empty()) {
                return std::unexpected(route_error(AccessConfigErrorCode::InvalidCombination, route_index, "gzip",
                                                   "gzip requires a non-empty response body"));
            }
            if (response_status_has_no_content(response.status) || response.status == 206) {
                return std::unexpected(route_error(AccessConfigErrorCode::InvalidCombination, route_index, "gzip",
                                                   "gzip is not supported for this response status"));
            }
            if (contains_header(source.response_headers, "Content-Encoding")) {
                return std::unexpected(route_error(AccessConfigErrorCode::InvalidCombination, route_index, "gzip",
                                                   "gzip cannot be combined with a Content-Encoding header"));
            }
            auto encoded = gzip_encode(response.body, source.gzip->level);
            if (!encoded) {
                return std::unexpected(route_error(AccessConfigErrorCode::InvalidField, route_index, "gzip",
                                                   "failed to precompress response body"));
            }
            auto accounted = budget.add_static_response(encoded->size(), route_index);
            if (!accounted) {
                return std::unexpected(std::move(accounted.error()));
            }
            response.gzip_level = source.gzip->level;
            response.gzip_body = std::move(*encoded);
        }
        route.response.emplace(std::move(response));
    } else {
        if (source.gzip.has_value()) {
            return std::unexpected(route_error(AccessConfigErrorCode::InvalidCombination, route_index, "gzip",
                                               "gzip is only valid for RESPONSE routes"));
        }
        route.max_client_body_size = source.max_client_body_size;
        if (source.timeout_millis && *source.timeout_millis < 5) {
            return std::unexpected(
                    route_error(AccessConfigErrorCode::OutOfRange, route_index, "timeout", "timeout is too small"));
        }

        CompiledProxyRoute proxy;
        proxy.timeout_millis = source.timeout_millis ? java_int32_narrow(*source.timeout_millis) : 60000;
        if (source.upstream_tls) {
            auto profile = compile_upstream_tls_transport_profile(*source.upstream_tls, route_index);
            if (!profile) {
                return std::unexpected(std::move(profile.error()));
            }
            proxy.upstream_tls.emplace(std::move(*profile));
        }
        if (is_nonempty(source.service)) {
            std::string service;
            std::string cluster(kDefaultServiceCluster);
            const std::size_t slash = source.service->find('/');
            if (slash > 0 && slash != std::string::npos) {
                service = source.service->substr(0, slash);
                const std::string_view service_cluster(*source.service);
                if (slash + 1 < service_cluster.size()) {
                    cluster = service_cluster.substr(slash + 1);
                }
            } else {
                service = *source.service;
            }
            if (is_nonempty(source.cluster)) {
                cluster = *source.cluster;
            }
            proxy.address_selector = make_unavailable_service_address_selector(std::move(service), std::move(cluster));
        } else if (!source.addresses.empty()) {
            std::vector<AccessUpstreamInstance> addresses;
            addresses.reserve(source.addresses.size());
            for (const std::optional<std::string> &address: source.addresses) {
                auto compiled_address = address ? compile_java_http_host(*address) : std::nullopt;
                if (!compiled_address) {
                    return std::unexpected(route_error(AccessConfigErrorCode::InvalidField, route_index, "addresses",
                                                       "invalid HTTP host"));
                }
                addresses.push_back(std::move(*compiled_address));
            }
            proxy.address_selector = make_static_proxy_address_selector(std::move(addresses));
        } else {
            return std::unexpected(route_error(AccessConfigErrorCode::InvalidCombination, route_index, "service",
                                               "no service or addresses"));
        }
        if (!proxy.address_selector) {
            return std::unexpected(route_error(AccessConfigErrorCode::InvalidCombination, route_index, "service",
                                               "upstream selector factory returned null"));
        }

        auto proxy_headers = compile_header_templates(source.proxy_headers, route_index, "proxy_headers", budget);
        if (!proxy_headers) {
            return std::unexpected(std::move(proxy_headers.error()));
        }
        pending.proxy_headers = std::move(*proxy_headers);

        auto response_headers =
                compile_header_templates(source.response_headers, route_index, "response_headers", budget);
        if (!response_headers) {
            return std::unexpected(std::move(response_headers.error()));
        }
        pending.response_headers = std::move(*response_headers);

        auto context = compile_context(source.context, route_index, budget);
        if (!context) {
            return std::unexpected(std::move(context.error()));
        }
        proxy.context = std::move(*context);

        if (is_nonempty(source.rewrite)) {
            auto rewrite = compile_template(*source.rewrite, route_index, "rewrite", budget);
            if (!rewrite) {
                return std::unexpected(std::move(rewrite.error()));
            }
            proxy.rewrite.emplace(std::move(*rewrite));
        }
        proxy.max_response_body_size = source.max_proxy_body_size;
        if (source.websocket_timeout_millis && *source.websocket_timeout_millis > 0) {
            proxy.websocket_timeout_millis = java_int32_narrow(*source.websocket_timeout_millis);
        }
        proxy.flush = source.flush;
        route.proxy.emplace(std::move(proxy));
    }

    std::vector<std::string_view> allows;
    std::vector<std::string_view> denies;
    allows.reserve(source.allows.size());
    denies.reserve(source.allows.size());
    for (const std::optional<std::string> &item: source.allows) {
        if (!item || item->empty()) {
            return std::unexpected(
                    route_error(AccessConfigErrorCode::InvalidField, route_index, "allows", "empty cidr"));
        }
        if (item->front() == '!') {
            denies.push_back(std::string_view(*item).substr(1));
        } else {
            allows.push_back(*item);
        }
    }
    if (!allows.empty()) {
        auto parsed = compile_cidr_list(allows, route_index);
        if (!parsed) {
            return std::unexpected(std::move(parsed.error()));
        }
        route.allow_cidrs = std::move(*parsed);
    }
    if (!denies.empty()) {
        auto parsed = compile_cidr_list(denies, route_index);
        if (!parsed) {
            return std::unexpected(std::move(parsed.error()));
        }
        route.deny_cidrs = std::move(*parsed);
    }
    return route;
}

class RouteDefiner {
public:
    RouteDefiner(std::vector<CompiledRoute> &routes, std::span<const PendingRouteCompile> pending,
                 std::size_t max_path_variables) :
        routes_(routes), pending_(pending), max_path_variables_(max_path_variables) {}

    void add_path_var_definer(std::uint32_t &route_index, std::string_view name, std::uint32_t index) {
        CompiledRoute &route = routes_[route_index];
        for (std::size_t i = 0; i < route.path_variable_names.size(); ++i) {
            if (route.path_variable_names[i] == name && i != index) {
                set_error(route_index, AccessConfigErrorCode::Conflict, "duplicated path variable");
                return;
            }
        }
        if (route.path_variable_names.size() >= max_path_variables_) {
            set_error(route_index, AccessConfigErrorCode::LimitExceeded,
                      "path variable count exceeds the configured limit");
            return;
        }
        route.path_variable_names.emplace_back(name);
    }

    std::uint32_t on_route_mount(std::uint32_t node_id, std::string_view, std::uint32_t &route_index) {
        if (last_node_id_ != node_id) {
            route_keys_.clear();
        }
        if (last_node_id_ == node_id && last_route_ != kNoRoute && !pending_[last_route_].has_predicate) {
            set_error(route_index, AccessConfigErrorCode::Conflict, "exists dead route");
        }
        if (!route_keys_.emplace(routes_[route_index].key).second) {
            set_error(route_index, AccessConfigErrorCode::Conflict, "route predicate is duplicate");
        }
        last_node_id_ = node_id;
        last_route_ = route_index;
        return route_index;
    }

    [[nodiscard]] const std::optional<AccessConfigError> &error() const noexcept { return error_; }

private:
    void set_error(std::uint32_t route_index, AccessConfigErrorCode code, std::string_view message) {
        if (!error_) {
            error_ = route_error(code, route_index, "path", message);
        }
    }

    static constexpr std::uint32_t kNoRoute = std::numeric_limits<std::uint32_t>::max();

    std::vector<CompiledRoute> &routes_;
    std::span<const PendingRouteCompile> pending_;
    std::unordered_set<std::string_view> route_keys_;
    std::optional<AccessConfigError> error_;
    std::uint32_t last_node_id_ = util::RoutePathMatcher<std::uint32_t>::kInvalidIndex;
    std::uint32_t last_route_ = kNoRoute;
    std::size_t max_path_variables_ = 0;
};

} // namespace

ProjectSnapshotResult compile_project_config(std::string_view project, const ProjectConfig &config) {
    return ProjectConfigCompiler{}.compile(project, config);
}

ProjectSnapshotResult compile_project_config(std::string_view project, const ProjectConfig &config,
                                             ScriptCompilerAdapter compiler) {
    return ProjectConfigCompiler(compiler).compile(project, config);
}

ProjectSnapshotResult compile_project_config(std::string_view project, const ProjectConfig &config,
                                             ScriptCompilerAdapter compiler,
                                             ProxyAddressSelectorFactory selector_factory,
                                             const AccessConfigLimits &limits) {
    auto compiled = ProjectConfigCompiler(compiler, limits).compile(project, config);
    if (!compiled || !*compiled || !selector_factory.create_service) {
        return compiled;
    }
    auto bound = bind_project_service_selectors(**compiled, selector_factory);
    if (!bound) {
        return std::unexpected(std::move(bound.error()));
    }
    return compiled;
}

ProjectSnapshotResult ProjectConfigCompiler::compile(std::string_view project, const ProjectConfig &config) const {
    if (project.empty()) {
        return std::unexpected(project_error("project", "project name is empty"));
    }
    auto project_limit = validate_project_name_limit(project, *limits_);
    if (!project_limit) {
        return std::unexpected(std::move(project_limit.error()));
    }
    auto config_limits = validate_project_config_limits(config, *limits_);
    if (!config_limits) {
        return std::unexpected(std::move(config_limits.error()));
    }
    if (!config.hosts || config.hosts->empty()) {
        return std::optional<ProjectRouteSnapshot>{};
    }
    if (!config.routes) {
        return std::unexpected(project_error("routes", "routes is null while host is configured"));
    }

    ProjectCompileBudget budget(limits_->project_route);
    auto accounted = budget.account_config(project, config);
    if (!accounted) {
        return std::unexpected(std::move(accounted.error()));
    }

    ProjectRouteSnapshot snapshot;
    http_script::ConstPackage::Builder constants;
    snapshot.project_ = project;
    snapshot.call_source_.reserve(project.size() + kCallSourceSuffix.size());
    snapshot.call_source_.append(project);
    snapshot.call_source_.append(kCallSourceSuffix);
    snapshot.version_ = config.version;
    snapshot.routes_.reserve(config.routes->size());
    std::vector<PendingRouteCompile> pending;
    pending.reserve(config.routes->size());
    for (std::size_t i = 0; i < config.routes->size(); ++i) {
        const std::optional<RouteConfig> &source = (*config.routes)[i];
        if (!source) {
            return std::unexpected(route_error(AccessConfigErrorCode::InvalidField, i, {}, "route entry is null"));
        }
        PendingRouteCompile &route_pending = pending.emplace_back();
        auto route = compile_route(*source, i, route_pending, budget);
        if (!route) {
            return std::unexpected(std::move(route.error()));
        }
        snapshot.routes_.push_back(std::move(*route));
    }

    RouteDefiner definer(snapshot.routes_, pending, limits_->project_route.max_path_variables);
    util::RoutePathMatcher<std::uint32_t>::Builder<std::uint32_t, RouteDefiner> path_builder(definer);
    for (std::uint32_t i = 0; i < snapshot.routes_.size(); ++i) {
        path_builder.add_route(snapshot.routes_[i].path, i);
    }
    snapshot.path_matcher_ = path_builder.build();
    if (definer.error()) {
        return std::unexpected(*definer.error());
    }
    for (std::size_t i = 0; i < snapshot.routes_.size(); ++i) {
        auto compiled = compile_route_scripts(snapshot.routes_[i], i, pending[i], script_compiler_, constants, budget);
        if (!compiled) {
            return std::unexpected(std::move(compiled.error()));
        }
        if (CompiledRoute &route = snapshot.routes_[i]; route.proxy) {
            route.proxy->proxy_headers = std::move(pending[i].proxy_headers).build();
            route.proxy->response_headers = std::move(pending[i].response_headers).build();
        }
    }

    snapshot.const_package_ = constants.build();
    if (!snapshot.const_package_) {
        return std::unexpected(project_error("routes", "failed to finalize route constant package"));
    }
    for (CompiledRoute &route: snapshot.routes_) {
        route.path_constant_indices.reserve(route.path_variable_names.size());
        for (const std::string &name: route.path_variable_names) {
            route.path_constant_indices.push_back(snapshot.const_package_->find(http_script::ConstType::Path, name));
        }
    }
    for (const http_script::ConstPackage::Entry &entry:
         snapshot.const_package_->entries(http_script::ConstType::Context)) {
        if (entry.name() == "cluster" || entry.name() == "hi_trace_cluster") {
            snapshot.context_cluster_indices_.push_back(entry.index);
        }
    }

    std::vector<HostPattern> patterns;
    patterns.reserve(config.hosts->size());
    snapshot.hosts_.reserve(config.hosts->size());
    for (const HostConfigEntry &host: *config.hosts) {
        if (host.pattern.empty()) {
            continue;
        }
        if (!host.strategy) {
            return std::unexpected(project_error("host." + host.pattern, "host strategy is null"));
        }
        const std::uint32_t index = static_cast<std::uint32_t>(snapshot.hosts_.size());
        snapshot.hosts_.push_back(CompiledHost{
                .pattern = host.pattern,
                .strategy = *host.strategy,
        });
        patterns.push_back(HostPattern{
                .pattern = snapshot.hosts_.back().pattern,
                .handler = index,
        });
    }

    auto host_matcher = HostMatcher::build(patterns);
    if (!host_matcher) {
        return std::unexpected(std::move(host_matcher.error()));
    }
    snapshot.host_matcher_ = std::move(*host_matcher);
    snapshot.estimated_memory_bytes_ = budget.estimated_bytes();
    snapshot.static_response_bytes_ = budget.static_response_bytes();
    snapshot.compiled_program_count_ = budget.compiled_programs();
    return std::optional<ProjectRouteSnapshot>(std::move(snapshot));
}

std::expected<void, AccessConfigError> bind_project_service_selectors(ProjectRouteSnapshot &snapshot,
                                                                      ProxyAddressSelectorFactory selector_factory) {
    if (!selector_factory.create_service) {
        return {};
    }
    std::optional<AccessConfigError> first_error;
    for (std::size_t route_index = 0; route_index < snapshot.routes_.size(); ++route_index) {
        CompiledRoute &route = snapshot.routes_[route_index];
        if (!route.proxy || !route.proxy->address_selector) {
            continue;
        }
        const std::string_view service_view = route.proxy->address_selector->service_name();
        if (service_view.empty()) {
            continue;
        }
        std::string service(service_view);
        std::string cluster(kDefaultServiceCluster);
        if (const auto configured = route.proxy->address_selector->configured_cluster()) {
            cluster = *configured;
        }
        auto selector =
                selector_factory.create_service(selector_factory.context, std::move(service), std::move(cluster));
        if (!selector) {
            if (!first_error) {
                first_error.emplace(selector_factory_error(route_index, std::move(selector.error())));
            }
            continue;
        }
        if (!*selector) {
            if (!first_error) {
                first_error.emplace(route_error(AccessConfigErrorCode::InvalidCombination, route_index, "service",
                                                "upstream selector factory returned null"));
            }
            continue;
        }
        route.proxy->address_selector = std::move(*selector);
    }
    if (first_error) {
        return std::unexpected(std::move(*first_error));
    }
    return {};
}

} // namespace fiber::access_server
