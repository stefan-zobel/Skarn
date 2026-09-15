/*
 * Copyright 2026 Stefan Zobel
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include <bit>
#include <cassert>
#include <vector>
#include "HashingPolicy.h"

struct Entry {
    Value key;
    Value value;

    constexpr bool isEmpty() const noexcept { return key.isNil(); }
    constexpr bool isTombstone() const noexcept { return key.isTombstone(); }
};

template <typename Policy = DefaultHashPolicy>
class HashTable {
    std::vector<Entry> entries;
    uint32_t count{ 0 };
    static constexpr float LOAD_FACTOR = 0.70f;

public:
    HashTable(uint32_t initialCapacity = 32) {
        assert(initialCapacity != 0 && "HashTable capacity must be > 0");
        assert(std::has_single_bit(initialCapacity) && "HashTable capacity must be a power of two");
        entries.resize(initialCapacity, { Value::fromNil(), Value::fromNil() });
    }

    ~HashTable() = default;

    // Standard insert: fully policy-driven.
    void set(Value key, Value value) {
        setWithHash(key, value, Policy::hash(key));
    }

    // Explicit insert with caller-provided hash.
    // IMPORTANT:
    //   customHash MUST be consistent with Policy::hash(key), otherwise
    //   later lookup/rehash/remove may fail.
    void setWithHash(Value key, Value value, uint32_t customHash) {
        if (count + 1 > entries.size() * LOAD_FACTOR) {
            adjustCapacity();
        }

        Entry* targetSlot = findEntryWithHash(
            key,
            customHash,
            [&](const Value& existing) noexcept {
                return Policy::isEqual(existing, key);
            });

        if (targetSlot->isEmpty()) {
            count++;
        }

        targetSlot->key = key;
        targetSlot->value = value;
    }

    bool remove(Value key) {
        if (count == 0) return false;

        Entry* entry = findEntry(key);
        if (entry->isEmpty() || entry->isTombstone()) return false;

        entry->key = Value::tombstone();
        entry->value = Value::fromNil();
        return true;
    }

    template<typename Predicate>
    [[nodiscard]] Entry* findCustom(uint32_t hash, Predicate&& isMatch) {
        if (entries.empty()) return nullptr;

        uint32_t mask = static_cast<uint32_t>(entries.size() - 1);
        uint32_t index = hash & mask;

        for (;;) {
            Entry& entry = entries[index];
            if (entry.isEmpty()) return &entry;

            if (!entry.isTombstone() && isMatch(entry.key)) {
                return &entry;
            }

            index = (index + 1) & mask;
        }
    }

    [[nodiscard]] [[msvc::forceinline]]
    Entry* findEntry(Value key) {
        return findEntryWithHash(
            key,
            Policy::hash(key),
            [&](const Value& existing) noexcept {
                return Policy::isEqual(existing, key);
            });
    }

    Value get(Value key) {
        Entry* entry = findEntry(key);
        if (entry->isEmpty() || entry->isTombstone()) {
            return Value::fromNil();
        }
        return entry->value;
    }

    Value getOrUndefined(Value key) {
        Entry* entry = findEntry(key);
        if (entry->isEmpty() || entry->isTombstone()) {
            return Value::fromUndefined();
        }
        return entry->value;
    }

    class FilterIterator {
        using VecIt = std::vector<Entry>::const_iterator;

        VecIt current{};
        VecIt end{};

        void skip_invalid() {
            while (current != end && (current->isEmpty() || current->isTombstone())) {
                ++current;
            }
        }

    public:
        using iterator_concept = std::forward_iterator_tag;
        using iterator_category = std::forward_iterator_tag;
        using value_type = Entry;
        using difference_type = std::ptrdiff_t;
        using pointer = const Entry*;
        using reference = const Entry&;

        FilterIterator() = default;

        FilterIterator(VecIt begin, VecIt end) : current(begin), end(end) {
            skip_invalid();
        }

        reference operator*() const { return *current; }
        pointer operator->() const { return &(*current); }

        FilterIterator& operator++() {
            if (current != end) {
                ++current;
                skip_invalid();
            }
            return *this;
        }

        FilterIterator operator++(int) {
            FilterIterator tmp = *this;
            ++(*this);
            return tmp;
        }

        bool operator==(const FilterIterator& other) const = default;
    };

    [[nodiscard]] FilterIterator begin() const noexcept { return FilterIterator(entries.begin(), entries.end()); }
    [[nodiscard]] FilterIterator end() const noexcept { return FilterIterator(entries.end(), entries.end()); }

    // Visit every live entry with mutable access. Used by a copying GC to
    // forward pointer-valued keys/values in place. The visitor MUST NOT change
    // an entry's hash identity -- forwarding a heap pointer is safe here only
    // because StringPoolPolicy hashes by string *content*, which the move does
    // not alter, so the slot placement stays valid.
    template <typename F>
    void for_each_entry(F&& visit) {
        for (auto& entry : entries) {
            if (!entry.isEmpty() && !entry.isTombstone())
                visit(entry);
        }
    }

    void removeUnmarked() {
        for (auto& entry : entries) {
            if (!entry.isEmpty() && !entry.isTombstone() && Policy::shouldRemove(entry.key)) {
                entry.key = Value::tombstone();
                entry.value = Value::fromNil();
            }
        }
    }

private:

    template<typename Predicate>
    [[nodiscard]] Entry* findEntryWithHash(Value /*key*/, uint32_t hash, Predicate&& isMatch) {
        uint32_t mask = static_cast<uint32_t>(entries.size() - 1);
        uint32_t index = hash & mask;
        Entry* firstTombstone = nullptr;

        for (;;) {
            Entry& entry = entries[index];

            if (entry.isEmpty()) {
                return firstTombstone != nullptr ? firstTombstone : &entry;
            }

            if (!entry.isTombstone() && isMatch(entry.key)) {
                return &entry;
            }

            if (entry.isTombstone() && firstTombstone == nullptr) {
                firstTombstone = &entry;
            }

            index = (index + 1) & mask;
        }
    }

    void adjustCapacity() {
        uint32_t newCap = static_cast<uint32_t>(entries.size() * 2);
        std::vector<Entry> newEntries(newCap, { Value::fromNil(), Value::fromNil() });
        uint32_t mask = newCap - 1;
        uint32_t newCount = 0;

        for (const auto& entry : entries) {
            if (entry.isEmpty() || entry.isTombstone()) continue;

            uint32_t h = Policy::hash(entry.key);
            uint32_t index = h & mask;

            for (;;) {
                if (newEntries[index].isEmpty()) {
                    newEntries[index] = entry;
                    newCount++;
                    break;
                }
                index = (index + 1) & mask;
            }
        }

        entries = std::move(newEntries);
        count = newCount;
    }
};

static_assert(std::forward_iterator<HashTable<DefaultHashPolicy>::FilterIterator>);
