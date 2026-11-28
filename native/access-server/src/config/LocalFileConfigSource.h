#ifndef FIBER_ACCESS_SERVER_LOCAL_FILE_CONFIG_SOURCE_H
#define FIBER_ACCESS_SERVER_LOCAL_FILE_CONFIG_SOURCE_H

#include "AccessConfig.h"
#include "TlsCertificateConfig.h"

#include <cstddef>
#include <expected>
#include <map>
#include <string>
#include <string_view>

#include <fiber/common/IoError.h>

namespace fiber::access_server {

struct LocalFileConfigLayout {
    std::string project_list_data_id = std::string(kProjectListDataId);
    std::string project_route_data_id_prefix = std::string(kProjectRouteDataIdPrefix);
    std::string project_route_group = std::string(kProjectRouteGroup);
    std::string tls_certificates_data_id = std::string(kTlsCertificatesDataId);
    std::string tls_certificates_group = std::string(kTlsCertificatesGroup);
    std::string edge_functions_data_id = std::string(kEdgeFunctionsIndexDataId);
    std::string edge_functions_group = std::string(kProjectRouteGroup);
    std::string edge_function_script_data_id_prefix = std::string(kEdgeFunctionDataIdPrefix);
    std::string gray_data_id = std::string(kGrayConfigDataId);
    std::string gray_group = std::string(kDefaultNacosGroup);
    std::string temp_cluster_data_id = std::string(kTempClusterConfigDataId);
    std::string temp_cluster_group = std::string(kDefaultNacosGroup);
    std::size_t max_file_bytes = 8U << 20U;
    std::size_t max_directory_entries = 2048;
};

struct LocalFileConfigKey {
    std::string data_id;
    std::string group;

    auto operator<=>(const LocalFileConfigKey &) const = default;
};

struct LocalFileConfigDocument {
    std::string content;
    std::string md5;
};

struct LocalFileConfigError {
    common::IoErr io_error = common::IoErr::Invalid;
    std::string path;
    std::string detail;
};

struct LocalFileConfigSnapshot {
    std::map<LocalFileConfigKey, LocalFileConfigDocument> documents;
};

// Reads a local configuration directory (JSON and YAML files) and normalizes it
// into the exact (data_id, group) -> (wire_content, md5) snapshot contract
// consumed by AccessConfigWatcher, TlsCertificateWatcher, GrayConfigWatcher,
// EdgeFunctionsWatcher, and TempClusterConfigWatcher.
//
// Supported directory layout under <dir>/:
//   - projects.txt / projects / <project_list_data_id>   (optional; auto-derived
//     from routes/ when omitted)
//   - routes/<project>.json | .yaml | .yml               (or <route_prefix><project>.*)
//   - tls-certificates.json | .yaml | .yml               (optional unless TLS enabled)
//   - gray-match.json | .yaml | .yml                     (optional)
//   - temp-cluster-apps.json | .yaml | .yml              (optional)
//   - edge-functions.json | .yaml | .yml                 (optional)
//   - edge-functions/<name>.js | <name>                  (optional scripts)
class LocalFileConfigSource final {
public:
    explicit LocalFileConfigSource(std::string directory, LocalFileConfigLayout layout = {});

    [[nodiscard]] const std::string &directory() const noexcept { return directory_; }
    [[nodiscard]] const LocalFileConfigLayout &layout() const noexcept { return layout_; }

    [[nodiscard]] std::expected<LocalFileConfigSnapshot, LocalFileConfigError> load() const;

private:
    std::string directory_;
    LocalFileConfigLayout layout_;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_LOCAL_FILE_CONFIG_SOURCE_H
