#include <gtest/gtest.h>

#include <iterator>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <fiber/http/HttpHeaderHash.h>
#include "routing/CompiledHeaderTemplates.h"

namespace {

using fiber::access_server::CompiledHeaderTemplates;
using fiber::access_server::CompiledTemplate;
using fiber::access_server::parse_template;

static_assert(std::forward_iterator<CompiledHeaderTemplates::ConstIterator>);
static_assert(std::is_nothrow_move_constructible_v<CompiledHeaderTemplates>);
static_assert(std::is_nothrow_move_assignable_v<CompiledHeaderTemplates>);
static_assert(noexcept(std::declval<const CompiledHeaderTemplates &>().contains({})));
static_assert(noexcept(std::declval<const CompiledHeaderTemplates &>().contains({}, 0)));
static_assert(noexcept(std::declval<const CompiledHeaderTemplates &>().size()));
static_assert(noexcept(std::declval<const CompiledHeaderTemplates &>().dynamic_size()));
static_assert(noexcept(std::declval<const CompiledHeaderTemplates &>().empty()));
static_assert(noexcept(std::declval<const CompiledHeaderTemplates &>().begin()));
static_assert(noexcept(std::declval<const CompiledHeaderTemplates &>().end()));

CompiledTemplate compiled_template(std::string_view source) {
    auto compiled = parse_template(source);
    EXPECT_TRUE(compiled);
    return compiled ? std::move(*compiled) : CompiledTemplate{};
}

TEST(CompiledHeaderTemplatesTest, BuildsCaseInsensitiveIndexAndPreservesInsertionOrder) {
    CompiledHeaderTemplates::Builder builder(3);
    ASSERT_TRUE(builder.insert("X-First", compiled_template("first")));
    ASSERT_TRUE(builder.insert("x-second", compiled_template("second")));
    ASSERT_TRUE(builder.insert("X-Third", compiled_template("third")));

    const CompiledHeaderTemplates headers = std::move(builder).build();

    EXPECT_EQ(headers.size(), 3U);
    EXPECT_EQ(headers.dynamic_size(), 0U);
    EXPECT_TRUE(headers.contains("x-first"));
    const std::string_view lowcase_second = "x-second";
    EXPECT_TRUE(headers.contains(lowcase_second, fiber::http::http_header_name_hash(lowcase_second)));
    EXPECT_FALSE(headers.contains("x-missing"));

    std::vector<std::string> names;
    std::vector<std::string> lowcase_names;
    std::vector<std::string> values;
    for (const CompiledHeaderTemplates::EntryView entry: headers) {
        names.emplace_back(entry.name());
        lowcase_names.emplace_back(entry.lowcase_name());
        values.push_back(entry.value().trailing_literal);
        EXPECT_EQ(entry.hash(), fiber::http::http_header_name_hash(entry.lowcase_name()));
    }
    EXPECT_EQ(names, (std::vector<std::string>{"X-First", "x-second", "X-Third"}));
    EXPECT_EQ(lowcase_names, (std::vector<std::string>{"x-first", "x-second", "x-third"}));
    EXPECT_EQ(values, (std::vector<std::string>{"first", "second", "third"}));

    auto entry = headers.begin();
    ASSERT_NE(entry, headers.end());
    EXPECT_EQ((*entry).lowcase_name(), "x-first");
    EXPECT_EQ((*entry).hash(), fiber::http::http_header_name_hash("x-first"));
}

TEST(CompiledHeaderTemplatesTest, RejectsCaseInsensitiveDuplicates) {
    CompiledHeaderTemplates::Builder builder(3);
    ASSERT_TRUE(builder.insert("X-Duplicate", compiled_template("first")));

    const auto duplicate = builder.insert("x-duplicate", compiled_template("second"));

    ASSERT_FALSE(duplicate);
    EXPECT_EQ(duplicate.error(), CompiledHeaderTemplates::InsertError::DuplicateName);
}

TEST(CompiledHeaderTemplatesTest, CountsOnlyDynamicValuesForRequestStorage) {
    CompiledHeaderTemplates::Builder builder(2);
    ASSERT_TRUE(builder.insert("X-Static", compiled_template("static")));
    ASSERT_TRUE(builder.insert("X-Dynamic", compiled_template("${value}")));

    const CompiledHeaderTemplates headers = std::move(builder).build();

    EXPECT_EQ(headers.size(), 2U);
    EXPECT_EQ(headers.dynamic_size(), 1U);
    const CompiledHeaderTemplates copied = headers;
    EXPECT_EQ(copied.dynamic_size(), 1U);
}

TEST(CompiledHeaderTemplatesTest, BuildsEmptyImmutableCollection) {
    CompiledHeaderTemplates::Builder builder;
    const CompiledHeaderTemplates headers = std::move(builder).build();

    EXPECT_TRUE(headers.empty());
    EXPECT_EQ(headers.begin(), headers.end());
    EXPECT_FALSE(headers.contains("anything"));
}

} // namespace
