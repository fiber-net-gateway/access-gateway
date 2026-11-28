#include "LocalFileConfigYaml.h"

#include <cctype>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fiber::access_server {
namespace {

constexpr std::size_t kMaxYamlNestingDepth = 64;

struct YamlLine {
    std::size_t number = 0;
    std::size_t indent = 0;
    std::string_view raw;
    std::string_view content;
};

struct YamlNode {
    enum class Kind : std::uint8_t {
        Null,
        Bool,
        Number,
        String,
        Array,
        Object,
    };

    Kind kind = Kind::Null;
    bool bool_value = false;
    std::string text;
    std::vector<YamlNode> array;
    std::vector<std::string> object_keys;
    std::vector<YamlNode> object_values;
};

[[nodiscard]] std::unexpected<LocalFileYamlError> yaml_error(std::size_t line, std::string message) {
    return std::unexpected(LocalFileYamlError{.line = line, .message = std::move(message)});
}

[[nodiscard]] std::string_view trim_spaces(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

[[nodiscard]] std::string_view strip_inline_comment(std::string_view text) noexcept {
    bool in_single = false;
    bool in_double = false;
    bool escaped = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char ch = text[i];
        if (in_double) {
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                in_double = false;
            }
            continue;
        }
        if (in_single) {
            if (ch == '\'') {
                if (i + 1 < text.size() && text[i + 1] == '\'') {
                    ++i;
                } else {
                    in_single = false;
                }
            }
            continue;
        }
        if (ch == '"') {
            in_double = true;
        } else if (ch == '\'') {
            in_single = true;
        } else if (ch == '#' && (i == 0 || text[i - 1] == ' ' || text[i - 1] == '\t')) {
            return trim_spaces(text.substr(0, i));
        }
    }
    return trim_spaces(text);
}

[[nodiscard]] bool is_valid_json_number(std::string_view text) noexcept {
    if (text.empty()) {
        return false;
    }
    std::size_t i = 0;
    if (text[i] == '-') {
        ++i;
        if (i >= text.size()) {
            return false;
        }
    }
    if (text[i] == '0') {
        ++i;
    } else if (text[i] >= '1' && text[i] <= '9') {
        ++i;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
            ++i;
        }
    } else {
        return false;
    }
    if (i < text.size() && text[i] == '.') {
        ++i;
        if (i >= text.size() || text[i] < '0' || text[i] > '9') {
            return false;
        }
        while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
            ++i;
        }
    }
    if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
        ++i;
        if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
            ++i;
        }
        if (i >= text.size() || text[i] < '0' || text[i] > '9') {
            return false;
        }
        while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
            ++i;
        }
    }
    return i == text.size();
}

void append_utf8(std::string &out, std::uint32_t cp) {
    if (cp <= 0x7FU) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FFU) {
        out.push_back(static_cast<char>(0xC0U | ((cp >> 6U) & 0x1FU)));
        out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
    } else if (cp <= 0xFFFFU) {
        out.push_back(static_cast<char>(0xE0U | ((cp >> 12U) & 0x0FU)));
        out.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
    } else {
        out.push_back(static_cast<char>(0xF0U | ((cp >> 18U) & 0x07U)));
        out.push_back(static_cast<char>(0x80U | ((cp >> 12U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
    }
}

[[nodiscard]] bool parse_hex4(std::string_view text, std::size_t pos, std::uint32_t &out) noexcept {
    if (pos + 4 > text.size()) {
        return false;
    }
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        const char ch = text[pos + i];
        value <<= 4U;
        if (ch >= '0' && ch <= '9') {
            value |= static_cast<std::uint32_t>(ch - '0');
        } else if (ch >= 'a' && ch <= 'f') {
            value |= static_cast<std::uint32_t>(10 + (ch - 'a'));
        } else if (ch >= 'A' && ch <= 'F') {
            value |= static_cast<std::uint32_t>(10 + (ch - 'A'));
        } else {
            return false;
        }
    }
    out = value;
    return true;
}

[[nodiscard]] std::expected<std::string, LocalFileYamlError>
parse_double_quoted_at(std::string_view text, std::size_t &pos, std::size_t line) {
    if (pos >= text.size() || text[pos] != '"') {
        return yaml_error(line, "expected '\"'");
    }
    ++pos;
    std::string out;
    while (pos < text.size()) {
        const char ch = text[pos++];
        if (ch == '"') {
            return out;
        }
        if (ch == '\\') {
            if (pos >= text.size()) {
                return yaml_error(line, "unterminated escape sequence in double-quoted string");
            }
            const char esc = text[pos++];
            switch (esc) {
                case '"':
                    out.push_back('"');
                    break;
                case '\\':
                    out.push_back('\\');
                    break;
                case '/':
                    out.push_back('/');
                    break;
                case 'b':
                    out.push_back('\b');
                    break;
                case 'f':
                    out.push_back('\f');
                    break;
                case 'n':
                    out.push_back('\n');
                    break;
                case 'r':
                    out.push_back('\r');
                    break;
                case 't':
                    out.push_back('\t');
                    break;
                case '0':
                    out.push_back('\0');
                    break;
                case 'u': {
                    std::uint32_t cp = 0;
                    if (!parse_hex4(text, pos, cp)) {
                        return yaml_error(line, "invalid \\u escape sequence in double-quoted string");
                    }
                    pos += 4;
                    if (cp >= 0xD800U && cp <= 0xDBFFU) {
                        if (pos + 6 > text.size() || text[pos] != '\\' || text[pos + 1] != 'u') {
                            return yaml_error(line, "unpaired high surrogate in double-quoted string");
                        }
                        std::uint32_t low = 0;
                        if (!parse_hex4(text, pos + 2, low) || low < 0xDC00U || low > 0xDFFFU) {
                            return yaml_error(line, "invalid low surrogate in double-quoted string");
                        }
                        pos += 6;
                        cp = 0x10000U + (((cp - 0xD800U) << 10U) | (low - 0xDC00U));
                    } else if (cp >= 0xDC00U && cp <= 0xDFFFU) {
                        return yaml_error(line, "unpaired low surrogate in double-quoted string");
                    }
                    append_utf8(out, cp);
                    break;
                }
                default:
                    return yaml_error(line, "unsupported escape sequence in double-quoted string");
            }
            continue;
        }
        out.push_back(ch);
    }
    return yaml_error(line, "unterminated double-quoted string");
}

[[nodiscard]] std::expected<std::string, LocalFileYamlError>
parse_single_quoted_at(std::string_view text, std::size_t &pos, std::size_t line) {
    if (pos >= text.size() || text[pos] != '\'') {
        return yaml_error(line, "expected \"'\"");
    }
    ++pos;
    std::string out;
    while (pos < text.size()) {
        const char ch = text[pos++];
        if (ch == '\'') {
            if (pos < text.size() && text[pos] == '\'') {
                out.push_back('\'');
                ++pos;
                continue;
            }
            return out;
        }
        out.push_back(ch);
    }
    return yaml_error(line, "unterminated single-quoted string");
}

[[nodiscard]] std::expected<YamlNode, LocalFileYamlError> parse_plain_scalar(std::string_view text, std::size_t line) {
    const std::string_view trimmed = trim_spaces(text);
    if (trimmed.empty() || trimmed == "null" || trimmed == "Null" || trimmed == "NULL" || trimmed == "~") {
        return YamlNode{.kind = YamlNode::Kind::Null};
    }
    if (trimmed == "true" || trimmed == "True" || trimmed == "TRUE") {
        return YamlNode{.kind = YamlNode::Kind::Bool, .bool_value = true};
    }
    if (trimmed == "false" || trimmed == "False" || trimmed == "FALSE") {
        return YamlNode{.kind = YamlNode::Kind::Bool, .bool_value = false};
    }
    if (is_valid_json_number(trimmed)) {
        return YamlNode{.kind = YamlNode::Kind::Number, .text = std::string(trimmed)};
    }
    if (trimmed.front() == '&' || trimmed.front() == '!' ||
        (trimmed.front() == '*' && trimmed.size() > 1 && trimmed[1] != '.')) {
        return yaml_error(line, "YAML anchors, aliases, and tags are not supported");
    }
    return YamlNode{.kind = YamlNode::Kind::String, .text = std::string(trimmed)};
}

class FlowParser final {
public:
    FlowParser(std::string_view input, std::size_t start_line) noexcept : input_(input), line_(start_line) {}

    [[nodiscard]] std::expected<YamlNode, LocalFileYamlError> parse(std::size_t depth) {
        skip_ws();
        auto node = parse_value(depth);
        if (!node) {
            return std::unexpected(std::move(node.error()));
        }
        skip_ws();
        if (pos_ != input_.size()) {
            return yaml_error(line_, "unexpected trailing characters after flow value");
        }
        return node;
    }

private:
    void skip_ws() noexcept {
        while (pos_ < input_.size()) {
            const char ch = input_[pos_];
            if (ch == ' ' || ch == '\t' || ch == '\r') {
                ++pos_;
            } else if (ch == '\n') {
                ++pos_;
                ++line_;
            } else if (ch == '#') {
                while (pos_ < input_.size() && input_[pos_] != '\n') {
                    ++pos_;
                }
            } else {
                break;
            }
        }
    }

    [[nodiscard]] std::expected<YamlNode, LocalFileYamlError> parse_value(std::size_t depth) {
        if (depth > kMaxYamlNestingDepth) {
            return yaml_error(line_, "YAML nesting depth exceeds limit");
        }
        skip_ws();
        if (pos_ >= input_.size()) {
            return yaml_error(line_, "unexpected end of flow collection");
        }
        const char ch = input_[pos_];
        if (ch == '{') {
            return parse_object(depth + 1);
        }
        if (ch == '[') {
            return parse_array(depth + 1);
        }
        if (ch == '"') {
            auto text = parse_double_quoted_at(input_, pos_, line_);
            if (!text) {
                return std::unexpected(std::move(text.error()));
            }
            return YamlNode{.kind = YamlNode::Kind::String, .text = std::move(*text)};
        }
        if (ch == '\'') {
            auto text = parse_single_quoted_at(input_, pos_, line_);
            if (!text) {
                return std::unexpected(std::move(text.error()));
            }
            return YamlNode{.kind = YamlNode::Kind::String, .text = std::move(*text)};
        }
        const std::size_t start = pos_;
        while (pos_ < input_.size()) {
            const char cur = input_[pos_];
            if (cur == ',' || cur == ']' || cur == '}' || cur == '\n' || cur == '\r') {
                break;
            }
            if (cur == '#' && pos_ > start && (input_[pos_ - 1] == ' ' || input_[pos_ - 1] == '\t')) {
                break;
            }
            ++pos_;
        }
        return parse_plain_scalar(input_.substr(start, pos_ - start), line_);
    }

    [[nodiscard]] std::expected<YamlNode, LocalFileYamlError> parse_array(std::size_t depth) {
        ++pos_; // '['
        YamlNode node{.kind = YamlNode::Kind::Array};
        skip_ws();
        if (pos_ < input_.size() && input_[pos_] == ']') {
            ++pos_;
            return node;
        }
        while (pos_ < input_.size()) {
            auto item = parse_value(depth);
            if (!item) {
                return std::unexpected(std::move(item.error()));
            }
            node.array.push_back(std::move(*item));
            skip_ws();
            if (pos_ >= input_.size()) {
                return yaml_error(line_, "unterminated flow sequence '['");
            }
            if (input_[pos_] == ',') {
                ++pos_;
                skip_ws();
                if (pos_ < input_.size() && input_[pos_] == ']') {
                    ++pos_;
                    return node;
                }
                continue;
            }
            if (input_[pos_] == ']') {
                ++pos_;
                return node;
            }
            return yaml_error(line_, "expected ',' or ']' in flow sequence");
        }
        return yaml_error(line_, "unterminated flow sequence '['");
    }

    [[nodiscard]] std::expected<std::string, LocalFileYamlError> parse_key() {
        skip_ws();
        if (pos_ >= input_.size()) {
            return yaml_error(line_, "unexpected end of flow mapping");
        }
        if (input_[pos_] == '"') {
            return parse_double_quoted_at(input_, pos_, line_);
        }
        if (input_[pos_] == '\'') {
            return parse_single_quoted_at(input_, pos_, line_);
        }
        const std::size_t start = pos_;
        while (pos_ < input_.size()) {
            const char ch = input_[pos_];
            if (ch == ':' && (pos_ + 1 >= input_.size() || input_[pos_ + 1] == ' ' || input_[pos_ + 1] == '\t' ||
                              input_[pos_ + 1] == '\n' || input_[pos_ + 1] == '\r' || input_[pos_ + 1] == ',' ||
                              input_[pos_ + 1] == '}' || input_[pos_ + 1] == '{' || input_[pos_ + 1] == '[')) {
                break;
            }
            if (ch == ',' || ch == '}' || ch == '\n' || ch == '\r') {
                return yaml_error(line_, "expected ':' after key in flow mapping");
            }
            ++pos_;
        }
        std::string_view key = trim_spaces(input_.substr(start, pos_ - start));
        if (key.empty()) {
            return yaml_error(line_, "empty key in flow mapping");
        }
        return std::string(key);
    }

    [[nodiscard]] std::expected<YamlNode, LocalFileYamlError> parse_object(std::size_t depth) {
        ++pos_; // '{'
        YamlNode node{.kind = YamlNode::Kind::Object};
        skip_ws();
        if (pos_ < input_.size() && input_[pos_] == '}') {
            ++pos_;
            return node;
        }
        while (pos_ < input_.size()) {
            auto key = parse_key();
            if (!key) {
                return std::unexpected(std::move(key.error()));
            }
            skip_ws();
            if (pos_ >= input_.size() || input_[pos_] != ':') {
                return yaml_error(line_, "expected ':' in flow mapping");
            }
            ++pos_;
            auto val = parse_value(depth);
            if (!val) {
                return std::unexpected(std::move(val.error()));
            }
            node.object_keys.push_back(std::move(*key));
            node.object_values.push_back(std::move(*val));
            skip_ws();
            if (pos_ >= input_.size()) {
                return yaml_error(line_, "unterminated flow mapping '{'");
            }
            if (input_[pos_] == ',') {
                ++pos_;
                skip_ws();
                if (pos_ < input_.size() && input_[pos_] == '}') {
                    ++pos_;
                    return node;
                }
                continue;
            }
            if (input_[pos_] == '}') {
                ++pos_;
                return node;
            }
            return yaml_error(line_, "expected ',' or '}' in flow mapping");
        }
        return yaml_error(line_, "unterminated flow mapping '{'");
    }

    std::string_view input_;
    std::size_t pos_ = 0;
    std::size_t line_ = 1;
};

// Finds the mapping separator ':' in a block line (not inside quotes or flow
// brackets, and followed by whitespace or end-of-line).
[[nodiscard]] std::size_t find_mapping_colon(std::string_view text) noexcept {
    bool in_single = false;
    bool in_double = false;
    bool escaped = false;
    int brace_depth = 0;
    int bracket_depth = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char ch = text[i];
        if (in_double) {
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                in_double = false;
            }
            continue;
        }
        if (in_single) {
            if (ch == '\'') {
                if (i + 1 < text.size() && text[i + 1] == '\'') {
                    ++i;
                } else {
                    in_single = false;
                }
            }
            continue;
        }
        if (ch == '"') {
            in_double = true;
        } else if (ch == '\'') {
            in_single = true;
        } else if (ch == '{') {
            ++brace_depth;
        } else if (ch == '}' && brace_depth > 0) {
            --brace_depth;
        } else if (ch == '[') {
            ++bracket_depth;
        } else if (ch == ']' && bracket_depth > 0) {
            --bracket_depth;
        } else if (ch == ':' && brace_depth == 0 && bracket_depth == 0) {
            if (i + 1 == text.size() || text[i + 1] == ' ' || text[i + 1] == '\t') {
                return i;
            }
        }
    }
    return std::string_view::npos;
}

[[nodiscard]] bool is_sequence_item(std::string_view content) noexcept {
    return content == "-" || (content.size() >= 2 && content[0] == '-' && (content[1] == ' ' || content[1] == '\t'));
}

[[nodiscard]] bool is_block_scalar_header(std::string_view text) noexcept {
    if (text.empty() || (text[0] != '|' && text[0] != '>')) {
        return false;
    }
    bool has_chomp = false;
    bool has_indent = false;
    for (std::size_t i = 1; i < text.size(); ++i) {
        const char ch = text[i];
        if ((ch == '-' || ch == '+') && !has_chomp) {
            has_chomp = true;
        } else if (ch >= '1' && ch <= '9' && !has_indent) {
            has_indent = true;
        } else {
            return false;
        }
    }
    return true;
}

class BlockParser final {
public:
    explicit BlockParser(std::vector<YamlLine> lines) noexcept : lines_(std::move(lines)) {}

    [[nodiscard]] std::expected<YamlNode, LocalFileYamlError> parse_document() {
        skip_blank_and_comments();
        if (index_ < lines_.size() && lines_[index_].content == "---") {
            ++index_;
            skip_blank_and_comments();
        }
        if (index_ >= lines_.size() || lines_[index_].content == "...") {
            return YamlNode{.kind = YamlNode::Kind::Null};
        }
        auto root = parse_node(lines_[index_].indent, 0);
        if (!root) {
            return std::unexpected(std::move(root.error()));
        }
        skip_blank_and_comments();
        if (index_ < lines_.size() && lines_[index_].content == "...") {
            ++index_;
            skip_blank_and_comments();
        }
        if (index_ < lines_.size()) {
            return yaml_error(lines_[index_].number, "unexpected trailing YAML content or multiple documents");
        }
        return root;
    }

private:
    void skip_blank_and_comments() noexcept {
        while (index_ < lines_.size() && lines_[index_].content.empty()) {
            ++index_;
        }
    }

    [[nodiscard]] std::expected<std::string, LocalFileYamlError> parse_mapping_key(std::string_view raw_key,
                                                                                   std::size_t line) {
        const std::string_view trimmed = trim_spaces(raw_key);
        if (trimmed.empty()) {
            return yaml_error(line, "empty mapping key");
        }
        if (trimmed.front() == '"') {
            std::size_t pos = 0;
            auto parsed = parse_double_quoted_at(trimmed, pos, line);
            if (!parsed) {
                return std::unexpected(std::move(parsed.error()));
            }
            if (pos != trimmed.size()) {
                return yaml_error(line, "unexpected trailing characters after quoted mapping key");
            }
            return parsed;
        }
        if (trimmed.front() == '\'') {
            std::size_t pos = 0;
            auto parsed = parse_single_quoted_at(trimmed, pos, line);
            if (!parsed) {
                return std::unexpected(std::move(parsed.error()));
            }
            if (pos != trimmed.size()) {
                return yaml_error(line, "unexpected trailing characters after quoted mapping key");
            }
            return parsed;
        }
        return std::string(trimmed);
    }

    [[nodiscard]] std::expected<std::string, LocalFileYamlError> collect_flow_text(std::string_view first_piece,
                                                                                   std::size_t start_line) {
        std::string combined(first_piece);
        auto balanced = [](std::string_view text) noexcept {
            bool in_single = false;
            bool in_double = false;
            bool escaped = false;
            int braces = 0;
            int brackets = 0;
            for (std::size_t i = 0; i < text.size(); ++i) {
                const char ch = text[i];
                if (in_double) {
                    if (escaped) {
                        escaped = false;
                    } else if (ch == '\\') {
                        escaped = true;
                    } else if (ch == '"') {
                        in_double = false;
                    }
                    continue;
                }
                if (in_single) {
                    if (ch == '\'') {
                        if (i + 1 < text.size() && text[i + 1] == '\'') {
                            ++i;
                        } else {
                            in_single = false;
                        }
                    }
                    continue;
                }
                if (ch == '"') {
                    in_double = true;
                } else if (ch == '\'') {
                    in_single = true;
                } else if (ch == '#' && (i == 0 || text[i - 1] == ' ' || text[i - 1] == '\t' || text[i - 1] == '\n')) {
                    while (i + 1 < text.size() && text[i + 1] != '\n') {
                        ++i;
                    }
                } else if (ch == '{') {
                    ++braces;
                } else if (ch == '}') {
                    --braces;
                } else if (ch == '[') {
                    ++brackets;
                } else if (ch == ']') {
                    --brackets;
                }
            }
            return !in_single && !in_double && braces <= 0 && brackets <= 0;
        };
        while (!balanced(combined) && index_ < lines_.size()) {
            combined.push_back('\n');
            combined.append(lines_[index_].raw);
            ++index_;
        }
        if (!balanced(combined)) {
            return yaml_error(start_line, "unterminated flow collection");
        }
        return combined;
    }

    [[nodiscard]] std::expected<YamlNode, LocalFileYamlError>
    parse_block_scalar(std::string_view header, std::size_t parent_indent, std::size_t header_line) {
        const bool folded = header[0] == '>';
        char chomp = 'c'; // clip
        std::size_t explicit_indent = 0;
        for (std::size_t i = 1; i < header.size(); ++i) {
            if (header[i] == '-' || header[i] == '+') {
                chomp = header[i];
            } else if (header[i] >= '1' && header[i] <= '9') {
                explicit_indent = static_cast<std::size_t>(header[i] - '0');
            }
        }

        std::size_t block_indent = explicit_indent > 0 ? parent_indent + explicit_indent : 0;
        std::vector<std::string> collected;
        while (index_ < lines_.size()) {
            const YamlLine &line = lines_[index_];
            const bool blank = trim_spaces(line.raw).empty();
            if (blank) {
                if (block_indent > 0 && line.raw.size() > block_indent) {
                    collected.emplace_back(line.raw.substr(block_indent));
                } else {
                    collected.emplace_back();
                }
                ++index_;
                continue;
            }
            if (block_indent == 0) {
                if (line.indent <= parent_indent) {
                    break;
                }
                block_indent = line.indent;
            } else if (line.indent < block_indent) {
                break;
            }
            collected.emplace_back(line.raw.substr(block_indent));
            ++index_;
        }

        std::size_t trailing_blanks = 0;
        while (!collected.empty() && collected.back().empty()) {
            ++trailing_blanks;
            collected.pop_back();
        }
        if (collected.empty()) {
            std::string empty_out;
            if (chomp == '+') {
                empty_out.assign(trailing_blanks, '\n');
            }
            (void) header_line;
            return YamlNode{.kind = YamlNode::Kind::String, .text = std::move(empty_out)};
        }

        std::string out;
        if (!folded) {
            for (std::size_t i = 0; i < collected.size(); ++i) {
                if (i > 0) {
                    out.push_back('\n');
                }
                out.append(collected[i]);
            }
        } else {
            for (std::size_t i = 0; i < collected.size(); ++i) {
                if (i > 0) {
                    const bool prev_empty = collected[i - 1].empty();
                    const bool cur_empty = collected[i].empty();
                    const bool prev_indented = !prev_empty && collected[i - 1][0] == ' ';
                    const bool cur_indented = !cur_empty && collected[i][0] == ' ';
                    if (prev_empty || cur_empty || prev_indented || cur_indented) {
                        out.push_back('\n');
                    } else {
                        out.push_back(' ');
                    }
                }
                out.append(collected[i]);
            }
        }
        if (chomp == 'c') {
            out.push_back('\n');
        } else if (chomp == '+') {
            out.push_back('\n');
            out.append(trailing_blanks, '\n');
        }
        return YamlNode{.kind = YamlNode::Kind::String, .text = std::move(out)};
    }

    [[nodiscard]] std::expected<YamlNode, LocalFileYamlError>
    parse_inline_value(std::string_view value, std::size_t parent_indent, std::size_t line, std::size_t depth) {
        if (depth > kMaxYamlNestingDepth) {
            return yaml_error(line, "YAML nesting depth exceeds limit");
        }
        const std::string_view cleaned = strip_inline_comment(value);
        if (cleaned.empty()) {
            skip_blank_and_comments();
            if (index_ >= lines_.size()) {
                return YamlNode{.kind = YamlNode::Kind::Null};
            }
            const YamlLine &next = lines_[index_];
            if (next.indent > parent_indent || (next.indent == parent_indent && is_sequence_item(next.content))) {
                return parse_node(next.indent, depth + 1);
            }
            return YamlNode{.kind = YamlNode::Kind::Null};
        }
        if (is_block_scalar_header(cleaned)) {
            return parse_block_scalar(cleaned, parent_indent, line);
        }
        if (cleaned.front() == '{' || cleaned.front() == '[') {
            auto flow_text = collect_flow_text(cleaned, line);
            if (!flow_text) {
                return std::unexpected(std::move(flow_text.error()));
            }
            FlowParser flow(*flow_text, line);
            return flow.parse(depth + 1);
        }
        if (cleaned.front() == '"') {
            std::size_t pos = 0;
            auto parsed = parse_double_quoted_at(cleaned, pos, line);
            if (!parsed) {
                return std::unexpected(std::move(parsed.error()));
            }
            if (pos != cleaned.size()) {
                return yaml_error(line, "unexpected trailing characters after double-quoted scalar");
            }
            return YamlNode{.kind = YamlNode::Kind::String, .text = std::move(*parsed)};
        }
        if (cleaned.front() == '\'') {
            std::size_t pos = 0;
            auto parsed = parse_single_quoted_at(cleaned, pos, line);
            if (!parsed) {
                return std::unexpected(std::move(parsed.error()));
            }
            if (pos != cleaned.size()) {
                return yaml_error(line, "unexpected trailing characters after single-quoted scalar");
            }
            return YamlNode{.kind = YamlNode::Kind::String, .text = std::move(*parsed)};
        }
        return parse_plain_scalar(cleaned, line);
    }

    [[nodiscard]] std::expected<YamlNode, LocalFileYamlError> parse_node(std::size_t expected_indent,
                                                                         std::size_t depth) {
        if (depth > kMaxYamlNestingDepth) {
            return yaml_error(lines_[index_].number, "YAML nesting depth exceeds limit");
        }
        skip_blank_and_comments();
        if (index_ >= lines_.size()) {
            return YamlNode{.kind = YamlNode::Kind::Null};
        }
        const YamlLine &line = lines_[index_];
        if (line.indent != expected_indent) {
            return yaml_error(line.number, "unexpected indentation");
        }
        if (is_sequence_item(line.content)) {
            return parse_sequence(expected_indent, depth);
        }
        if (line.content.front() == '{' || line.content.front() == '[') {
            const std::string_view first = line.content;
            const std::size_t line_no = line.number;
            ++index_;
            auto flow_text = collect_flow_text(first, line_no);
            if (!flow_text) {
                return std::unexpected(std::move(flow_text.error()));
            }
            FlowParser flow(*flow_text, line_no);
            return flow.parse(depth + 1);
        }
        if (find_mapping_colon(line.content) != std::string_view::npos) {
            return parse_mapping(expected_indent, depth);
        }
        const std::string_view content = line.content;
        const std::size_t line_no = line.number;
        ++index_;
        return parse_inline_value(content, expected_indent, line_no, depth);
    }

    [[nodiscard]] std::expected<YamlNode, LocalFileYamlError> parse_sequence(std::size_t seq_indent,
                                                                             std::size_t depth) {
        YamlNode arr{.kind = YamlNode::Kind::Array};
        while (true) {
            skip_blank_and_comments();
            if (index_ >= lines_.size()) {
                break;
            }
            const YamlLine &line = lines_[index_];
            if (line.indent < seq_indent || line.content == "...") {
                break;
            }
            if (line.indent > seq_indent) {
                return yaml_error(line.number, "unexpected indentation inside sequence");
            }
            if (!is_sequence_item(line.content)) {
                break;
            }
            const std::size_t line_no = line.number;
            std::string_view after_dash = line.content.substr(1);
            std::size_t extra_spaces = 0;
            while (extra_spaces < after_dash.size() && after_dash[extra_spaces] == ' ') {
                ++extra_spaces;
            }
            const std::string_view rest = trim_spaces(after_dash);
            const std::size_t item_col = seq_indent + 1 + extra_spaces;
            ++index_;

            if (rest.empty()) {
                skip_blank_and_comments();
                if (index_ < lines_.size() && lines_[index_].indent > seq_indent) {
                    auto child = parse_node(lines_[index_].indent, depth + 1);
                    if (!child) {
                        return std::unexpected(std::move(child.error()));
                    }
                    arr.array.push_back(std::move(*child));
                } else {
                    arr.array.push_back(YamlNode{.kind = YamlNode::Kind::Null});
                }
                continue;
            }

            if (is_sequence_item(rest)) {
                // Nested compact sequence: "- - item"
                YamlLine synthetic{
                        .number = line_no,
                        .indent = item_col,
                        .raw = line.raw,
                        .content = rest,
                };
                lines_.insert(lines_.begin() + static_cast<std::ptrdiff_t>(index_), synthetic);
                auto child = parse_sequence(item_col, depth + 1);
                if (!child) {
                    return std::unexpected(std::move(child.error()));
                }
                arr.array.push_back(std::move(*child));
                continue;
            }

            const std::size_t colon = find_mapping_colon(rest);
            if (colon != std::string_view::npos && rest.front() != '{' && rest.front() != '[') {
                // Inline mapping entry starting on the sequence dash line: "- key: val"
                YamlNode obj{.kind = YamlNode::Kind::Object};
                auto first_key = parse_mapping_key(rest.substr(0, colon), line_no);
                if (!first_key) {
                    return std::unexpected(std::move(first_key.error()));
                }
                auto first_val = parse_inline_value(rest.substr(colon + 1), item_col, line_no, depth + 1);
                if (!first_val) {
                    return std::unexpected(std::move(first_val.error()));
                }
                obj.object_keys.push_back(std::move(*first_key));
                obj.object_values.push_back(std::move(*first_val));
                auto rest_entries = parse_mapping_entries(obj, item_col, depth + 1);
                if (!rest_entries) {
                    return std::unexpected(std::move(rest_entries.error()));
                }
                arr.array.push_back(std::move(obj));
                continue;
            }

            auto val = parse_inline_value(rest, seq_indent, line_no, depth + 1);
            if (!val) {
                return std::unexpected(std::move(val.error()));
            }
            arr.array.push_back(std::move(*val));
        }
        return arr;
    }

    [[nodiscard]] std::expected<void, LocalFileYamlError> parse_mapping_entries(YamlNode &obj, std::size_t map_indent,
                                                                                std::size_t depth) {
        while (true) {
            skip_blank_and_comments();
            if (index_ >= lines_.size()) {
                break;
            }
            const YamlLine &line = lines_[index_];
            if (line.indent < map_indent || line.content == "...") {
                break;
            }
            if (line.indent > map_indent) {
                return yaml_error(line.number, "unexpected indentation inside mapping");
            }
            if (is_sequence_item(line.content)) {
                break;
            }
            const std::size_t colon = find_mapping_colon(line.content);
            if (colon == std::string_view::npos) {
                return yaml_error(line.number, "expected 'key: value' entry in mapping");
            }
            const std::size_t line_no = line.number;
            const std::string_view content = line.content;
            ++index_;
            auto key = parse_mapping_key(content.substr(0, colon), line_no);
            if (!key) {
                return std::unexpected(std::move(key.error()));
            }
            auto val = parse_inline_value(content.substr(colon + 1), map_indent, line_no, depth + 1);
            if (!val) {
                return std::unexpected(std::move(val.error()));
            }
            obj.object_keys.push_back(std::move(*key));
            obj.object_values.push_back(std::move(*val));
        }
        return {};
    }

    [[nodiscard]] std::expected<YamlNode, LocalFileYamlError> parse_mapping(std::size_t map_indent, std::size_t depth) {
        YamlNode obj{.kind = YamlNode::Kind::Object};
        auto parsed = parse_mapping_entries(obj, map_indent, depth);
        if (!parsed) {
            return std::unexpected(std::move(parsed.error()));
        }
        return obj;
    }

    std::vector<YamlLine> lines_;
    std::size_t index_ = 0;
};

[[nodiscard]] bool append_checked(std::string &out, std::string_view text, std::size_t max_bytes) {
    if (text.size() > max_bytes || out.size() > max_bytes - text.size()) {
        return false;
    }
    out.append(text);
    return true;
}

[[nodiscard]] bool append_json_string(std::string &out, std::string_view text, std::size_t max_bytes) {
    if (!append_checked(out, "\"", max_bytes)) {
        return false;
    }
    static constexpr char kHex[] = "0123456789abcdef";
    for (const unsigned char ch: text) {
        switch (ch) {
            case '"':
                if (!append_checked(out, "\\\"", max_bytes)) {
                    return false;
                }
                break;
            case '\\':
                if (!append_checked(out, "\\\\", max_bytes)) {
                    return false;
                }
                break;
            case '\b':
                if (!append_checked(out, "\\b", max_bytes)) {
                    return false;
                }
                break;
            case '\f':
                if (!append_checked(out, "\\f", max_bytes)) {
                    return false;
                }
                break;
            case '\n':
                if (!append_checked(out, "\\n", max_bytes)) {
                    return false;
                }
                break;
            case '\r':
                if (!append_checked(out, "\\r", max_bytes)) {
                    return false;
                }
                break;
            case '\t':
                if (!append_checked(out, "\\t", max_bytes)) {
                    return false;
                }
                break;
            default:
                if (ch < 0x20U) {
                    char escaped[7] = {'\\', 'u', '0', '0', kHex[ch >> 4U], kHex[ch & 0x0FU], '\0'};
                    if (!append_checked(out, std::string_view(escaped, 6), max_bytes)) {
                        return false;
                    }
                } else {
                    if (out.size() >= max_bytes) {
                        return false;
                    }
                    out.push_back(static_cast<char>(ch));
                }
                break;
        }
    }
    return append_checked(out, "\"", max_bytes);
}

[[nodiscard]] bool emit_json(const YamlNode &node, std::string &out, std::size_t max_bytes) {
    switch (node.kind) {
        case YamlNode::Kind::Null:
            return append_checked(out, "null", max_bytes);
        case YamlNode::Kind::Bool:
            return append_checked(out, node.bool_value ? "true" : "false", max_bytes);
        case YamlNode::Kind::Number:
            return append_checked(out, node.text, max_bytes);
        case YamlNode::Kind::String:
            return append_json_string(out, node.text, max_bytes);
        case YamlNode::Kind::Array:
            if (!append_checked(out, "[", max_bytes)) {
                return false;
            }
            for (std::size_t i = 0; i < node.array.size(); ++i) {
                if (i > 0 && !append_checked(out, ",", max_bytes)) {
                    return false;
                }
                if (!emit_json(node.array[i], out, max_bytes)) {
                    return false;
                }
            }
            return append_checked(out, "]", max_bytes);
        case YamlNode::Kind::Object:
            if (!append_checked(out, "{", max_bytes)) {
                return false;
            }
            for (std::size_t i = 0; i < node.object_keys.size(); ++i) {
                if (i > 0 && !append_checked(out, ",", max_bytes)) {
                    return false;
                }
                if (!append_json_string(out, node.object_keys[i], max_bytes) || !append_checked(out, ":", max_bytes) ||
                    !emit_json(node.object_values[i], out, max_bytes)) {
                    return false;
                }
            }
            return append_checked(out, "}", max_bytes);
    }
    return false;
}

} // namespace

std::expected<std::string, LocalFileYamlError> convert_yaml_to_json(std::string_view yaml,
                                                                    std::size_t max_output_bytes) {
    if (yaml.find('\0') != std::string_view::npos) {
        return yaml_error(1, "YAML input must not contain NUL bytes");
    }
    // Strip UTF-8 BOM if present.
    if (yaml.size() >= 3 && static_cast<unsigned char>(yaml[0]) == 0xEFU &&
        static_cast<unsigned char>(yaml[1]) == 0xBBU && static_cast<unsigned char>(yaml[2]) == 0xBFU) {
        yaml.remove_prefix(3);
    }

    std::vector<YamlLine> lines;
    std::size_t pos = 0;
    std::size_t line_number = 0;
    while (pos <= yaml.size()) {
        ++line_number;
        const std::size_t nl = yaml.find('\n', pos);
        std::string_view raw = nl == std::string_view::npos ? yaml.substr(pos) : yaml.substr(pos, nl - pos);
        if (!raw.empty() && raw.back() == '\r') {
            raw.remove_suffix(1);
        }
        std::size_t indent = 0;
        while (indent < raw.size() && raw[indent] == ' ') {
            ++indent;
        }
        if (indent < raw.size() && raw[indent] == '\t') {
            return yaml_error(line_number, "tab indentation is not allowed in YAML");
        }
        std::string_view content = trim_spaces(raw.substr(indent));
        if (!content.empty() && content.front() == '#') {
            content = {};
        }
        lines.push_back(YamlLine{
                .number = line_number,
                .indent = indent,
                .raw = raw,
                .content = content,
        });
        if (nl == std::string_view::npos) {
            break;
        }
        pos = nl + 1;
    }

    BlockParser parser(std::move(lines));
    auto parsed = parser.parse_document();
    if (!parsed) {
        return std::unexpected(std::move(parsed.error()));
    }

    std::string json;
    if (!emit_json(*parsed, json, max_output_bytes)) {
        return yaml_error(0, "converted JSON output exceeds maximum allowed bytes");
    }
    return json;
}

} // namespace fiber::access_server
