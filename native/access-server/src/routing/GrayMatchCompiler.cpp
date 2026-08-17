#include "GrayMatchCompiler.h"

#include "../config/AccessConfigLimits.h"

#include <optional>
#include <utility>

namespace fiber::access_server {

bool recognized_gray_match_entry(std::string_view entry) noexcept {
    return entry == "vdi" || entry == "desktop" || entry == "internet" || entry == "custom";
}

std::expected<CompiledGrayMatchConfig, AccessConfigError>
compile_gray_match_config(const GrayMatchConfig &config) {
    auto within_limits = validate_gray_match_config_limits(config);
    if (!within_limits) {
        return std::unexpected(std::move(within_limits.error()));
    }

    CompiledGrayMatchConfig compiled;
    compiled.rules_.reserve(config.size());
    for (const GrayMatchConfigEntry &entry: config) {
        if (!recognized_gray_match_entry(entry.entry) || entry.ratio < 0 ||
            (entry.ratio == 0 && entry.cidrs.empty())) {
            continue;
        }

        CompiledGrayMatchRule rule{
                .entry = entry.entry,
                .ratio = entry.ratio,
        };
        rule.cidrs.reserve(entry.cidrs.size());
        for (const std::optional<std::string> &text: entry.cidrs) {
            if (!text) {
                continue;
            }
            auto cidr = Cidr::parse(*text, entry.entry);
            if (cidr) {
                rule.cidrs.push_back(std::move(*cidr));
            }
        }
        compiled.rules_.push_back(std::move(rule));
    }
    return compiled;
}

} // namespace fiber::access_server
