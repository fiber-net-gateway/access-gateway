#ifndef FIBER_ACCESS_SERVER_ACCESS_CONFIG_COMPILER_H
#define FIBER_ACCESS_SERVER_ACCESS_CONFIG_COMPILER_H

#include "../config/AccessConfigError.h"
#include "../routing/AccessScriptCompiler.h"
#include "../routing/ProjectConfigCompiler.h"
#include "TlsCertificateStore.h"

#include <cstdint>
#include <expected>
#include <optional>
#include <string_view>

#include <fiber/common/NonCopyable.h>
#include <fiber/common/NonMovable.h>
#include <fiber/event/EventLoop.h>

namespace fiber::access_server {

enum class AccessProjectCompileFailureStage : std::uint8_t {
    Decode,
    Compile,
};

struct AccessProjectCompileFailure {
    AccessProjectCompileFailureStage stage = AccessProjectCompileFailureStage::Decode;
    std::optional<std::int32_t> observed_version;
    AccessConfigError error;
};

struct CompiledProjectConfig {
    std::optional<std::int32_t> version;
    std::optional<ProjectRouteSnapshot> snapshot;
    bool compilation_skipped = false;
};

using CompiledProjectConfigResult = std::expected<CompiledProjectConfig, AccessProjectCompileFailure>;

struct CompiledTlsCertificateConfig {
    std::optional<std::uint64_t> version;
    TlsCertificateContentDigest content_digest{};
    std::optional<TlsCertificateStore::PreparedUpdate> prepared;
    bool compilation_skipped = false;
};

using CompiledTlsCertificateConfigResult = std::expected<CompiledTlsCertificateConfig, TlsCertificateConfigError>;

// CPU-only configuration compiler. Every method is compiler-loop-only and may
// allocate or perform bounded local CPU work, but never accesses Nacos or
// runtime publication state.
class AccessConfigCompiler final : public common::NonCopyable, public common::NonMovable {
public:
    explicit AccessConfigCompiler(event::EventLoop &loop) noexcept : loop_(&loop) {}

    [[nodiscard]] event::EventLoop &loop() const noexcept { return *loop_; }
    [[nodiscard]] CompiledProjectConfigResult compile_project(std::string_view project, std::string_view content,
                                                              std::optional<std::int32_t> published_version,
                                                              bool force_compile = false);
    [[nodiscard]] CompiledTlsCertificateConfigResult compile_tls(std::string_view content,
                                                                 const TlsCertificateVersionState &published_state,
                                                                 bool quic_enabled, bool prepare_bootstrap,
                                                                 bool force_compile = false);

private:
    event::EventLoop *loop_ = nullptr;
    std::optional<AccessScriptCompiler> script_compiler_;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_ACCESS_CONFIG_COMPILER_H
