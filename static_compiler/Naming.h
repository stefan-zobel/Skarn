#pragma once

// =============================================================================
// Naming.h -- module prefixes and the name-mangling rule for Skarn (namespace svc).
//
// One file = one module; a declaration is stored under its MANGLED name
// (`module::name`), and every reference resolves back to that key. These few helpers
// are the single source of truth for that mapping.
//
// They live here, below both the syntactic AST (Ast.h) and the semantic type layer
// (Types.h), because BOTH need them and neither may depend on the other: the checker
// mangles declarations, and `describe()` has to render a mangled type name back into
// something a reader recognizes. Duplicating the prefix constants in the two layers
// is exactly the drift that let a user `fn toInt` hijack the name inside `std::math`.
// =============================================================================

#include <string>

namespace svc {

// The scoping module-prefix given to PRELUDE items. It gives the prelude its OWN import scope -- so
// the entry program's `use`/imports never leak into the prelude's internal name resolution -- while
// still mangling to BARE names (see mangle_name), so every hardcoded prelude name
// (find_struct("Some"), the ?/must-use/for-loop recognizers) keeps working and a program can reach a
// shadowed prelude name via the virtual `std::` qualifier. The leading '$' cannot occur in a real
// (lident) module path, so it can never collide with one.
constexpr const char* PRELUDE_MODULE_PREFIX = "$prelude";

// The module prefix of the ENTRY program (the script / the `main.skn` handed to the driver). It is
// an ORDINARY prefix: unlike `$prelude` it is NOT special-cased in `mangle_name`, so the entry's
// declarations mangle to `$entry::name` exactly as a real module's do.
//
// That is the point. The entry used to carry the empty prefix, which mangles to a BARE name -- into
// the same namespace as the builtins, natives and trait-method names, which belong to no module and
// are never mangled. One user `fn toInt` therefore captured the name inside the sealed standard
// library, and (a milder consequence of the same accident) every entry declaration -- including a
// non-`pub` one -- was reachable from any imported module, though nothing can `import` the entry.
// Prefixing it leaves the bare namespace holding ambient names ONLY.
//
// Like `$prelude`, the leading '$' cannot occur in a real (lident) module path, so it can never
// collide with one. `prettify_fn_name` (vmcore/Interpreter.h) strips it for a runtime trace and
// `display_name` below for a diagnostic, so it never reaches the reader.
constexpr const char* ENTRY_MODULE_PREFIX = "$entry";

// The single mangling rule (also used by Checker::mangle, which delegates here).
inline std::string mangle_name(const std::string& prefix, const std::string& name) {
    if (prefix.empty() || prefix == PRELUDE_MODULE_PREFIX) return name;
    return prefix + "::" + name;
}

// Does module `prefix` mangle its declarations to BARE names? Since the entry program gained a real
// prefix, the monolithic dev prelude behind `--prelude <path>` is the only such scope left -- and it
// is why `Checker::user_fn_in_scope` is still live: a bare declaration shares the namespace of the
// ambient builtins/natives/trait methods, so it may shadow one only from a scope that can name it.
// (The empty prefix is kept as a defensive third case: it is what an un-stamped item would carry.)
inline bool bare_scope(const std::string& prefix) {
    return prefix.empty() || prefix == PRELUDE_MODULE_PREFIX;
}

// The last "::"-segment of a (possibly module-mangled) name -- std::core::Some -> "Some". Used to
// recognize a Result/Option carrier REGARDLESS of module (a user's own `Result` or the ring's
// std::core::Result both work with `?`), and as the VM display name.
inline std::string short_name(const std::string& name) {
    auto pos = name.rfind("::");
    return pos == std::string::npos ? name : name.substr(pos + 2);
}

// The module-prefix part of a mangled name -- std::core::Some -> "std::core"; a bare name -> "".
// A variant shares its enum's prefix, so this recovers where to find the sibling Ok/Err/Some/None.
inline std::string module_prefix_of(const std::string& name) {
    auto pos = name.rfind("::");
    return pos == std::string::npos ? std::string() : name.substr(0, pos);
}

// A (possibly module-mangled) name -- or a whole DIAGNOSTIC containing several -- as it should read
// for a human. The entry program's prefix is an internal device the user never wrote, so
// `$entry::helper` reads back as `helper`; a real module keeps its path (`util::helper`), which IS
// what the reader wrote. The compile-time sibling of `prettify_fn_name` (vmcore/Interpreter.h),
// which does the same job for a runtime trace.
//
// It strips EVERY occurrence, not just a leading one, so it can be applied once to a finished
// message ("impl of '$entry::T' for '$entry::P' is missing method 'b'") instead of at each of the
// ~40 sites that interpolate a name. That is how it is used: `Checker::error`/`warn` and the
// `CodegenError` constructors are the two choke points every diagnostic passes through. Safe as a
// blanket text substitution -- '$' cannot occur in a Skarn identifier, so the sequence never
// appears in a message legitimately.
inline std::string display_name(std::string s) {
    static const std::string p = std::string(ENTRY_MODULE_PREFIX) + "::";
    for (size_t i = s.find(p); i != std::string::npos; i = s.find(p, i))
        s.erase(i, p.size());
    return s;
}

} // namespace svc
