#ifndef FIBER_ACCESS_SERVER_TEMPLATE_EVALUATOR_H
#define FIBER_ACCESS_SERVER_TEMPLATE_EVALUATOR_H

#include "AccessResult.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "../routing/CompiledTemplate.h"

namespace fiber::access_server {

class EvaluatedTemplate {
public:
    EvaluatedTemplate() noexcept = default;

    [[nodiscard]] static EvaluatedTemplate borrowed(std::string_view value) noexcept;
    [[nodiscard]] static EvaluatedTemplate owned(std::string value) noexcept;

    [[nodiscard]] std::string_view view() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept { return view().size(); }
    [[nodiscard]] bool empty() const noexcept { return view().empty(); }
    [[nodiscard]] bool owns_storage() const noexcept;
    [[nodiscard]] std::string &materialize();
    [[nodiscard]] std::string into_string() &&;

private:
    explicit EvaluatedTemplate(std::variant<std::string_view, std::string> storage) noexcept :
        storage_(std::move(storage)) {}

    std::variant<std::string_view, std::string> storage_{std::string_view{}};
};

struct TemplateEvaluator {
    // On success, appends the Java JsonNode.asText("") compatible expression
    // result to `output`. The callback must preserve existing output bytes.
    // It is an adapter boundary for this repository's script engine and is not
    // a Java VM compatibility promise.
    using Function = Result<void> (*)(void *context, const script::Script &program, std::string_view expression,
                                      std::string &output) noexcept;

    void *context = nullptr;
    Function evaluate = nullptr;
};

// A successful static result borrows `value`; callers must keep the compiled
// snapshot alive for as long as they retain the returned view.
[[nodiscard]] Result<EvaluatedTemplate> evaluate_template(const CompiledTemplate &value, TemplateEvaluator evaluator);

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_TEMPLATE_EVALUATOR_H
