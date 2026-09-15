#pragma once

#include <cassert>
#include <cstring>
#include <stdexcept>
#include <string_view>
#include "Heap.h"
#include "HashTable.h"
#include "HashingPolicy.h"
#include "VM.h"

class StringInterner {
public:
    explicit StringInterner(uint32_t initialCapacity = 32)
        : table_(initialCapacity) {
    }

    // Intern a string by content.
    // Returns the existing interned string if present, otherwise allocates
    // a new KIND_STRING object in the heap and inserts it into the table.
    //
    // If the heap is full and ctx != nullptr:
    //   - temporarily installs this interner as the active TLS interner
    //   - runs GC once
    //   - retries allocation
    //
    // If ctx == nullptr and the heap is full, this throws.
    [[nodiscard]] Value intern(std::string_view s, Heap& heap, Context* ctx = nullptr) {
        const uint32_t h = StringPoolPolicy::hashBytes(s);

        Entry* entry = table_.findCustom(h, [&](const Value& key) noexcept {
            if (!key.isPtr()) {
                return false;
            }

            const GcObject* obj = GcObject::from_slots(key.asPtr());
            if (obj->kind != GcObject::KIND_STRING) {
                return false;
            }

            if (obj->string_length() != s.size()) {
                return false;
            }

            return std::memcmp(obj->bytes(), s.data(), s.size()) == 0;
        });

        if (entry && !entry->isEmpty() && !entry->isTombstone()) {
            return entry->key;
        }

        GcObject* obj = heap.alloc_string(s);
        if (!obj) {
            if (!ctx) {
                throw std::runtime_error("StringInterner: heap exhausted");
            }

            // No self-install needed: collect() reaches this interner as an
            // external strong root via ctx->vm->interner (which the caller has
            // set to this interner). See forward_vm_external_roots in VM.h.
            heap.collect(ctx);

            // Another thread-local user on the same thread could theoretically
            // have interned the string during callbacks/finalizers in a future
            // design, so re-check before retrying allocation.
            entry = table_.findCustom(h, [&](const Value& key) noexcept {
                if (!key.isPtr()) {
                    return false;
                }

                const GcObject* retry_obj = GcObject::from_slots(key.asPtr());
                if (retry_obj->kind != GcObject::KIND_STRING) {
                    return false;
                }

                if (retry_obj->string_length() != s.size()) {
                    return false;
                }

                return std::memcmp(retry_obj->bytes(), s.data(), s.size()) == 0;
            });

            if (entry && !entry->isEmpty() && !entry->isTombstone()) {
                return entry->key;
            }

            obj = heap.alloc_string(s);
            if (!obj) {
                // Last resort: grow the heap to fit this string, then retry.
                if (heap.reserve_string(s)) {
                    obj = heap.alloc_string(s);
                }
                if (!obj) {
                    throw std::runtime_error("StringInterner: heap exhausted after GC");
                }
            }
        }

        Value str = Value::fromPtr(obj->bytes());
        table_.setWithHash(str, str, h);
        return str;
    }

    // Lookup only -- returns Undefined if not interned.
    [[nodiscard]] Value find(std::string_view s) {
        const uint32_t h = StringPoolPolicy::hashBytes(s);

        Entry* entry = table_.findCustom(h, [&](const Value& key) noexcept {
            if (!key.isPtr()) {
                return false;
            }

            const GcObject* obj = GcObject::from_slots(key.asPtr());
            if (obj->kind != GcObject::KIND_STRING) {
                return false;
            }

            if (obj->string_length() != s.size()) {
                return false;
            }

            return std::memcmp(obj->bytes(), s.data(), s.size()) == 0;
        });

        if (!entry || entry->isEmpty() || entry->isTombstone()) {
            return Value::fromUndefined();
        }

        return entry->key;
    }

    // Strong-root forwarding for all interned strings currently held by the
    // table. Each interned string is stored as both key and value, so both
    // pointer slots are evacuated in place. Content-based hashing means the
    // moved pointer keeps its slot, so no rehash is needed (see
    // HashTable::for_each_entry).
    void forward_roots(Heap& heap) noexcept {
        table_.for_each_entry([&](Entry& entry) noexcept {
            heap.forward(&entry.key);
            heap.forward(&entry.value);
        });
    }

    [[nodiscard]] auto begin() const noexcept { return table_.begin(); }
    [[nodiscard]] auto end()   const noexcept { return table_.end();   }

private:
    HashTable<StringPoolPolicy> table_;
};
