#include "LocalFileConfigSource.h"

#include "LocalFileConfigYaml.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <openssl/evp.h>

namespace fiber::access_server {
namespace {

[[nodiscard]] std::unexpected<LocalFileConfigError> file_error(common::IoErr io_error, std::string path,
                                                               std::string detail) {
    return std::unexpected(LocalFileConfigError{
            .io_error = io_error,
            .path = std::move(path),
            .detail = std::move(detail),
    });
}

[[nodiscard]] std::string md5_hex(std::string_view content) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digest_len = 0;
    (void) EVP_Digest(content.data(), content.size(), digest.data(), &digest_len, EVP_md5(), nullptr);
    static constexpr char kHex[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(static_cast<std::size_t>(digest_len) * 2U);
    for (unsigned int i = 0; i < digest_len; ++i) {
        hex.push_back(kHex[digest[i] >> 4U]);
        hex.push_back(kHex[digest[i] & 0x0FU]);
    }
    return hex;
}

[[nodiscard]] bool is_yaml_extension(std::string_view ext) noexcept { return ext == ".yaml" || ext == ".yml"; }

[[nodiscard]] bool is_structured_extension(std::string_view ext) noexcept {
    return ext == ".json" || is_yaml_extension(ext);
}

[[nodiscard]] std::expected<std::string, LocalFileConfigError> read_bounded_file(const std::filesystem::path &path,
                                                                                 std::size_t max_bytes) {
    std::error_code ec;
    const auto status = std::filesystem::status(path, ec);
    if (ec) {
        return file_error(common::io_err_from_errno(ec.value()), path.string(), "failed to stat configuration file");
    }
    if (!std::filesystem::is_regular_file(status)) {
        return file_error(common::IoErr::Invalid, path.string(), "configuration path is not a regular file");
    }
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        return file_error(common::io_err_from_errno(ec.value()), path.string(), "failed to inspect file size");
    }
    if (size > max_bytes) {
        return file_error(common::IoErr::Invalid, path.string(), "configuration file exceeds maximum allowed bytes");
    }

    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        return file_error(common::io_err_from_errno(errno), path.string(), "failed to open configuration file");
    }
    std::string content(static_cast<std::size_t>(size), '\0');
    if (size > 0) {
        input.read(content.data(), static_cast<std::streamsize>(size));
        if (!input || static_cast<std::uintmax_t>(input.gcount()) != size) {
            return file_error(common::IoErr::Unknown, path.string(), "failed to read configuration file");
        }
    }
    // Check for unexpected extra bytes (file grew while reading).
    char extra = '\0';
    if (input.read(&extra, 1)) {
        return file_error(common::IoErr::Busy, path.string(), "configuration file changed while being read");
    }
    return content;
}

[[nodiscard]] std::expected<std::string, LocalFileConfigError> read_structured_file(const std::filesystem::path &path,
                                                                                    std::size_t max_bytes) {
    auto raw = read_bounded_file(path, max_bytes);
    if (!raw) {
        return std::unexpected(std::move(raw.error()));
    }
    const std::string ext = path.extension().string();
    if (is_yaml_extension(ext)) {
        auto converted = convert_yaml_to_json(*raw, max_bytes);
        if (!converted) {
            std::string detail = "YAML parse error";
            if (converted.error().line > 0) {
                detail += " at line " + std::to_string(converted.error().line);
            }
            detail += ": " + converted.error().message;
            return file_error(common::IoErr::Invalid, path.string(), std::move(detail));
        }
        return std::move(*converted);
    }
    return raw;
}

[[nodiscard]] std::string normalize_project_list_text(std::string_view raw) {
    // Preserve exact semicolon-delimited Nacos format while also accepting
    // newline-delimited lists with optional # comments in projects.txt.
    std::string normalized;
    std::size_t pos = 0;
    while (pos <= raw.size()) {
        const std::size_t nl = raw.find('\n', pos);
        std::string_view line = nl == std::string_view::npos ? raw.substr(pos) : raw.substr(pos, nl - pos);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
            line.remove_prefix(1);
        }
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) {
            line.remove_suffix(1);
        }
        if (!line.empty() && line.front() != '#') {
            if (!normalized.empty()) {
                normalized.push_back(';');
            }
            normalized.append(line);
        }
        if (nl == std::string_view::npos) {
            break;
        }
        pos = nl + 1;
    }
    return normalized;
}

struct CandidateMatch {
    std::filesystem::path path;
    bool structured = true;
};

[[nodiscard]] std::expected<std::optional<CandidateMatch>, LocalFileConfigError>
find_unique_candidate(const std::filesystem::path &root, const std::vector<CandidateMatch> &candidates,
                      std::string_view resource_name) {
    std::optional<CandidateMatch> matched;
    for (const auto &candidate: candidates) {
        std::error_code ec;
        if (!std::filesystem::exists(candidate.path, ec)) {
            if (ec) {
                return file_error(common::io_err_from_errno(ec.value()), candidate.path.string(),
                                  "failed to inspect configuration path");
            }
            continue;
        }
        if (matched) {
            return file_error(common::IoErr::Invalid, root.string(),
                              "ambiguous duplicate files for " + std::string(resource_name) + ": " +
                                      matched->path.filename().string() + " and " + candidate.path.filename().string());
        }
        matched = candidate;
    }
    return matched;
}

[[nodiscard]] std::expected<void, LocalFileConfigError>
load_optional_structured_resource(const std::filesystem::path &root, std::string_view short_stem,
                                  std::string_view data_id, std::string_view group, std::size_t max_bytes,
                                  LocalFileConfigSnapshot &snapshot) {
    std::vector<CandidateMatch> candidates = {
            {.path = root / (std::string(short_stem) + ".json"), .structured = true},
            {.path = root / (std::string(short_stem) + ".yaml"), .structured = true},
            {.path = root / (std::string(short_stem) + ".yml"), .structured = true},
    };
    if (!data_id.empty()) {
        candidates.push_back({.path = root / std::string(data_id), .structured = true});
        candidates.push_back({.path = root / (std::string(data_id) + ".json"), .structured = true});
        candidates.push_back({.path = root / (std::string(data_id) + ".yaml"), .structured = true});
        candidates.push_back({.path = root / (std::string(data_id) + ".yml"), .structured = true});
    }
    auto match = find_unique_candidate(root, candidates, short_stem);
    if (!match) {
        return std::unexpected(std::move(match.error()));
    }
    if (!*match) {
        return {};
    }
    auto content = read_structured_file((*match)->path, max_bytes);
    if (!content) {
        return std::unexpected(std::move(content.error()));
    }
    LocalFileConfigDocument doc{
            .md5 = md5_hex(*content),
    };
    doc.content = std::move(*content);
    snapshot.documents.emplace(LocalFileConfigKey{.data_id = std::string(data_id), .group = std::string(group)},
                               std::move(doc));
    return {};
}

[[nodiscard]] std::expected<void, LocalFileConfigError>
scan_route_directory(const std::filesystem::path &routes_dir, const LocalFileConfigLayout &layout,
                     std::map<std::string, std::filesystem::path, std::less<>> &project_files) {
    std::error_code ec;
    if (!std::filesystem::exists(routes_dir, ec)) {
        if (ec) {
            return file_error(common::io_err_from_errno(ec.value()), routes_dir.string(),
                              "failed to inspect routes directory");
        }
        return {};
    }
    if (!std::filesystem::is_directory(routes_dir, ec)) {
        return file_error(common::IoErr::Invalid, routes_dir.string(), "routes path is not a directory");
    }
    std::filesystem::directory_iterator it(routes_dir, ec);
    if (ec) {
        return file_error(common::io_err_from_errno(ec.value()), routes_dir.string(),
                          "failed to open routes directory");
    }
    std::size_t entries = 0;
    for (; it != std::filesystem::directory_iterator(); it.increment(ec)) {
        if (ec) {
            return file_error(common::io_err_from_errno(ec.value()), routes_dir.string(),
                              "failed to enumerate routes directory");
        }
        if (++entries > layout.max_directory_entries) {
            return file_error(common::IoErr::Invalid, routes_dir.string(),
                              "routes directory exceeds maximum entry limit");
        }
        const std::filesystem::path entry_path = it->path();
        const std::string filename = entry_path.filename().string();
        if (filename.empty() || filename.front() == '.') {
            continue;
        }
        const std::string ext = entry_path.extension().string();
        if (!is_structured_extension(ext)) {
            continue;
        }
        const std::string project = entry_path.stem().string();
        if (project.empty()) {
            return file_error(common::IoErr::Invalid, entry_path.string(), "route file has empty project name");
        }
        auto [existing, inserted] = project_files.emplace(project, entry_path);
        if (!inserted) {
            return file_error(common::IoErr::Invalid, entry_path.string(),
                              "ambiguous duplicate route files for project '" + project +
                                      "': " + existing->second.string() + " and " + entry_path.string());
        }
    }
    return {};
}

[[nodiscard]] std::expected<void, LocalFileConfigError>
scan_root_route_files(const std::filesystem::path &root, const LocalFileConfigLayout &layout,
                      std::map<std::string, std::filesystem::path, std::less<>> &project_files,
                      std::map<std::string, std::filesystem::path, std::less<>> &script_files) {
    std::error_code ec;
    std::filesystem::directory_iterator it(root, ec);
    if (ec) {
        return file_error(common::io_err_from_errno(ec.value()), root.string(),
                          "failed to open configuration directory");
    }
    std::size_t entries = 0;
    for (; it != std::filesystem::directory_iterator(); it.increment(ec)) {
        if (ec) {
            return file_error(common::io_err_from_errno(ec.value()), root.string(),
                              "failed to enumerate configuration directory");
        }
        if (++entries > layout.max_directory_entries) {
            return file_error(common::IoErr::Invalid, root.string(),
                              "configuration directory exceeds maximum entry limit");
        }
        const std::filesystem::path entry_path = it->path();
        const std::string filename = entry_path.filename().string();
        if (filename.empty() || filename.front() == '.') {
            continue;
        }
        if (!layout.project_route_data_id_prefix.empty() && filename.starts_with(layout.project_route_data_id_prefix)) {
            std::string_view suffix = std::string_view(filename).substr(layout.project_route_data_id_prefix.size());
            const std::string ext = entry_path.extension().string();
            if (is_structured_extension(ext) && suffix.size() > ext.size()) {
                suffix.remove_suffix(ext.size());
            }
            if (suffix.empty()) {
                return file_error(common::IoErr::Invalid, entry_path.string(), "route file has empty project name");
            }
            std::string project(suffix);
            auto [existing, inserted] = project_files.emplace(project, entry_path);
            if (!inserted) {
                return file_error(common::IoErr::Invalid, entry_path.string(),
                                  "ambiguous duplicate route files for project '" + project +
                                          "': " + existing->second.string() + " and " + entry_path.string());
            }
            continue;
        }
        if (!layout.edge_function_script_data_id_prefix.empty() &&
            filename.starts_with(layout.edge_function_script_data_id_prefix)) {
            std::string_view suffix =
                    std::string_view(filename).substr(layout.edge_function_script_data_id_prefix.size());
            if (suffix.ends_with(".js") && suffix.size() > 3) {
                suffix.remove_suffix(3);
            }
            if (!suffix.empty()) {
                std::string name(suffix);
                auto [existing, inserted] = script_files.emplace(name, entry_path);
                if (!inserted) {
                    return file_error(common::IoErr::Invalid, entry_path.string(),
                                      "ambiguous duplicate edge function script for '" + name + "'");
                }
            }
        }
    }
    return {};
}

[[nodiscard]] std::expected<void, LocalFileConfigError>
scan_edge_function_scripts(const std::filesystem::path &scripts_dir, const LocalFileConfigLayout &layout,
                           std::map<std::string, std::filesystem::path, std::less<>> &script_files) {
    std::error_code ec;
    if (!std::filesystem::exists(scripts_dir, ec)) {
        if (ec) {
            return file_error(common::io_err_from_errno(ec.value()), scripts_dir.string(),
                              "failed to inspect edge-functions directory");
        }
        return {};
    }
    if (!std::filesystem::is_directory(scripts_dir, ec)) {
        return file_error(common::IoErr::Invalid, scripts_dir.string(), "edge-functions path is not a directory");
    }
    std::filesystem::directory_iterator it(scripts_dir, ec);
    if (ec) {
        return file_error(common::io_err_from_errno(ec.value()), scripts_dir.string(),
                          "failed to open edge-functions directory");
    }
    std::size_t entries = 0;
    for (; it != std::filesystem::directory_iterator(); it.increment(ec)) {
        if (ec) {
            return file_error(common::io_err_from_errno(ec.value()), scripts_dir.string(),
                              "failed to enumerate edge-functions directory");
        }
        if (++entries > layout.max_directory_entries) {
            return file_error(common::IoErr::Invalid, scripts_dir.string(),
                              "edge-functions directory exceeds maximum entry limit");
        }
        const std::filesystem::path entry_path = it->path();
        const std::string filename = entry_path.filename().string();
        if (filename.empty() || filename.front() == '.') {
            continue;
        }
        std::string name = filename;
        if (name.ends_with(".js") && name.size() > 3) {
            name.resize(name.size() - 3);
        }
        auto [existing, inserted] = script_files.emplace(name, entry_path);
        if (!inserted) {
            return file_error(common::IoErr::Invalid, entry_path.string(),
                              "ambiguous duplicate edge function script for '" + name +
                                      "': " + existing->second.string() + " and " + entry_path.string());
        }
    }
    return {};
}

} // namespace

LocalFileConfigSource::LocalFileConfigSource(std::string directory, LocalFileConfigLayout layout) :
    directory_(std::move(directory)), layout_(std::move(layout)) {}

std::expected<LocalFileConfigSnapshot, LocalFileConfigError> LocalFileConfigSource::load() const {
    if (directory_.empty()) {
        return file_error(common::IoErr::Invalid, directory_, "configuration directory path must not be empty");
    }
    const std::filesystem::path root(directory_);
    std::error_code ec;
    if (!std::filesystem::exists(root, ec)) {
        const common::IoErr io_error = ec ? common::io_err_from_errno(ec.value()) : common::IoErr::NotFound;
        return file_error(io_error, directory_, "configuration directory does not exist");
    }
    if (!std::filesystem::is_directory(root, ec)) {
        return file_error(common::IoErr::Invalid, directory_, "configuration path is not a directory");
    }

    LocalFileConfigSnapshot snapshot;
    std::map<std::string, std::filesystem::path, std::less<>> project_files;
    std::map<std::string, std::filesystem::path, std::less<>> script_files;

    if (auto scanned = scan_route_directory(root / "routes", layout_, project_files); !scanned) {
        return std::unexpected(std::move(scanned.error()));
    }
    if (auto scanned = scan_edge_function_scripts(root / "edge-functions", layout_, script_files); !scanned) {
        return std::unexpected(std::move(scanned.error()));
    }
    if (auto scanned = scan_root_route_files(root, layout_, project_files, script_files); !scanned) {
        return std::unexpected(std::move(scanned.error()));
    }

    for (const auto &[project, path]: project_files) {
        auto content = read_structured_file(path, layout_.max_file_bytes);
        if (!content) {
            return std::unexpected(std::move(content.error()));
        }
        LocalFileConfigDocument doc{
                .md5 = md5_hex(*content),
        };
        doc.content = std::move(*content);
        snapshot.documents.emplace(
                LocalFileConfigKey{
                        .data_id = layout_.project_route_data_id_prefix + project,
                        .group = layout_.project_route_group,
                },
                std::move(doc));
    }

    for (const auto &[name, path]: script_files) {
        auto content = read_bounded_file(path, layout_.max_file_bytes);
        if (!content) {
            return std::unexpected(std::move(content.error()));
        }
        LocalFileConfigDocument doc{
                .md5 = md5_hex(*content),
        };
        doc.content = std::move(*content);
        snapshot.documents.emplace(
                LocalFileConfigKey{
                        .data_id = layout_.edge_function_script_data_id_prefix + name,
                        .group = layout_.edge_functions_group,
                },
                std::move(doc));
    }

    // Explicit project list file takes precedence; otherwise auto-derive sorted
    // project names from discovered route files.
    std::vector<CandidateMatch> project_list_candidates = {
            {.path = root / "projects.txt", .structured = false},
            {.path = root / "projects", .structured = false},
    };
    if (!layout_.project_list_data_id.empty()) {
        project_list_candidates.push_back({.path = root / layout_.project_list_data_id, .structured = false});
    }
    auto project_list_match = find_unique_candidate(root, project_list_candidates, "projects");
    if (!project_list_match) {
        return std::unexpected(std::move(project_list_match.error()));
    }
    if (*project_list_match) {
        auto raw_list = read_bounded_file((*project_list_match)->path, layout_.max_file_bytes);
        if (!raw_list) {
            return std::unexpected(std::move(raw_list.error()));
        }
        std::string normalized = normalize_project_list_text(*raw_list);
        LocalFileConfigDocument doc{
                .md5 = md5_hex(normalized),
        };
        doc.content = std::move(normalized);
        snapshot.documents.emplace(
                LocalFileConfigKey{
                        .data_id = layout_.project_list_data_id,
                        .group = layout_.project_route_group,
                },
                std::move(doc));
    } else if (!project_files.empty()) {
        std::string derived;
        for (const auto &[project, path]: project_files) {
            (void) path;
            if (!derived.empty()) {
                derived.push_back(';');
            }
            derived.append(project);
        }
        LocalFileConfigDocument doc{
                .md5 = md5_hex(derived),
        };
        doc.content = std::move(derived);
        snapshot.documents.emplace(
                LocalFileConfigKey{
                        .data_id = layout_.project_list_data_id,
                        .group = layout_.project_route_group,
                },
                std::move(doc));
    }

    if (auto res = load_optional_structured_resource(root, "tls-certificates", layout_.tls_certificates_data_id,
                                                     layout_.tls_certificates_group, layout_.max_file_bytes, snapshot);
        !res) {
        return std::unexpected(std::move(res.error()));
    }
    if (auto res = load_optional_structured_resource(root, "gray-match", layout_.gray_data_id, layout_.gray_group,
                                                     layout_.max_file_bytes, snapshot);
        !res) {
        return std::unexpected(std::move(res.error()));
    }
    if (auto res = load_optional_structured_resource(root, "temp-cluster-apps", layout_.temp_cluster_data_id,
                                                     layout_.temp_cluster_group, layout_.max_file_bytes, snapshot);
        !res) {
        return std::unexpected(std::move(res.error()));
    }
    if (auto res = load_optional_structured_resource(root, "edge-functions", layout_.edge_functions_data_id,
                                                     layout_.edge_functions_group, layout_.max_file_bytes, snapshot);
        !res) {
        return std::unexpected(std::move(res.error()));
    }

    return snapshot;
}

} // namespace fiber::access_server
