#pragma once

// =============================================================================
// Analyze.h -- run the Skarn front end over one entry program and turn every error
// and warning it reports into editor diagnostics.
//
// One analysis = the front half of `skarnvm` in its error-tolerant form:
// svc::load_modules_for_tools with a file-system resolver (a syntax error is reported and the
// parser resumes after it, so a file being typed still has a tree), then
// svc::check_modules_for_tools against the sealed built-in std, which keeps the checked program
// even when it has errors (the queries read it) and reports the warnings alongside the errors.
// The checker's errors and warnings inside an item that lost part of itself to a syntax error
// are dropped, and so are all warnings of a file with a syntax error: they are mostly fallout of
// the missing text (a use that was dropped makes a binding look unused). Nothing is lowered or run. The
// whole program is re-checked every time; a full check including the std is a few tens of
// milliseconds, so there is no incremental state.
//
// Module resolution matches skarnvm: `import net::http` is `<entry-dir>/net/http.skn`.
// An OPEN editor buffer wins over the file on disk, so a module being edited is checked
// in its current, unsaved state.
//
// Positions: the front end reports a 1-based line and a 1-based BYTE column of the
// offending node's first character, and no end. A Diagnostic is 0-based and needs a
// range, so the range covers the identifier run starting at that column, or exactly one
// character when the column is not on an identifier. Columns are bytes; for ASCII source
// that equals the UTF-16 unit count LSP specifies.
// =============================================================================

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace svc { struct Program; }

namespace lsp {

struct Position { int line = 0; int character = 0; };            // 0-based
struct Range    { Position start; Position end; };

struct RelatedInfo {
    std::string path;       // normalized file path the note points into
    Range       range;
    std::string message;
};

enum class Severity { Error = 1, Warning = 2 };                   // LSP DiagnosticSeverity values

struct Diagnostic {
    Range                    range;
    Severity                 severity = Severity::Error;
    std::string              message;
    std::vector<RelatedInfo> related;
};

bool operator==(const Diagnostic& a, const Diagnostic& b);

// Open editor buffers: normalized path -> current text.
using Overlay = std::map<std::string, std::string>;

// A builtin or native: `module` is the opt-in module whose `use` reaches it ("" = always).
struct AmbientName {
    std::string name;
    std::string module;
    std::string signature;   // `fn name(A) -> R`; empty for a builtin typed per call (`len`)
};

struct AnalysisResult {
    std::map<std::string, std::vector<Diagnostic>> by_path;   // normalized path -> diagnostics
    std::set<std::string> modules;   // normalized paths of every module the entry imported (not itself)
    // The checked program (std items first, then every module's items), with final types.
    // Null when loading failed (a missing module, an import cycle) -- there is no tree then.
    std::shared_ptr<const svc::Program> program;
    // Normalized paths of the files with a syntax error; their recovered tree lacks the text
    // around each error, so rename refuses to work on the program.
    std::set<std::string> syntax_error_paths;
    std::map<std::string, std::string> module_of;   // normalized path -> module prefix ("$entry", "a::b")
    std::map<std::string, std::string> path_of;     // module prefix -> normalized path
    std::map<std::string, std::string> texts;       // normalized path -> the source that was checked
    std::vector<AmbientName> ambient;               // what a user may call besides the program's items
};

// Check `entry_text` as the entry program at `entry_path`. Never throws: an unexpected
// failure becomes one "internal error" diagnostic on the entry.
AnalysisResult analyze(const std::string& entry_path, const std::string& entry_text, const Overlay& overlay);

// The range of the identifier run at 1-based `line`/`col` of `text` (one character when the
// column is not on an identifier). A position of 0 means "unknown" and yields 1:1.
Range ident_range(const std::string& text, uint32_t line, uint32_t col);

// The text of 1-based line `line` without its terminator; empty when out of range.
std::string line_text(const std::string& text, uint32_t line);

// ---- paths and URIs ----------------------------------------------------------------

// Forward slashes, and on a drive-letter path the letter lower-cased ("F:\a" -> "f:/a"),
// so a URI-derived path and a resolver-built path compare equal.
std::string normalize_path(std::string p);

// "file:///f%3A/dir/a%20b.skn" -> "f:/dir/a b.skn"; "file:///home/u/x.skn" -> "/home/u/x.skn".
// Returns an empty string for anything that is not a file URI.
std::string uri_to_path(const std::string& uri);

// The inverse, in the spelling VS Code itself uses (drive colon percent-encoded).
std::string path_to_uri(const std::string& path);

} // namespace lsp
