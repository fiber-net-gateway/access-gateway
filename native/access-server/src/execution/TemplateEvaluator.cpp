#include "TemplateEvaluator.h"

#include <utility>

namespace fiber::access_server {

EvaluatedTemplate EvaluatedTemplate::borrowed(std::string_view value) noexcept {
    return EvaluatedTemplate(std::variant<std::string_view, std::string>(std::in_place_type<std::string_view>, value));
}

EvaluatedTemplate EvaluatedTemplate::owned(std::string value) noexcept {
    return EvaluatedTemplate(
            std::variant<std::string_view, std::string>(std::in_place_type<std::string>, std::move(value)));
}

std::string_view EvaluatedTemplate::view() const noexcept {
    if (const auto *borrowed = std::get_if<std::string_view>(&storage_)) {
        return *borrowed;
    }
    return std::get<std::string>(storage_);
}

bool EvaluatedTemplate::owns_storage() const noexcept { return std::holds_alternative<std::string>(storage_); }

std::string &EvaluatedTemplate::materialize() {
    if (const auto *borrowed = std::get_if<std::string_view>(&storage_)) {
        const std::string_view value = *borrowed;
        storage_.emplace<std::string>(value);
    }
    return std::get<std::string>(storage_);
}

std::string EvaluatedTemplate::into_string() && {
    if (auto *owned = std::get_if<std::string>(&storage_)) {
        return std::move(*owned);
    }
    return std::string(std::get<std::string_view>(storage_));
}

Result<EvaluatedTemplate> evaluate_template(const CompiledTemplate &value, TemplateEvaluator evaluator) {
    if (!value.dynamic()) {
        return EvaluatedTemplate::borrowed(value.trailing_literal);
    }

    std::string output;
    output.reserve(value.output_reserve_size);

    for (const CompiledTemplateExpression &expression: value.expressions) {
        output.append(expression.leading_literal);
        if (!evaluator.evaluate) {
            return std::unexpected(Err::from_exception(Exception{
                    .name = "TEMPLATE_SCRIPT",
                    .message = "error exec for template expression: template evaluator is not configured",
                    .status = 500,
            }));
        }

        auto evaluated = evaluator.evaluate(evaluator.context, expression.program, expression.source, output);
        if (!evaluated) {
            return std::unexpected(evaluated.error());
        }
    }
    output.append(value.trailing_literal);
    return EvaluatedTemplate::owned(std::move(output));
}

} // namespace fiber::access_server
