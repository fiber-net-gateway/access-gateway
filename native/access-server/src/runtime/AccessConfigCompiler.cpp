#include "AccessConfigCompiler.h"

#include "../config/AccessConfigCodec.h"

#include <utility>

#include <fiber/common/Assert.h>

namespace fiber::access_server {

CompiledProjectConfigResult AccessConfigCompiler::compile_project(std::string_view project, std::string_view content,
                                                                  std::optional<std::int32_t> published_version,
                                                                  bool force_compile) {
    FIBER_ASSERT(loop_->in_loop());
    auto parsed = parse_project_config(content);
    if (!parsed) {
        return std::unexpected(AccessProjectCompileFailure{
                .stage = AccessProjectCompileFailureStage::Decode,
                .error = std::move(parsed.error()),
        });
    }
    if (!*parsed) {
        return CompiledProjectConfig{};
    }

    const std::int32_t version = (*parsed)->version;
    if (!force_compile && published_version && *published_version == version) {
        return CompiledProjectConfig{
                .version = version,
                .compilation_skipped = true,
        };
    }
    if (!script_runtime_) {
        script_runtime_.emplace();
    }
    auto compiled = compile_project_config(project, **parsed, script_runtime_->compiler_adapter());
    if (!compiled) {
        return std::unexpected(AccessProjectCompileFailure{
                .stage = AccessProjectCompileFailureStage::Compile,
                .observed_version = version,
                .error = std::move(compiled.error()),
        });
    }
    return CompiledProjectConfig{
            .version = version,
            .snapshot = std::move(*compiled),
    };
}

CompiledTlsCertificateConfigResult AccessConfigCompiler::compile_tls(std::string_view content,
                                                                     const TlsCertificateVersionState &published_state,
                                                                     bool quic_enabled, bool prepare_bootstrap,
                                                                     bool force_compile) {
    FIBER_ASSERT(loop_->in_loop());
    auto parsed = parse_tls_certificate_config(content);
    if (!parsed) {
        return std::unexpected(std::move(parsed.error()));
    }
    if (!*parsed) {
        return CompiledTlsCertificateConfig{};
    }

    const std::uint64_t version = (*parsed)->version;
    const TlsCertificateContentDigest digest = TlsCertificateStore::content_digest(content);
    if (!force_compile && published_state.active && version <= published_state.version) {
        return CompiledTlsCertificateConfig{
                .version = version,
                .content_digest = digest,
                .compilation_skipped = true,
        };
    }
    auto prepared = TlsCertificateStore::prepare(**parsed, digest, quic_enabled, prepare_bootstrap);
    if (!prepared) {
        return std::unexpected(std::move(prepared.error()));
    }
    return CompiledTlsCertificateConfig{
            .version = version,
            .content_digest = digest,
            .prepared = std::move(*prepared),
    };
}

} // namespace fiber::access_server
