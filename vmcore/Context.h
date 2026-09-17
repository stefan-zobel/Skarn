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

#include <cstdint>
#include <cassert>
#include "Value.h"

struct VM;

struct alignas(16) ReturnFrame {
    const uint8_t* old_ip;
    Value*         old_window;
};
static_assert(sizeof(ReturnFrame)  == 16);
static_assert(alignof(ReturnFrame) == 16);

// 7 x 8 + 1 + 7 (implicit pad) = 64 Byte
struct alignas(64) Context {
    const uint8_t* ip;
    Value*         window_ptr;
    VM*            vm;              // heap + globals + interner + const pool
    ReturnFrame*   ret_stack_ptr;
    ReturnFrame*   ret_stack_limit;
    ReturnFrame*   ret_stack_base;
    uint8_t*       frame_size_ptr;
    uint8_t        current_frame_size;
};
static_assert(sizeof(Context) == 64);

// Handler typedef (retired with the threaded tail-call dispatcher; kept as a
// forward-compat alias in case external tooling references it).
typedef void(*Handler)(Context* ctx);
