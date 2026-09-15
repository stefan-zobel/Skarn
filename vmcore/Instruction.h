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

// =============================================================================
// Instruction encoding -- all layouts share the same 32-bit raw word.
// =============================================================================
union Instruction {

    uint32_t raw;

    // r6 -- Standard 3-register layout, 6-bit fields, 64 addressable registers.
    struct {
        uint32_t opcode :  8;
        uint32_t rd     :  6;
        uint32_t ra     :  6;
        uint32_t rb     :  6;
        uint32_t flags  :  6;
    } r6;

    // r8 -- 3-register layout, 8-bit fields, 256 addressable registers.
    struct {
        uint32_t opcode :  8;
        uint32_t rd     :  8;
        uint32_t ra     :  8;
        uint32_t rb     :  8;
    } r8;

    // c2 -- 1 register + 16-bit signed constant.
    struct {
        uint32_t opcode :  8;
        uint32_t rd     :  8;
        int32_t  cnst   : 16;
    } c2;

    // j -- No registers, 24-bit signed offset. Used for J, RET, HALT, NOP.
    struct {
        uint32_t opcode :  8;
        int32_t  offset : 24;
    } j;

    // call -- CALL/TCO_CALL: window_size(8) + offset(16, signed).
    // window_size is the CALLEE's frame size (its register-window size = GC scan
    // count), NOT the window slide distance. The slide is the CALLER's frame size,
    // taken from ctx->current_frame_size at run time (see op_call in op_control.h
    // and "Calling convention" in docs/VirtualMachine.md).
    struct {
        uint32_t opcode      :  8;
        uint32_t window_size :  8;
        int32_t  offset      : 16;
    } call;

    // q4 -- 4 x 5-bit registers + 4-bit extra. 32 addressable registers.
    struct {
        uint32_t opcode :  8;
        uint32_t rd     :  5;
        uint32_t ra     :  5;
        uint32_t rb     :  5;
        uint32_t rc     :  5;
        uint32_t extra  :  4;
    } q4;

    // b -- Branch layout: 2 x 6-bit registers + 12-bit signed offset.
    struct {
        uint32_t opcode :  8;
        uint32_t ra     :  6;
        uint32_t rb     :  6;
        int32_t  offset : 12;
    } b;

    // b1 -- Single-register branch: 1 x 8-bit register + 16-bit signed offset.
    struct {
        uint32_t opcode :  8;
        uint32_t ra     :  8;
        int32_t  offset : 16;
    } b1;

    // wide -- LOAD_CONST_WIDE extension word.
    // Always paired with a preceding LOAD_CONST (c2):
    //   value = (c2.cnst << 16) | wide.payload
    // Together they form a 32-bit signed constant.
    // Chain multiple LOAD_CONST_WIDE instructions for wider values:
    //   3 instructions cover 48-bit (full Int range).
    struct {
        uint32_t opcode  :  8;
        uint32_t rd      :  8;
        uint32_t payload : 16; // lower 16 bits of the extended constant
    } wide;

    [[msvc::forceinline]] static constexpr Instruction from_raw(uint32_t v) {
        Instruction i{}; i.raw = v; return i;
    }

    // Prop-layout slot index: the 12-bit immediate of GET_PROP / SET_PROP, split
    // across the r6 rb(6) and flags(6) fields (index = rb | flags<<6). Decoded via
    // Layout::Prop so the frame validator / disassembler treat those two fields as
    // an immediate, not registers (rd/ra remain real registers).
    [[nodiscard]] [[msvc::forceinline]] constexpr uint32_t prop_slot() const noexcept {
        return static_cast<uint32_t>(r6.rb) | (static_cast<uint32_t>(r6.flags) << 6);
    }

    [[msvc::forceinline]] static constexpr Instruction R6(uint8_t op, uint8_t rd, uint8_t ra, uint8_t rb, uint8_t flags = 0) {
        // r6 fields are 6-bit -- a wider index would silently truncate (and slip
        // past validate_frames, which reads the field after truncation).
        assert(rd < 64 && ra < 64 && rb < 64 && flags < 64 && "R6 field exceeds 6 bits");
        Instruction i{};
        i.r6.opcode = op; i.r6.rd = rd; i.r6.ra = ra; i.r6.rb = rb; i.r6.flags = flags;
        return i;
    }

    [[msvc::forceinline]] static constexpr Instruction R8(uint8_t op, uint8_t rd, uint8_t ra, uint8_t rb) {
        Instruction i{};
        i.r8.opcode = op; i.r8.rd = rd; i.r8.ra = ra; i.r8.rb = rb;
        return i;
    }

    [[msvc::forceinline]] static constexpr Instruction C2(uint8_t op, uint8_t rd, int16_t val) {
        Instruction i{};
        i.c2.opcode = op; i.c2.rd = rd; i.c2.cnst = val;
        return i;
    }

    [[msvc::forceinline]] static constexpr Instruction J(uint8_t op, int32_t offset = 0) {
        Instruction i{};
        i.j.opcode = op; i.j.offset = offset;
        return i;
    }

    [[msvc::forceinline]] static constexpr Instruction CALL(uint8_t op, uint8_t window_size, int16_t offset = 0) {
        Instruction i{};
        i.call.opcode      = op;
        i.call.window_size = window_size;
        i.call.offset      = offset;
        return i;
    }

    [[msvc::forceinline]] static constexpr Instruction Q4(uint8_t op, uint8_t rd, uint8_t ra, uint8_t rb, uint8_t rc, uint8_t flags = 0) {
        // q4 registers are 5-bit, extra is 4-bit.
        assert(rd < 32 && ra < 32 && rb < 32 && rc < 32 && flags < 16 && "Q4 field out of range");
        Instruction i{};
        i.q4.opcode = op; i.q4.rd = rd; i.q4.ra = ra; i.q4.rb = rb; i.q4.rc = rc; i.q4.extra = flags;
        return i;
    }

    // CallNative -- CALL_NATIVE: 3 registers + a 6-bit argument COUNT. Reuses the r6
    // bit-field positions (so every field is 6-bit and the whole 64-register file is
    // reachable), but `flags` carries nargs, not a register:
    //   rd = result, ra = register holding the NativeId, rb = first argument register,
    //   nargs = number of arguments the native reads in place from [rb, rb + nargs).
    // It was q4-encoded (5-bit fields) earlier; a frame with >= 32 live
    // registers then truncated the id/result fields in Release, silently calling a
    // DIFFERENT native and writing its result to a DIFFERENT register.
    [[msvc::forceinline]] static constexpr Instruction CallNative(uint8_t op, uint8_t rd, uint8_t ra, uint8_t rb, uint8_t nargs) {
        assert(rd < 64 && ra < 64 && rb < 64 && "CallNative register field exceeds 6 bits");
        assert(nargs < 64 && "CallNative nargs exceeds 6 bits");
        Instruction i{};
        i.r6.opcode = op; i.r6.rd = rd; i.r6.ra = ra; i.r6.rb = rb; i.r6.flags = nargs;
        return i;
    }

    [[msvc::forceinline]] static constexpr Instruction B(uint8_t op, uint8_t ra, uint8_t rb, int16_t offset = 0) {
        // b registers are 6-bit (the 12-bit offset is range-checked in Assembler::patch).
        assert(ra < 64 && rb < 64 && "B register field exceeds 6 bits");
        Instruction i{};
        i.b.opcode = op; i.b.ra = ra; i.b.rb = rb; i.b.offset = offset;
        return i;
    }

    [[msvc::forceinline]] static constexpr Instruction B1(uint8_t op, uint8_t ra, int16_t offset = 0) {
        Instruction i{};
        i.b1.opcode = op; i.b1.ra = ra; i.b1.offset = offset;
        return i;
    }

    // Prop -- GET_PROP / SET_PROP: 2 registers + 12-bit slot index. Reuses the r6
    // bit-field positions, but the slot occupies rb(6)|flags(6) as one immediate
    // (0..4095), decoded via prop_slot() / Layout::Prop. rd/ra are real registers:
    //   GET_PROP: rd = destination, ra = object
    //   SET_PROP: rd = value source, ra = object
    [[msvc::forceinline]] static constexpr Instruction Prop(uint8_t op, uint8_t rd, uint8_t ra, uint16_t slot) {
        assert(rd < 64 && ra < 64 && "Prop register field exceeds 6 bits");
        assert(slot < 4096 && "Prop slot index exceeds 12 bits");
        Instruction i{};
        i.r6.opcode = op; i.r6.rd = rd; i.r6.ra = ra;
        i.r6.rb = slot & 0x3F; i.r6.flags = (slot >> 6) & 0x3F;
        return i;
    }

    // LOAD_CONST_WIDE part 2: rd + lower 16-bit payload
    [[msvc::forceinline]] static constexpr Instruction Wide(uint8_t op, uint8_t rd, uint16_t payload) {
        Instruction i{};
        i.wide.opcode  = op;
        i.wide.rd      = rd;
        i.wide.payload = payload;
        return i;
    }
};

static_assert(sizeof(Instruction) == 4);
