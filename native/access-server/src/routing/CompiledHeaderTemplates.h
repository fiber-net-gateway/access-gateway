#ifndef FIBER_ACCESS_SERVER_COMPILED_HEADER_TEMPLATES_H
#define FIBER_ACCESS_SERVER_COMPILED_HEADER_TEMPLATES_H

#include <fiber/http/HeaderMap.h>
#include "CompiledTemplate.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <iterator>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fiber::access_server {

struct CompiledTemplateEntry {
    std::string name;
    CompiledTemplate value;
};

class CompiledHeaderTemplates {
    struct StoredValue {
        CompiledTemplate value;
        std::string lowcase_name;
        std::uint64_t name_hash = 0;
    };

    using Storage = http::HeaderMap<StoredValue>;

public:
    enum class InsertError : std::uint8_t {
        DuplicateName,
        TooLarge,
    };

    class ConstIterator;

    class EntryView {
    public:
        [[nodiscard]] std::string_view name() const noexcept { return entry_.name(); }
        [[nodiscard]] std::string_view lowcase_name() const noexcept { return entry_.value().lowcase_name; }
        [[nodiscard]] std::uint64_t hash() const noexcept { return entry_.value().name_hash; }
        [[nodiscard]] const CompiledTemplate &value() const noexcept { return entry_.value().value; }

    private:
        friend class ConstIterator;

        explicit EntryView(Storage::EntryView entry) noexcept : entry_(entry) {}

        Storage::EntryView entry_;
    };

    class ConstIterator {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = EntryView;
        using difference_type = std::ptrdiff_t;
        using pointer = void;
        using reference = EntryView;

        ConstIterator() noexcept = default;

        [[nodiscard]] EntryView operator*() const noexcept { return EntryView(*iterator_); }
        ConstIterator &operator++() noexcept {
            ++iterator_;
            return *this;
        }
        ConstIterator operator++(int) noexcept {
            ConstIterator copy = *this;
            ++(*this);
            return copy;
        }
        [[nodiscard]] bool operator==(const ConstIterator &other) const noexcept {
            return iterator_ == other.iterator_;
        }
        [[nodiscard]] bool operator!=(const ConstIterator &other) const noexcept { return !(*this == other); }

    private:
        friend class CompiledHeaderTemplates;

        explicit ConstIterator(Storage::ConstIterator iterator) noexcept : iterator_(iterator) {}

        Storage::ConstIterator iterator_;
    };

    class Builder;

    CompiledHeaderTemplates() noexcept = default;
    CompiledHeaderTemplates(const CompiledHeaderTemplates &) = default;
    CompiledHeaderTemplates &operator=(const CompiledHeaderTemplates &) = default;
    CompiledHeaderTemplates(CompiledHeaderTemplates &&) noexcept = default;
    CompiledHeaderTemplates &operator=(CompiledHeaderTemplates &&) noexcept = default;
    ~CompiledHeaderTemplates() = default;

    [[nodiscard]] bool contains(std::string_view name) const noexcept;
    [[nodiscard]] bool contains(std::string_view lowcase_name, std::uint64_t hash) const noexcept;

    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::size_t dynamic_size() const noexcept { return dynamic_size_; }
    [[nodiscard]] bool empty() const noexcept { return size() == 0; }

    [[nodiscard]] ConstIterator begin() const noexcept;
    [[nodiscard]] ConstIterator end() const noexcept;

private:
    explicit CompiledHeaderTemplates(Storage storage, std::size_t dynamic_size) noexcept :
        storage_(std::move(storage)), dynamic_size_(dynamic_size) {}

    Storage storage_;
    std::size_t dynamic_size_ = 0;
};

class CompiledHeaderTemplates::Builder {
public:
    Builder() = default;
    explicit Builder(std::size_t expected_size);

    Builder(const Builder &) = delete;
    Builder &operator=(const Builder &) = delete;
    Builder(Builder &&) noexcept = default;
    Builder &operator=(Builder &&) noexcept = default;

    [[nodiscard]] std::expected<void, InsertError> insert(std::string name, CompiledTemplate value);

    [[nodiscard]] std::span<CompiledTemplateEntry> entries() noexcept { return entries_; }
    [[nodiscard]] std::span<const CompiledTemplateEntry> entries() const noexcept { return entries_; }
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }

    [[nodiscard]] CompiledHeaderTemplates build() &&;

private:
    static constexpr std::size_t kMaxEntries = std::numeric_limits<std::uint32_t>::max() / 4U;
    static constexpr std::size_t kMaxNameBytes =
            static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) - (sizeof(std::uint32_t) - 1);

    std::vector<CompiledTemplateEntry> entries_;
    std::size_t name_bytes_ = 0;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_COMPILED_HEADER_TEMPLATES_H
