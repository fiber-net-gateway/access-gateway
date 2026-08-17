#ifndef FIBER_ACCESS_SERVER_GRAY_MATCH_COMPILER_H
#define FIBER_ACCESS_SERVER_GRAY_MATCH_COMPILER_H

#include "../config/AccessConfig.h"
#include "../config/AccessConfigError.h"
#include "Cidr.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fiber::access_server {

struct CompiledGrayMatchRule {
    std::string entry;
    std::int32_t ratio = 0;
    std::vector<Cidr> cidrs;
};

class CompiledGrayMatchConfig final {
public:
    [[nodiscard]] std::span<const CompiledGrayMatchRule> rules() const noexcept { return rules_; }
    [[nodiscard]] std::size_t rule_count() const noexcept { return rules_.size(); }

private:
    friend std::expected<CompiledGrayMatchConfig, AccessConfigError>
    compile_gray_match_config(const GrayMatchConfig &config);

    std::vector<CompiledGrayMatchRule> rules_;
};

[[nodiscard]] bool recognized_gray_match_entry(std::string_view entry) noexcept;

// Compiles the permissive Java-compatible runtime model. Strict publication
// validation may reject entries before invoking this function.
[[nodiscard]] std::expected<CompiledGrayMatchConfig, AccessConfigError>
compile_gray_match_config(const GrayMatchConfig &config);

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_GRAY_MATCH_COMPILER_H
