#ifndef FIBER_ACCESS_SERVER_ACCESS_CONFIG_H
#define FIBER_ACCESS_SERVER_ACCESS_CONFIG_H

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fiber::access_server {

inline constexpr std::string_view kProjectListDataId = "ploto.unified-access.projects";
inline constexpr std::string_view kProjectRouteDataIdPrefix = "ploto.unified-access.route.";
inline constexpr std::string_view kProjectRouteGroup = "ACCESS-SERVER";
inline constexpr std::string_view kGrayConfigDataId = "ploto.unified-access.gray-match";
inline constexpr std::string_view kTempClusterConfigDataId = "ploto.test.temp-cluster-apps";
inline constexpr std::string_view kDefaultNacosGroup = "DEFAULT_GROUP";
// Edge functions: one versioned index plus one plain-text script per function
// (docs/edge-functions-and-waf-design.md). Same group as project routes.
inline constexpr std::string_view kEdgeFunctionsIndexDataId = "ploto.unified-access.edge-functions";
inline constexpr std::string_view kEdgeFunctionDataIdPrefix = "ploto.unified-access.edge-function.";

enum class RouteType : std::uint8_t {
    Proxy,
    Response,
    Script,
};

// INHERIT is only valid for a route transport profile. Process-level policy
// must resolve to one of the three concrete modes before startup.
enum class UpstreamTlsVerificationMode : std::uint8_t {
    Inherit,
    LegacyInsecure,
    SystemCa,
    CustomCa,
};

struct RouteUpstreamTlsConfig {
    std::uint64_t generation = 0;
    UpstreamTlsVerificationMode verification = UpstreamTlsVerificationMode::Inherit;
    std::optional<std::string> ca_pem;
    std::optional<std::string> server_name;
    std::optional<std::string> verify_name;
    std::optional<std::string> client_identity_ref;

    bool operator==(const RouteUpstreamTlsConfig &) const = default;
};

enum class BodyType : std::uint8_t {
    Text,
    Base64,
    Template,
};

inline constexpr std::uint8_t kDefaultGzipLevel = 6;

struct ResponseGzipConfig {
    bool enabled = false;
    std::uint8_t level = kDefaultGzipLevel;

    bool operator==(const ResponseGzipConfig &) const = default;
};

enum class HttpsStrategy : std::uint16_t {
    NotRequired = 0,
    Redirect301 = 301,
    Redirect302 = 302,
    Redirect307 = 307,
    Redirect308 = 308,
};

inline constexpr std::uint8_t kNetVdi = 1U << 0U;
inline constexpr std::uint8_t kNetOffice = 1U << 1U;
inline constexpr std::uint8_t kNetInternet = 1U << 2U;
inline constexpr std::uint8_t kNetCustom = 0;

struct HostStrategyConfig {
    // Java initializes this field to S_NOT_MUST, but an explicit null or an
    // unknown enum value replaces it with null.
    std::optional<HttpsStrategy> https = HttpsStrategy::NotRequired;
    std::uint8_t net_mask = 0;

    bool operator==(const HostStrategyConfig &) const = default;
};

struct HostConfigEntry {
    std::string pattern;
    std::optional<HostStrategyConfig> strategy;

    bool operator==(const HostConfigEntry &) const = default;
};

struct StringConfigEntry {
    std::string name;
    std::optional<std::string> value;

    bool operator==(const StringConfigEntry &) const = default;
};

using StringConfigMap = std::vector<StringConfigEntry>;
using NullableStringSet = std::vector<std::optional<std::string>>;

struct RouteBodyConfig {
    std::optional<BodyType> type;
    std::optional<std::string> content;

    bool operator==(const RouteBodyConfig &) const = default;
};

struct RouteConfig {
    std::optional<std::string> path;
    // Missing/null matches every request method. A configured value is matched
    // byte-for-byte because HTTP method names are case-sensitive tokens.
    std::optional<std::string> method;
    // Java initializes this field to PROXY. Explicit null and unknown enum
    // values replace it with null and are rejected later by route compilation.
    std::optional<RouteType> type = RouteType::Proxy;
    std::optional<std::string> service;
    std::optional<std::string> cluster;
    NullableStringSet addresses;
    std::optional<std::string> condition;
    StringConfigMap proxy_headers;
    StringConfigMap response_headers;
    StringConfigMap context;
    std::optional<std::string> rewrite;
    std::int32_t status = 0;
    std::optional<RouteBodyConfig> body;
    std::optional<ResponseGzipConfig> gzip;
    std::optional<std::int64_t> timeout_millis;
    std::optional<std::int64_t> max_client_body_size;
    std::optional<std::int64_t> max_proxy_body_size;
    // Opt-in for request-body edge functions (design §3.2): only PROXY routes
    // with this flag buffer their request body (within limits) for
    // request_body-phase functions. Absent/false keeps the zero-cost stream.
    std::optional<bool> edge_function_body;
    std::optional<std::int64_t> websocket_timeout_millis;
    std::optional<bool> flush;
    NullableStringSet allows;
    std::optional<std::string> script;
    std::optional<RouteUpstreamTlsConfig> upstream_tls;

    bool operator==(const RouteConfig &) const = default;
};

struct ProjectConfig {
    std::int32_t version = 0;
    std::optional<std::vector<HostConfigEntry>> hosts;
    std::optional<std::vector<std::optional<RouteConfig>>> routes;

    bool operator==(const ProjectConfig &) const = default;
};

struct GrayMatchConfigEntry {
    std::string entry;
    std::int32_t ratio = 0;
    NullableStringSet cidrs;

    bool operator==(const GrayMatchConfigEntry &) const = default;
};

using GrayMatchConfig = std::vector<GrayMatchConfigEntry>;

// Java ReqTraceSpecifiedClusterDecider's ploto.test.temp-cluster-apps content:
// service name -> clusters for which a request-specified cluster with no
// instance must NOT fall back to the configured cluster (the empty-cluster
// error is kept instead).
struct TempClusterConfigEntry {
    std::string service;
    std::vector<std::string> clusters;

    bool operator==(const TempClusterConfigEntry &) const = default;
};

using TempClusterConfig = std::vector<TempClusterConfigEntry>;

// Edge-function index (ploto.unified-access.edge-functions): which functions
// exist, where their script payloads live, and how they are mounted. The wire
// carries no policy semantics — WAF and any future consumers are control-plane
// compile targets (docs/edge-functions-and-waf-design.md).
struct EdgeFunctionProjectsSelector {
    std::vector<std::string> include;
    std::vector<std::string> exclude;

    bool operator==(const EdgeFunctionProjectsSelector &) const = default;
};

// Sampling denominator: mount-level ratio is basis points out of this value.
// 10000 (kEdgeFunctionFullRatio) means must-run; below it is a sticky sample
// over crc32(peer IP) % kEdgeFunctionFullRatio; 0 is mounted-but-disabled.
inline constexpr std::int32_t kEdgeFunctionFullRatio = 10000;

// Execution phase of one function (design §3.2). Head functions run before
// route matching on every matching request; body functions run after the
// route matched, only for opted-in PROXY routes whose body passed the
// content-type and size gates, with the buffered body replayed upstream.
enum class EdgeFunctionPhase : std::uint8_t {
    RequestHead,
    RequestBody,
    Count,
};

struct EdgeFunctionMountConfig {
    EdgeFunctionProjectsSelector projects;
    std::int32_t priority = 0;
    // [0, kEdgeFunctionFullRatio]; the codec rejects anything outside.
    std::int32_t ratio = kEdgeFunctionFullRatio;

    bool operator==(const EdgeFunctionMountConfig &) const = default;
};

struct EdgeFunctionIndexEntry {
    // Single slug identity ([a-z0-9-], alphanumeric edges): the script Data ID
    // is derived from it and it feeds metric labels, event logs, and evidence.
    // Display names are console-DB-only; renaming is delete-and-create.
    std::string name;
    // Lowercase-hex sha256 of the script payload. The watcher verifies script
    // content against it and uses it for content-addressed compile reuse.
    std::string sha256_hex;
    // request_head (default) or request_body; a head-phase script that
    // references $req.body fails to compile (fail-closed).
    EdgeFunctionPhase phase = EdgeFunctionPhase::RequestHead;
    std::vector<EdgeFunctionMountConfig> mounts;

    bool operator==(const EdgeFunctionIndexEntry &) const = default;
};

struct EdgeFunctionIndexConfig {
    std::int32_t version = 0;
    // Declaration order is the same-priority execution tie-break.
    std::vector<EdgeFunctionIndexEntry> functions;

    bool operator==(const EdgeFunctionIndexConfig &) const = default;
};

// Script Data ID of a function: derived from the slug name (docs
// edge-functions-and-waf-design.md §3.1). The wire index never carries it —
// the watcher and validator call this instead.
[[nodiscard]] inline std::string edge_function_script_data_id(std::string_view function_name) {
    std::string data_id(kEdgeFunctionDataIdPrefix);
    data_id.append(function_name);
    return data_id;
}

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_ACCESS_CONFIG_H
