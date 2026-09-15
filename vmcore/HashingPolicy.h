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

#include <cassert>
#include <cstdint>
#include <cstring>
#include <string_view>
#include "Value.h"
#include "Heap.h"

struct DefaultHashPolicy {
    [[nodiscard]] [[msvc::forceinline]]
    static uint32_t hash(const Value& v) noexcept {
        return v.hash();
    }

    [[nodiscard]] [[msvc::forceinline]]
    static bool isEqual(const Value& a, const Value& b) noexcept {
        return a == b;
    }

    [[nodiscard]] [[msvc::forceinline]]
    static bool shouldRemove(const Value& /*v*/) noexcept {
        return false;
    }
};

struct StringPoolPolicy {
private:
    [[nodiscard]] [[msvc::forceinline]]
    static const GcObject* asStringObject(const Value& v) noexcept {
        assert(v.isPtr() && "StringPoolPolicy requires pointer Values");
        const GcObject* obj = GcObject::from_slots(v.asPtr());
        assert(obj->kind == GcObject::KIND_STRING && "StringPoolPolicy requires KIND_STRING objects");
        return obj;
    }

public:
    [[nodiscard]] [[msvc::forceinline]]
    static uint32_t hashBytes(const char* data, uint32_t len) noexcept {
        // FNV-1a 32-bit
        uint32_t h = 2166136261u;
        for (uint32_t i = 0; i < len; ++i) {
            h ^= static_cast<uint8_t>(data[i]);
            h *= 16777619u;
        }
        return h;
    }

    [[nodiscard]] [[msvc::forceinline]]
    static uint32_t hashBytes(std::string_view s) noexcept {
        return hashBytes(s.data(), static_cast<uint32_t>(s.size()));
    }

    [[nodiscard]] [[msvc::forceinline]]
    static uint32_t hash(const Value& v) noexcept {
        const GcObject* s = asStringObject(v);
        return hashBytes(s->bytes(), s->string_length());
    }

    [[nodiscard]] [[msvc::forceinline]]
    static bool isEqual(const Value& a, const Value& b) noexcept {
        if (a == b) return true;
        if (!a.isPtr() || !b.isPtr()) return false;

        const GcObject* sA = GcObject::from_slots(a.asPtr());
        const GcObject* sB = GcObject::from_slots(b.asPtr());

        if (sA->kind != GcObject::KIND_STRING || sB->kind != GcObject::KIND_STRING) {
            return false;
        }

        if (sA->string_length() != sB->string_length()) {
            return false;
        }

        return std::memcmp(sA->bytes(), sB->bytes(), sA->string_length()) == 0;
    }

    [[nodiscard]] [[msvc::forceinline]]
    static bool shouldRemove(const Value& v) noexcept {
        const GcObject* s = asStringObject(v);
        return s->color == GcObject::WHITE;
    }
};
