#pragma once

// =============================================================================
// Loader.h -- multi-module loading for the Skarn front end (Slice 1b).
//
// A Skarn program is one `.skn` file per module (Python/Deno model). The loader
// starts from an entry (root) source, follows its `import`/`use` declarations to the
// modules they depend on, loads + parses each transitively, builds the import DAG,
// detects cycles, and returns every reachable module in TOPOLOGICAL order (a module
// appears after all the modules it imports; the entry module is last).
//
// The loader is deliberately FILESYSTEM-FREE -- it keeps `static_compiler` pure, the
// way `svc::compile` takes source text, not a path. A driver (static_vmrun, Slice 6)
// supplies a std::filesystem-backed `ModuleResolver` that maps `net::http` to
// `net/http.skn` under the entry directory; the tests supply an in-memory map. So
// the DAG / cycle / topological logic is unit-testable without touching disk.
//
// Scope note: this slice only LOADS + orders the modules. Cross-module NAME resolution
// (per-module scopes, mangling, `use`/glob shadowing, the orphan rule) is Slice 2, and
// feeding the module set through the driver is Slice 6. Namespace `svc`.
// =============================================================================

#include "Ast.h"

#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace svc {

// Maps a module path (segments, e.g. {"net","http"}) to its source text, or
// std::nullopt if no such module exists. The one seam through which the loader
// touches the outside world.
using ModuleResolver =
    std::function<std::optional<std::string>(const std::vector<std::string>&)>;

// One loaded + parsed module.
struct LoadedModule {
    std::vector<std::string> path;    // canonical segments; EMPTY = the entry/root module
    std::string              name;    // "a::b" (joined) for diagnostics; "" for the root
    Program                  program; // the parsed AST of this module
};

// Every transitively-reachable module in TOPOLOGICAL order (dependencies before
// dependents; the entry/root module last).
struct ModuleSet { std::vector<LoadedModule> modules; };

// Thrown on a load-time error: a module path that does not resolve, an import cycle, or a
// lex/parse error INSIDE a module (the loader wraps those so the offending module + position
// travel with the error, letting a driver render the caret against that module's source).
class LoadError : public std::runtime_error {
public:
    explicit LoadError(const std::string& msg) : std::runtime_error(msg) {}
    // For a lex/parse error inside a module: `mod` = the module path key ("" = the entry),
    // `line`/`col` = the 1-based position in that module's source. `line == 0` means no position.
    LoadError(const std::string& msg, std::string mod, uint32_t line, uint32_t col)
        : std::runtime_error(msg), module(std::move(mod)), line(line), col(col) {}
    std::string module;
    uint32_t line = 0, col = 0;
};

// Parse `entry_source` (the already-read root program) and, following the `import`/`use`
// declarations of it and every module it transitively reaches via `resolve`, load + parse
// them all. Returns the modules in topological order (entry last). Throws LoadError on a
// missing module or an import cycle; propagates LexError/ParseError from a malformed module.
ModuleSet load_modules(const char* entry_source, const ModuleResolver& resolve);

// One recovered syntax error of the tool-mode loader: the module key ("" = the entry), the
// 1-based position in that module's source, and "lex error: ..." / "parse error: ...".
struct SyntaxError {
    std::string module;
    uint32_t    line = 0, col = 0;
    std::string message;
};

struct ToolLoad {
    ModuleSet                modules;
    std::vector<SyntaxError> syntax_errors;
};

// Editor tooling: load_modules with Lexer::tokenize_tolerant + Parser::parse_program_tolerant, so a
// module with syntax errors still yields its recovered tree (and its imports are still followed);
// the errors are collected instead of thrown. A missing module or an import cycle still throws
// LoadError. On valid sources the modules equal load_modules'.
ToolLoad load_modules_for_tools(const char* entry_source, const ModuleResolver& resolve);

// Join module-path segments with "::" (a canonical key / display name). "" for an empty path.
std::string join_module_path(const std::vector<std::string>& path);

} // namespace svc
