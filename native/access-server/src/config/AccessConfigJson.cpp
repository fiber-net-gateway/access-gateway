#include "AccessConfigJson.h"

#include <string_view>

#include <fiber/common/json/JsonParse.h>
#include <fiber/common/json/JsonParser.h>
#include <fiber/common/mem/BufPool.h>

namespace fiber::access_server::config_detail {
namespace {

json::ParseStatus parse_access_json_value(json::JsonParser &parser, mem::BufPool &pool, AccessJsonValue &out,
                                          std::string_view input) noexcept {
    const json::Token *token = parser.current_token();
    if (!token || token->role != json::TokenRole::Value) {
        (void) parser.fail("expected JSON value");
        return json::ParseStatus::Error;
    }

    AccessJsonValue result;
    switch (token->kind) {
        case json::TokenKind::Null:
            result.set_null();
            break;
        case json::TokenKind::Bool:
            result.set_bool(token->bval);
            break;
        case json::TokenKind::Integer:
            result.set_integer(token->inum);
            break;
        case json::TokenKind::Double: {
            const std::size_t offset = parser.current_offset();
            const std::size_t end = parser.current_end_offset();
            if (offset > end || end > input.size()) {
                (void) parser.fail("invalid JSON number range");
                return json::ParseStatus::Error;
            }
            result.set_number(token->fnum, input.substr(offset, end - offset));
            break;
        }
        case json::TokenKind::BigNumber:
            result.set_big_number(token->view);
            break;
        case json::TokenKind::Text: {
            std::string_view text;
            if (json::parse_text(parser, pool, text) != json::ParseStatus::Done) {
                return json::ParseStatus::Error;
            }
            result.set_text(text);
            break;
        }
        case json::TokenKind::StartArr: {
            json::JsonArray<AccessJsonValue> array;
            auto element_parser = [input](json::JsonParser &value_parser, mem::BufPool &value_pool,
                                          AccessJsonValue &value) noexcept {
                return parse_access_json_value(value_parser, value_pool, value, input);
            };
            if (json::parse_array(parser, pool, array, element_parser) != json::ParseStatus::Done) {
                return json::ParseStatus::Error;
            }
            result.set_array(array);
            break;
        }
        case json::TokenKind::StartObj: {
            json::JsonObject<AccessJsonValue> object;
            auto property_parser = [input](json::JsonParser &value_parser, mem::BufPool &value_pool,
                                           AccessJsonValue &value) noexcept {
                return parse_access_json_value(value_parser, value_pool, value, input);
            };
            if (json::parse_object(parser, pool, object, property_parser) != json::ParseStatus::Done) {
                return json::ParseStatus::Error;
            }
            result.set_object(object);
            break;
        }
        case json::TokenKind::EndObj:
        case json::TokenKind::EndArr:
            (void) parser.fail("expected JSON value");
            return json::ParseStatus::Error;
    }

    out = result;
    return json::ParseStatus::Done;
}

} // namespace

DecodeResult<AccessJsonValue> parse_access_json(std::string_view content, mem::BufPool &pool,
                                                json::JsonParser &parser) {
    if (!parser.feed(content.data(), content.size())) {
        const json::ParseError &error = parser.error();
        return std::unexpected(make_error(AccessConfigErrorCode::InvalidJson, {},
                                          error.message ? error.message : "invalid JSON", error.offset));
    }
    parser.finish();
    AccessJsonValue root;
    const auto status = json::parse_document(
            parser, pool, root,
            [content](json::JsonParser &value_parser, mem::BufPool &value_pool, AccessJsonValue &out) noexcept {
                return parse_access_json_value(value_parser, value_pool, out, content);
            });
    if (status != json::ParseStatus::Done) {
        const json::ParseError &error = parser.error();
        return std::unexpected(make_error(AccessConfigErrorCode::InvalidJson, {},
                                          error.message ? error.message : "invalid JSON", error.offset));
    }
    return root;
}

} // namespace fiber::access_server::config_detail
