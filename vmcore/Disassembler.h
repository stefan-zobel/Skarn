#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <format>
#include <iostream>
#include "Instruction.h"
#include "Opcodes.h"

// =============================================================================
// Disassembler -- decodes a flat uint32_t bytecode array and prints it.
// Uses layout_of() from Opcodes.h for fully generic operand decoding
// -- no switch cascades over opcode ranges needed.
// =============================================================================
class Disassembler {
public:
    Disassembler(const uint32_t* code, size_t count)
        : code_(code), count_(count) {}

    void add_label(size_t index, const std::string& name) {
        labels_[index] = name;
    }

    void print() const {
        for (size_t i = 0; i < count_; ++i) {
            if (auto it = labels_.find(i); it != labels_.end())
                std::cout << "\n" << it->second << ":\n";

            const Instruction instr       = Instruction::from_raw(code_[i]);
            const uint8_t     opcode_byte = static_cast<uint8_t>(instr.r6.opcode);
            const char*       name        = opcode_name(opcode_byte);
            const std::string operands    = decode_operands(instr, opcode_byte, i);

            const uint32_t raw = code_[i];
            const uint32_t idx = static_cast<uint32_t>(i);
            std::cout << std::format("  [{:>2}]  {:08X}  {:<16}  {}\n",
                idx, raw, name, operands);
        }
        std::cout << "\n";
    }

private:
    const uint32_t*                          code_;
    size_t                                   count_;
    std::unordered_map<size_t, std::string>  labels_;

    static const char* opcode_name(uint8_t byte) {
        for (const auto& e : OpList)
            if (e.id == byte) return e.name;
        return "???";
    }

    // Generic operand decoder -- driven entirely by Layout, no opcode checks needed
    std::string decode_operands(Instruction instr, uint8_t opcode_byte, size_t site) const {
        switch (layout_of(opcode_byte)) {

            case Layout::J: {
                const auto opc = static_cast<OpCode>(opcode_byte);
                if (opc == OpCode::HALT || opc == OpCode::RET || opc == OpCode::NOP)
                    return "";
                const int32_t     off    = instr.j.offset;
                const int32_t     target = static_cast<int32_t>(site) + 1 + off;
                const std::string lbl    = label_at(static_cast<size_t>(target));
                if (lbl.empty())
                    return std::format("offset {:+d}  -> [{}]", off, target);
                return std::format("{}  (offset {:+d})", lbl, off);
            }

            case Layout::Call: {
                const unsigned    ws     = instr.call.window_size;
                const int32_t     off    = instr.call.offset;
                const int32_t     target = static_cast<int32_t>(site) + 1 + off;
                const std::string lbl    = label_at(static_cast<size_t>(target));
                if (lbl.empty())
                    return std::format("callee_frame={}, offset {:+d}  -> [{}]", ws, off, target);
                return std::format("{}  (callee_frame={}, offset {:+d})", lbl, ws, off);
            }

            case Layout::C2: {
                const auto     opc  = static_cast<OpCode>(opcode_byte);
                const unsigned rd   = instr.c2.rd;
                const int32_t  cnst = instr.c2.cnst;
                if (opc == OpCode::INCR || opc == OpCode::DECR)
                    return std::format("r{}", rd);
                if (opc == OpCode::LOAD_CONST_POOL)
                    return std::format("r{}, k[{}]", rd,
                                       static_cast<uint16_t>(instr.c2.cnst));
                if (opc == OpCode::NEW_STRUCT)
                    return std::format("r{}, type#{}", rd,
                                       static_cast<uint16_t>(instr.c2.cnst));
                if (opc == OpCode::LOAD_FN)
                    return std::format("r{}, fn#{}", rd,
                                       static_cast<uint16_t>(instr.c2.cnst));
                if (opc == OpCode::LOAD_CAPTURE)
                    return std::format("r{}, capture#{}", rd,
                                       static_cast<uint16_t>(instr.c2.cnst));
                return std::format("r{}, {}", rd, cnst);
            }

            case Layout::Wide: {
                const unsigned rd      = instr.wide.rd;
                const unsigned payload = instr.wide.payload;
                return std::format("r{}, 0x{:04X}", rd, payload);
            }

            case Layout::B: {
                const unsigned    ra     = instr.b.ra;
                const unsigned    rb     = instr.b.rb;
                const int32_t     off    = instr.b.offset;
                const int32_t     target = static_cast<int32_t>(site) + 1 + off;
                const std::string lbl    = label_at(static_cast<size_t>(target));
                if (lbl.empty())
                    return std::format("r{}, r{}, offset {:+d}  -> [{}]", ra, rb, off, target);
                return std::format("r{}, r{}, {}  (offset {:+d})", ra, rb, lbl, off);
            }

            case Layout::B1: {
                const unsigned    ra     = instr.b1.ra;
                const int32_t     off    = instr.b1.offset;
                const int32_t     target = static_cast<int32_t>(site) + 1 + off;
                const std::string lbl    = label_at(static_cast<size_t>(target));
                if (lbl.empty())
                    return std::format("r{}, offset {:+d}  -> [{}]", ra, off, target);
                return std::format("r{}, {}  (offset {:+d})", ra, lbl, off);
            }

            case Layout::Q4: {
                // All four fields are registers (e.g. CMOV)
                const unsigned rd = instr.q4.rd;
                const unsigned ra = instr.q4.ra;
                const unsigned rb = instr.q4.rb;
                const unsigned rc = instr.q4.rc;
                return std::format("r{}, r{}, r{}, r{}", rd, ra, rb, rc);
            }

            case Layout::CallNative: {
                // r6-encoded; flags is nargs (a count), not a register
                const unsigned rd    = instr.r6.rd;
                const unsigned ra    = instr.r6.ra;
                const unsigned rb    = instr.r6.rb;
                const unsigned nargs = instr.r6.flags;
                return std::format("r{} = r{}(&r{}, nargs={})", rd, ra, rb, nargs);
            }

            case Layout::CallIndirect: {
                // ra is the callee register; rb is nargs (a count), not a register.
                const unsigned ra    = instr.r6.ra;
                const unsigned nargs  = instr.r6.rb;
                return std::format("r{}(nargs={})", ra, nargs);
            }

            case Layout::R8: {
                const unsigned rd = instr.r8.rd;
                const unsigned ra = instr.r8.ra;
                const unsigned rb = instr.r8.rb;
                return std::format("r{}, r{}, r{}", rd, ra, rb);
            }

            case Layout::ResolveCall: {
                // rd = nargs (a count); slot is the method id. No register operands (the receiver is
                // arg 0 -- window[frame_size] for RESOLVE_CALL, window[0] for the tail RESOLVE_TCO_CALL).
                const unsigned nargs = instr.r6.rd;
                const unsigned slot  = instr.prop_slot();
                return std::format("method#{}(nargs={})", slot, nargs);
            }

            case Layout::Prop: {
                // rd + ra are registers; rb|flags is a 12-bit slot index.
                const unsigned rd   = instr.r6.rd;
                const unsigned ra   = instr.r6.ra;
                const unsigned slot = instr.prop_slot();
                if (static_cast<OpCode>(opcode_byte) == OpCode::SET_PROP)
                    return std::format("r{}.#{} = r{}", ra, slot, rd);
                if (static_cast<OpCode>(opcode_byte) == OpCode::MAKE_CLOSURE)
                    // ra is the capture BASE register; slot is the fn id.
                    return std::format("r{} = closure(fn#{}, captures@r{})", rd, slot, ra);
                return std::format("r{} = r{}.#{}", rd, ra, slot);
            }

            case Layout::R6:
            default: {
                const unsigned rd = instr.r6.rd;
                const unsigned ra = instr.r6.ra;
                const unsigned rb = instr.r6.rb;
                return std::format("r{}, r{}, r{}", rd, ra, rb);
            }
        }
    }

    std::string label_at(size_t index) const {
        if (auto it = labels_.find(index); it != labels_.end())
            return it->second;
        return {};
    }
};
