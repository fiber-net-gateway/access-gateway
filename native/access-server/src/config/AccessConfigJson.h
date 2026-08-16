#ifndef FIBER_ACCESS_SERVER_ACCESS_CONFIG_JSON_H
#define FIBER_ACCESS_SERVER_ACCESS_CONFIG_JSON_H

#include "AccessConfigErrorBuilder.h"

#include <cstdint>
#include <memory>
#include <string_view>
#include <type_traits>

#include <fiber/common/json/JsonValue.h>

namespace fiber::json {
class JsonParser;
}

namespace fiber::mem {
class BufPool;
}

namespace fiber::access_server::config_detail {

enum class AccessJsonKind : std::uint8_t {
    Null,
    Bool,
    Integer,
    Double,
    BigNumber,
    Text,
    Array,
    Object,
};

class AccessJsonValue {
public:
    [[nodiscard]] AccessJsonKind kind() const noexcept { return kind_; }
    [[nodiscard]] bool is_null() const noexcept { return kind_ == AccessJsonKind::Null; }
    [[nodiscard]] bool is_bool() const noexcept { return kind_ == AccessJsonKind::Bool; }
    [[nodiscard]] bool is_integer() const noexcept { return kind_ == AccessJsonKind::Integer; }
    [[nodiscard]] bool is_double() const noexcept { return kind_ == AccessJsonKind::Double; }
    [[nodiscard]] bool is_big_number() const noexcept { return kind_ == AccessJsonKind::BigNumber; }
    [[nodiscard]] bool is_text() const noexcept { return kind_ == AccessJsonKind::Text; }
    [[nodiscard]] bool is_array() const noexcept { return kind_ == AccessJsonKind::Array; }
    [[nodiscard]] bool is_object() const noexcept { return kind_ == AccessJsonKind::Object; }

    [[nodiscard]] bool as_bool() const noexcept { return value_.boolean; }
    [[nodiscard]] std::int64_t as_integer() const noexcept { return value_.integer; }
    [[nodiscard]] double as_double() const noexcept { return value_.number.value; }
    [[nodiscard]] std::string_view number_text() const noexcept { return value_.number.text; }
    [[nodiscard]] std::string_view as_text() const noexcept { return value_.text; }
    [[nodiscard]] const json::JsonArray<AccessJsonValue> &as_array() const noexcept { return value_.array; }
    [[nodiscard]] const json::JsonObject<AccessJsonValue> &as_object() const noexcept { return value_.object; }

    void set_null() noexcept {
        value_.integer = 0;
        kind_ = AccessJsonKind::Null;
    }

    void set_bool(bool value) noexcept {
        value_.boolean = value;
        kind_ = AccessJsonKind::Bool;
    }

    void set_integer(std::int64_t value) noexcept {
        value_.integer = value;
        kind_ = AccessJsonKind::Integer;
    }

    void set_number(double value, std::string_view text) noexcept {
        std::construct_at(&value_.number, Number{.value = value, .text = text});
        kind_ = AccessJsonKind::Double;
    }

    void set_big_number(std::string_view text) noexcept {
        std::construct_at(&value_.number, Number{.value = 0, .text = text});
        kind_ = AccessJsonKind::BigNumber;
    }

    void set_text(std::string_view value) noexcept {
        std::construct_at(&value_.text, value);
        kind_ = AccessJsonKind::Text;
    }

    void set_array(json::JsonArray<AccessJsonValue> value) noexcept {
        std::construct_at(&value_.array, value);
        kind_ = AccessJsonKind::Array;
    }

    void set_object(json::JsonObject<AccessJsonValue> value) noexcept {
        std::construct_at(&value_.object, value);
        kind_ = AccessJsonKind::Object;
    }

private:
    struct Number {
        double value = 0;
        std::string_view text;
    };

    union Value {
        bool boolean;
        std::int64_t integer;
        Number number;
        std::string_view text;
        json::JsonArray<AccessJsonValue> array;
        json::JsonObject<AccessJsonValue> object;

        constexpr Value() noexcept : integer(0) {}
    };

    AccessJsonKind kind_ = AccessJsonKind::Null;
    Value value_;
};

static_assert(std::is_trivially_copyable_v<AccessJsonValue>);

[[nodiscard]] DecodeResult<AccessJsonValue> parse_access_json(std::string_view content, mem::BufPool &pool,
                                                              json::JsonParser &parser);

} // namespace fiber::access_server::config_detail

#endif // FIBER_ACCESS_SERVER_ACCESS_CONFIG_JSON_H
