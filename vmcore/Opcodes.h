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

// =============================================================================
// Opcodes.h -- the ISA *description*: the OpCode enum, the Layout enum, the
// per-opcode OpList (id / name / layout), and layout_of(). This is the stable
// ISA surface that the Assembler, the Disassembler, and the switch Interpreter
// build against.
//
// The shipping switch runtime (run_switch, Interpreter.h) references only
// OpCode / Layout / OpList / layout_of. The old threaded tail-call path (a
// Handler typedef + dispatch_table + Dispatch.h handoff macros) was retired when
// vmcore moved to while{switch}; it survived in the vm_bench benchmark, which was
// removed earlier -- so none of that scaffolding exists here any more.
// =============================================================================

#include <array>
#include "Context.h"

using NativeFunc = Value(*)(Value* args, uint8_t nargs, Context* ctx);

enum class OpCode : uint8_t {
    NOP             =  0,
    HALT            =  1,
    CALL            =  2,
    RET             =  3,
    TCO_CALL        =  4,
    J               =  5,
    LOAD_CONST      =  6,
    MOV             =  7,
    BEQ_INT         =  8,
    BNE_INT         =  9,
    BLT_INT         = 10,
    BGE_INT         = 11,
    ADD_INT         = 12,
    SUB_INT         = 13,
    MUL_INT         = 14,
    INCR            = 15,
    DECR            = 16,
    SET_EQ          = 17,
    SET_LT          = 18,
    SET_LE          = 19,
    AND_BOOL        = 20,
    OR_BOOL         = 21,
    NOT_BOOL        = 22,
    BT              = 23,
    BF              = 24,
    CMOV            = 25,
    LOAD_CONST_WIDE = 26,
    CALL_NATIVE     = 27,
    LOAD_GLOBAL     = 28,
    STORE_GLOBAL    = 29,
    ALLOC           = 30, // rd = new array object with nslots
    DIV_INT         = 31, // rd = ra / rb  (signed, truncates toward zero; traps on rb == 0)
    MOD_INT         = 32, // rd = ra % rb  (signed, sign of dividend; traps on rb == 0)
    NEG_INT         = 33, // rd = -ra      (rb unused)
    AND_INT         = 34, // rd = ra & rb  (bitwise)
    OR_INT          = 35, // rd = ra | rb  (bitwise)
    XOR_INT         = 36, // rd = ra ^ rb  (bitwise)
    SHL_INT         = 37, // rd = ra << (rb & 63)  (logical left)
    SHR_INT         = 38, // rd = ra >> (rb & 63)  (arithmetic/signed right)
    USHR_INT        = 39, // rd = ra >>> (rb & 63) (logical/unsigned right)
    LOAD_CONST_POOL = 40, // rd = constants[index] (index in low 16 bits of cnst)
    ADD             = 41, // rd = ra + rb  (numeric; int+int wraps, else -> double)
    SUB             = 42, // rd = ra - rb  (numeric; int+int wraps, else -> double)
    MUL             = 43, // rd = ra * rb  (numeric; int+int wraps, else -> double)
    DIV             = 44, // rd = ra / rb  (int/int truncates + traps on 0; else IEEE)
    NEG             = 45, // rd = -ra      (numeric; rb unused)
    IS_INT          = 46, // rd = Bool(ra is Int)
    IS_DOUBLE       = 47, // rd = Bool(ra is Double)
    IS_BOOL         = 48, // rd = Bool(ra is Bool)
    IS_NIL          = 49, // rd = Bool(ra is Nil)
    IS_UNDEF        = 50, // rd = Bool(ra is Undefined)
    MOD             = 51, // rd = ra % rb  (numeric; int%int traps on 0; else fmod)
    SET_EQ_NUM      = 52, // rd = Bool(ra == rb)  (numeric, IEEE)
    SET_NE_NUM      = 53, // rd = Bool(ra != rb)  (numeric, IEEE; NaN != NaN)
    SET_LT_NUM      = 54, // rd = Bool(ra <  rb)  (numeric, IEEE; false if NaN)
    SET_LE_NUM      = 55, // rd = Bool(ra <= rb)  (numeric, IEEE; false if NaN)
    SET_GT_NUM      = 56, // rd = Bool(ra >  rb)  (numeric, IEEE; false if NaN)
    SET_GE_NUM      = 57, // rd = Bool(ra >= rb)  (numeric, IEEE; false if NaN)
    BEQ_NUM         = 58, // if (ra == rb) jump  (numeric, IEEE)
    BNE_NUM         = 59, // if (ra != rb) jump  (numeric, IEEE; NaN != NaN)
    BLT_NUM         = 60, // if (ra <  rb) jump  (numeric, IEEE; false if NaN)
    BLE_NUM         = 61, // if (ra <= rb) jump  (numeric, IEEE; false if NaN)
    BGT_NUM         = 62, // if (ra >  rb) jump  (numeric, IEEE; false if NaN)
    BGE_NUM         = 63, // if (ra >= rb) jump  (numeric, IEEE; false if NaN)
    NEW_STRUCT      = 64, // rd = new KIND_OBJECT of type #cnst (arity from registry)
    GET_PROP        = 65, // rd = robj.field[#slot]
    SET_PROP        = 66, // robj.field[#slot] = rval
    IS_OBJECT       = 67, // rd = Bool(ra is a heap struct object)
    TO_BOOL         = 68, // rd = Bool(truthy(ra)) -- coerce any Value to a truth value
    LNOT            = 69, // rd = Bool(!truthy(ra)) -- logical not with truthiness (rb unused)
    GET_TYPE_ID     = 70, // rd = Int(struct type id of ra), or Int(-1) if ra is not a struct
    EQ              = 71, // rd = Bool(ra == rb)  -- GENERAL Value equality (operator==)
    NE              = 72, // rd = Bool(ra != rb)  -- general Value inequality
    LOAD_STR        = 73, // rd = string_pool[cnst] -- interned string literal (c2, unsigned index)
    LT              = 74, // rd = Bool(ra < rb)  -- generic ordering: num (IEEE) or string (content)
    LE              = 75, // rd = Bool(ra <= rb) -- generic ordering: num (IEEE) or string (content)
    TO_STRING       = 76, // rd = String(ra) -- generic to-string coercion (rb unused; allocates for numbers)
    LOAD_FN         = 77, // rd = Func(fn_id) -- first-class function value (c2, unsigned fn-table id)
    CALL_INDIRECT   = 78, // call the function value in ra (nargs in rb); callee frame/entry from fn_table
    MAKE_CLOSURE    = 79, // rd = new KIND_CLOSURE(#fn_id) capturing ncaptures values from window[ra..]
    LOAD_CAPTURE    = 80, // rd = current_closure.captures[#cnst] (read a by-value capture; c2, unsigned index)
    TCO_CALL_INDIRECT = 81, // tail-call the function value in ra (nargs in rb); reuse frame, no push (CallIndirect)
    ARRAY_GET       = 82, // rd = arr[i]; ra=array, rb=index reg; bounds-checked (traps on OOB)
    ARRAY_SET       = 83, // arr[i] = rd (value); ra=array, rb=index reg; bounds-checked (traps on OOB)
    LEN             = 84, // rd = Int(length of the KIND_ARRAY in ra) (rb unused)
    ANEW            = 85, // rd = new KIND_ARRAY[ra] -- element count from a REGISTER (rb unused); allocates
    AFILL           = 86, // fill every slot of the KIND_ARRAY in rd with the value in ra (rb unused); no alloc
    PRINT           = 87, // write the KIND_STRING in ra to vm->out (rb unused); rd = Nil; no alloc/newline
    PRINTLN         = 88, // like PRINT but append '\n' (rb unused); rd = Nil
    MAP_NEW         = 89, // rd = new empty KIND_MAP (ra/rb unused); allocates header + backing
    MAP_GET         = 90, // rd = map[key] or Nil on miss; ra=map, rb=key reg; no alloc
    MAP_SET         = 91, // map[key] = rd (value); ra=map, rb=key reg; allocates (grow/rehash)
    MAP_HAS         = 92, // rd = Bool(key present in map ra); rb=key reg; no alloc (presence, not value)
    MAP_DELETE      = 93, // rd = Bool(key was present, now removed); ra=map, rb=key reg; no alloc (tombstone)
    MAP_KEYS        = 94, // rd = new KIND_ARRAY of the live keys of map ra (rb unused); allocates
    MAP_VALUES      = 95, // rd = new KIND_ARRAY of the live values of map ra (rb unused); allocates
    GET_KIND        = 96, // rd = Int(heap kind of ra: 0=ARRAY 1=STRING 2=OBJECT 3=CLOSURE 4=MAP), or Int(-1) if not a heap pointer
    PROTO_RESOLVE   = 97, // rd = Func(fn_id) for (method_id, typeof(ra)); Prop layout (slot=method id); trap on miss
    VEC_NEW         = 98, // rd = new empty KIND_VEC (ra/rb unused); allocates header + backing
    VEC_PUSH        = 99, // push value ra onto vector rd (rd stays the vector); may grow/allocate
    VEC_POP         = 100, // rd = last element of vector ra, removed; ra empty -> Nil; no alloc
    VEC_NEW_CAP     = 101, // rd = new empty KIND_VEC with backing capacity = ra (count 0); allocates
    PANIC           = 102, // abort with a located fault carrying the KIND_STRING message in ra
                           // (rd/rb unused); [[noreturn]] -- diverges via raise_located
    BYTES_NEW_CAP   = 103, // rd = new empty KIND_BYTES with backing capacity = ra bytes (count 0); allocates
    BYTES_FROM_STR  = 104, // rd = new KIND_BYTES holding a copy of the KIND_STRING bytes in ra (toBytes); allocates
    BYTES_TO_STR    = 105, // rd = new KIND_STRING decoding the live bytes of the KIND_BYTES in ra (fromBytes); allocates
    I2D             = 106, // rd = Double(asSigned48(ra)) -- widen a 48-bit Int to a double (rb unused; no alloc)
    MAP_GET_OR_TRAP = 107, // rd = map[key]; ra=map, rb=key reg; TRAPS (located fault) on a missing key --
                           // the total-index sibling of MAP_GET (mirrors ARRAY_GET's OOB trap). No alloc
    D2I             = 108, // rd = saturating Int(Double in ra) -- Java: NaN->0, +-inf/overflow->MAX/MIN_48,
                           // else trunc toward zero (rb unused; no alloc, no trap). The mirror of I2D
    MAP_ITER_NEXT   = 109, // rd = Int(next live pair-index >= cursor) in the KIND_MAP in ra, scanning the
                           // backing and skipping empty/tombstone slots; Int(-1) when none. rb = cursor.
                           // The no-copy live map-iteration primitive (no alloc, no safepoint)
    MAP_KEY_AT      = 110, // rd = the key at backing pair-index rb of the KIND_MAP in ra (no alloc). Paired
                           // with MAP_ITER_NEXT; traps (located) on an out-of-range index (internal-only)
    MAP_VAL_AT      = 111, // rd = the value at backing pair-index rb of the KIND_MAP in ra (no alloc). The
                           // value sibling of MAP_KEY_AT
    RESOLVE_CALL    = 112, // FUSED PROTO_RESOLVE + CALL_INDIRECT: resolve trait method #slot for the receiver
                           // = outgoing arg0 (window[frame_size]), then call it. ResolveCall layout:
                           // rd = nargs (count), slot(12) = global method id. One dispatch instead of two;
                           // traps on a resolve miss (like PROTO_RESOLVE). No alloc
    RESOLVE_TCO_CALL = 113,// FUSED PROTO_RESOLVE + TCO_CALL_INDIRECT: the TAIL sibling of RESOLVE_CALL. Resolve
                           // trait method #slot for the receiver, then REUSE the current frame in place (no
                           // push/slide). Same ResolveCall layout (rd = nargs, slot(12) = method id). One
                           // asymmetry from RESOLVE_CALL: a tail call's args live in the REUSED frame [0,nargs),
                           // so the receiver = arg0 is at window[0] (NOT window[frame_size]). Traps on a miss.
                           // No alloc; resolved trait method is a plain Func => current_closure = Undefined
    DROUND          = 114, // rd = round(Double in ra) by MODE in the flags field: 0=floor 1=ceil 2=trunc
                           // 3=round(ties away from zero) 4=roundHalfToEven(ties to even). Double->Double
                           // (rb unused; no alloc, no trap; -0.0 normalized by the Value ctor). Backs the
                           // floor/ceil/trunc/round/roundHalfToEven builtins; sibling of I2D/D2I
    BYTES_APPEND    = 115, // append the WHOLE live source in ra onto the KIND_BYTES buffer in rd (rd stays the
                           // buffer, returned; rb unused). Source is a KIND_STRING (its full byte payload) OR a
                           // KIND_BYTES (its live prefix [0,count)) -- one grow + memcpy, replacing a per-byte
                           // push loop. Receiver checks + GC-safe re-fetch. Allocating -> GC safepoint
    BYTES_APPEND_RANGE = 116, // append the sub-range src.bytes[lo,hi) onto the KIND_BYTES buffer in rd (q4:
                           // rd=dst, ra=src KIND_BYTES, rb=lo, rc=hi; returns dst). Traps (located) unless
                           // 0<=lo<=hi<=count(src). The range sibling of BYTES_APPEND (slice/_appendRange).
                           // Allocating -> GC safepoint
    LOAD_CONST_ARRAY = 117, // rd = const_array_pool[cnst] -- a pre-built immutable const Array literal (c2,
                           // unsigned 16-bit index). The pool is materialized ONCE at execute() setup (one
                           // KIND_ARRAY per const, filled with the scalar Int/Double/Bool element immediates)
                           // and GC-ROOTED like string_pool, so LOAD_CONST_ARRAY is a pure rooted-pool read --
                           // no allocation, no safepoint. Backs a `const NAME: Array[T] = [lit, ...]` reference;
                           // the shared instance is sound because the checker keeps a const array non-escaping
                           // (read-only). The aggregate analogue of LOAD_STR (a heap ptr can't ride the
                           // pointer-free const_pool). The STATIC front end emits it; the dynamic one never did
    EQ_DEEP         = 118, // rd = Bool(ra STRUCTURALLY-equals rb) -- the deep/value equality behind the surface
                           // `==` on a composite (struct / tuple / payload-or-nullary enum / Vec / Array / Map /
                           // Bytes), the structural sibling of TO_STRING. Immediates + String defer to the
                           // general EQ semantics (Value::operator== + string CONTENT, so numeric promotion,
                           // -0.0==0.0, NaN!=NaN all hold); two heap composites compare by SHAPE: same struct
                           // type-id + field-slots pairwise, same length + elements pairwise (Array/Vec), byte
                           // compare (Bytes), same size + every key -> deeply-equal value ORDER-INDEPENDENT
                           // (Map); a KIND_CLOSURE operand traps (the checker forbids `==` on function-carrying
                           // types, so it is unreachable from well-typed source). Uses an explicit worklist (NOT
                           // native C++ recursion), so a deep-but-finite value -- a long cons list, a deep tree --
                           // compares correctly and never overflows the native stack; a CYCLIC value (buildable
                           // only via a `mut` field) is ANSWERED co-inductively -- equal when both unfold alike --
                           // and every compare terminates. Read-only:
                           // NO allocation, no GC safepoint, but CAN trap (closure). `!=` = EQ_DEEP then
                           // NOT_BOOL. The STATIC front end emits it for a composite `==`/`!=`; a leaf `==` keeps
                           // the cheaper EQ/NE fast path. See "Equality" in docs/Compiler.md.
    MOV_TAKE        = 119, // rd = ra, then ra = Undefined -- a MOVE that leaves the source EMPTY (r6; rb
                           // unused). Not an optimization: it is one of the three mechanisms that keep the
                           // frame_size GC-roots contract true. A callee's return value comes back at
                           // window[caller frame_size], which is ABOVE the caller's frame -- so once the
                           // caller has copied it out, nothing scans that slot any more and nothing forwards
                           // the pointer left in it. It goes stale across the next collection, and a LATER
                           // frame at the same window base with a bigger frame_size covers that slot and scans
                           // it as a root -- reading a bogus header, copying garbage into to-space, and
                           // desynchronizing the Cheney scan. Taking the value instead of copying it costs
                           // nothing (it replaces the MOV the codegen already emits after every call, so no
                           // extra dispatch) and leaves the slot a non-pointer. The other two mechanisms are
                           // RET (clears the dying frame's [1, cfs)) and the tail-call ops (clear the range a
                           // SHRINKING reused frame abandons). NO allocation, no GC safepoint, never traps
};

constexpr uint8_t op(OpCode c) { return static_cast<uint8_t>(c); }

// Bytecode versioning (see vmcore/BytecodeIO.h). BYTECODE_ISA_VERSION is the GLOBAL
// bytecode version -- bump it whenever opcode SEMANTICS change (a new/changed opcode)
// so a serialized image records the ISA it was built against and an incompatible reader
// rejects it. CONTAINER_FORMAT_VERSION is the on-disk .skbc FILE structure, versioned
// independently of the ISA.
constexpr uint16_t BYTECODE_ISA_VERSION     = 6;   // v6: CALL_NATIVE re-encoded q4 -> r6; v5 was MOV_TAKE (119); v4 EQ_DEEP (118)
constexpr uint16_t CONTAINER_FORMAT_VERSION = 3;   // v3: dropped the ATOM chunk (atoms removed from Skarn); v2 added CARR

// =============================================================================
// Layout enum
// CallNative is an r6-encoded decoding whose `flags` field is nargs (a count),
// not a register.
// =============================================================================
enum class Layout : uint8_t {
    R6,
    R8,
    C2,
    J,
    Call,
    Q4,         // rd(5), ra(5), rb(5), rc(5) -- all four are registers
    CallNative, // r6-encoded: rd(6)=result, ra(6)=id register, rb(6)=first_arg, flags(6)=nargs (count!)
    CallIndirect, // r6-encoded: ra(6)=callee register, rb(6)=nargs (count!), rd unused
    B,
    B1,
    Wide,
    Prop,       // rd + ra registers + 12-bit slot index in rb(6)|flags(6) (GET_PROP/SET_PROP)
    ResolveCall, // Prop-encoded: rd(6) = nargs (count!), 12-bit slot = method id, ra unused. No reg operands
                 // (the receiver is the outgoing arg0 at window[frame_size]). RESOLVE_CALL
};

// =============================================================================
// OpEntry + OpList
// =============================================================================
struct OpEntry {
    uint8_t     id;
    const char* name;
    Layout      layout;
};

constexpr OpEntry OpList[] = {
    { op(OpCode::NOP), "NOP",                 Layout::J          },
    { op(OpCode::HALT), "HALT",               Layout::J          },
    { op(OpCode::CALL), "CALL",               Layout::Call       },
    { op(OpCode::RET), "RET",                 Layout::J          },
    { op(OpCode::TCO_CALL), "TCO_CALL",       Layout::Call       },
    { op(OpCode::J), "J",                     Layout::J          },
    { op(OpCode::LOAD_CONST), "LOAD_CONST",   Layout::C2         },
    { op(OpCode::LOAD_CONST_WIDE), "LOAD_CONST_WIDE", Layout::Wide },
    { op(OpCode::MOV), "MOV",                 Layout::R6         },
    { op(OpCode::BEQ_INT), "BEQ_INT",         Layout::B          },
    { op(OpCode::BNE_INT), "BNE_INT",         Layout::B          },
    { op(OpCode::BLT_INT), "BLT_INT",         Layout::B          },
    { op(OpCode::BGE_INT), "BGE_INT",         Layout::B          },
    { op(OpCode::ADD_INT), "ADD_INT",         Layout::R6         },
    { op(OpCode::SUB_INT), "SUB_INT",         Layout::R6         },
    { op(OpCode::MUL_INT), "MUL_INT",         Layout::R6         },
    { op(OpCode::INCR), "INCR",               Layout::C2         },
    { op(OpCode::DECR), "DECR",               Layout::C2         },
    { op(OpCode::SET_EQ), "SET_EQ",           Layout::R6         },
    { op(OpCode::SET_LT), "SET_LT",           Layout::R6         },
    { op(OpCode::SET_LE), "SET_LE",           Layout::R6         },
    { op(OpCode::AND_BOOL), "AND_BOOL",       Layout::R6         },
    { op(OpCode::OR_BOOL), "OR_BOOL",         Layout::R6         },
    { op(OpCode::NOT_BOOL), "NOT_BOOL",       Layout::R6         },
    { op(OpCode::BT), "BT",                   Layout::B1         },
    { op(OpCode::BF), "BF",                   Layout::B1         },
    { op(OpCode::CMOV), "CMOV",               Layout::Q4         },
    { op(OpCode::CALL_NATIVE), "CALL_NATIVE",     Layout::CallNative },
    { op(OpCode::LOAD_GLOBAL), "LOAD_GLOBAL",     Layout::C2         },
    { op(OpCode::STORE_GLOBAL), "STORE_GLOBAL",   Layout::C2         },
    { op(OpCode::ALLOC), "ALLOC",             Layout::C2         },
    { op(OpCode::DIV_INT), "DIV_INT",         Layout::R6         },
    { op(OpCode::MOD_INT), "MOD_INT",         Layout::R6         },
    { op(OpCode::NEG_INT), "NEG_INT",         Layout::R6         },
    { op(OpCode::AND_INT), "AND_INT",         Layout::R6         },
    { op(OpCode::OR_INT), "OR_INT",           Layout::R6         },
    { op(OpCode::XOR_INT), "XOR_INT",         Layout::R6         },
    { op(OpCode::SHL_INT), "SHL_INT",         Layout::R6         },
    { op(OpCode::SHR_INT), "SHR_INT",         Layout::R6         },
    { op(OpCode::USHR_INT), "USHR_INT",       Layout::R6         },
    { op(OpCode::LOAD_CONST_POOL), "LOAD_CONST_POOL", Layout::C2 },
    { op(OpCode::ADD), "ADD",                 Layout::R6         },
    { op(OpCode::SUB), "SUB",                 Layout::R6         },
    { op(OpCode::MUL), "MUL",                 Layout::R6         },
    { op(OpCode::DIV), "DIV",                 Layout::R6         },
    { op(OpCode::NEG), "NEG",                 Layout::R6         },
    { op(OpCode::IS_INT), "IS_INT",           Layout::R6         },
    { op(OpCode::IS_DOUBLE), "IS_DOUBLE",     Layout::R6         },
    { op(OpCode::IS_BOOL), "IS_BOOL",         Layout::R6         },
    { op(OpCode::IS_NIL), "IS_NIL",           Layout::R6         },
    { op(OpCode::IS_UNDEF), "IS_UNDEF",       Layout::R6         },
    { op(OpCode::MOD), "MOD",                 Layout::R6         },
    { op(OpCode::SET_EQ_NUM), "SET_EQ_NUM",   Layout::R6         },
    { op(OpCode::SET_NE_NUM), "SET_NE_NUM",   Layout::R6         },
    { op(OpCode::SET_LT_NUM), "SET_LT_NUM",   Layout::R6         },
    { op(OpCode::SET_LE_NUM), "SET_LE_NUM",   Layout::R6         },
    { op(OpCode::SET_GT_NUM), "SET_GT_NUM",   Layout::R6         },
    { op(OpCode::SET_GE_NUM), "SET_GE_NUM",   Layout::R6         },
    { op(OpCode::BEQ_NUM), "BEQ_NUM",         Layout::B          },
    { op(OpCode::BNE_NUM), "BNE_NUM",         Layout::B          },
    { op(OpCode::BLT_NUM), "BLT_NUM",         Layout::B          },
    { op(OpCode::BLE_NUM), "BLE_NUM",         Layout::B          },
    { op(OpCode::BGT_NUM), "BGT_NUM",         Layout::B          },
    { op(OpCode::BGE_NUM), "BGE_NUM",         Layout::B          },
    { op(OpCode::NEW_STRUCT), "NEW_STRUCT",   Layout::C2         },
    { op(OpCode::GET_PROP), "GET_PROP",       Layout::Prop       },
    { op(OpCode::SET_PROP), "SET_PROP",       Layout::Prop       },
    { op(OpCode::IS_OBJECT), "IS_OBJECT",     Layout::R6         },
    { op(OpCode::TO_BOOL), "TO_BOOL",         Layout::R6         },
    { op(OpCode::LNOT), "LNOT",               Layout::R6         },
    { op(OpCode::GET_TYPE_ID), "GET_TYPE_ID", Layout::R6         },
    { op(OpCode::EQ), "EQ",                   Layout::R6         },
    { op(OpCode::NE), "NE",                   Layout::R6         },
    { op(OpCode::LOAD_STR), "LOAD_STR",       Layout::C2         },
    { op(OpCode::LT), "LT",                   Layout::R6         },
    { op(OpCode::LE), "LE",                   Layout::R6         },
    { op(OpCode::TO_STRING), "TO_STRING",     Layout::R6         },
    { op(OpCode::LOAD_FN), "LOAD_FN",         Layout::C2         },
    { op(OpCode::CALL_INDIRECT), "CALL_INDIRECT",  Layout::CallIndirect },
    { op(OpCode::MAKE_CLOSURE), "MAKE_CLOSURE",    Layout::Prop         },
    { op(OpCode::LOAD_CAPTURE), "LOAD_CAPTURE",    Layout::C2           },
    { op(OpCode::TCO_CALL_INDIRECT), "TCO_CALL_INDIRECT", Layout::CallIndirect },
    { op(OpCode::ARRAY_GET), "ARRAY_GET",     Layout::R6         },
    { op(OpCode::ARRAY_SET), "ARRAY_SET",     Layout::R6         },
    { op(OpCode::LEN), "LEN",                 Layout::R6         },
    { op(OpCode::ANEW), "ANEW",               Layout::R6         },
    { op(OpCode::AFILL), "AFILL",             Layout::R6         },
    { op(OpCode::PRINT), "PRINT",             Layout::R6         },
    { op(OpCode::PRINTLN), "PRINTLN",         Layout::R6         },
    { op(OpCode::MAP_NEW), "MAP_NEW",         Layout::R6         },
    { op(OpCode::MAP_GET), "MAP_GET",         Layout::R6         },
    { op(OpCode::MAP_SET), "MAP_SET",         Layout::R6         },
    { op(OpCode::MAP_HAS), "MAP_HAS",         Layout::R6         },
    { op(OpCode::MAP_DELETE), "MAP_DELETE",   Layout::R6         },
    { op(OpCode::MAP_KEYS), "MAP_KEYS",       Layout::R6         },
    { op(OpCode::MAP_VALUES), "MAP_VALUES",   Layout::R6         },
    { op(OpCode::GET_KIND), "GET_KIND",       Layout::R6         },
    { op(OpCode::PROTO_RESOLVE), "PROTO_RESOLVE", Layout::Prop   },
    { op(OpCode::VEC_NEW), "VEC_NEW",         Layout::R6         },
    { op(OpCode::VEC_PUSH), "VEC_PUSH",       Layout::R6         },
    { op(OpCode::VEC_POP), "VEC_POP",         Layout::R6         },
    { op(OpCode::VEC_NEW_CAP), "VEC_NEW_CAP", Layout::R6         },
    { op(OpCode::PANIC), "PANIC",             Layout::R6         },
    { op(OpCode::BYTES_NEW_CAP),  "BYTES_NEW_CAP",  Layout::R6 },
    { op(OpCode::BYTES_FROM_STR), "BYTES_FROM_STR", Layout::R6 },
    { op(OpCode::BYTES_TO_STR),   "BYTES_TO_STR",   Layout::R6 },
    { op(OpCode::I2D),            "I2D",            Layout::R6 },
    { op(OpCode::MAP_GET_OR_TRAP), "MAP_GET_OR_TRAP", Layout::R6 },
    { op(OpCode::D2I),            "D2I",            Layout::R6 },
    { op(OpCode::MAP_ITER_NEXT),  "MAP_ITER_NEXT",  Layout::R6 },
    { op(OpCode::MAP_KEY_AT),     "MAP_KEY_AT",     Layout::R6 },
    { op(OpCode::MAP_VAL_AT),     "MAP_VAL_AT",     Layout::R6 },
    { op(OpCode::RESOLVE_CALL),   "RESOLVE_CALL",   Layout::ResolveCall },
    { op(OpCode::RESOLVE_TCO_CALL), "RESOLVE_TCO_CALL", Layout::ResolveCall },
    { op(OpCode::DROUND),         "DROUND",         Layout::R6 },
    { op(OpCode::BYTES_APPEND),   "BYTES_APPEND",   Layout::R6 },
    { op(OpCode::BYTES_APPEND_RANGE), "BYTES_APPEND_RANGE", Layout::Q4 },
    { op(OpCode::LOAD_CONST_ARRAY), "LOAD_CONST_ARRAY", Layout::C2 },
    { op(OpCode::EQ_DEEP), "EQ_DEEP",     Layout::R6         },
    { op(OpCode::MOV_TAKE), "MOV_TAKE",   Layout::R6         },
};

constexpr Layout layout_of(uint8_t opcode_byte) {
    for (const auto& e : OpList)
        if (e.id == opcode_byte) return e.layout;
    return Layout::R6;
}


