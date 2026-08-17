#include "AccessServerConfig.h"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <iterator>
#include <limits>
#include <set>
#include <utility>
#include <vector>

namespace fiber::access_server {
namespace {

constexpr std::string_view kListenAddress = "ACCESS_SERVER_LISTEN_ADDRESS";
constexpr std::string_view kListenPort = "ACCESS_SERVER_LISTEN_PORT";
constexpr std::string_view kTlsEnabled = "ACCESS_SERVER_TLS_ENABLED";
constexpr std::string_view kTlsCertificatesDataIdSetting = "ACCESS_SERVER_TLS_CERTIFICATES_DATA_ID";
constexpr std::string_view kTlsCertificatesGroupSetting = "ACCESS_SERVER_TLS_CERTIFICATES_GROUP";
constexpr std::string_view kHttp3Enabled = "ACCESS_SERVER_HTTP3_ENABLED";
constexpr std::string_view kMetricsListenAddress = "ACCESS_SERVER_METRICS_LISTEN_ADDRESS";
constexpr std::string_view kMetricsListenPort = "ACCESS_SERVER_METRICS_LISTEN_PORT";
constexpr std::string_view kActivationEvidenceEnabled = "ACCESS_SERVER_ACTIVATION_EVIDENCE_ENABLED";
constexpr std::string_view kActivationInstanceId = "ACCESS_SERVER_INSTANCE_ID";
constexpr std::string_view kActivationToken = "ACCESS_SERVER_ACTIVATION_TOKEN";
constexpr std::string_view kInitialConfigTimeout = "ACCESS_SERVER_INITIAL_CONFIG_TIMEOUT_MILLIS";
constexpr std::string_view kMaxRequestBody = "ACCESS_SERVER_MAX_REQUEST_BODY_SIZE";
constexpr std::string_view kTestMode = "ACCESS_SERVER_TEST_MODE";
constexpr std::string_view kClientMetadataMode = "ACCESS_SERVER_CLIENT_METADATA_MODE";
constexpr std::string_view kTrustedProxyCidrs = "ACCESS_SERVER_TRUSTED_PROXY_CIDRS";
constexpr std::string_view kAccessLogQueryAllowlist = "ACCESS_SERVER_ACCESS_LOG_QUERY_ALLOWLIST";
constexpr std::string_view kAccessLogSensitiveQueryKeys = "ACCESS_SERVER_ACCESS_LOG_SENSITIVE_QUERY_KEYS";
constexpr std::string_view kAccessLogQueryHashEnabled = "ACCESS_SERVER_ACCESS_LOG_QUERY_HASH_ENABLED";
constexpr std::string_view kAccessLogSuccessSampleRate = "ACCESS_SERVER_ACCESS_LOG_SUCCESS_SAMPLE_RATE_BPS";
constexpr std::string_view kAccessLogMaxPathBytes = "ACCESS_SERVER_ACCESS_LOG_MAX_PATH_BYTES";
constexpr std::string_view kAccessLogMaxQueryBytes = "ACCESS_SERVER_ACCESS_LOG_MAX_QUERY_BYTES";
constexpr std::string_view kUpstreamTlsMode = "ACCESS_SERVER_UPSTREAM_TLS_MODE";
constexpr std::string_view kUpstreamTlsCaFile = "ACCESS_SERVER_UPSTREAM_TLS_CA_FILE";
constexpr std::string_view kDnsMode = "ACCESS_SERVER_DNS_MODE";
constexpr std::string_view kDnsServers = "ACCESS_SERVER_DNS_SERVERS";
constexpr std::string_view kDnsResolverConfig = "ACCESS_SERVER_DNS_RESOLV_CONF";
constexpr std::string_view kProjectsDataId = "ACCESS_SERVER_PROJECTS_DATA_ID";
constexpr std::string_view kRouteDataIdPrefix = "ACCESS_SERVER_ROUTE_DATA_ID_PREFIX";
constexpr std::string_view kRouteGroup = "ACCESS_SERVER_ROUTE_GROUP";
constexpr std::string_view kGrayDataId = "ACCESS_SERVER_GRAY_DATA_ID";
constexpr std::string_view kNamingGroup = "ACCESS_SERVER_NAMING_GROUP";
constexpr std::string_view kZone = "ACCESS_SERVER_ZONE";
constexpr std::string_view kNacosServers = "NACOS_SERVER_ADDRESSES";
constexpr std::string_view kNacosHttpPort = "NACOS_HTTP_PORT";
constexpr std::string_view kNacosGrpcPort = "NACOS_GRPC_PORT";
constexpr std::string_view kNacosNamespace = "NACOS_NAMESPACE";
constexpr std::string_view kNacosTenant = "NACOS_TENANT";
constexpr std::string_view kNacosUsername = "NACOS_USERNAME";
constexpr std::string_view kNacosPassword = "NACOS_PASSWORD";
constexpr std::string_view kNacosClientVersion = "NACOS_CLIENT_VERSION";
constexpr std::string_view kCatAppKey = "CAT_APP_KEY";
constexpr std::string_view kCatHostname = "CAT_HOSTNAME";
constexpr std::string_view kCatIp = "CAT_IP";
constexpr std::string_view kCatRouters = "CAT_ROUTER_ADDRESSES";
constexpr std::string_view kCatCollectors = "CAT_COLLECTOR_ADDRESSES";
constexpr std::size_t kMaxUpstreamTlsCaFileBytes = 4096;
constexpr std::size_t kMaxDnsResolverConfigPathBytes = 4096;
constexpr std::size_t kMinActivationTokenBytes = 32;
constexpr std::size_t kMaxActivationTokenBytes = 512;

struct Entry {
    std::string key;
    std::string value;
    std::size_t line = 0;
};

AccessServerConfigError error(AccessServerConfigErrorCode code, std::size_t line, std::string_view key,
                              std::string detail) {
    return AccessServerConfigError{
            .code = code,
            .line = line,
            .key = std::string(key),
            .detail = std::move(detail),
    };
}

std::string_view trim(std::string_view value) noexcept {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t' || value.front() == '\r')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r')) {
        value.remove_suffix(1);
    }
    return value;
}

std::expected<std::vector<Entry>, AccessServerConfigError> parse_entries(std::string_view input) {
    std::vector<Entry> entries;
    std::set<std::string, std::less<>> keys;
    std::size_t line_number = 0;
    while (!input.empty()) {
        ++line_number;
        const std::size_t newline = input.find('\n');
        std::string_view line = newline == std::string_view::npos ? input : input.substr(0, newline);
        input = newline == std::string_view::npos ? std::string_view{} : input.substr(newline + 1);
        line = trim(line);
        if (line.empty() || line.front() == '#') {
            continue;
        }
        const std::size_t equals = line.find('=');
        if (equals == std::string_view::npos) {
            return std::unexpected(
                    error(AccessServerConfigErrorCode::InvalidSyntax, line_number, {}, "expected KEY=VALUE"));
        }
        const std::string_view key = trim(line.substr(0, equals));
        const std::string_view value = trim(line.substr(equals + 1));
        if (key.empty()) {
            return std::unexpected(
                    error(AccessServerConfigErrorCode::InvalidSyntax, line_number, {}, "setting name is empty"));
        }
        if (!keys.emplace(key).second) {
            return std::unexpected(error(AccessServerConfigErrorCode::DuplicateKey, line_number, key,
                                         "setting is defined more than once"));
        }
        entries.push_back(Entry{
                .key = std::string(key),
                .value = std::string(value),
                .line = line_number,
        });
    }
    return entries;
}

template<typename T>
bool parse_unsigned(std::string_view input, T &output) noexcept {
    if (input.empty()) {
        return false;
    }
    T value = 0;
    const auto parsed = std::from_chars(input.data(), input.data() + input.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != input.data() + input.size()) {
        return false;
    }
    output = value;
    return true;
}

bool parse_boolean(std::string_view input, bool &output) noexcept {
    if (input == "true") {
        output = true;
        return true;
    }
    if (input == "false") {
        output = false;
        return true;
    }
    return false;
}

bool valid_query_key(std::string_view value) noexcept {
    if (value.empty() || value.size() > kMaxAccessLogQueryKeyBytes) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
               (character >= '0' && character <= '9') || character == '-' || character == '.' || character == '_' ||
               character == '~';
    });
}

bool valid_instance_id(std::string_view value) noexcept {
    return !value.empty() && value.size() <= 255 &&
           std::all_of(value.begin(), value.end(), [](unsigned char character) {
               return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
                      (character >= '0' && character <= '9') || character == '-' || character == '_' ||
                      character == '.' || character == ':';
           });
}

bool valid_activation_token(std::string_view value) noexcept {
    return value.size() >= kMinActivationTokenBytes && value.size() <= kMaxActivationTokenBytes &&
           std::all_of(value.begin(), value.end(),
                       [](unsigned char character) { return character >= 0x21U && character <= 0x7eU; });
}

std::expected<std::vector<std::string>, AccessServerConfigError>
parse_query_keys(std::string_view input, std::size_t line, std::string_view setting, bool case_insensitive) {
    std::vector<std::string> output;
    std::set<std::string, std::less<>> unique;
    if (input.empty()) {
        return output;
    }
    while (true) {
        const std::size_t comma = input.find(',');
        const std::string_view item = trim(comma == std::string_view::npos ? input : input.substr(0, comma));
        if (!valid_query_key(item)) {
            return std::unexpected(error(AccessServerConfigErrorCode::InvalidValue, line, setting,
                                         "expected comma-separated query keys using [A-Za-z0-9_.~-]"));
        }
        std::string normalized(item);
        if (case_insensitive) {
            std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char character) {
                return character >= 'A' && character <= 'Z' ? static_cast<char>(character + ('a' - 'A'))
                                                            : static_cast<char>(character);
            });
        }
        if (!unique.emplace(normalized).second) {
            return std::unexpected(error(AccessServerConfigErrorCode::InvalidValue, line, setting,
                                         "query key list contains a duplicate"));
        }
        output.push_back(std::move(normalized));
        if (output.size() > kMaxAccessLogQueryKeys) {
            return std::unexpected(error(AccessServerConfigErrorCode::InvalidValue, line, setting,
                                         "query key list exceeds 64 entries"));
        }
        if (comma == std::string_view::npos) {
            break;
        }
        input.remove_prefix(comma + 1);
        if (input.empty()) {
            return std::unexpected(error(AccessServerConfigErrorCode::InvalidValue, line, setting,
                                         "query key list contains an empty entry"));
        }
    }
    return output;
}

std::expected<std::vector<Cidr>, AccessServerConfigError> parse_trusted_proxy_cidrs(std::string_view input,
                                                                                    std::size_t line) {
    std::vector<Cidr> output;
    if (input.empty()) {
        return output;
    }
    while (true) {
        const std::size_t comma = input.find(',');
        const std::string_view item = trim(comma == std::string_view::npos ? input : input.substr(0, comma));
        auto cidr = Cidr::parse_strict(item, kTrustedProxyCidrs);
        if (!cidr) {
            return std::unexpected(error(AccessServerConfigErrorCode::InvalidValue, line, kTrustedProxyCidrs,
                                         "expected comma-separated strict IPv4 or IPv6 CIDRs"));
        }
        output.push_back(*cidr);
        if (output.size() > kMaxTrustedProxyCidrs) {
            return std::unexpected(error(AccessServerConfigErrorCode::InvalidValue, line, kTrustedProxyCidrs,
                                         "trusted proxy CIDR list exceeds 64 entries"));
        }
        if (comma == std::string_view::npos) {
            break;
        }
        input.remove_prefix(comma + 1);
        if (input.empty()) {
            return std::unexpected(error(AccessServerConfigErrorCode::InvalidValue, line, kTrustedProxyCidrs,
                                         "trusted proxy CIDR list contains an empty entry"));
        }
    }
    return output;
}

std::expected<std::pair<net::IpAddress, std::uint16_t>, AccessServerConfigError>
parse_cat_endpoint(std::string_view text, std::size_t line, std::string_view key) {
    text = trim(text);
    std::string_view host;
    std::string_view port_text;
    if (text.starts_with('[')) {
        const std::size_t bracket = text.find(']');
        if (bracket == std::string_view::npos || bracket + 1 >= text.size() || text[bracket + 1] != ':') {
            return std::unexpected(
                    error(AccessServerConfigErrorCode::InvalidValue, line, key, "expected IP:port or [IPv6]:port"));
        }
        host = text.substr(1, bracket - 1);
        port_text = text.substr(bracket + 2);
    } else {
        const std::size_t colon = text.rfind(':');
        if (colon == std::string_view::npos || text.substr(0, colon).find(':') != std::string_view::npos) {
            return std::unexpected(
                    error(AccessServerConfigErrorCode::InvalidValue, line, key, "expected IP:port or [IPv6]:port"));
        }
        host = text.substr(0, colon);
        port_text = text.substr(colon + 1);
    }

    net::IpAddress address;
    std::uint16_t port = 0;
    if (!net::IpAddress::parse(host, address) || address.is_unspecified() || address.is_multicast() ||
        !parse_unsigned(port_text, port) || port == 0) {
        return std::unexpected(error(AccessServerConfigErrorCode::InvalidValue, line, key,
                                     "expected a specified unicast IP and non-zero port"));
    }
    return std::pair(address, port);
}

std::expected<void, AccessServerConfigError> parse_cat_endpoints(std::string_view value, std::size_t line,
                                                                 std::string_view key, bool routers,
                                                                 cat::CatClientConfigParams &params) {
    if (value.empty()) {
        return {};
    }
    while (true) {
        const std::size_t separator = value.find(',');
        const std::string_view item = separator == std::string_view::npos ? value : value.substr(0, separator);
        auto endpoint = parse_cat_endpoint(item, line, key);
        if (!endpoint) {
            return std::unexpected(std::move(endpoint.error()));
        }
        if (routers) {
            params.routers.push_back(cat::CatRouterEndpoint{
                    .host = endpoint->first.to_string(),
                    .port = endpoint->second,
            });
        } else {
            params.bootstrap_collectors.emplace_back(endpoint->first, endpoint->second);
        }
        if (separator == std::string_view::npos) {
            break;
        }
        value.remove_prefix(separator + 1);
        if (value.empty()) {
            return std::unexpected(error(AccessServerConfigErrorCode::InvalidValue, line, key,
                                         "empty endpoint in comma-separated list"));
        }
    }
    return {};
}

std::expected<std::vector<std::string>, AccessServerConfigError> parse_nacos_servers(std::string_view input,
                                                                                     std::size_t line) {
    std::vector<std::string> servers;
    while (!input.empty()) {
        const std::size_t comma = input.find(',');
        const std::string_view token = trim(comma == std::string_view::npos ? input : input.substr(0, comma));
        net::IpAddress address;
        if (token.empty() || !net::IpAddress::parse(token, address)) {
            return std::unexpected(error(AccessServerConfigErrorCode::InvalidValue, line, kNacosServers,
                                         "expected a comma-separated list of IP literals"));
        }
        servers.push_back(address.to_string());
        input = comma == std::string_view::npos ? std::string_view{} : input.substr(comma + 1);
    }
    return servers;
}

std::expected<dns::DnsNameserverList, AccessServerConfigError>
parse_dns_nameservers(std::string_view input, std::size_t line) {
    dns::DnsNameserverList nameservers;
    if (input.empty()) {
        return std::unexpected(error(AccessServerConfigErrorCode::InvalidValue, line, kDnsServers,
                                     "DNS override requires at least one nameserver IP literal"));
    }
    while (true) {
        const std::size_t comma = input.find(',');
        const std::string_view token = trim(comma == std::string_view::npos ? input : input.substr(0, comma));
        net::IpAddress address;
        if (token.empty() || !net::IpAddress::parse(token, address) || address.is_unspecified() ||
            address.is_multicast()) {
            return std::unexpected(error(AccessServerConfigErrorCode::InvalidValue, line, kDnsServers,
                                         "expected up to three specified unicast IP literals"));
        }
        if (!nameservers.add(net::SocketAddress(address, 53))) {
            return std::unexpected(error(AccessServerConfigErrorCode::InvalidValue, line, kDnsServers,
                                         "DNS nameserver list exceeds three entries"));
        }
        if (comma == std::string_view::npos) {
            break;
        }
        input.remove_prefix(comma + 1);
        if (input.empty()) {
            return std::unexpected(error(AccessServerConfigErrorCode::InvalidValue, line, kDnsServers,
                                         "DNS nameserver list contains an empty entry"));
        }
    }
    return nameservers;
}

AccessServerConfigError nacos_error(const nacos::NacosConfigError &source) {
    std::string detail = "invalid Nacos client configuration";
    if (source.code == nacos::NacosConfigErrorCode::EmptyServerList) {
        return error(AccessServerConfigErrorCode::MissingRequiredKey, 0, kNacosServers, "required setting is missing");
    }
    if (source.code == nacos::NacosConfigErrorCode::EmptyUsername ||
        source.code == nacos::NacosConfigErrorCode::EmptyPassword) {
        detail = "NACOS_USERNAME and NACOS_PASSWORD must both be empty or both be set";
    }
    return error(AccessServerConfigErrorCode::InvalidNacosConfig, 0, kNacosServers, std::move(detail));
}

} // namespace

AccessServerConfig::AccessServerConfig(
        net::SocketAddress listen_address, http::HttpServerOptions http_server_options,
        net::SocketAddress metrics_listen_address, AccessActivationEndpointOptions activation_endpoint_options,
        std::chrono::milliseconds initial_config_timeout, std::size_t default_max_request_body_size, bool test_mode,
        ClientMetadataResolverOptions client_metadata_options, AccessLogOptions access_log_options,
        UpstreamTlsClientPolicy upstream_tls_client_policy, AccessDnsMode dns_mode,
        std::string dns_resolver_config_path, dns::DnsNameserverList dns_override_nameservers,
        std::optional<cat::CatClientConfig> cat_config,
        nacos::NacosClientConfig nacos_config, AccessConfigWatcherOptions watcher_options,
        GrayConfigWatcherOptions gray_watcher_options, TlsCertificateWatcherOptions tls_certificate_watcher_options,
        AccessServiceDiscoveryOptions service_discovery_options) noexcept :
    listen_address_(std::move(listen_address)), http_server_options_(std::move(http_server_options)),
    metrics_listen_address_(std::move(metrics_listen_address)),
    activation_endpoint_options_(std::move(activation_endpoint_options)),
    initial_config_timeout_(initial_config_timeout), default_max_request_body_size_(default_max_request_body_size),
    test_mode_(test_mode), client_metadata_options_(std::move(client_metadata_options)),
    access_log_options_(std::move(access_log_options)),
    upstream_tls_client_policy_(std::move(upstream_tls_client_policy)), dns_mode_(dns_mode),
    dns_resolver_config_path_(std::move(dns_resolver_config_path)),
    dns_override_nameservers_(std::move(dns_override_nameservers)), cat_config_(std::move(cat_config)),
    nacos_config_(std::move(nacos_config)), watcher_options_(std::move(watcher_options)),
    gray_watcher_options_(std::move(gray_watcher_options)),
    tls_certificate_watcher_options_(std::move(tls_certificate_watcher_options)),
    service_discovery_options_(std::move(service_discovery_options)) {}

std::expected<AccessServerConfig, AccessServerConfigError> AccessServerConfig::load_from_file(std::string_view path) {
    std::ifstream input(std::string(path), std::ios::binary);
    if (!input) {
        return std::unexpected(
                error(AccessServerConfigErrorCode::OpenFailed, 0, {}, "failed to open configuration file"));
    }
    std::string contents{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    if (input.bad()) {
        return std::unexpected(
                error(AccessServerConfigErrorCode::ReadFailed, 0, {}, "failed to read configuration file"));
    }
    return load_from_string(contents);
}

std::expected<AccessServerConfig, AccessServerConfigError>
AccessServerConfig::load_from_string(std::string_view input) {
    auto entries = parse_entries(input);
    if (!entries) {
        return std::unexpected(std::move(entries.error()));
    }

    net::IpAddress listen_ip = net::IpAddress::any_v4();
    std::uint16_t listen_port = 16688;
    bool tls_enabled = true;
    bool http3_enabled = true;
    std::optional<net::IpAddress> metrics_ip;
    std::optional<std::uint16_t> metrics_port;
    AccessActivationEndpointOptions activation_endpoint_options;
    std::uint64_t timeout_millis = 60000;
    std::size_t max_request_body = 400U << 20U;
    bool test_mode = false;
    ClientMetadataResolverOptions client_metadata_options;
    AccessLogOptions access_log_options;
    UpstreamTlsClientPolicy upstream_tls_client_policy;
    AccessDnsMode dns_mode = AccessDnsMode::System;
    std::string dns_resolver_config_path = "/etc/resolv.conf";
    dns::DnsNameserverList dns_override_nameservers;
    bool dns_servers_present = false;
    bool dns_resolver_config_present = false;
    cat::CatClientConfigParams cat_params{
            .thread_group_name = "access-server-cat",
            .thread_id = "0",
            .thread_name = "cat-sender",
    };
    bool cat_setting_present = false;
    nacos::NacosClientConfigParams nacos_params;
    nacos_params.namespace_id = "public";
    AccessConfigWatcherOptions watcher_options;
    GrayConfigWatcherOptions gray_options;
    TlsCertificateWatcherOptions tls_certificate_options;
    AccessServiceDiscoveryOptions service_discovery_options;

    for (const Entry &entry: *entries) {
        const std::string_view value = entry.value;
        if (entry.key == kListenAddress) {
            if (!net::IpAddress::parse(value, listen_ip) || listen_ip.is_multicast()) {
                return std::unexpected(error(AccessServerConfigErrorCode::InvalidValue, entry.line, entry.key,
                                             "expected a non-multicast IP literal"));
            }
        } else if (entry.key == kListenPort) {
            if (!parse_unsigned(value, listen_port) || listen_port == 0) {
                return std::unexpected(error(AccessServerConfigErrorCode::InvalidValue, entry.line, entry.key,
                                             "expected a port in range 1..65535"));
            }
        } else if (entry.key == kTlsEnabled) {
            if (!parse_boolean(value, tls_enabled)) {
                return std::unexpected(error(AccessServerConfigErrorCode::InvalidValue, entry.line, entry.key,
                                             "expected true or false"));
            }
        } else if (entry.key == kTlsCertificatesDataIdSetting) {
            tls_certificate_options.data_id = entry.value;
        } else if (entry.key == kTlsCertificatesGroupSetting) {
            tls_certificate_options.group = entry.value;
        } else if (entry.key == kHttp3Enabled) {
            if (!parse_boolean(value, http3_enabled)) {
                return std::unexpected(error(AccessServerConfigErrorCode::InvalidValue, entry.line, entry.key,
                                             "expected true or false"));
            }
        } else if (entry.key == kMetricsListenAddress) {
            net::IpAddress address;
            if (!net::IpAddress::parse(value, address) || address.is_multicast()) {
                return std::unexpected(error(AccessServerConfigErrorCode::InvalidValue, entry.line, entry.key,
                                             "expected a non-multicast IP literal"));
            }
            metrics_ip = address;
        } else if (entry.key == kMetricsListenPort) {
            std::uint16_t port = 0;
            if (!parse_unsigned(value, port) || port == 0) {
                return std::unexpected(error(AccessServerConfigErrorCode::InvalidValue, entry.line, entry.key,
                                             "expected a port in range 1..65535"));
            }
            metrics_port = port;
        } else if (entry.key == kActivationEvidenceEnabled) {
            if (!parse_boolean(value, activation_endpoint_options.enabled)) {
                return std::unexpected(error(AccessServerConfigErrorCode::InvalidValue, entry.line, entry.key,
                                             "expected true or false"));
            }
        } else if (entry.key == kActivationInstanceId) {
            activation_endpoint_options.instance_id = entry.value;
        } else if (entry.key == kActivationToken) {
            activation_endpoint_options.bearer_token = entry.value;
        } else if (entry.key == kInitialConfigTimeout) {
            if (!parse_unsigned(value, timeout_millis) ||
                timeout_millis > static_cast<std::uint64_t>(std::chrono::milliseconds::max().count())) {
                return std::unexpected(error(AccessServerConfigErrorCode::InvalidValue, entry.line, entry.key,
                                             "expected a non-negative millisecond duration"));
            }
        } else if (entry.key == kMaxRequestBody) {
            if (!parse_unsigned(value, max_request_body)) {
                return std::unexpected(error(AccessServerConfigErrorCode::InvalidValue, entry.line, entry.key,
                                             "expected a non-negative byte count"));
            }
        } else if (entry.key == kTestMode) {
            if (!parse_boolean(value, test_mode)) {
                return std::unexpected(error(AccessServerConfigErrorCode::InvalidValue, entry.line, entry.key,
                                             "expected true or false"));
            }
        } else if (entry.key == kClientMetadataMode) {
            if (value == "direct") {
                client_metadata_options.mode = ClientMetadataMode::Direct;
            } else if (value == "trusted_proxy") {
                client_metadata_options.mode = ClientMetadataMode::TrustedProxy;
            } else if (value == "legacy_headers") {
                client_metadata_options.mode = ClientMetadataMode::LegacyHeaders;
            } else {
                return std::unexpected(error(AccessServerConfigErrorCode::InvalidValue, entry.line, entry.key,
                                             "expected direct, trusted_proxy, or legacy_headers"));
            }
        } else if (entry.key == kTrustedProxyCidrs) {
            auto parsed = parse_trusted_proxy_cidrs(value, entry.line);
            if (!parsed) {
                return std::unexpected(std::move(parsed.error()));
            }
            client_metadata_options.trusted_proxy_cidrs = std::move(*parsed);
        } else if (entry.key == kAccessLogQueryAllowlist || entry.key == kAccessLogSensitiveQueryKeys) {
            const bool sensitive = entry.key == kAccessLogSensitiveQueryKeys;
            auto parsed = parse_query_keys(value, entry.line, entry.key, sensitive);
            if (!parsed) {
                return std::unexpected(std::move(parsed.error()));
            }
            if (sensitive) {
                access_log_options.additional_sensitive_query_keys = std::move(*parsed);
            } else {
                access_log_options.query_allowlist = std::move(*parsed);
            }
        } else if (entry.key == kAccessLogQueryHashEnabled) {
            if (!parse_boolean(value, access_log_options.query_hash_enabled)) {
                return std::unexpected(error(AccessServerConfigErrorCode::InvalidValue, entry.line, entry.key,
                                             "expected true or false"));
            }
        } else if (entry.key == kAccessLogSuccessSampleRate) {
            if (!parse_unsigned(value, access_log_options.success_sample_rate_bps) ||
                access_log_options.success_sample_rate_bps > kAccessLogSampleScale) {
                return std::unexpected(error(AccessServerConfigErrorCode::InvalidValue, entry.line, entry.key,
                                             "expected basis points in range 0..10000"));
            }
        } else if (entry.key == kAccessLogMaxPathBytes || entry.key == kAccessLogMaxQueryBytes) {
            std::size_t maximum = 0;
            if (!parse_unsigned(value, maximum) || maximum < kMinAccessLogFieldBytes ||
                maximum > kMaxAccessLogFieldBytes) {
                return std::unexpected(error(AccessServerConfigErrorCode::InvalidValue, entry.line, entry.key,
                                             "expected a byte limit in range 16..65536"));
            }
            if (entry.key == kAccessLogMaxPathBytes) {
                access_log_options.max_path_bytes = maximum;
            } else {
                access_log_options.max_query_bytes = maximum;
            }
        } else if (entry.key == kUpstreamTlsMode) {
            if (value == "legacy_insecure") {
                upstream_tls_client_policy.verification = UpstreamTlsVerificationMode::LegacyInsecure;
            } else if (value == "system_ca") {
                upstream_tls_client_policy.verification = UpstreamTlsVerificationMode::SystemCa;
            } else if (value == "custom_ca") {
                upstream_tls_client_policy.verification = UpstreamTlsVerificationMode::CustomCa;
            } else {
                return std::unexpected(error(AccessServerConfigErrorCode::InvalidValue, entry.line, entry.key,
                                             "expected legacy_insecure, system_ca, or custom_ca"));
            }
        } else if (entry.key == kUpstreamTlsCaFile) {
            if (value.size() > kMaxUpstreamTlsCaFileBytes || value.find('\0') != std::string_view::npos) {
                return std::unexpected(error(AccessServerConfigErrorCode::InvalidValue, entry.line, entry.key,
                                             "expected a path no longer than 4096 bytes"));
            }
            upstream_tls_client_policy.ca_file = entry.value;
        } else if (entry.key == kDnsMode) {
            if (value == "system") {
                dns_mode = AccessDnsMode::System;
            } else if (value == "override") {
                dns_mode = AccessDnsMode::Override;
            } else {
                return std::unexpected(error(AccessServerConfigErrorCode::InvalidValue, entry.line, entry.key,
                                             "expected system or override"));
            }
        } else if (entry.key == kDnsServers) {
            auto parsed = parse_dns_nameservers(value, entry.line);
            if (!parsed) {
                return std::unexpected(std::move(parsed.error()));
            }
            dns_override_nameservers = *parsed;
            dns_servers_present = true;
        } else if (entry.key == kDnsResolverConfig) {
            if (value.empty() || value.size() > kMaxDnsResolverConfigPathBytes ||
                value.find('\0') != std::string_view::npos) {
                return std::unexpected(error(AccessServerConfigErrorCode::InvalidValue, entry.line, entry.key,
                                             "expected a non-empty path no longer than 4096 bytes"));
            }
            dns_resolver_config_path = entry.value;
            dns_resolver_config_present = true;
        } else if (entry.key == kProjectsDataId) {
            watcher_options.project_list_data_id = entry.value;
        } else if (entry.key == kRouteDataIdPrefix) {
            watcher_options.project_route_data_id_prefix = entry.value;
        } else if (entry.key == kRouteGroup) {
            watcher_options.project_route_group = entry.value;
        } else if (entry.key == kGrayDataId) {
            gray_options.data_id = entry.value;
        } else if (entry.key == kNamingGroup) {
            service_discovery_options.group = entry.value;
            gray_options.group = entry.value;
        } else if (entry.key == kZone) {
            service_discovery_options.zone = entry.value;
        } else if (entry.key == kNacosServers) {
            auto parsed = parse_nacos_servers(value, entry.line);
            if (!parsed) {
                return std::unexpected(std::move(parsed.error()));
            }
            nacos_params.server_hosts = std::move(*parsed);
        } else if (entry.key == kNacosHttpPort) {
            if (!parse_unsigned(value, nacos_params.http_port) || nacos_params.http_port == 0) {
                return std::unexpected(error(AccessServerConfigErrorCode::InvalidValue, entry.line, entry.key,
                                             "expected a port in range 1..65535"));
            }
        } else if (entry.key == kNacosGrpcPort) {
            if (!parse_unsigned(value, nacos_params.grpc_port) || nacos_params.grpc_port == 0) {
                return std::unexpected(error(AccessServerConfigErrorCode::InvalidValue, entry.line, entry.key,
                                             "expected a port in range 1..65535"));
            }
        } else if (entry.key == kNacosNamespace) {
            nacos_params.namespace_id = entry.value;
        } else if (entry.key == kNacosTenant) {
            nacos_params.tenant = entry.value;
        } else if (entry.key == kNacosUsername) {
            nacos_params.username = entry.value;
        } else if (entry.key == kNacosPassword) {
            nacos_params.password = entry.value;
        } else if (entry.key == kNacosClientVersion) {
            nacos_params.client_version = entry.value;
        } else if (entry.key == kCatAppKey) {
            cat_params.app_key = entry.value;
            cat_setting_present = cat_setting_present || !entry.value.empty();
        } else if (entry.key == kCatHostname) {
            cat_params.hostname = entry.value;
            cat_setting_present = cat_setting_present || !entry.value.empty();
        } else if (entry.key == kCatIp) {
            cat_params.ip = entry.value;
            cat_setting_present = cat_setting_present || !entry.value.empty();
        } else if (entry.key == kCatRouters || entry.key == kCatCollectors) {
            auto parsed = parse_cat_endpoints(value, entry.line, entry.key, entry.key == kCatRouters, cat_params);
            if (!parsed) {
                return std::unexpected(std::move(parsed.error()));
            }
            cat_setting_present = cat_setting_present || !entry.value.empty();
        } else {
            return std::unexpected(error(AccessServerConfigErrorCode::UnknownKey, entry.line, entry.key,
                                         "unknown access-server setting"));
        }
    }

    if (watcher_options.project_list_data_id.empty() || watcher_options.project_route_data_id_prefix.empty() ||
        watcher_options.project_route_group.empty() || gray_options.data_id.empty() || gray_options.group.empty() ||
        tls_certificate_options.data_id.empty() || tls_certificate_options.group.empty() ||
        service_discovery_options.group.empty()) {
        return std::unexpected(
                error(AccessServerConfigErrorCode::InvalidValue, 0, {}, "Nacos data IDs and groups must be non-empty"));
    }
    if (!tls_enabled && http3_enabled) {
        return std::unexpected(error(AccessServerConfigErrorCode::InvalidValue, 0, kHttp3Enabled,
                                     "HTTP/3 requires ACCESS_SERVER_TLS_ENABLED=true"));
    }
    if (client_metadata_options.mode == ClientMetadataMode::TrustedProxy &&
        client_metadata_options.trusted_proxy_cidrs.empty()) {
        return std::unexpected(error(AccessServerConfigErrorCode::InvalidValue, 0, kTrustedProxyCidrs,
                                     "trusted_proxy mode requires at least one trusted proxy CIDR"));
    }
    if (client_metadata_options.mode != ClientMetadataMode::TrustedProxy &&
        !client_metadata_options.trusted_proxy_cidrs.empty()) {
        return std::unexpected(error(AccessServerConfigErrorCode::InvalidValue, 0, kTrustedProxyCidrs,
                                     "trusted proxy CIDRs are only valid in trusted_proxy mode"));
    }
    if (upstream_tls_client_policy.verification == UpstreamTlsVerificationMode::CustomCa &&
        upstream_tls_client_policy.ca_file.empty()) {
        return std::unexpected(error(AccessServerConfigErrorCode::InvalidValue, 0, kUpstreamTlsCaFile,
                                     "custom_ca mode requires a non-empty CA file path"));
    }
    if (upstream_tls_client_policy.verification != UpstreamTlsVerificationMode::CustomCa &&
        !upstream_tls_client_policy.ca_file.empty()) {
        return std::unexpected(error(AccessServerConfigErrorCode::InvalidValue, 0, kUpstreamTlsCaFile,
                                     "a CA file path is only valid in custom_ca mode"));
    }
    if (dns_mode == AccessDnsMode::System && dns_servers_present) {
        return std::unexpected(error(AccessServerConfigErrorCode::InvalidValue, 0, kDnsServers,
                                     "DNS server overrides require ACCESS_SERVER_DNS_MODE=override"));
    }
    if (dns_mode == AccessDnsMode::Override && !dns_servers_present) {
        return std::unexpected(error(AccessServerConfigErrorCode::InvalidValue, 0, kDnsServers,
                                     "override mode requires at least one DNS server"));
    }
    if (dns_mode == AccessDnsMode::Override && dns_resolver_config_present) {
        return std::unexpected(error(AccessServerConfigErrorCode::InvalidValue, 0, kDnsResolverConfig,
                                     "a resolver configuration path is only valid in system mode"));
    }
    if (activation_endpoint_options.enabled) {
        if (!valid_instance_id(activation_endpoint_options.instance_id)) {
            return std::unexpected(error(AccessServerConfigErrorCode::InvalidValue, 0, kActivationInstanceId,
                                         "activation evidence requires a 1-255 byte instance ID using "
                                         "[A-Za-z0-9._:-]"));
        }
        if (!valid_activation_token(activation_endpoint_options.bearer_token)) {
            return std::unexpected(error(AccessServerConfigErrorCode::InvalidValue, 0, kActivationToken,
                                         "activation evidence requires a 32-512 byte printable ASCII token"));
        }
    } else if (!activation_endpoint_options.instance_id.empty() || !activation_endpoint_options.bearer_token.empty()) {
        const std::string_view setting =
                activation_endpoint_options.instance_id.empty() ? kActivationToken : kActivationInstanceId;
        return std::unexpected(error(AccessServerConfigErrorCode::InvalidValue, 0, setting,
                                     "activation identity and token are only valid "
                                     "when activation evidence is enabled"));
    }
    client_metadata_options.connection_secure = tls_enabled;
    auto nacos_config = nacos::NacosClientConfig::create(std::move(nacos_params));
    if (!nacos_config) {
        return std::unexpected(nacos_error(nacos_config.error()));
    }
    std::optional<cat::CatClientConfig> cat_config;
    if (cat_setting_present) {
        auto created = cat::CatClientConfig::create(std::move(cat_params));
        if (!created) {
            return std::unexpected(error(AccessServerConfigErrorCode::InvalidValue, 0, {},
                                         "CAT_APP_KEY, CAT_HOSTNAME, CAT_IP and at "
                                         "least one CAT endpoint are required"));
        }
        cat_config = std::move(*created);
    }
    if (!metrics_port) {
        if (listen_port == std::numeric_limits<std::uint16_t>::max()) {
            return std::unexpected(error(AccessServerConfigErrorCode::InvalidValue, 0, kMetricsListenPort,
                                         "metrics port must be explicit when the HTTP port is 65535"));
        }
        metrics_port = static_cast<std::uint16_t>(listen_port + 1);
    }
    http::HttpServerOptions http_options;
    http_options.tls.enabled = tls_enabled;
    http_options.tls.alpn = {"h2", "http/1.1"};
    http_options.http3.enabled = http3_enabled;
    return AccessServerConfig(
            net::SocketAddress(listen_ip, listen_port), std::move(http_options),
            net::SocketAddress(metrics_ip.value_or(listen_ip), *metrics_port), std::move(activation_endpoint_options),
            std::chrono::milliseconds(timeout_millis), max_request_body, test_mode, std::move(client_metadata_options),
            std::move(access_log_options), std::move(upstream_tls_client_policy), dns_mode,
            std::move(dns_resolver_config_path), std::move(dns_override_nameservers), std::move(cat_config),
            std::move(*nacos_config), std::move(watcher_options), std::move(gray_options),
            std::move(tls_certificate_options), std::move(service_discovery_options));
}

std::expected<AccessDnsServiceOptions, AccessServerConfigError> AccessServerConfig::resolve_dns_options() const {
    AccessDnsServiceOptions options;
    if (dns_mode_ == AccessDnsMode::Override) {
        options.client.nameservers = dns_override_nameservers_;
        options.client.timeout = std::chrono::milliseconds(2000);
        options.client.attempts = 2;
        options.source = AccessDnsConfigSource::Override;
        return options;
    }

    auto loaded = dns::load_system_resolver_config(dns_resolver_config_path_.c_str());
    if (!loaded) {
        const dns::ResolverConfigError &source = loaded.error();
        AccessServerConfigErrorCode code = AccessServerConfigErrorCode::InvalidValue;
        if (source.code == dns::ResolverConfigErrorCode::OpenFailed) {
            code = AccessServerConfigErrorCode::OpenFailed;
        } else if (source.code == dns::ResolverConfigErrorCode::ReadFailed) {
            code = AccessServerConfigErrorCode::ReadFailed;
        }
        std::string detail = "failed to load DNS resolver configuration: ";
        detail.append(dns::resolver_config_error_name(source.code));
        return std::unexpected(error(code, source.line, kDnsResolverConfig, std::move(detail)));
    }
    options.client.nameservers = loaded->nameservers;
    options.client.timeout = loaded->timeout;
    options.client.attempts = loaded->attempts;
    options.client.rotate_nameservers = loaded->rotate;
    options.source = AccessDnsConfigSource::System;
    options.unsupported = loaded->unsupported;
    return options;
}

} // namespace fiber::access_server
