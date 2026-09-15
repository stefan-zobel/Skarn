#pragma once

#include <cstdint>
#include <cstring>
#include <vector>
#include <string>
#include <unordered_map>
#include <stdexcept>
#include <format>
#include <algorithm>
#include <utility>
#include <optional>
#include "Instruction.h"
#include "Opcodes.h"
#include "Value.h"
#include "StructType.h"
#include "FunctionTable.h"

// =============================================================================
// Assembler -- two-pass label-aware bytecode builder.
// =============================================================================
class Assembler {
public:

    Assembler& func(const std::string& func_name, uint8_t frame_size) {
        frame_sizes_[func_name] = frame_size;
        return *this;
    }

    // Declare the top-level ("main") region's frame size -- the value the caller
    // passes to execute() as top_frame_size. This is the top-level counterpart of
    // func(name, frame_size): it lets validate_frames() check the [0, first-function)
    // region against the same GC-roots contract as every named function (closing the
    // window_size contract's consequence (c)). Opt-in: bytecode that never calls this
    // leaves the top-level region unchecked (backward compatible with hand-assembled
    // programs that pass top_frame_size straight to execute()).
    Assembler& set_top_frame_size(uint8_t frame_size) {
        top_frame_size_ = frame_size;
        return *this;
    }

    // -----------------------------------------------------------------------
    // First-class functions (the function table)
    //
    // declare_fn registers a function that may be referenced as a VALUE (LOAD_FN)
    // or called INDIRECTLY (CALL_INDIRECT) or captured (MAKE_CLOSURE). It assigns a
    // stable 16-bit function id (first-seen order), records the metadata an indirect
    // call needs (frame_size / arity / ncaptures / self_slot), and ALSO registers
    // the frame size for the direct-CALL patch path -- so a declared function need
    // not be func()'d separately. The finished table (with each entry's code_offset
    // resolved from its label) is built during assemble(); read it via
    // function_table() and pass its address to execute() as fn_table.
    //
    // A function that is only ever the target of a static CALL does not need an id
    // and can keep using func(); declare_fn is for functions that become values.
    // -----------------------------------------------------------------------
    uint16_t declare_fn(const std::string& name, uint8_t frame_size, uint8_t arity,
                        uint8_t ncaptures = 0, uint8_t self_slot = FnInfo::NO_SELF,
                        const std::string& module = "") {
        if (fn_ids_.contains(name))
            throw std::runtime_error(std::format("Assembler: function '{}' already declared", name));
        if (fn_decls_.size() >= 65536)
            throw std::runtime_error("Assembler: function table exceeds 65536 entries");
        const uint16_t id = static_cast<uint16_t>(fn_decls_.size());
        fn_ids_[name] = id;
        fn_decls_.push_back(FnDecl{ name, frame_size, arity, ncaptures, self_slot, module });
        frame_sizes_[name] = frame_size;   // keep the direct-CALL patch path working too
        return id;
    }

    // Function id of a previously declared function -- for LOAD_FN / MAKE_CLOSURE.
    uint16_t func_id(const std::string& name) const {
        auto it = fn_ids_.find(name);
        if (it == fn_ids_.end())
            throw std::runtime_error(std::format(
                "Assembler: '{}' is not a declared function. Call declare_fn(\"{}\", ...) "
                "before referencing it as a value.", name, name));
        return it->second;
    }

    // The assembled function table (index == function id). Only valid AFTER
    // assemble() has run (that is where each entry's code_offset is resolved from
    // the function's label). Pass its address to execute() as fn_table.
    [[nodiscard]] const std::vector<FnInfo>& function_table() const { return function_table_; }

    // LOAD_FN rd, fn_id -- rd = a first-class function value (a Func immediate).
    // The id must be one returned by declare_fn()/func_id().
    Assembler& LOAD_FN(uint8_t rd, uint16_t fn_id) {
        emit(Instruction::C2(op(OpCode::LOAD_FN), rd, static_cast<int16_t>(fn_id)));
        return *this;
    }

    // CALL_INDIRECT rcallee, nargs -- call the function VALUE held in register
    // rcallee (a Func immediate, or a KIND_CLOSURE in Milestone A). The callee's
    // frame size and entry point are recovered from the function table at run time;
    // arguments must already sit in [caller_frame_size, caller_frame_size + nargs)
    // and the return value comes back at r[caller_frame_size] -- exactly the window
    // overlap a static CALL uses. nargs backs the Debug-only arity check.
    Assembler& CALL_INDIRECT(uint8_t rcallee, uint8_t nargs) {
        emit(Instruction::R6(op(OpCode::CALL_INDIRECT), 0, rcallee, nargs));
        return *this;
    }

    // TCO_CALL_INDIRECT rcallee, nargs -- tail-call the function VALUE in register
    // rcallee, reusing the current frame in place (no return-stack push, no slide).
    // Like a self-tail-call's TCO_CALL but for a runtime callee: arguments are written
    // into the reused frame at [0, nargs), so the caller must place them there and keep
    // rcallee ABOVE that window (a high temp). The callee frame size / entry come from
    // the function table at run time; nargs backs the Debug-only arity check.
    Assembler& TCO_CALL_INDIRECT(uint8_t rcallee, uint8_t nargs) {
        emit(Instruction::R6(op(OpCode::TCO_CALL_INDIRECT), 0, rcallee, nargs));
        return *this;
    }

    // MAKE_CLOSURE rd, fn_id, capture_base -- rd = a new KIND_CLOSURE for fn_id that
    // captures the function's ncaptures values from the CONTIGUOUS register window
    // [capture_base, capture_base + ncaptures). ncaptures / self_slot are taken from
    // the function table (declare_fn), so the emitter needs only the base register.
    // fn_id rides the Prop layout's 12-bit slot (capacity 4096; use LOAD_FN for the
    // capture-free case). The capture window is validated by validate_frames.
    Assembler& MAKE_CLOSURE(uint8_t rd, uint16_t fn_id, uint8_t capture_base) {
        emit(Instruction::Prop(op(OpCode::MAKE_CLOSURE), rd, capture_base, fn_id));
        return *this;
    }

    // LOAD_CAPTURE rd, index -- rd = the current closure's by-value capture at slot
    // `index`. Valid only inside a closure body (where CALL_INDIRECT has installed the
    // closure as current_closure); the index must be < the function's ncaptures.
    Assembler& LOAD_CAPTURE(uint8_t rd, uint16_t index) {
        emit(Instruction::C2(op(OpCode::LOAD_CAPTURE), rd, static_cast<int16_t>(index)));
        return *this;
    }

    // Smart constant load: 1 or 2 instructions depending on value range
    Assembler& load_const(uint8_t rd, int64_t value) {
        if (value >= -32'768 && value <= 32'767) {
            // 16-bit: 1 instruction
            emit(Instruction::C2(op(OpCode::LOAD_CONST), rd, static_cast<int16_t>(value)));
        } else if (value >= INT32_MIN && value <= INT32_MAX) {
            // 32-bit: 2 instructions
            const int16_t  high = static_cast<int16_t>(value >> 16);
            const uint16_t low  = static_cast<uint16_t>(value & 0xFFFF);
            emit(Instruction::C2  (op(OpCode::LOAD_CONST),      rd, high));
            emit(Instruction::Wide(op(OpCode::LOAD_CONST_WIDE), rd, low));
        } else {
            // 48-bit: 3 instructions (covers full Int range)
            const int16_t  h2 = static_cast<int16_t> ((value >> 32) & 0xFFFF);
            const uint16_t h1 = static_cast<uint16_t>((value >> 16) & 0xFFFF);
            const uint16_t h0 = static_cast<uint16_t>( value        & 0xFFFF);
            emit(Instruction::C2  (op(OpCode::LOAD_CONST),      rd, h2));
            emit(Instruction::Wide(op(OpCode::LOAD_CONST_WIDE), rd, h1));
            emit(Instruction::Wide(op(OpCode::LOAD_CONST_WIDE), rd, h0));
        }
        return *this;
    }

    // -----------------------------------------------------------------------
    // Constant pool
    //
    // Values that a LOAD_CONST(_WIDE) chain cannot build correctly (doubles,
    // tagged pointers, arbitrary 64-bit bit patterns) go into a side pool built at
    // assemble time. LOAD_CONST_POOL then copies a finished Value into a register.
    // The pool must be handed to execute() alongside the code (see constant_pool()).
    // -----------------------------------------------------------------------

    // Interns a Value in the pool (bit-exact dedup) and returns its index.
    uint16_t add_constant(Value v) {
        for (size_t i = 0; i < constants_.size(); ++i) {
            if (std::memcmp(&constants_[i], &v, sizeof(Value)) == 0)
                return static_cast<uint16_t>(i);
        }
        if (constants_.size() >= 65536)
            throw std::runtime_error("Assembler: constant pool exceeds 65536 entries");
        constants_.push_back(v);
        return static_cast<uint16_t>(constants_.size() - 1);
    }

    // Interns a double literal (canonicalized via Value::fromDouble) and returns
    // its pool index.
    uint16_t add_double(double d) { return add_constant(Value::fromDouble(d)); }

    // Emits LOAD_CONST_POOL rd, <index-of(v)>.
    Assembler& load_constant(uint8_t rd, Value v) {
        const uint16_t index = add_constant(v);
        emit(Instruction::C2(op(OpCode::LOAD_CONST_POOL), rd,
                             static_cast<int16_t>(index)));
        return *this;
    }

    // Emits LOAD_CONST_POOL rd, <index-of(double d)>.
    Assembler& load_double(uint8_t rd, double d) {
        return load_constant(rd, Value::fromDouble(d));
    }

    // The assembled constant pool. Pass its address to execute() as const_pool.
    // Valid for the lifetime of this Assembler.
    [[nodiscard]] const std::vector<Value>& constant_pool() const { return constants_; }

    // -----------------------------------------------------------------------
    // Atoms
    //
    // An atom `:name` is a full-bit-pattern Value (TAG_SPECIAL | Atom sub-tag | id),
    // so it needs no dedicated load opcode -- it rides the constant pool via
    // LOAD_CONST_POOL. Names are interned to stable ids (content-dedup), so two `:ok`
    // in one program collapse to one id and compare equal by bits (EQ / operator==).
    // -----------------------------------------------------------------------

    // Interns an atom name and returns its stable id (assigned in first-seen order).
    uint32_t atom_id(const std::string& name) {
        auto it = atom_ids_.find(name);
        if (it != atom_ids_.end()) return it->second;
        const uint32_t id = static_cast<uint32_t>(atom_names_.size());
        atom_ids_.emplace(name, id);
        atom_names_.push_back(name);
        return id;
    }

    // Emits LOAD_CONST_POOL rd, <index-of atom :name>.
    Assembler& load_atom(uint8_t rd, const std::string& name) {
        return load_constant(rd, Value::fromAtom(atom_id(name)));
    }

    // id -> name table (index == atom id), for a future runtime atom registry / printer.
    [[nodiscard]] const std::vector<std::string>& atom_names() const { return atom_names_; }

    // -----------------------------------------------------------------------
    // String literals
    //
    // A string is a heap KIND_STRING object (a TAG_PTR), so -- unlike an atom --
    // it CANNOT ride the pointer-free constant pool. The assembler instead collects
    // the literal TEXT (content-dedup); execute() interns each text into the heap
    // before running and exposes the results as a GC-rooted string pool, which
    // LOAD_STR indexes. Pass string_literals() to execute() as its string_literals.
    // -----------------------------------------------------------------------

    // Interns a string-literal's text (content-dedup) and returns its 16-bit index.
    uint16_t string_literal_id(const std::string& text) {
        auto it = string_ids_.find(text);
        if (it != string_ids_.end()) return it->second;
        if (string_texts_.size() >= 65536)
            throw std::runtime_error("Assembler: string literal pool exceeds 65536 entries");
        const uint16_t id = static_cast<uint16_t>(string_texts_.size());
        string_ids_.emplace(text, id);
        string_texts_.push_back(text);
        return id;
    }

    // Emits LOAD_STR rd, <index-of text>.
    Assembler& load_str(uint8_t rd, const std::string& text) {
        const uint16_t index = string_literal_id(text);
        emit(Instruction::C2(op(OpCode::LOAD_STR), rd, static_cast<int16_t>(index)));
        return *this;
    }

    // The literal texts (index == LOAD_STR operand). Pass to execute() as string_literals.
    [[nodiscard]] const std::vector<std::string>& string_literals() const { return string_texts_; }

    // -----------------------------------------------------------------------
    // Const-array literals (LOAD_CONST_ARRAY). A `const NAME: Array[T] = [...]`
    // is a heap KIND_ARRAY, so -- like a string literal -- it can't ride the
    // pointer-free constant pool; execute() builds each into a GC-rooted array
    // pool once, which LOAD_CONST_ARRAY indexes. Elements are scalar immediates
    // (Int/Double/Bool). No dedup (by design): one entry per declared const.
    // Pass const_arrays() to execute() as its const_arrays.
    // -----------------------------------------------------------------------

    // Appends a const array (its scalar element Values) and returns its 16-bit index.
    uint16_t add_const_array(std::vector<Value> elems) {
        if (const_arrays_.size() >= 65536)
            throw std::runtime_error("Assembler: const-array pool exceeds 65536 entries");
        const uint16_t id = static_cast<uint16_t>(const_arrays_.size());
        const_arrays_.push_back(std::move(elems));
        return id;
    }

    // Emits LOAD_CONST_ARRAY rd, <index>.
    Assembler& load_const_array(uint8_t rd, uint16_t index) {
        emit(Instruction::C2(op(OpCode::LOAD_CONST_ARRAY), rd, static_cast<int16_t>(index)));
        return *this;
    }

    // The const-array pool (index == LOAD_CONST_ARRAY operand). Pass to execute() as const_arrays.
    [[nodiscard]] const std::vector<std::vector<Value>>& const_arrays() const { return const_arrays_; }

    // (The legacy baked-pointer emitter `call_native` + `load_native_ptr` were retired:
    //  every CALL_NATIVE now goes through the registry `call_native_id`, so
    //  the runtime is single-mode and no runtime address is ever baked into bytecode.)

    // Registry-based native call -- the COMPILER path. Bakes only a small STABLE id
    // (a NativeId), not a runtime address, so the bytecode is process-independent /
    // serializable and can be produced without knowing any function address. The
    // runtime resolves the id via VM::native_table (present iff execute() was given a
    // native_table). See "Native functions" in docs/VirtualMachine.md and NativeRegistry.h.
    //   id_reg = register to hold the id (temporary); rd = result register;
    //   first_arg_reg = first argument register; nargs = 0..63 (6-bit flags field).
    // Emits: LOAD_CONST id_reg, native_id ; CALL_NATIVE rd, id_reg, first_arg_reg, nargs
    //
    // The field ranges are enforced HERE, for every caller, and throw rather than
    // truncate: this encoding was q4 (5-bit) earlier, where an ordinary
    // program with >= 32 live registers silently truncated `id_reg` and called a
    // DIFFERENT native (Release only -- Debug caught it in the Q4 builder's assert).
    // The r6 form now matches the codegen's own 63-register frame ceiling, so a
    // throw here is unreachable from a well-formed frame; it stays as the guard that
    // makes "unreachable" true instead of merely intended.
    Assembler& call_native_id(uint8_t rd, uint8_t id_reg, uint8_t first_arg_reg,
                              uint8_t nargs, uint16_t native_id) {
        if (rd >= 64 || id_reg >= 64 || first_arg_reg >= 64)
            throw std::runtime_error(std::format(
                "Assembler: CALL_NATIVE register out of range (rd=r{}, id=r{}, first_arg=r{}) "
                "-- the r6 fields address 64 registers", rd, id_reg, first_arg_reg));
        if (nargs >= 64)
            throw std::runtime_error(std::format(
                "Assembler: CALL_NATIVE nargs {} exceeds the 6-bit field (max 63)", nargs));
        C2(OpCode::LOAD_CONST, id_reg, static_cast<int16_t>(native_id));
        emit(Instruction::CallNative(op(OpCode::CALL_NATIVE), rd, id_reg, first_arg_reg, nargs));
        return *this;
    }

    // LOAD_GLOBAL convenience wrapper:
    //   rd           = destination register
    //   global_index = compile-time assigned global slot index (0..255)
    //
    // Encoding:
    //   c2-layout: opcode(8) | rd(8) | index(16)
    //   Only the low 8 bits of index are currently used by the opcode.
    Assembler& LOAD_GLOBAL(uint8_t rd, uint8_t global_index) {
        emit(Instruction::C2(op(OpCode::LOAD_GLOBAL), rd, static_cast<int16_t>(global_index)));
        return *this;
    }

    // STORE_GLOBAL convenience wrapper:
    //   rs           = source register
    //   global_index = compile-time assigned global slot index (0..255)
    //
    // Encoding:
    //   c2-layout: opcode(8) | reg(8) | index(16)
    //   For STORE_GLOBAL the register is carried in the c2.rd field.
    Assembler& STORE_GLOBAL(uint8_t global_index, uint8_t rs) {
        emit(Instruction::C2(op(OpCode::STORE_GLOBAL), rs, static_cast<int16_t>(global_index)));
        return *this;
    }

    // Raw instruction builders
    Assembler& R6(OpCode opc, uint8_t rd, uint8_t ra, uint8_t rb, uint8_t flags = 0) {
        emit(Instruction::R6(op(opc), rd, ra, rb, flags));
        return *this;
    }

    Assembler& C2(OpCode opc, uint8_t rd, int16_t cnst = 0) {
        emit(Instruction::C2(op(opc), rd, cnst));
        return *this;
    }

    Assembler& Q4(OpCode opc, uint8_t rd, uint8_t ra, uint8_t rb, uint8_t rc, uint8_t extra = 0) {
        emit(Instruction::Q4(op(opc), rd, ra, rb, rc, extra));
        return *this;
    }

    Assembler& J(OpCode opc, int32_t offset = 0) {
        emit(Instruction::J(op(opc), offset));
        return *this;
    }

    Assembler& J(OpCode opc, const std::string& lbl) {
        fixups_.push_back({ current_index(), lbl, FixupKind::JLayout });
        emit(Instruction::J(op(opc), 0));
        return *this;
    }

    Assembler& B(OpCode opc, uint8_t ra, uint8_t rb, int16_t offset = 0) {
        emit(Instruction::B(op(opc), ra, rb, offset));
        return *this;
    }

    Assembler& B(OpCode opc, uint8_t ra, uint8_t rb, const std::string& lbl) {
        fixups_.push_back({ current_index(), lbl, FixupKind::BLayout });
        emit(Instruction::B(op(opc), ra, rb, 0));
        return *this;
    }

    Assembler& B1(OpCode opc, uint8_t ra, int16_t offset = 0) {
        emit(Instruction::B1(op(opc), ra, offset));
        return *this;
    }

    Assembler& B1(OpCode opc, uint8_t ra, const std::string& lbl) {
        fixups_.push_back({ current_index(), lbl, FixupKind::B1Layout });
        emit(Instruction::B1(op(opc), ra, 0));
        return *this;
    }

    Assembler& CALL(const std::string& target_label) {
        fixups_.push_back({ current_index(), target_label, FixupKind::CallLayout });
        emit(Instruction::CALL(op(OpCode::CALL), 0, 0));
        return *this;
    }

    Assembler& TCO_CALL(const std::string& target_label) {
        fixups_.push_back({ current_index(), target_label, FixupKind::CallLayout });
        emit(Instruction::CALL(op(OpCode::TCO_CALL), 0, 0));
        return *this;
    }

    Assembler& label(const std::string& name) {
        if (labels_.contains(name))
            throw std::runtime_error(std::format("Assembler: duplicate label '{}'", name));
        labels_[name] = current_index();
        return *this;
    }

    // ALLOC convenience wrapper:
    //   rd     = destination register
    //   nslots = number of Value slots to allocate (0..32767; 0 = empty array)
    Assembler& ALLOC(uint8_t rd, uint16_t nslots) {
        if (nslots > 32767)
            throw std::runtime_error("Assembler: ALLOC nslots must be in 0..32767");
        emit(Instruction::C2(op(OpCode::ALLOC), rd, static_cast<int16_t>(nslots)));
        return *this;
    }

    // -----------------------------------------------------------------------
    // Struct types (fixed-shape objects, KIND_OBJECT)
    //
    // define_struct registers a type: the field-name list gives the arity (the
    // VM's single source of truth, read by NEW_STRUCT) and a name->slot-index map
    // for readable tests -- the VM itself only ever sees the resolved slot index.
    // Pass struct_types() to execute() as its struct_types argument.
    // -----------------------------------------------------------------------
    uint16_t define_struct(const std::string& name, std::vector<std::string> fields,
                           DumpStyle style = DumpStyle::Struct) {
        if (struct_ids_.contains(name))
            throw std::runtime_error(std::format("Assembler: duplicate struct type '{}'", name));
        if (struct_types_.size() >= 65536)
            throw std::runtime_error("Assembler: struct type registry exceeds 65536 entries");
        const uint16_t id = static_cast<uint16_t>(struct_types_.size());
        // Record arity + the printable shape (name + field names) in one place; the
        // names are what TO_STRING's default field dump reads via ctx->vm->struct_types.
        // `style` lets the compiler tag its $TupleN / $List fiction types so TO_STRING
        // renders them in surface syntax (host-side only -- NEW_STRUCT/GC ignore it).
        // The DISPLAY name is the last "::"-segment (a module-qualified type prints UNQUALIFIED --
        // e.g. std::core::Some -> "Some"), while the registry KEY stays the full mangled `name`.
        std::string display = name;
        if (auto pos = name.rfind("::"); pos != std::string::npos) display = name.substr(pos + 2);
        struct_types_.push_back(StructType{
            static_cast<uint32_t>(fields.size()), std::move(display), fields, style });
        struct_ids_[name]         = id;
        struct_field_names_[name] = std::move(fields);
        return id;
    }

    // Slot index of a named field of a previously-defined struct type.
    uint16_t field(const std::string& struct_name, const std::string& field_name) const {
        auto it = struct_field_names_.find(struct_name);
        if (it == struct_field_names_.end())
            throw std::runtime_error(std::format("Assembler: unknown struct type '{}'", struct_name));
        const auto& names = it->second;
        for (size_t i = 0; i < names.size(); ++i)
            if (names[i] == field_name)
                return static_cast<uint16_t>(i);
        throw std::runtime_error(std::format(
            "Assembler: struct '{}' has no field '{}'", struct_name, field_name));
    }

    // Type id of a previously-defined struct type.
    uint16_t struct_id(const std::string& name) const {
        auto it = struct_ids_.find(name);
        if (it == struct_ids_.end())
            throw std::runtime_error(std::format("Assembler: unknown struct type '{}'", name));
        return it->second;
    }

    // The assembled struct-type registry. Pass its address to execute() as
    // struct_types. Valid for the lifetime of this Assembler.
    [[nodiscard]] const std::vector<StructType>& struct_types() const { return struct_types_; }

    // NEW_STRUCT rd, #type_id -- rd = new struct (arity from the registry).
    Assembler& NEW_STRUCT(uint8_t rd, uint16_t type_id) {
        emit(Instruction::C2(op(OpCode::NEW_STRUCT), rd, static_cast<int16_t>(type_id)));
        return *this;
    }

    // GET_PROP rd, robj, #slot -- rd = robj.field[slot].
    Assembler& GET_PROP(uint8_t rd, uint8_t robj, uint16_t slot) {
        emit(Instruction::Prop(op(OpCode::GET_PROP), rd, robj, slot));
        return *this;
    }

    // SET_PROP robj, #slot, rval -- robj.field[slot] = rval.
    Assembler& SET_PROP(uint8_t robj, uint16_t slot, uint8_t rval) {
        emit(Instruction::Prop(op(OpCode::SET_PROP), rval, robj, slot));
        return *this;
    }

    // IS_OBJECT rd, ra -- rd = Bool(ra is a heap struct object).
    Assembler& IS_OBJECT(uint8_t rd, uint8_t ra) {
        emit(Instruction::R6(op(OpCode::IS_OBJECT), rd, ra, 0));
        return *this;
    }

    // GET_TYPE_ID rd, ra -- rd = Int(struct type id of ra), or Int(-1) if not a struct.
    Assembler& GET_TYPE_ID(uint8_t rd, uint8_t ra) {
        emit(Instruction::R6(op(OpCode::GET_TYPE_ID), rd, ra, 0));
        return *this;
    }

    // GET_KIND rd, ra -- rd = Int(heap kind of ra: 0=ARRAY 1=STRING 2=OBJECT
    // 3=CLOSURE 4=MAP), or Int(-1) if ra is not a heap pointer. Reads the GcObject
    // header kind byte; the primitive the compiler's `for … in …` lowering uses to
    // tell an array from a map at run time.
    Assembler& GET_KIND(uint8_t rd, uint8_t ra) {
        emit(Instruction::R6(op(OpCode::GET_KIND), rd, ra, 0));
        return *this;
    }

    // I2D rd, ra -- rd = Double(the 48-bit Int in ra). The Int->Double widening the
    // static front end emits at an N1 coercion boundary (an Int value flowing into a
    // Double context). rb unused; no allocation.
    Assembler& I2D(uint8_t rd, uint8_t ra) {
        emit(Instruction::R6(op(OpCode::I2D), rd, ra, 0));
        return *this;
    }

    // D2I rd, ra -- rd = the saturating Int conversion of the Double in ra (Java:
    // NaN->0, +-inf/overflow->MAX/MIN_48, else trunc toward zero). The `toInt`
    // builtin; mirror of I2D. rb unused; no allocation, no trap.
    Assembler& D2I(uint8_t rd, uint8_t ra) {
        emit(Instruction::R6(op(OpCode::D2I), rd, ra, 0));
        return *this;
    }

    // DROUND rd, ra, mode -- Double rounding; mode (0..4) rides the flags field:
    // 0=floor 1=ceil 2=trunc 3=round(ties away) 4=roundHalfToEven(ties to even).
    Assembler& DROUND(uint8_t rd, uint8_t ra, uint8_t mode) {
        emit(Instruction::R6(op(OpCode::DROUND), rd, ra, 0, mode));
        return *this;
    }

    // PROTO_RESOLVE rd, rrecv, method_id -- rd = the Func value implementing the
    // trait method `method_id` for the runtime type of the receiver in rrecv; traps
    // if that type has no implementation. Prop layout: the global method id rides the
    // 12-bit slot (capacity 4096). Follow with CALL_INDIRECT/TCO_CALL_INDIRECT on rd.
    Assembler& PROTO_RESOLVE(uint8_t rd, uint8_t rrecv, uint16_t method_id) {
        emit(Instruction::Prop(op(OpCode::PROTO_RESOLVE), rd, rrecv, method_id));
        return *this;
    }

    // RESOLVE_CALL nargs, method_id -- fused PROTO_RESOLVE + CALL_INDIRECT. The receiver is
    // the OUTGOING arg 0 (window[frame_size]), so no receiver register is encoded: resolve
    // `method_id` for its runtime type, then call the resolved fn with `nargs` args. ResolveCall
    // layout (Prop-encoded): rd = nargs (a count, <= the r6 field's 63), 12-bit slot = method id,
    // ra unused. The caller must have placed all args in the outgoing window [frame_size, +nargs),
    // arg 0 = the receiver, before this op (exactly as for PROTO_RESOLVE + CALL_INDIRECT).
    Assembler& RESOLVE_CALL(uint8_t nargs, uint16_t method_id) {
        emit(Instruction::Prop(op(OpCode::RESOLVE_CALL), nargs, 0, method_id));
        return *this;
    }

    // MOV_TAKE rd, ra -- rd = ra, then ra = Undefined. The return-value fetch after a call:
    // taking (rather than copying) leaves no heap pointer in a slot nothing will forward
    // again. See MOV_TAKE in the opcode table for why that matters.
    Assembler& MOV_TAKE(uint8_t rd, uint8_t ra) {
        emit(Instruction::R6(op(OpCode::MOV_TAKE), rd, ra, 0));
        return *this;
    }

    // RESOLVE_TCO_CALL nargs, method_id -- fused PROTO_RESOLVE + TCO_CALL_INDIRECT (the TAIL sibling
    // of RESOLVE_CALL). Resolve `method_id` for the receiver, then reuse the current frame in place.
    // Same ResolveCall layout, but a tail call places its args in the REUSED frame [0, nargs), so the
    // receiver = arg 0 is at window[0] (NOT the outgoing window[frame_size]). The caller must have
    // placed all args in [0, nargs), arg 0 = the receiver, before this op.
    Assembler& RESOLVE_TCO_CALL(uint8_t nargs, uint16_t method_id) {
        emit(Instruction::Prop(op(OpCode::RESOLVE_TCO_CALL), nargs, 0, method_id));
        return *this;
    }

    // ARRAY_GET rd, rarr, ridx -- rd = rarr[ridx] (bounds-checked; traps on OOB).
    Assembler& ARRAY_GET(uint8_t rd, uint8_t rarr, uint8_t ridx) {
        emit(Instruction::R6(op(OpCode::ARRAY_GET), rd, rarr, ridx));
        return *this;
    }

    // ARRAY_SET rarr, ridx, rval -- rarr[ridx] = rval (bounds-checked; traps on OOB).
    // rd carries the VALUE (as SET_PROP), ra the array, rb the index.
    Assembler& ARRAY_SET(uint8_t rarr, uint8_t ridx, uint8_t rval) {
        emit(Instruction::R6(op(OpCode::ARRAY_SET), rval, rarr, ridx));
        return *this;
    }

    // LEN rd, rarr -- rd = Int(length of the array in rarr).
    Assembler& LEN(uint8_t rd, uint8_t rarr) {
        emit(Instruction::R6(op(OpCode::LEN), rd, rarr, 0));
        return *this;
    }

    // ANEW rd, rcount -- rd = new KIND_ARRAY of rcount slots (count from a register).
    Assembler& ANEW(uint8_t rd, uint8_t rcount) {
        emit(Instruction::R6(op(OpCode::ANEW), rd, rcount, 0));
        return *this;
    }

    // AFILL rarr, rval -- fill every slot of the array in rarr with rval.
    Assembler& AFILL(uint8_t rarr, uint8_t rval) {
        emit(Instruction::R6(op(OpCode::AFILL), rarr, rval, 0));
        return *this;
    }

    // PRINT rd, rstr -- write the KIND_STRING in rstr to vm->out; rd = Nil. rd may
    // alias rstr (the string is read before rd is written). PRINTLN appends '\n'.
    Assembler& PRINT(uint8_t rd, uint8_t rstr) {
        emit(Instruction::R6(op(OpCode::PRINT), rd, rstr, 0));
        return *this;
    }
    Assembler& PRINTLN(uint8_t rd, uint8_t rstr) {
        emit(Instruction::R6(op(OpCode::PRINTLN), rd, rstr, 0));
        return *this;
    }

    // PANIC rstr -- abort with a located fault carrying the KIND_STRING in rstr (rd/rb
    // unused). [[noreturn]] at run time -- it never falls through.
    Assembler& PANIC(uint8_t rstr) {
        emit(Instruction::R6(op(OpCode::PANIC), 0, rstr, 0));
        return *this;
    }

    // MAP_NEW rd -- rd = new empty KIND_MAP (allocates header + backing).
    Assembler& MAP_NEW(uint8_t rd) {
        emit(Instruction::R6(op(OpCode::MAP_NEW), rd, 0, 0));
        return *this;
    }

    // MAP_GET rd, rmap, rkey -- rd = rmap[rkey] or Nil on miss (no allocation).
    Assembler& MAP_GET(uint8_t rd, uint8_t rmap, uint8_t rkey) {
        emit(Instruction::R6(op(OpCode::MAP_GET), rd, rmap, rkey));
        return *this;
    }

    // MAP_GET_OR_TRAP rd, rmap, rkey -- rd = rmap[rkey]; TRAPS (located fault) on a
    // missing key, the total-index sibling of MAP_GET (mirrors ARRAY_GET). No allocation.
    Assembler& MAP_GET_OR_TRAP(uint8_t rd, uint8_t rmap, uint8_t rkey) {
        emit(Instruction::R6(op(OpCode::MAP_GET_OR_TRAP), rd, rmap, rkey));
        return *this;
    }

    // MAP_ITER_NEXT rd, rmap, rcursor -- rd = next live pair-index >= rcursor in rmap
    // (skipping empty/tombstone slots), or -1 once exhausted. No-copy live iteration; no alloc.
    Assembler& MAP_ITER_NEXT(uint8_t rd, uint8_t rmap, uint8_t rcursor) {
        emit(Instruction::R6(op(OpCode::MAP_ITER_NEXT), rd, rmap, rcursor));
        return *this;
    }

    // MAP_KEY_AT rd, rmap, ridx -- rd = the key at backing pair-index ridx of rmap. No allocation.
    Assembler& MAP_KEY_AT(uint8_t rd, uint8_t rmap, uint8_t ridx) {
        emit(Instruction::R6(op(OpCode::MAP_KEY_AT), rd, rmap, ridx));
        return *this;
    }

    // MAP_VAL_AT rd, rmap, ridx -- rd = the value at backing pair-index ridx of rmap. No allocation.
    Assembler& MAP_VAL_AT(uint8_t rd, uint8_t rmap, uint8_t ridx) {
        emit(Instruction::R6(op(OpCode::MAP_VAL_AT), rd, rmap, ridx));
        return *this;
    }

    // MAP_SET rmap, rkey, rval -- rmap[rkey] = rval (may grow/rehash). rval is
    // carried in the rd field (as ARRAY_SET/SET_PROP); ra = map, rb = key.
    Assembler& MAP_SET(uint8_t rmap, uint8_t rkey, uint8_t rval) {
        emit(Instruction::R6(op(OpCode::MAP_SET), rval, rmap, rkey));
        return *this;
    }

    // MAP_HAS rd, rmap, rkey -- rd = Bool(rkey present in rmap) (no allocation).
    Assembler& MAP_HAS(uint8_t rd, uint8_t rmap, uint8_t rkey) {
        emit(Instruction::R6(op(OpCode::MAP_HAS), rd, rmap, rkey));
        return *this;
    }

    // MAP_DELETE rd, rmap, rkey -- rd = Bool(rkey was present, now removed) (no allocation).
    Assembler& MAP_DELETE(uint8_t rd, uint8_t rmap, uint8_t rkey) {
        emit(Instruction::R6(op(OpCode::MAP_DELETE), rd, rmap, rkey));
        return *this;
    }

    // MAP_KEYS rd, rmap -- rd = new KIND_ARRAY of rmap's live keys (allocates).
    Assembler& MAP_KEYS(uint8_t rd, uint8_t rmap) {
        emit(Instruction::R6(op(OpCode::MAP_KEYS), rd, rmap, 0));
        return *this;
    }

    // MAP_VALUES rd, rmap -- rd = new KIND_ARRAY of rmap's live values (allocates).
    Assembler& MAP_VALUES(uint8_t rd, uint8_t rmap) {
        emit(Instruction::R6(op(OpCode::MAP_VALUES), rd, rmap, 0));
        return *this;
    }

    // VEC_NEW rd -- rd = new empty KIND_VEC (allocates header + backing).
    Assembler& VEC_NEW(uint8_t rd) {
        emit(Instruction::R6(op(OpCode::VEC_NEW), rd, 0, 0));
        return *this;
    }

    // VEC_PUSH rvec, rval -- push rval onto rvec (rvec stays the vector; may grow).
    // rvec is carried in the rd field, the value in ra.
    Assembler& VEC_PUSH(uint8_t rvec, uint8_t rval) {
        emit(Instruction::R6(op(OpCode::VEC_PUSH), rvec, rval, 0));
        return *this;
    }

    // VEC_POP rd, rvec -- rd = last element of rvec, removed; Nil if empty (no alloc).
    Assembler& VEC_POP(uint8_t rd, uint8_t rvec) {
        emit(Instruction::R6(op(OpCode::VEC_POP), rd, rvec, 0));
        return *this;
    }

    // VEC_NEW_CAP rd, rcap -- rd = new empty KIND_VEC with backing capacity = rcap
    // (count 0; allocates header + backing). The capacity primitive behind vec(n)
    // and toVec(x). (It also backed a `#v[..]` literal, which the static front end
    // removed -- `[...]` is the sole sequence literal, lifted by toVec.)
    Assembler& VEC_NEW_CAP(uint8_t rd, uint8_t rcap) {
        emit(Instruction::R6(op(OpCode::VEC_NEW_CAP), rd, rcap, 0));
        return *this;
    }

    // BYTES_NEW_CAP rd, rcap -- rd = new empty KIND_BYTES with backing capacity =
    // rcap bytes (count 0; allocates header + KIND_STRING backing). The primitive
    // behind bytes() / bytes(n).
    Assembler& BYTES_NEW_CAP(uint8_t rd, uint8_t rcap) {
        emit(Instruction::R6(op(OpCode::BYTES_NEW_CAP), rd, rcap, 0));
        return *this;
    }

    // BYTES_FROM_STR rd, rstr -- rd = new KIND_BYTES holding a copy of the string
    // bytes in rstr (the toBytes(str) builtin; allocates).
    Assembler& BYTES_FROM_STR(uint8_t rd, uint8_t rstr) {
        emit(Instruction::R6(op(OpCode::BYTES_FROM_STR), rd, rstr, 0));
        return *this;
    }

    // BYTES_TO_STR rd, rbuf -- rd = new KIND_STRING decoding the live bytes of the
    // KIND_BYTES in rbuf (the fromBytes(b) builtin; allocates).
    Assembler& BYTES_TO_STR(uint8_t rd, uint8_t rbuf) {
        emit(Instruction::R6(op(OpCode::BYTES_TO_STR), rd, rbuf, 0));
        return *this;
    }

    // BYTES_APPEND rdst, rsrc -- append the whole source (rsrc = KIND_STRING or KIND_BYTES)
    // onto the KIND_BYTES buffer in rdst (rdst stays the buffer; may grow). One memcpy.
    Assembler& BYTES_APPEND(uint8_t rdst, uint8_t rsrc) {
        emit(Instruction::R6(op(OpCode::BYTES_APPEND), rdst, rsrc, 0));
        return *this;
    }

    // BYTES_APPEND_RANGE rdst, rsrc, rlo, rhi -- append src.bytes[lo, hi) (rsrc = KIND_BYTES)
    // onto rdst (q4: 4 registers, all < 32). Traps unless 0 <= lo <= hi <= len(src).
    Assembler& BYTES_APPEND_RANGE(uint8_t rdst, uint8_t rsrc, uint8_t rlo, uint8_t rhi) {
        emit(Instruction::Q4(op(OpCode::BYTES_APPEND_RANGE), rdst, rsrc, rlo, rhi, 0));
        return *this;
    }

    [[nodiscard]] std::vector<uint32_t> assemble() {
        // Route any direct CALL whose offset would overflow the 16-bit `call` field through a central
        // trampoline island. A no-op unless the program is large enough to overflow (so ordinary programs
        // are byte-identical). See relax_far_calls().
        relax_far_calls();

        for (const auto& fix : fixups_) {
            auto it = labels_.find(fix.label);
            if (it == labels_.end())
                throw std::runtime_error(
                    std::format("Assembler: undefined label '{}'", fix.label));

            const int32_t target_idx = static_cast<int32_t>(it->second);
            const int32_t base_idx   = static_cast<int32_t>(fix.site) + 1;
            const int32_t offset     = target_idx - base_idx;
            patch(fix.site, offset, fix.kind, fix.label);
        }

        // Frame-size contract check runs AFTER fixups: it needs each CALL's
        // patched window_size (the callee frame size).
        validate_frames();

        // Build the function table: resolve each declared function's code_offset
        // from its label (an instruction index). Labels are populated during
        // emission, so they are all known here.
        function_table_.clear();
        function_table_.reserve(fn_decls_.size());
        for (const FnDecl& d : fn_decls_) {
            auto it = labels_.find(d.name);
            if (it == labels_.end())
                throw std::runtime_error(std::format(
                    "Assembler: declared function '{}' has no label -- emit its entry "
                    "with as.label(\"{}\").", d.name, d.name));
            function_table_.push_back(FnInfo{
                static_cast<uint32_t>(it->second),
                d.frame_size, d.arity, d.ncaptures, d.self_slot });
        }
        // Function names parallel to function_table_ (index == function id), for the
        // fault-time stacktrace's "in <fn>" resolution. Purely diagnostic, not a GC root.
        function_names_.clear();
        function_names_.reserve(fn_decls_.size());
        for (const FnDecl& d : fn_decls_) function_names_.push_back(d.name);

        function_modules_.clear();
        function_modules_.reserve(fn_decls_.size());
        for (const FnDecl& d : fn_decls_) function_modules_.push_back(d.module);

        std::vector<uint32_t> result;
        result.reserve(code_.size());
        for (const auto& i : code_) result.push_back(i.raw);
        return result;
    }

    size_t instruction_count() const { return code_.size(); }

    // Source-position tracking for VM fault reporting. Stamp the (line, col) onto every
    // instruction emitted after this call (statement/expression granularity is enough).
    Assembler& set_line(uint32_t line) { current_line_ = line; return *this; }
    Assembler& set_pos(uint32_t line, uint32_t col) { current_line_ = line; current_col_ = col; return *this; }

    // Per-instruction source line / column tables (index == instruction index), 1:1
    // with the bytecode from assemble(). Pass their addresses to execute() as
    // line_table / column_table. Unset instructions carry 0. Valid for this Assembler.
    [[nodiscard]] const std::vector<uint32_t>& line_table()   const { return line_of_instr_; }
    [[nodiscard]] const std::vector<uint32_t>& column_table() const { return col_of_instr_; }

    // Function names (index == function id), built by assemble(). Pass to execute()
    // as function_names for the fault-time stacktrace. Empty before assemble().
    [[nodiscard]] const std::vector<std::string>& function_names() const { return function_names_; }

    // Owning module prefix per function id (parallel to function_names). Pass to execute() as
    // function_modules so a fault caret renders against the RIGHT module source. Empty before assemble().
    [[nodiscard]] const std::vector<std::string>& function_modules() const { return function_modules_; }

    // NOTE: there is deliberately no top_level_frame_size() helper. Since the
    // window_size contract was decoupled (op_call slides by the CALLER's frame
    // size), the top-level frame size is the boundary between the top-level's own
    // locals and its outgoing-argument window -- and that boundary cannot be
    // recovered from "highest register touched" (which would include the argument
    // slots and yield a slide that overshoots the callee's r0). The top level, like
    // every function, must therefore state its frame size explicitly: pass it as
    // execute()'s top_frame_size.

private:
    enum class FixupKind { JLayout, BLayout, B1Layout, CallLayout };

    struct Fixup {
        size_t      site;
        std::string label;
        FixupKind   kind;
    };

    // A declared first-class function, pending code_offset resolution at assemble().
    struct FnDecl {
        std::string name;
        uint8_t     frame_size;
        uint8_t     arity;
        uint8_t     ncaptures;
        uint8_t     self_slot;
        std::string module;   // owning module's mangle prefix ("" = root/entry); for the fault caret
    };

    std::vector<Instruction>                 code_;
    std::unordered_map<std::string, size_t>  labels_;
    std::unordered_map<std::string, uint8_t> frame_sizes_;
    // Declared top-level frame size (set_top_frame_size). Unset -> the top-level region
    // is not validated (opt-in; see validate_frames / set_top_frame_size).
    std::optional<uint8_t>                   top_frame_size_;
    std::vector<Fixup>                       fixups_;
    // First-class function declarations (index == function id) and the name->id map,
    // plus the finished table built in assemble() (code_offset resolved from labels).
    std::vector<FnDecl>                       fn_decls_;
    std::unordered_map<std::string, uint16_t> fn_ids_;
    std::vector<FnInfo>                        function_table_;
    std::vector<Value>                       constants_;
    std::vector<std::vector<Value>>          const_arrays_;
    std::vector<StructType>                  struct_types_;
    std::unordered_map<std::string, uint16_t>                 struct_ids_;
    std::unordered_map<std::string, std::vector<std::string>> struct_field_names_;
    // Atom interning: name -> stable id (dedup), and the reverse (id == vector index)
    // for a future id->name registry once printing exists. Atoms load as full-bit-pattern
    // Values through the constant pool -- no dedicated atom opcode is needed.
    std::unordered_map<std::string, uint32_t> atom_ids_;
    std::vector<std::string>                  atom_names_;
    // String-literal interning: text -> index (content-dedup), and the reverse
    // (index == vector position). Strings are heap TAG_PTRs, so they cannot ride
    // the constant pool -- execute() interns these texts into a GC-rooted pool.
    std::unordered_map<std::string, uint16_t> string_ids_;
    std::vector<std::string>                  string_texts_;
    // Per-instruction source line (index == instruction index), for the VM's
    // fault-time PC->line lookup. `current_line_` is the line stamped onto every
    // subsequently emitted instruction; set it via set_line() before emitting a
    // node. Unset == 0 (no known line). Built alongside code_ in emit(); patch
    // fixups edit instructions in place, so the index stays 1:1 with the bytecode.
    std::vector<uint32_t>                     line_of_instr_;
    uint32_t                                  current_line_ = 0;
    // Parallel per-instruction column (for the fault-time caret). Same lifetime rule
    // as line_of_instr_; 0 = unknown.
    std::vector<uint32_t>                     col_of_instr_;
    uint32_t                                  current_col_ = 0;
    // Function names (index == function id), parallel to function_table_, built in
    // assemble(). Feeds the fault-time stacktrace's "in <fn>" resolution.
    std::vector<std::string>                  function_names_;
    std::vector<std::string>                  function_modules_;   // parallel to function_names_ (fault caret)
    // Monotonic counter for synthetic trampoline labels ("$tramp$N") created by relax_far_calls().
    size_t                                    next_tramp_ = 0;
    size_t current_index() const { return code_.size(); }
    void   emit(Instruction i)   {
        code_.push_back(i);
        line_of_instr_.push_back(current_line_);
        col_of_instr_.push_back(current_col_);
    }

    // -------------------------------------------------------------------
    // max_reg_of -- highest register index an instruction reads or writes,
    // or -1 if it has no register operands. Layout-driven, so it stays correct
    // as opcodes are added (reuses layout_of()).
    //
    // CallNative is special: `flags` is an argument COUNT, and the native reads
    // arguments in place from [rb, rb + nargs), so those registers are the calling
    // frame's own locals and count toward its footprint.
    // Call/TCO_CALL carry a slide + offset, not register operands.
    // -------------------------------------------------------------------
    // Local max helper -- std::max is unusable here because <Windows.h> (pulled in
    // by the single translation unit before this header) defines a max() macro.
    static int hi(int a, int b) { return a > b ? a : b; }

    static int max_reg_of(Instruction i) {
        const uint8_t opcode_byte = static_cast<uint8_t>(i.raw & 0xFFu);
        switch (layout_of(opcode_byte)) {
            case Layout::R6:
                return hi(static_cast<int>(i.r6.rd),
                          hi(static_cast<int>(i.r6.ra), static_cast<int>(i.r6.rb)));
            case Layout::R8:
                return hi(static_cast<int>(i.r8.rd),
                          hi(static_cast<int>(i.r8.ra), static_cast<int>(i.r8.rb)));
            case Layout::C2:
                return static_cast<int>(i.c2.rd);
            case Layout::Q4:
                return hi(hi(static_cast<int>(i.q4.rd), static_cast<int>(i.q4.ra)),
                          hi(static_cast<int>(i.q4.rb), static_cast<int>(i.q4.rc)));
            case Layout::CallNative: {
                // r6-encoded: rd/ra/rb are registers, flags is nargs (a count).
                const int nargs  = static_cast<int>(i.r6.flags);
                const int arg_hi = nargs > 0
                    ? static_cast<int>(i.r6.rb) + nargs - 1
                    : static_cast<int>(i.r6.rb);
                return hi(hi(static_cast<int>(i.r6.rd), static_cast<int>(i.r6.ra)), arg_hi);
            }
            case Layout::CallIndirect:
                // Only ra is a register (the callee); rb is nargs (a count), rd unused.
                // The outgoing argument window is written by separate MOVs (counted on
                // their own) and legalized in validate_frames via nargs.
                return static_cast<int>(i.r6.ra);
            case Layout::B:
                return hi(static_cast<int>(i.b.ra), static_cast<int>(i.b.rb));
            case Layout::B1:
                return static_cast<int>(i.b1.ra);
            case Layout::Prop:
                // rd + ra are registers; rb|flags is a 12-bit slot index, not a register.
                return hi(static_cast<int>(i.r6.rd), static_cast<int>(i.r6.ra));
            case Layout::ResolveCall:
                // No register operands: rd is nargs (a count), the 12-bit slot is the method id,
                // and the receiver is the outgoing arg 0 (window[frame_size]). The argument window
                // is legalized in validate_frames via nargs (like CALL_INDIRECT), not here.
                return -1;
            case Layout::Wide:
                return static_cast<int>(i.wide.rd);
            case Layout::J:
            case Layout::Call:
            default:
                return -1;
        }
    }

    // -------------------------------------------------------------------
    // validate_frames -- best-effort enforcement of the frame_size GC-roots
    // contract for every registered function.
    //
    // A function's own locals live in [0, frame_size). Registers >= frame_size
    // are the *next window*: outgoing-argument and return-value overlap slots.
    // Since the window_size contract was decoupled, a CALL slides by the CALLER's
    // frame size S, so the callee's frame C maps to caller registers
    // [S, S + C): the widest register a normal CALL can legitimately touch is
    // S + C - 1. A TCO_CALL reuses the current frame in place (no slide) and writes
    // the callee's C arguments into [0, C), so it can touch up to C - 1. The set of
    // legal indices is therefore [0, legal_end), where legal_end starts at
    // frame_size and is widened per call site to (frame_size + callee_size) for a
    // CALL and to callee_size for a TCO_CALL. Anything at or above legal_end is a
    // genuine out-of-frame local -- flagged here. (callee_size is the patched
    // window_size of the call instruction.)
    //
    // This is a sound upper bound (no false positives on the overlap idiom), but
    // NOT a proof: a stray pointer inside the overlap window cannot be told apart
    // from a legitimate argument without dataflow / stack maps (future work).
    //
    // Region model: functions are laid out contiguously, top-level code first.
    // A function spans from its label to the next function label (or end).
    // -------------------------------------------------------------------
    void validate_frames() const {
        // Validate one contiguous region [begin, end) against its declared frame size.
        // Used for every named-function region AND -- when the codegen declared it via
        // set_top_frame_size() -- the synthetic top-level region. Identical logic for both.
        auto validate_region = [&](size_t begin, size_t end, int frame_size, const std::string& name) {
            if (frame_size <= 0)
                throw std::runtime_error(std::format(
                    "Assembler: function '{}' has frame_size 0 -- must be > 0", name));

            int legal_end = frame_size; // exclusive upper bound on legal indices
            int body_max  = -1;
            for (size_t k = begin; k < end; ++k) {
                body_max = hi(body_max, max_reg_of(code_[k]));
                const uint8_t opcode_byte  = static_cast<uint8_t>(code_[k].raw & 0xFFu);
                const int     callee_size  = static_cast<int>(code_[k].call.window_size);
                if (opcode_byte == op(OpCode::CALL)) {
                    // Normal call: overlap window opens past the caller's frame.
                    legal_end = hi(legal_end, frame_size + callee_size);
                } else if (opcode_byte == op(OpCode::TCO_CALL)) {
                    // Tail call: arguments written into the reused frame at [0, callee_size).
                    legal_end = hi(legal_end, callee_size);
                } else if (opcode_byte == op(OpCode::CALL_INDIRECT)) {
                    // Indirect call: the callee frame size is unknown at assemble time
                    // (it comes from the function table at run time). The caller writes
                    // nargs outgoing arguments into [frame_size, frame_size + nargs) AND
                    // reads the return value back at r[frame_size] (the window base = the
                    // callee's r0) even when nargs == 0. So widen by at least 1 -- a bare
                    // nargs would leave a 0-argument indirect call's result read (e.g. a
                    // `next()` on a fn-valued field) exactly one register out of bounds.
                    const int nargs = static_cast<int>(code_[k].r6.rb);
                    legal_end = hi(legal_end, frame_size + (nargs > 0 ? nargs : 1));
                } else if (opcode_byte == op(OpCode::RESOLVE_CALL)) {
                    // Fused resolve-and-call: same outgoing window as CALL_INDIRECT (args at
                    // [frame_size, frame_size + nargs), receiver = arg 0, return value read back
                    // at r[frame_size]). nargs rides the rd field (ResolveCall = Prop-encoded).
                    const int nargs = static_cast<int>(code_[k].r6.rd);
                    legal_end = hi(legal_end, frame_size + (nargs > 0 ? nargs : 1));
                } else if (opcode_byte == op(OpCode::TCO_CALL_INDIRECT)) {
                    // Indirect tail call: reuses the current frame, so the nargs arguments
                    // are written into [0, nargs) (not an outgoing overlap window). Widen
                    // by nargs alone -- mirrors the TCO_CALL case. The callee register ra
                    // is folded into body_max via max_reg_of like any other local read.
                    const int nargs = static_cast<int>(code_[k].r6.rb);
                    legal_end = hi(legal_end, nargs);
                } else if (opcode_byte == op(OpCode::RESOLVE_TCO_CALL)) {
                    // Fused resolve-and-tail-call: reuses the current frame, args (incl. the
                    // receiver = arg 0 at window[0]) in [0, nargs). Same widening as
                    // TCO_CALL_INDIRECT (by nargs), NOT the RESOLVE_CALL rule -- a tail call
                    // reads no return value. nargs rides the rd field (ResolveCall = Prop-encoded).
                    const int nargs = static_cast<int>(code_[k].r6.rd);
                    legal_end = hi(legal_end, nargs);
                } else if (opcode_byte == op(OpCode::MAKE_CLOSURE)) {
                    // Reads the capture window [ra, ra + ncaptures) as regular locals
                    // (the closure's environment). ncaptures is NOT in the instruction --
                    // recover it from the function table via the 12-bit fn id in the Prop
                    // slot, and fold the top capture register into body_max so an
                    // under-declared frame is caught like any other local read.
                    const uint16_t fn_id = static_cast<uint16_t>(code_[k].prop_slot());
                    if (fn_id < fn_decls_.size()) {
                        const int ncap = static_cast<int>(fn_decls_[fn_id].ncaptures);
                        if (ncap > 0)
                            body_max = hi(body_max, static_cast<int>(code_[k].r6.ra) + ncap - 1);
                    }
                }
            }

            if (body_max >= legal_end)
                throw std::runtime_error(std::format(
                    "Assembler: function '{}' touches register r{}, but its frame_size "
                    "is {} (highest legal register index is {}). Either its declared "
                    "frame_size is too small (heap pointers in higher registers would be "
                    "invisible to the GC) or a local escapes the call-overlap window. "
                    "See the window_size contract.",
                    name, body_max, frame_size, legal_end - 1));
        };

        std::vector<std::pair<size_t, const std::string*>> starts;
        for (const auto& kv : frame_sizes_) {
            auto it = labels_.find(kv.first);
            if (it != labels_.end())
                starts.emplace_back(it->second, &kv.first);
        }
        std::sort(starts.begin(), starts.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });

        // The synthetic top-level region [0, first function label), validated only when the
        // codegen declared its size via set_top_frame_size() (closes the window_size contract's
        // consequence (c): the top-level frame is otherwise the one unchecked GC-roots region).
        // Hand-assembled bytecode that doesn't opt in stays unchecked -- backward compatible.
        // The top-level "main" region is always emitted at index 0, so first-func > 0.
        if (top_frame_size_.has_value()) {
            const size_t first_func_begin = starts.empty() ? code_.size() : starts.front().first;
            validate_region(0, first_func_begin, static_cast<int>(*top_frame_size_), "<script>");
        }

        for (size_t s = 0; s < starts.size(); ++s) {
            const size_t       begin      = starts[s].first;
            const size_t       end        = (s + 1 < starts.size()) ? starts[s + 1].first
                                                                     : code_.size();
            const std::string& name       = *starts[s].second;
            validate_region(begin, end, frame_sizes_.at(name), name);
        }
    }

    // -------------------------------------------------------------------
    // relax_far_calls -- keep near CALLs as fast direct calls, but route any CALL whose offset would overflow
    // the 16-bit `call` field through a one-instruction trampoline `J <realTarget>` (the j layout is a 24-bit
    // offset, reaching anywhere). A `CALL <tramp>` still stamps the callee window_size and sets up the frame;
    // the trampoline then jumps to the real body -- so no scratch register and no validate_frames change are
    // needed. The trampolines cluster in one island at the function boundary nearest the code midpoint, so a
    // single island serves programs up to ~2x the 32767 span. Iterated to a fixpoint (inserting words shifts
    // offsets and can tip a near call over). A NO-OP when nothing overflows -> ordinary programs are unchanged.
    // -------------------------------------------------------------------
    static bool call_off_ok(int32_t off) { return off >= -32768 && off <= 32767; }

    void relax_far_calls() {
        for (int guard = 0; guard < 64; ++guard) {
            // 1. Collect CALL fixups whose current offset overflows int16.
            std::vector<size_t> farCalls;
            for (size_t fi = 0; fi < fixups_.size(); ++fi) {
                if (fixups_[fi].kind != FixupKind::CallLayout) continue;
                auto it = labels_.find(fixups_[fi].label);
                if (it == labels_.end()) continue;   // an undefined label surfaces later in assemble()
                const int32_t off = static_cast<int32_t>(it->second) - static_cast<int32_t>(fixups_[fi].site + 1);
                if (!call_off_ok(off)) farCalls.push_back(fi);
            }
            if (farCalls.empty()) return;

            // 2. Insert point M: the function-boundary label nearest the code midpoint (never 0, so the entry
            //    point stays at index 0; never a "$tramp$" so we don't split an existing island).
            const size_t mid = code_.size() / 2;
            size_t M = 0;   bool haveM = false;   size_t bestDist = 0;
            for (const auto& kv : frame_sizes_) {
                if (kv.first.rfind("$tramp$", 0) == 0) continue;
                auto it = labels_.find(kv.first);
                if (it == labels_.end() || it->second == 0) continue;
                const size_t pos = it->second;
                const size_t dist = pos > mid ? pos - mid : mid - pos;
                if (!haveM || dist < bestDist) { M = pos; bestDist = dist; haveM = true; }
            }
            if (!haveM)
                throw std::runtime_error("Assembler: far call but no function boundary to place a trampoline island");

            // 3. Build the island (one J placeholder per far call) and re-point each far CALL to its trampoline.
            std::vector<Instruction> island;
            std::vector<std::string> trampNames;
            std::vector<std::string> trampTargets;
            for (size_t fi : farCalls) {
                const std::string tname  = "$tramp$" + std::to_string(next_tramp_++);
                const std::string target = fixups_[fi].label;                 // the real callee (capture first)
                auto fsit = frame_sizes_.find(target);
                frame_sizes_[tname] = (fsit != frame_sizes_.end()) ? fsit->second : 0;   // preserve window_size
                fixups_[fi].label = tname;                                    // CALL now targets the trampoline
                trampNames.push_back(tname);
                trampTargets.push_back(target);
                island.push_back(Instruction::J(op(OpCode::J), 0));
            }
            const size_t K = island.size();

            // 4. Splice the island into code_ (+ the parallel line/col tables) at M; shift labels & fixup sites.
            code_.insert(code_.begin() + M, island.begin(), island.end());
            line_of_instr_.insert(line_of_instr_.begin() + M, K, 0u);
            col_of_instr_.insert(col_of_instr_.begin() + M, K, 0u);
            for (auto& kv : labels_) if (kv.second >= M) kv.second += K;
            for (auto& fx : fixups_) if (fx.site >= M) fx.site += K;

            // 5. Place each trampoline label + add its J fixup to the real target.
            for (size_t t = 0; t < trampNames.size(); ++t) {
                labels_[trampNames[t]] = M + t;
                fixups_.push_back({ M + t, trampTargets[t], FixupKind::JLayout });
            }
            // loop -> re-check (a near call may have tipped over by K)
        }
        throw std::runtime_error("Assembler: far-call relaxation did not converge (program needs multi-island support)");
    }

    void patch(size_t site, int32_t offset, FixupKind kind, const std::string& lbl) {
        Instruction& i = code_[site];
        switch (kind) {
            case FixupKind::JLayout:
                if (offset < -(1 << 23) || offset > (1 << 23) - 1)
                    throw std::runtime_error(std::format(
                        "Assembler: j-layout offset {} out of 24-bit range at site {}",
                        offset, site));
                i.j.offset = offset;
                break;
            case FixupKind::BLayout:
                if (offset < -2048 || offset > 2047)
                    throw std::runtime_error(std::format(
                        "Assembler: b-layout offset {} out of 12-bit range at site {}",
                        offset, site));
                i.b.offset = offset;
                break;
            case FixupKind::B1Layout:
                if (offset < -32768 || offset > 32767)
                    throw std::runtime_error(std::format(
                        "Assembler: b1-layout offset {} out of 16-bit range at site {}",
                        offset, site));
                i.b1.offset = static_cast<int16_t>(offset);
                break;
            case FixupKind::CallLayout: {
                if (offset < -32768 || offset > 32767)
                    throw std::runtime_error(std::format(
                        "Assembler: call-layout offset {} out of 16-bit range at site {}",
                        offset, site));
                auto fs_it = frame_sizes_.find(lbl);
                if (fs_it == frame_sizes_.end())
                    throw std::runtime_error(std::format(
                        "Assembler: no frame size registered for '{}'. "
                        "Call as.func(\"{}\", N) anywhere before assemble().", lbl, lbl));
                // window_size carries the CALLEE's frame size (its GC scan count).
                // op_call slides by the CALLER's frame size, not by this field.
                i.call.offset      = static_cast<int16_t>(offset);
                i.call.window_size = fs_it->second;
                break;
            }
        }
    }

    static constexpr uint8_t op(OpCode c) { return static_cast<uint8_t>(c); }
};
