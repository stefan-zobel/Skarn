// =============================================================================
// SelfTest.cpp -- see SelfTest.h.
//
// The analysis tests use paths under a directory that does not exist, so every module
// comes from the in-memory overlay and nothing depends on the machine's file system.
// =============================================================================

#include "SelfTest.h"

#include "Analyze.h"
#include "Complete.h"
#include "Format.h"
#include "Json.h"
#include "Query.h"
#include "Server.h"
#include "Symbols.h"
#include "Transport.h"

#include <algorithm>
#include <optional>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

namespace lsp {

namespace {

class Suite {
public:
    explicit Suite(std::ostream& out) : out_(out) {}

    void check(bool ok, const std::string& name, const std::string& detail = {}) {
        if (ok) {
            ++passed_;
            out_ << "  ok    " << name << "\n";
        } else {
            ++failed_;
            out_ << "  FAIL  " << name;
            if (!detail.empty()) out_ << "  -- " << detail;
            out_ << "\n";
        }
    }

    void section(const char* name) { out_ << name << "\n"; }

    int finish() {
        out_ << "==== skarn_lsp selftest: " << passed_ << " passed, " << failed_ << " failed ====\n";
        return failed_ == 0 ? 0 : 1;
    }

private:
    std::ostream& out_;
    int passed_ = 0;
    int failed_ = 0;
};

bool parse_fails(const std::string& text) {
    try { Json::parse(text); } catch (const JsonError&) { return true; }
    return false;
}

// ---- S1: JSON -------------------------------------------------------------------

void test_json(Suite& t) {
    t.section("json");
    const std::string doc = R"({"a":1,"b":[true,false,null],"c":{"d":"x"},"e":-2.5,"f":1e3})";
    const Json j = Json::parse(doc);
    t.check(j.dump() == R"({"a":1,"b":[true,false,null],"c":{"d":"x"},"e":-2.5,"f":1000})",
            "round trip keeps member order", j.dump());
    t.check(j.get("c").get("d").as_string() == "x", "nested member read");
    t.check(j.get("missing").get("deeper").is_null(), "absent member reads as null");
    t.check(j.get("b").size() == 3 && j.get("b").at(0).as_bool(), "array access");

    const Json esc = Json::parse(R"("q\"b\\s\/n\nt\tu\u00e9e\ud83d\ude00")");
    t.check(esc.as_string() == "q\"b\\s/n\nt\tu\xC3\xA9" "e\xF0\x9F\x98\x80", "escapes incl. surrogate pair decode to UTF-8");
    t.check(Json(std::string("a\"b\\c\n\x01")).dump() == R"("a\"b\\c\n\u0001")", "serializer escapes");
    t.check(Json(std::string("\xC3\xA9")).dump() == "\"\xC3\xA9\"", "UTF-8 passes through the serializer");
    t.check(Json(42).dump() == "42" && Json(0.5).dump() == "0.5" && Json(-7.0).dump() == "-7", "number formatting");

    Json o = Json::object();
    o.set("x", 1);
    o.set("y", 2);
    o.set("x", 3);
    t.check(o.dump() == R"({"x":3,"y":2})", "set replaces in place");

    t.check(parse_fails("{"), "rejects an unterminated object");
    t.check(parse_fails("[1,]"), "rejects a trailing comma");
    t.check(parse_fails("01"), "rejects a leading zero");
    t.check(parse_fails("\"\\ud800\""), "rejects an unpaired surrogate");
    t.check(parse_fails("{} x"), "rejects trailing characters");
    t.check(parse_fails(std::string(300, '[')), "caps the nesting depth");
    t.check(Json::parse(" [ 1 , { } ] ") == Json::parse("[1,{}]"), "whitespace is insignificant");
}

// ---- S2: framing ----------------------------------------------------------------

void test_transport(Suite& t) {
    t.section("transport");
    std::ostringstream w;
    write_message(w, R"({"a":1})");
    write_message(w, "{}");
    t.check(w.str() == "Content-Length: 7\r\n\r\n{\"a\":1}Content-Length: 2\r\n\r\n{}", "writes framed messages", w.str());

    std::istringstream r(w.str());
    const auto m1 = read_message(r);
    const auto m2 = read_message(r);
    const auto m3 = read_message(r);
    t.check(m1 && *m1 == R"({"a":1})" && m2 && *m2 == "{}", "reads two messages back to back");
    t.check(!m3, "clean end of input yields no message");

    std::istringstream extra("Content-Type: application/vscode-jsonrpc; charset=utf-8\r\ncontent-length: 2\r\n\r\n[]");
    const auto m4 = read_message(extra);
    t.check(m4 && *m4 == "[]", "ignores other headers, header name is case-insensitive");

    auto throws = [](const std::string& s) {
        std::istringstream in(s);
        try { read_message(in); } catch (const TransportError&) { return true; }
        return false;
    };
    t.check(throws("Content-Length: 10\r\n\r\nshort"), "truncated body is an error");
    t.check(throws("Content-Type: x\r\n\r\n{}"), "missing Content-Length is an error");
    t.check(throws("Content-Length: 1x\r\n\r\n{"), "malformed Content-Length is an error");
}

// ---- S3: analysis ---------------------------------------------------------------

const std::string DIR   = "/skarn-lsp-selftest/";
const std::string ENTRY = DIR + "main.skn";

std::string describe(const AnalysisResult& r) {
    std::ostringstream s;
    for (const auto& [path, diags] : r.by_path)
        for (const Diagnostic& d : diags)
            s << path << ":" << d.range.start.line << ":" << d.range.start.character << "-"
              << d.range.end.character << " [" << static_cast<int>(d.severity) << "] " << d.message << "; ";
    return s.str();
}

bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

void test_analysis(Suite& t) {
    t.section("analysis");
    const Overlay none;

    {   // clean program
        const AnalysisResult r = analyze(ENTRY, "println(1 + 2)\n", none);
        t.check(r.by_path.empty(), "clean program has no diagnostics", describe(r));
    }
    {   // unknown name: the range covers the identifier
        const AnalysisResult r = analyze(ENTRY, "let a = 1\nprintln(undefinedName + a)\n", none);
        const auto it = r.by_path.find(ENTRY);
        const bool ok = it != r.by_path.end() && it->second.size() == 1 &&
                        it->second[0].severity == Severity::Error &&
                        it->second[0].range.start.line == 1 && it->second[0].range.start.character == 8 &&
                        it->second[0].range.end.character == 21;
        t.check(ok, "type error: 0-based position, range covers the identifier", describe(r));
    }
    {   // argument mismatch: a secondary note pointing at the parameter
        const AnalysisResult r = analyze(ENTRY, "fn f(n: Int) -> Int { n }\nprintln(f(\"a\"))\n", none);
        const auto it = r.by_path.find(ENTRY);
        bool ok = false;
        if (it != r.by_path.end() && it->second.size() == 1) {
            const Diagnostic& d = it->second[0];
            ok = d.range.start.line == 1 && d.range.start.character == 10 && d.range.end.character == 11 &&
                 d.related.size() == 1 && d.related[0].path == ENTRY &&
                 d.related[0].range.start.line == 0 && contains(d.related[0].message, "parameter 'n'");
        }
        t.check(ok, "type error carries the parameter note as related information", describe(r));
    }
    {   // parse error in the entry
        const AnalysisResult r = analyze(ENTRY, "let = 5\n", none);
        const auto it = r.by_path.find(ENTRY);
        const bool ok = it != r.by_path.end() && it->second.size() == 1 &&
                        it->second[0].range.start.line == 0 && it->second[0].message.rfind("parse error: ", 0) == 0;
        t.check(ok, "parse error: positioned, module prefix stripped", describe(r));
    }
    {   // warnings are reported while errors exist, and the program is kept
        const AnalysisResult r = analyze(ENTRY,
            "fn g() -> Result[Int, String] { Ok(1) }\ng()\nlet s: String = 5\nprintln(s)\n", none);
        const auto it = r.by_path.find(ENTRY);
        bool warn = false, err = false;
        if (it != r.by_path.end())
            for (const Diagnostic& d : it->second) {
                warn = warn || (d.severity == Severity::Warning && d.range.start.line == 1);
                err  = err  || (d.severity == Severity::Error && d.range.start.line == 2);
            }
        t.check(warn && err && r.program != nullptr, "warnings and errors together, program kept", describe(r));
    }
    {   // a parse error keeps the recovered program, and marks the file
        const AnalysisResult r = analyze(ENTRY, "let = 5\n", none);
        t.check(r.program != nullptr && r.syntax_error_paths.count(ENTRY) == 1,
                "a parse error keeps the recovered program and marks its file");
    }
    {   // recovery: every syntax error is reported, the checker's fallout inside a broken item is
        // dropped, the other items keep their diagnostics and their outline
        const AnalysisResult r = analyze(ENTRY,
            "fn f() -> Int {\n    let a = 1\n    let b = foo(1 2)\n    undefined1\n}\n"
            "fn g() -> Int { undefined2 }\nfn h( -> Int { 3 }\nprintln(f() + g())\n", none);
        const auto it = r.by_path.find(ENTRY);
        std::vector<int> lines;
        if (it != r.by_path.end())
            for (const Diagnostic& d : it->second) lines.push_back(d.range.start.line);
        std::sort(lines.begin(), lines.end());
        t.check(lines == std::vector<int>{ 2, 5, 6 },
                "two syntax errors and the error of the intact item, nothing from the broken one", describe(r));
        const std::vector<Symbol> syms = document_symbols(r, ENTRY);
        t.check(syms.size() == 2 && syms[0].name == "f" && syms[1].name == "g",
                "the outline keeps the recovered items", std::to_string(syms.size()));
    }
    {   // warning tier
        const AnalysisResult r = analyze(ENTRY,
            "fn g() -> Result[Int, String] { Ok(1) }\ng()\nprintln(0)\n", none);
        const auto it = r.by_path.find(ENTRY);
        const bool ok = it != r.by_path.end() && !it->second.empty() &&
                        it->second[0].severity == Severity::Warning && it->second[0].range.start.line == 1;
        t.check(ok, "a discarded Result is a warning", describe(r));
    }
    {   // an error inside an imported module lands on that module's file
        Overlay ov;
        ov[DIR + "util.skn"] = "pub fn f() -> Int { \"x\" }\n";
        const AnalysisResult r = analyze(ENTRY, "import util\nprintln(util::f())\n", ov);
        const auto it = r.by_path.find(DIR + "util.skn");
        const bool ok = r.by_path.count(ENTRY) == 0 && it != r.by_path.end() && it->second.size() == 1 &&
                        it->second[0].range.start.line == 0 && r.modules.count(DIR + "util.skn") == 1;
        t.check(ok, "error in an imported module is reported on that module", describe(r));
    }
    {   // a parse error inside an imported module
        Overlay ov;
        ov[DIR + "util.skn"] = "pub fn f( -> Int { 1 }\n";
        const AnalysisResult r = analyze(ENTRY, "import util\nprintln(util::f())\n", ov);
        const auto it = r.by_path.find(DIR + "util.skn");
        t.check(it != r.by_path.end() && it->second.size() == 1 && it->second[0].message.rfind("parse error: ", 0) == 0,
                "parse error in an imported module is reported on that module", describe(r));
    }
    {   // a missing module: no position from the loader, located at the import line
        const AnalysisResult r = analyze(ENTRY, "println(0)\nimport nosuch\n", none);
        const auto it = r.by_path.find(ENTRY);
        const bool ok = it != r.by_path.end() && it->second.size() == 1 &&
                        contains(it->second[0].message, "cannot resolve module 'nosuch'") &&
                        it->second[0].range.start.line == 1 && it->second[0].range.start.character == 7 &&
                        it->second[0].range.end.character == 13;
        t.check(ok, "missing module is pinned to its import line", describe(r));
    }
    {   // an open buffer overrides the file on disk, and a BOM is stripped
        Overlay ov;
        ov[DIR + "util.skn"] = "\xEF\xBB\xBFpub fn f() -> Int { 1 }\n";
        const AnalysisResult r = analyze(ENTRY, "import util\nprintln(util::f())\n", ov);
        t.check(r.by_path.empty(), "overlay module with a BOM checks clean", describe(r));
    }
}

// ---- step B: outline, hover, definition -------------------------------------------

const std::string UTIL = DIR + "util.skn";

const std::string UTIL_SRC =
    "pub fn double(n: Int) -> Int { n * 2 }\n"
    "pub struct Pair { a: Int, b: Int }\n"
    "impl Pair { fn sum(self) -> Int { self.a + self.b } }\n";

const std::string MAIN_SRC =
    "import util\n"                                                        // 1
    "use util::{Pair}\n"                                                   // 2
    "enum Shape { Circle(Double), Square(Double) }\n"                      // 3
    "trait Named { fn name(self) -> String }\n"                            // 4
    "impl Named for Pair { fn name(self) -> String { \"pair\" } }\n"       // 5
    "const LIMIT: Int = 3\n"                                               // 6
    "fn area(s: Shape) -> Double {\n"                                      // 7
    "    match s {\n"                                                      // 8
    "        Circle(r) => r * r,\n"                                        // 9
    "        Shape::Square(w) => w * w,\n"                                 // 10
    "    }\n"                                                              // 11
    "}\n"                                                                  // 12
    "let x = 1\n"                                                          // 13
    "let x = x + util::double(LIMIT)\n"                                    // 14
    "let mut v = vec()\n"                                                  // 15
    "push(v, x)\n"                                                         // 16
    "let p = Pair { a: 1, b: 2 }\n"                                        // 17
    "let g: fn(Int) -> Int = fn(k) { k + p.a }\n"                          // 18
    "println(g(len(v)), area(Circle(1.0)), Named::name(p), Some(2))\n"     // 19
    "println(p.name(), p.sum(), p.b)\n";                                    // 20

// The 0-based position of the `nth` occurrence of `needle` on 1-based line `line`.
Position at(const std::string& text, int line, const std::string& needle, int nth = 1) {
    const std::string t = line_text(text, static_cast<uint32_t>(line));
    std::size_t i = std::string::npos;
    for (int k = 0; k < nth; ++k) i = t.find(needle, i == std::string::npos ? 0 : i + 1);
    return Position{ line - 1, i == std::string::npos ? -1 : static_cast<int>(i) };
}

std::string show(const std::optional<std::string>& h) {
    if (!h) return "(none)";
    std::string s = *h;
    const std::string open = "```skarn\n", close = "\n```";
    if (s.rfind(open, 0) == 0) s.erase(0, open.size());
    if (s.size() >= close.size() && s.compare(s.size() - close.size(), close.size(), close) == 0)
        s.erase(s.size() - close.size());
    return s;
}

std::string show(const std::optional<Location>& l) {
    if (!l) return "(none)";
    return l->path.substr(l->path.find_last_of('/') + 1) + ":" + std::to_string(l->range.start.line + 1) + ":" +
           std::to_string(l->range.start.character + 1) + "-" + std::to_string(l->range.end.character + 1);
}

void test_outline(Suite& t, const AnalysisResult& r) {
    t.section("outline");
    const std::vector<Symbol> syms = document_symbols(r, ENTRY);
    std::string names;
    for (const Symbol& s : syms) names += s.name + "/" + std::to_string(static_cast<int>(s.kind)) + " ";
    t.check(names == "Shape/10 Named/11 impl Named for Pair/3 LIMIT/14 area/12 ", "items, kinds, order", names);
    if (syms.size() == 5) {
        t.check(syms[0].children.size() == 2 && syms[0].children[1].name == "Square" &&
                syms[0].children[1].kind == SymbolKind::EnumMember, "enum variants are children");
        t.check(syms[2].children.size() == 1 && syms[2].children[0].detail == "fn name(self) -> String",
                "impl methods are children, with their signature");
        const Symbol& area = syms[4];
        t.check(area.selection.start.line == 6 && area.selection.start.character == 3 &&
                area.selection.end.character == 7, "selection range is the name");
        t.check(area.range.start.line == 6 && area.range.end.line == 11, "range ends before the next item");
        t.check(area.detail == "fn area(s: Shape) -> Double", "fn detail is its signature", area.detail);
    }
    const std::vector<Symbol> util = document_symbols(r, UTIL);
    t.check(util.size() == 3 && util[0].name == "double" && util[1].detail == "struct Pair { a: Int, b: Int }",
            "an imported module's outline comes from its importer's analysis");
}

void test_hover(Suite& t, const AnalysisResult& r) {
    t.section("hover");
    const std::string& m = MAIN_SRC;
    auto h = [&](int line, const std::string& needle, int nth = 1) { return show(hover(r, ENTRY, at(m, line, needle, nth))); };
    auto expect = [&](const std::string& got, const std::string& want, const std::string& name) { t.check(got == want, name, got); };
    expect(h(14, "x", 2), "x: Int", "local use");
    expect(h(15, "v"), "v: Vec[Int]", "let binding shows the type solved later (vec() then push)");
    expect(h(14, "double"), "fn double(n: Int) -> Int", "a fn from another module shows its signature");
    expect(h(14, "LIMIT"), "const LIMIT: Int", "a const use");
    expect(h(17, "Pair"), "struct Pair { a: Int, b: Int }", "a struct literal names its struct");
    expect(h(18, "k", 2), "k: Int", "lambda parameter typed from the expected fn type");
    expect(h(18, "a"), "a: Int", "field access shows the field type");
    expect(h(7, "s"), "s: Shape", "parameter at its declaration");
    expect(h(9, "r", 3), "r: Double", "match binding, at a use");
    expect(h(3, "Shape"), "enum Shape", "enum at its declaration");
    expect(h(3, "Square"), "Shape::Square(Double)", "variant at its declaration");
    expect(h(10, "Square"), "Shape::Square(Double)", "qualified variant: the tail token");
    expect(h(19, "1.0"), "Double", "literal");
    expect(h(19, "len"), "len(..) -> Int", "a builtin shows what the call produces");
    expect(h(19, "Some"), "Some(..) -> Option[Int]", "a std constructor call shows what it builds");
    expect(h(16, "push"), "push(..) -> Vec[Int]", "the checker types push as its receiver");
    expect(h(20, "name"), "fn name(self) -> String", "method call through a trait impl");
    expect(h(20, "sum"), "fn sum(self) -> Int", "inherent method call");

    // An unrelated type error does not stop hover.
    const AnalysisResult bad = analyze(ENTRY, "let a = 1\nlet b: String = a\nprintln(a, b)\n", Overlay());
    t.check(!bad.by_path.empty() && show(hover(bad, ENTRY, Position{ 2, 8 })) == "a: Int",
            "hover works while the program has a type error", show(hover(bad, ENTRY, Position{ 2, 8 })));
}

void test_definition(Suite& t, const AnalysisResult& r) {
    t.section("definition");
    const std::string& m = MAIN_SRC;
    auto d = [&](int line, const std::string& needle, int nth = 1) { return show(definition(r, ENTRY, at(m, line, needle, nth))); };
    auto expect = [&](const std::string& got, const std::string& want, const std::string& name) { t.check(got == want, name, got); };
    expect(d(14, "x", 2), "main.skn:13:5-6", "`let x = x + ..` reads the PREVIOUS x");
    expect(d(16, "x"), "main.skn:14:5-6", "a later use reads the shadowing x");
    expect(d(14, "double"), "util.skn:1:8-14", "fn in another module");
    expect(d(17, "Pair"), "util.skn:2:12-16", "struct literal -> struct in another module");
    expect(d(2, "Pair"), "util.skn:2:12-16", "a `use` list name -> the declaration it imports");
    expect(d(14, "LIMIT"), "main.skn:6:7-12", "const");
    expect(d(8, "s"), "main.skn:7:9-10", "parameter");
    expect(d(9, "r", 3), "main.skn:9:16-17", "match binding");
    expect(d(9, "Circle"), "main.skn:3:14-20", "variant pattern -> variant");
    expect(d(10, "Square"), "main.skn:3:30-36", "qualified variant pattern -> variant");
    expect(d(18, "k", 2), "main.skn:18:28-29", "lambda parameter");
    expect(d(18, "p"), "main.skn:17:5-6", "a lambda sees the enclosing locals");
    expect(d(19, "area"), "main.skn:7:4-8", "fn call");
    expect(d(19, "name"), "main.skn:4:18-22", "Trait::m -> the trait's method");
    expect(d(5, "name"), "main.skn:4:18-22", "an impl method -> the trait's method");
    expect(d(7, "Shape"), "main.skn:3:6-11", "a type in an annotation");
    expect(d(19, "Some"), "(none)", "a std declaration has no file");
    expect(d(13, "let"), "(none)", "a keyword is not a name");
    expect(d(20, "name"), "main.skn:4:18-22", "recv.m() via a trait impl -> the trait's method");
    expect(d(20, "sum"), "util.skn:3:16-19", "recv.m() inherent -> the method in another module");
    expect(d(20, "b"), "util.skn:2:27-28", "recv.field -> the field in the struct");
    expect(d(18, "a"), "util.skn:2:19-20", "field access inside a lambda");
}

std::string show(const std::vector<Location>& ls) {
    std::string s;
    for (const Location& l : ls)
        s += l.path.substr(l.path.find_last_of('/') + 1) + ":" + std::to_string(l.range.start.line + 1) + ":" +
             std::to_string(l.range.start.character + 1) + " ";
    return s;
}

std::string show(const std::map<std::string, std::vector<TextEdit>>& changes) {
    std::string s;
    for (const auto& [path, edits] : changes)
        for (const TextEdit& e : edits)
            s += path.substr(path.find_last_of('/') + 1) + ":" + std::to_string(e.range.start.line + 1) + ":" +
                 std::to_string(e.range.start.character + 1) + "=" + e.new_text + " ";
    return s;
}

void test_references(Suite& t, const AnalysisResult& r) {
    t.section("references");
    const std::string& m = MAIN_SRC;
    const std::vector<const AnalysisResult*> all{ &r };
    auto refs = [&](int line, const std::string& needle, int nth = 1, bool decl = true) {
        return show(references(all, ENTRY, at(m, line, needle, nth), decl));
    };
    auto expect = [&](const std::string& got, const std::string& want, const std::string& name) { t.check(got == want, name, got); };
    expect(refs(13, "x"), "main.skn:13:5 main.skn:14:9 ", "a local: its binding and the one use it reaches");
    expect(refs(13, "x", 1, false), "main.skn:14:9 ", "without the declaration");
    expect(refs(14, "double"), "main.skn:14:19 util.skn:1:8 ", "a fn across modules, at the tail of `util::double`");
    expect(refs(17, "Pair"), "main.skn:2:12 main.skn:5:16 main.skn:17:9 util.skn:2:12 util.skn:3:6 ",
           "a struct: use list, trait impl target, literal, declaration, inherent impl target");
    expect(refs(3, "Square"), "main.skn:3:30 main.skn:10:16 ", "a variant: declaration and the tail of `Shape::Square(w)`");
    expect(refs(3, "Shape"), "main.skn:3:6 main.skn:7:12 main.skn:10:9 ",
           "an enum: declaration, annotation, and the head of `Shape::Square(w)`");
    expect(refs(4, "name"), "main.skn:4:18 main.skn:5:26 main.skn:19:46 main.skn:20:11 ",
           "a trait method: declaration, impl method, `Named::name(p)`, `p.name()`");
    expect(refs(4, "Named"), "main.skn:4:7 main.skn:5:6 main.skn:19:39 ",
           "a trait: declaration, impl header, the head of `Named::name`");
    expect(refs(20, "b"), "main.skn:20:30 util.skn:2:27 util.skn:3:49 ", "a field: accesses in both modules and the declaration");
    expect(refs(19, "Some"), "", "a std name has no references here");

    const AnalysisResult c = analyze(ENTRY, "let mut c = 0\nc = c + 1\nprintln(c)\n", Overlay());
    std::string kinds;
    for (const Highlight& h : highlights(c, ENTRY, Position{ 2, 8 }))
        kinds += std::to_string(h.range.start.line + 1) + ":" + std::to_string(h.range.start.character + 1) +
                 (h.write ? "W " : "R ");
    expect(kinds, "1:9W 2:1W 2:5R 3:9R ", "highlight: declaration and assignment are writes");
}

void test_rename(Suite& t, const AnalysisResult& r) {
    t.section("rename");
    const std::string& m = MAIN_SRC;
    const std::vector<const AnalysisResult*> all{ &r };
    auto ren = [&](const AnalysisResult& a, const std::string& src, int line, const std::string& needle,
                   const std::string& to, int nth = 1) {
        const std::vector<const AnalysisResult*> one{ &a };
        const RenameResult rr = rename(one, ENTRY, at(src, line, needle, nth), to);
        return rr.error.empty() ? show(rr.changes) : "ERROR " + rr.error;
    };
    auto expect = [&](const std::string& got, const std::string& want, const std::string& name) { t.check(got == want, name, got); };
    auto refused = [&](const std::string& got, const std::string& reason_part, const std::string& name) {
        t.check(got.rfind("ERROR ", 0) == 0 && got.find(reason_part) != std::string::npos, name, got);
    };

    expect(ren(r, m, 13, "x", "y"), "main.skn:13:5=y main.skn:14:9=y ", "a local");
    expect(ren(r, m, 14, "double", "twice"), "main.skn:14:19=twice util.skn:1:8=twice ", "a fn across modules");
    expect(ren(r, m, 17, "Pair", "Couple"),
           "main.skn:2:12=Couple main.skn:5:16=Couple main.skn:17:9=Couple util.skn:2:12=Couple util.skn:3:6=Couple ",
           "a struct everywhere, incl. the use list");
    expect(ren(r, m, 3, "Shape", "Form"), "main.skn:3:6=Form main.skn:7:12=Form main.skn:10:9=Form ",
           "an enum, incl. the head of a qualified pattern");
    expect(ren(r, m, 20, "name", "title"),
           "main.skn:4:18=title main.skn:5:26=title main.skn:19:46=title main.skn:20:11=title ",
           "a trait method with its impl and both call forms");

    const PrepareRename p = prepare_rename(all, ENTRY, at(m, 14, "double"));
    t.check(p.error.empty() && p.placeholder == "double" && p.range.start.character == 18 && p.range.end.character == 24,
            "prepareRename: the tail token and its text", p.error);
    refused(ren(r, m, 13, "let", "z"), "nothing to rename", "a keyword");
    refused(ren(r, m, 13, "x", "match"), "not a valid name", "the new name is a keyword");
    refused(ren(r, m, 17, "Pair", "couple"), "uppercase", "a type needs an uppercase name");
    refused(ren(r, m, 19, "Some", "Just"), "standard library", "a std declaration");
    refused(ren(r, m, 20, "b", "c"), "field", "a field");
    refused(ren(r, m, 20, "sum", "sum"), "same", "the same name");

    {   // struct shorthands: a literal `P { x }` reads x, a pattern `P { x }` binds it -- both expand
        const std::string src = "struct P { x: Int }\nlet x = 3\nlet p = P { x }\n"
                                "match p { P { x } => println(x) }\n";
        const AnalysisResult c = analyze(ENTRY, src, Overlay());
        expect(ren(c, src, 2, "x", "y"), "main.skn:2:5=y main.skn:3:13=x: y ", "a local read by a struct-literal shorthand");
        expect(ren(c, src, 4, "x", "z"), "main.skn:4:15=x: z main.skn:4:30=z ", "a binding made by a struct-pattern shorthand");
    }
    {   // the new name would capture a use of another binding
        const std::string src = "let a = 1\nlet b = 2\nprintln(a + b)\n";
        const AnalysisResult c = analyze(ENTRY, src, Overlay());
        refused(ren(c, src, 1, "a", "b"), "would refer to something else", "shadowing capture is refused");
    }
    {   // the new name collides with another declaration
        const std::string src = "fn f() -> Int { 1 }\nfn g() -> Int { 2 }\nprintln(f() + g())\n";
        const AnalysisResult c = analyze(ENTRY, src, Overlay());
        refused(ren(c, src, 1, "f()", "g"), "would", "a collision is refused");
    }
    {   // a bare trait-method call cannot be attributed, so the rename could miss it
        const std::string src = "trait T { fn nm(self) -> Int }\nimpl T for Int { fn nm(self) -> Int { 1 } }\n"
                                "println(nm(3))\n";
        const AnalysisResult c = analyze(ENTRY, src, Overlay());
        refused(ren(c, src, 1, "nm", "num"), "cannot be attributed", "an unattributable same-named call blocks the rename");
    }
}

void test_queries(Suite& t) {
    Overlay ov;
    ov[UTIL] = UTIL_SRC;
    const AnalysisResult r = analyze(ENTRY, MAIN_SRC, ov);
    t.section("queries: the program");
    t.check(r.by_path.empty() && r.program != nullptr, "the query program checks clean", describe(r));
    t.check(r.module_of.count(UTIL) == 1 && r.module_of.at(ENTRY) == "$entry", "module map");
    test_outline(t, r);
    test_hover(t, r);
    test_definition(t, r);
    test_references(t, r);
    test_rename(t, r);
}

void test_uris(Suite& t) {
    t.section("uris");
    t.check(uri_to_path("file:///f%3A/Entwicklung/a%20b/main.skn") == "f:/Entwicklung/a b/main.skn", "windows URI -> path");
    t.check(uri_to_path("file:///F:/x/y.skn") == "f:/x/y.skn", "unencoded drive colon, drive lower-cased");
    t.check(uri_to_path("file:///home/u/x.skn") == "/home/u/x.skn", "posix URI -> path");
    t.check(uri_to_path("untitled:Untitled-1").empty(), "a non-file URI yields no path");
    t.check(path_to_uri("F:\\Entwicklung\\a b\\main.skn") == "file:///f%3A/Entwicklung/a%20b/main.skn", "path -> URI in VS Code's spelling");
    t.check(path_to_uri("/home/u/x.skn") == "file:///home/u/x.skn", "posix path -> URI");
    t.check(normalize_path("C:\\a\\b") == "c:/a/b", "normalize: separators and drive letter");
}

// ---- completion ---------------------------------------------------------------------

// `<|>` marks the cursor; it is removed from the text.
Position take_cursor(std::string& text) {
    const std::size_t i = text.find("<|>");
    text.erase(i, 3);
    Position p;
    for (std::size_t k = 0; k < i; ++k)
        if (text[k] == '\n') { ++p.line; p.character = 0; } else ++p.character;
    return p;
}

std::vector<CompletionItem> complete_entry(std::string text, const Overlay& modules = {}) {
    const Position pos = take_cursor(text);
    return complete(analyze(ENTRY, text, modules), ENTRY, pos);
}

const CompletionItem* item(const std::vector<CompletionItem>& items, const std::string& label) {
    for (const CompletionItem& c : items) if (c.label == label) return &c;
    return nullptr;
}

std::string labels(const std::vector<CompletionItem>& items) {
    std::string s;
    for (const CompletionItem& c : items) s += c.label + " ";
    return s;
}

bool offers(const std::vector<CompletionItem>& items, std::initializer_list<const char*> want,
            std::initializer_list<const char*> not_want = {}) {
    for (const char* w : want) if (!item(items, w)) return false;
    for (const char* w : not_want) if (item(items, w)) return false;
    return true;
}

const std::string COMPLETION_DECLS =
    "trait Named { fn name(self) -> String }\n"
    "trait Loud: Named { fn shout(self) -> String }\n"
    "struct P { x: Int, y: Int }\n"
    "impl P {\n"
    "    fn new(x: Int) -> P { P { x: x, y: 0 } }\n"
    "    fn sum(self) -> Int { self.x + self.y }\n"
    "}\n"
    "impl Named for P { fn name(self) -> String { \"p\" } }\n"
    "impl Loud for P { fn shout(self) -> String { \"P!\" } }\n"
    "fn helper() -> Int { 1 }\n";

void test_completion(Suite& t) {
    t.section("completion");
    {
        const auto c = complete_entry(COMPLETION_DECLS + "fn f(p: P) -> Int { p.<|> }\n");
        t.check(offers(c, { "x", "y", "sum", "name", "shout" }, { "new", "helper", "let" }),
                "p. offers fields, methods with self and trait methods, not an associated fn", labels(c));
        const CompletionItem* x = item(c, "x");
        const CompletionItem* sum = item(c, "sum");
        t.check(x && x->kind == CompletionKind::Field && x->detail == "x: Int" && sum &&
                sum->kind == CompletionKind::Method && sum->detail == "fn sum(self) -> Int" && x->sort < sum->sort,
                "a member's kind and detail; fields sort first", labels(c));
    }
    {
        const auto c = complete_entry(COMPLETION_DECLS + "impl P { fn g(self) -> Int { self.<|> } }\n");
        t.check(offers(c, { "x", "sum", "shout" }), "self. offers the members", labels(c));
    }
    {
        const auto c = complete_entry("fn f(v: Vec[Int]) -> Int { v.<|> }\n");
        t.check(offers(c, { "clone", "intoIter" }), "v. on a Vec offers the std trait methods", labels(c));
    }
    {
        const auto c = complete_entry(COMPLETION_DECLS + "fn f(d: dyn Loud) -> String { d.<|> }\n");
        t.check(offers(c, { "shout", "name" }, { "x" }), "d. on a dyn offers the trait's and the supertrait's methods", labels(c));
    }
    {
        const auto c = complete_entry(COMPLETION_DECLS + "fn f[T: Named](t: T) -> String { t.<|> }\n");
        t.check(offers(c, { "name" }, { "shout" }), "t. on a bounded type parameter offers the bound's methods", labels(c));
    }
    {
        const auto c = complete_entry(COMPLETION_DECLS +
            "fn f(a: Int) -> Int {\n"
            "    let b = 2\n"
            "    if a > 0 {\n"
            "        let inner = 3\n"
            "        println(inner)\n"
            "    }\n"
            "    let c = <|>\n"
            "    let later = 4\n"
            "    a + b + c + later\n"
            "}\n");
        t.check(offers(c, { "a", "b", "helper", "P", "println", "len", "Some", "let" }, { "inner", "later", "c", "sqrt", "x" }),
                "names in a fn: params, earlier lets, items, std, builtins, keywords", labels(c));
        const CompletionItem* b = item(c, "b");
        const CompletionItem* helper = item(c, "helper");
        const CompletionItem* println = item(c, "println");
        const CompletionItem* let = item(c, "let");
        t.check(b && helper && println && let && b->sort < helper->sort && helper->sort < println->sort &&
                println->sort < let->sort && b->detail == "b: Int" && helper->detail == "fn helper() -> Int",
                "names sort locals, file items, std, keywords; a local shows its type", labels(c));
    }
    {
        const auto c = complete_entry("use std::math::*\nfn f() -> Double { <|> }\n");
        const CompletionItem* sq = item(c, "sqrt");
        t.check(sq && sq->detail == "fn sqrt(Double) -> Double", "a gated native is offered after its use", labels(c));
        const auto d = complete_entry("use std::math::{sqrt}\nfn f() -> Double { <|> }\n");
        t.check(offers(d, { "sqrt" }, { "cos", "PI" }), "an explicit use brings in just that name", labels(d));
    }
    {
        const auto c = complete_entry(COMPLETION_DECLS + "fn f(a: Int) -> Int {\n    let b: <|> = 1\n    b\n}\n");
        t.check(offers(c, { "P", "Named", "Int", "Vec", "Option" }, { "a", "helper", "println", "let" }),
                "a type position offers types only", labels(c));
        const auto d = complete_entry(COMPLETION_DECLS + "fn f(a: Int) -> Int {\n    let b: <|>\n    a\n}\n");
        t.check(offers(d, { "P", "Int" }, { "a", "helper" }), "`let b: ` without a value is a type position", labels(d));
        const auto e = complete_entry(COMPLETION_DECLS + "fn g(a: Int, b: <|>\n");
        t.check(offers(e, { "P", "Int" }, { "a", "helper" }), "an unfinished fn header's parameter type", labels(e));
        const auto f = complete_entry(COMPLETION_DECLS + "fn g(a: Int) -> <|>\n");
        t.check(offers(f, { "P", "Int" }, { "a", "helper" }), "an unfinished fn header's return type", labels(f));
    }
    {
        const auto c = complete_entry(COMPLETION_DECLS + "fn f(p: P) -> Int {\n    if p.<|>\n    0\n}\n");
        t.check(offers(c, { "x", "sum" }), "if p. without its block is completed by the second attempt", labels(c));
    }
    {
        const auto c = complete_entry(COMPLETION_DECLS + "fn f(a: Int) -> Int {\n    let b = 2\n    println(a, <|>\n    a\n}\n");
        t.check(offers(c, { "a", "b", "helper" }), "a dropped statement still offers the block's names", labels(c));
    }
    {
        const auto c = complete_entry(COMPLETION_DECLS + "fn f(p: P) -> Int {\n    println(helper(), p.<|>\n    0\n}\n");
        t.check(offers(c, { "x", "sum" }), "p. inside an unclosed call: the second attempt closes the line", labels(c));
        const auto d = complete_entry(COMPLETION_DECLS + "fn f(p: P) -> P {\n    P { x: p.<|>\n}\n");
        t.check(offers(d, { "y" }), "p. inside an unclosed struct literal", labels(d));
        const auto e = complete_entry(COMPLETION_DECLS + "fn f(p: P) -> Int {\n    let k = match p.<|>\n    0\n}\n");
        t.check(offers(e, { "x", "sum" }), "p. in a match head in the middle of a line", labels(e));
        const auto g = complete_entry(COMPLETION_DECLS + "fn f(p: P) -> Int {\n    let k = match toString(p.<|>\n    0\n}\n");
        t.check(offers(g, { "x" }), "p. inside a call inside a match head: the call closes before the block", labels(g));
        const auto h = complete_entry(COMPLETION_DECLS + "fn f(p: P) -> Int {\n    println(\"v=${p.<|>\n    0\n}\n");
        t.check(offers(h, { "x" }), "p. inside an unclosed interpolation hole", labels(h));
    }
    {
        const auto c = complete_entry(COMPLETION_DECLS + "fn f(p: P) -> String { na<|> }\n");
        const CompletionItem* name = item(c, "name");
        t.check(name && name->kind == CompletionKind::Method && name->detail == "fn name(self) -> String",
                "a trait method is offered as a bare name (`name(p)`)", labels(c));
    }
    {
        const auto c = complete_entry(COMPLETION_DECLS + "fn f[T: Named](t: T) -> Int {\n    let v: Vec[<|>] = vec()\n    0\n}\n");
        const CompletionItem* tp = item(c, "T");
        t.check(tp && tp->kind == CompletionKind::TypeParameter && offers(c, { "P" }, { "t" }),
                "a type position offers the generic parameters in scope", labels(c));
        const auto d = complete_entry(COMPLETION_DECLS + "impl P { fn g(self) -> <|> { self } }\n");
        t.check(offers(d, { "Self", "P" }, { "T" }), "Self in an impl, and no parameter of another item", labels(d));
    }
    t.check(complete_entry("fn f() -> String { \"abc <|>\" }\n").empty(), "nothing inside a string");
    t.check(complete_entry("fn f() -> Int {\n    // see <|>\n    1\n}\n").empty(), "nothing inside a comment");
    t.check(complete_entry("fn f() -> Int {\n    let <|>\n    1\n}\n").empty(), "nothing where a name is declared");
    t.check(complete_entry("fn f() -> Double { 1.<|> }\n").empty(), "nothing after a number's dot");
    {
        const auto c = complete_entry("fn f(a: Int) -> String { \"v=${<|>}\" }\n");
        t.check(offers(c, { "a" }), "an interpolation hole is code", labels(c));
    }
    {
        const std::string util = DIR + "shapes.skn";
        Overlay ov;
        std::string mod = "pub struct Q { z: Int }\npub fn g(q: Q) -> Int { q.<|> }\n";
        const Position pos = take_cursor(mod);
        ov[util] = mod;
        const AnalysisResult r = analyze(ENTRY, "import shapes\nprintln(shapes::g(shapes::Q { z: 1 }))\n", ov);
        const auto c = complete(r, util, pos);
        t.check(offers(c, { "z" }), "member completion in an imported module", labels(c) + describe(r));
        const auto m = complete_entry("import shapes\nprintln(sha<|>)\n", ov);
        const CompletionItem* shapes = item(m, "shapes");
        t.check(shapes && shapes->kind == CompletionKind::Module, "an imported module is offered by its name", labels(m));
    }
}

// ---- completion after `::` ------------------------------------------------------------

const std::string QUALIFIED_DECLS = COMPLETION_DECLS +
    "enum Shape { Circle(Double), Square }\n"
    "impl Shape { fn unit() -> Shape { Shape::Square } }\n"
    "impl Named for Shape { fn name(self) -> String { \"s\" } }\n";

const std::string UTIL_MODULE =
    "pub fn g() -> Int { 1 }\n"
    "fn hidden() -> Int { 2 }\n"
    "pub struct Q { z: Int }\n"
    "pub enum Color { Red, Green }\n";

void test_qualified_completion(Suite& t) {
    t.section("completion after ::");
    {
        const auto c = complete_entry(QUALIFIED_DECLS + "fn f() -> Shape { Shape::<|> }\n");
        t.check(offers(c, { "Circle", "Square", "unit" }, { "name", "helper", "P", "let" }),
                "an enum's variants and inherent fns, no trait method, nothing else", labels(c));
        const auto d = complete_entry(QUALIFIED_DECLS + "fn f() -> Int { P::<|> }\n");
        const CompletionItem* n = item(d, "new");
        const CompletionItem* s = item(d, "sum");
        t.check(n && n->kind == CompletionKind::Function && s && s->kind == CompletionKind::Method &&
                !item(d, "x") && !item(d, "name"), "a struct's associated fn and method, no field or trait method", labels(d));
        const auto e = complete_entry(QUALIFIED_DECLS + "fn f(p: P) -> String { Named::<|> }\n");
        t.check(offers(e, { "name" }, { "shout" }), "a trait's methods", labels(e));
        const auto o = complete_entry("fn f() -> Option[Int] { Option::<|> }\n");
        t.check(offers(o, { "Some", "None" }), "a std enum through the ring", labels(o));
    }
    {
        const auto c = complete_entry(QUALIFIED_DECLS + "fn f(s: Shape) -> Int {\n    match s {\n        Shape::<|>\n    }\n}\n");
        t.check(offers(c, { "Circle", "Square" }), "a variant in a match arm", labels(c));
        const auto d = complete_entry(QUALIFIED_DECLS + "fn f() -> Shape { Shape::Ci<|> }\n");
        t.check(offers(d, { "Circle" }), "with a typed prefix (the client filters)", labels(d));
    }
    {
        Overlay ov;
        ov[DIR + "util.skn"] = UTIL_MODULE;
        const auto c = complete_entry("import util\nfn f() -> Int { util::<|> }\n", ov);
        t.check(offers(c, { "g", "Q", "Color", "Red" }, { "hidden", "helper" }),
                "a module's pub items, not its private ones", labels(c));
        const auto d = complete_entry("import util\nfn f() -> Int {\n    let x: util::<|>\n    1\n}\n", ov);
        t.check(offers(d, { "Q", "Color" }, { "g", "Red" }), "a module path in a type: types only", labels(d));
        const auto e = complete_entry("fn f() -> Int { util::<|> }\n", ov);
        t.check(e.empty(), "nothing from a module the file does not import", labels(e));
        const auto u = complete_entry("import util\nuse util::Color::<|>\n", ov);
        t.check(offers(u, { "Red", "Green" }), "a use path to an enum: its variants", labels(u));
    }
    {
        const auto c = complete_entry("fn f() -> Int { std::<|> }\n");
        t.check(offers(c, { "map", "Option" }, { "sqrt", "readFile" }), "std:: is the ring, no natives", labels(c));
        const auto d = complete_entry("use std::<|>\n");
        const CompletionItem* m = item(d, "math");
        t.check(m && m->kind == CompletionKind::Module && offers(d, { "io", "json", "core" }), "use std:: lists the std modules", labels(d));
        const auto e = complete_entry("use std::math::<|>\n");
        t.check(offers(e, { "sqrt", "PI" }, { "math" }), "use std::math:: lists its items and natives", labels(e));
        const auto f = complete_entry("use std::math::{sqrt, <|>}\n");
        t.check(offers(f, { "PI", "cos" }), "inside a use list", labels(f));
    }
    t.check(complete_entry("fn f() -> Int { a::b::<|> }\n").empty(), "nothing after a two-segment path outside use");
    t.check(complete_entry("import std::<|>\n").empty(), "nothing in an import path");
    t.check(complete_entry(QUALIFIED_DECLS + "fn f() -> String { \"Shape::<|>\" }\n").empty(), "nothing inside a string");
    t.check(complete_entry(QUALIFIED_DECLS + "fn f() -> Int {\n    // Shape::<|>\n    1\n}\n").empty(), "nothing inside a comment");
}

// ---- signature help -------------------------------------------------------------------

std::optional<SignatureHelp> help_entry(std::string text, const Overlay& modules = {}) {
    const Position pos = take_cursor(text);
    return signature_help(analyze(ENTRY, text, modules), ENTRY, pos);
}

std::string show(const std::optional<SignatureHelp>& h) {
    if (!h) return "(none)";
    std::string s;
    for (const SignatureInfo& sig : h->signatures) s += sig.label + " | ";
    return s + "active " + std::to_string(h->active_signature) + "/" + std::to_string(h->active_parameter);
}

// The active signature's label, or empty.
std::string label_of(const std::optional<SignatureHelp>& h) {
    return h && !h->signatures.empty() ? h->signatures[h->active_signature].label : std::string();
}

// The text of the active parameter in the active signature's label, or empty.
std::string active_param(const std::optional<SignatureHelp>& h) {
    if (!h || h->signatures.empty()) return {};
    const SignatureInfo& s = h->signatures[h->active_signature];
    if (h->active_parameter < 0 || h->active_parameter >= static_cast<int>(s.params.size())) return {};
    const auto [a, b] = s.params[h->active_parameter];
    return s.label.substr(a, b - a);
}

const std::string SIGNATURE_DECLS =
    "fn add(a: Int, b: Int) -> Int { a + b }\n"
    "fn neg(x: Int) -> Int { 0 - x }\n"
    "fn say(s: String, n: Int) -> Int { n }\n"
    "fn app(f: fn(Int) -> Int) -> Int { f(1) }\n"
    "trait Shape { fn area(self) -> Double }\n"
    "trait Plot { fn area(self) -> Int }\n"
    "struct Sq { s: Double }\n"
    "impl Shape for Sq { fn area(self) -> Double { self.s * self.s } }\n"
    "impl Sq { fn scale(self, k: Double) -> Sq { Sq { s: self.s * k } } }\n"
    "struct Pair(Int, String)\n"
    "enum E { A(Int, String), B }\n";

void test_signature_help(Suite& t) {
    t.section("signature help");
    {
        const auto h = help_entry(SIGNATURE_DECLS + "fn f() -> Int { add(<|>) }\n");
        t.check(label_of(h) == "fn add(a: Int, b: Int) -> Int" && h->signatures[0].params.size() == 2 &&
                active_param(h) == "a: Int", "a fn's signature, its parameters, the first active", show(h));
        const auto i = help_entry(SIGNATURE_DECLS + "fn f() -> Int { add(1, <|>) }\n");
        t.check(active_param(i) == "b: Int", "after a comma the next parameter is active", show(i));
        const auto j = help_entry(SIGNATURE_DECLS + "fn f() -> Int { add(1<|>) }\n");
        t.check(active_param(j) == "a: Int", "in text that parses, at the end of an argument", show(j));
    }
    {
        const auto h = help_entry(SIGNATURE_DECLS + "fn f() -> Int {\n    let r = add(1, <|>\n    r\n}\n");
        t.check(active_param(h) == "b: Int", "an unclosed call on its line", show(h));
        const auto i = help_entry(SIGNATURE_DECLS + "fn f() -> Int {\n    if add(1, <|>\n    0\n}\n");
        t.check(active_param(i) == "b: Int", "an unclosed call in an if head", show(i));
    }
    {
        const auto h = help_entry(SIGNATURE_DECLS + "fn f() -> Int {\n    add(1, neg(<|>\n}\n");
        t.check(label_of(h) == "fn neg(x: Int) -> Int" && active_param(h) == "x: Int", "the innermost call", show(h));
        const auto i = help_entry(SIGNATURE_DECLS + "fn f() -> Int { add(1, neg(2), <|>) }\n");
        t.check(label_of(i).rfind("fn add", 0) == 0 && i->active_parameter == 2 && active_param(i).empty(),
                "a closed inner call; an argument too many highlights nothing", show(i));
        const auto j = help_entry(SIGNATURE_DECLS + "fn f() -> Int { add((1 + <|>)) }\n");
        t.check(active_param(j) == "a: Int", "a group inside the arguments", show(j));
        const auto k = help_entry(SIGNATURE_DECLS + "fn f() -> Int { say(\"a,b\", <|>) }\n");
        t.check(active_param(k) == "n: Int", "a comma inside a string does not count", show(k));
    }
    {
        const auto h = help_entry(SIGNATURE_DECLS + "fn f(q: Sq) -> Sq { q.scale(<|>) }\n");
        t.check(label_of(h) == "fn scale(self, k: Double) -> Sq" && h->signatures[0].params.size() == 1 &&
                active_param(h) == "k: Double", "a method call: the receiver is no argument", show(h));
        const auto i = help_entry(SIGNATURE_DECLS + "fn f(q: Sq) -> Sq { Sq::scale(<|>) }\n");
        t.check(active_param(i) == "self", "a qualified method call: self is the first argument", show(i));
        const auto j = help_entry(SIGNATURE_DECLS + "fn f(q: Sq) -> Double { area(<|>) }\n");
        t.check(j && j->signatures.size() == 2 && active_param(j) == "self",
                "a bare trait-method call: one signature per trait", show(j));
    }
    {
        const auto h = help_entry(SIGNATURE_DECLS + "fn f() -> E { E::A(1, <|>) }\n");
        t.check(label_of(h) == "E::A(Int, String)" && active_param(h) == "String", "a tuple variant", show(h));
        const auto i = help_entry(SIGNATURE_DECLS + "fn f() -> Pair { Pair(<|>) }\n");
        t.check(label_of(i) == "Pair(Int, String)" && active_param(i) == "Int", "a tuple struct", show(i));
        const auto j = help_entry("fn f() -> Option[Int] { Some(<|>) }\n");
        t.check(label_of(j) == "Option::Some(T)", "a std variant", show(j));
    }
    {
        const auto h = help_entry("fn f(s: String) -> Int { len(<|>) }\n");
        t.check(label_of(h).rfind("fn len(", 0) == 0 && active_param(h).find("String") == 0, "a builtin", show(h));
        const auto i = help_entry("fn f(m: Map[String, Int]) -> Int {\n    let o = get(m, <|>\n    0\n}\n");
        t.check(i && i->signatures.size() == 4 && active_param(i) == "K", "a builtin with one signature per shape", show(i));
        const auto j = help_entry("use std::math::*\nfn f() -> Double { sqrt(<|>) }\n");
        t.check(label_of(j) == "fn sqrt(Double) -> Double" && active_param(j) == "Double", "a native", show(j));
        const auto k = help_entry("fn f() -> () { println(1, 2, <|>) }\n");
        t.check(active_param(k) == "...", "a variadic builtin keeps its last parameter active", show(k));
    }
    {
        const auto h = help_entry("fn f() -> Int {\n    let g = fn(x: Int) -> Int { x }\n    g(<|>)\n}\n");
        t.check(label_of(h) == "fn g(Int) -> Int" && active_param(h) == "Int", "a local of fn type", show(h));
        const auto i = help_entry(SIGNATURE_DECLS + "fn f() -> Int { 1 |> add(<|>) }\n");
        t.check(active_param(i) == "b: Int", "a pipe's value is the first argument", show(i));
    }
    {
        const std::string util = DIR + "calc.skn";
        Overlay ov;
        std::string mod = "pub fn h(a: Int, b: Int) -> Int { a }\npub fn g() -> Int { h(1, <|>) }\n";
        const Position pos = take_cursor(mod);
        ov[util] = mod;
        const AnalysisResult r = analyze(ENTRY, "import calc\nprintln(calc::g())\n", ov);
        const auto h = signature_help(r, util, pos);
        t.check(active_param(h) == "b: Int", "a call in an imported module", show(h) + describe(r));
    }
    t.check(!help_entry(SIGNATURE_DECLS + "fn f() -> Int {\n    let t = (1, <|>)\n    0\n}\n"), "nothing in a tuple");
    t.check(!help_entry(SIGNATURE_DECLS + "fn f() -> Int { app(fn(x: Int) -> Int { x + <|> }) }\n"),
            "nothing inside a lambda body among the arguments");
    t.check(!help_entry(SIGNATURE_DECLS + "fn f() -> Int { say(\"a, <|>\", 1) }\n"), "nothing inside a string");
    t.check(!help_entry(SIGNATURE_DECLS + "fn f() -> Int {\n    // add(<|>\n    0\n}\n"), "nothing inside a comment");
    t.check(!help_entry(SIGNATURE_DECLS + "fn f() -> Int { <|> }\n"), "nothing outside a call");
    t.check(!help_entry(SIGNATURE_DECLS + "fn k(a: Int, <|>\n"), "nothing in a declaration's parameters");
}

// ---- formatting ---------------------------------------------------------------------

const std::string FORMAT_RULES_IN = R"SKN(import report
use std::math::{sqrt,   cos}
trait Named { fn label(self) -> String }
struct P { x: Int, y: Int, }
impl P {
    fn new(x: Int) -> P { P { x: x, y: 0 } }


    fn sum(self) -> Int { self.x + self.y }
    fn mul(self) -> Int { self.x * self.y }
}
enum Tree { Leaf, Node(Int), }
enum Status : Int { Ok = 200, Missing = 404 }
fn test(t: Tree) -> Int {
    let s : String = "a,b"
    let m = if 1 > 2 { 1 } else { 2 }
    let f = fn(x: Int) -> Int { x * 2 }
    let a = 1   let b = 2
    match t {
        Tree::Leaf => 0,
        Tree::Node(v) => v
        Tree::Node(w) => { let q = w  q },
    }
}
let r = someFunctionWithALongName(firstArgument, secondArgument, thirdArgument, 4)
let total = range(0, 100) |> filter(fn(x: Int) -> Bool { x % 2 == 0 }) |> map(fn(x: Int) -> Int { x * x }) |> sum
let ok = alphaCondition(1) && betaCondition(2) && gammaCondition(3) && deltaCondition(4)
println("x=${m:>3}", r#"raw "q""#, 'A', 0xFF_FF)   // trailing
)SKN";

const std::string FORMAT_RULES_OUT = R"SKN(import report
use std::math::{sqrt, cos}


trait Named {
    fn label(self) -> String
}

struct P {
    x: Int,
    y: Int
}

impl P {
    fn new(x: Int) -> P {
        P { x: x, y: 0 }
    }

    fn sum(self) -> Int {
        self.x + self.y
    }
    fn mul(self) -> Int {
        self.x * self.y
    }
}

enum Tree {
    Leaf,
    Node(Int)
}

enum Status : Int {
    Ok = 200,
    Missing = 404
}

fn test(t: Tree) -> Int {
    let s: String = "a,b"
    let m = if 1 > 2 { 1 } else { 2 }
    let f = fn(x: Int) -> Int { x * 2 }
    let a = 1
    let b = 2
    match t {
        Tree::Leaf => 0,
        Tree::Node(v) => v,
        Tree::Node(w) => {
            let q = w
            q
        }
    }
}

let r = someFunctionWithALongName(
    firstArgument,
    secondArgument,
    thirdArgument,
    4
)
let total = range(0, 100)
    |> filter(fn(x: Int) -> Bool { x % 2 == 0 })
    |> map(fn(x: Int) -> Int { x * x })
    |> sum
let ok = alphaCondition(1)
    && betaCondition(2)
    && gammaCondition(3)
    && deltaCondition(4)
println("x=${m:>3}", r#"raw "q""#, 'A', 0xFF_FF) // trailing
)SKN";

const std::string FORMAT_BREAKS_IN = R"SKN(// header comment

fn longParameters(firstParameter: Int, secondParameter: String, third: Double) -> Int {
    let v = foo(1, // one
        2)
    /* block */ let w = bar(v)
    let chosen = if someLongConditionName > anotherLongConditionName { firstResultValue } else { secondResultValue }
    let g = fn(element: Int) -> Int { element * someMultiplierFactor + someOffsetValueName }
    let c = builder.withName("n").withSize(10).withColor("red").withBorder(true).build()
    println("a single string argument that is longer than the eighty columns of a line")
    // last comment
}
)SKN";

const std::string FORMAT_BREAKS_OUT = R"SKN(// header comment

fn longParameters(
    firstParameter: Int,
    secondParameter: String,
    third: Double
) -> Int {
    let v = foo(
        1, // one
        2
    )
    /* block */
    let w = bar(v)
    let chosen = if someLongConditionName > anotherLongConditionName {
        firstResultValue
    } else {
        secondResultValue
    }
    let g = fn(element: Int) -> Int {
        element * someMultiplierFactor + someOffsetValueName
    }
    let c = builder
        .withName("n")
        .withSize(10)
        .withColor("red")
        .withBorder(true)
        .build()
    println("a single string argument that is longer than the eighty columns of a line")
    // last comment
}
)SKN";

void test_formatting(Suite& t) {
    t.section("formatting");
    auto golden = [&](const std::string& name, const std::string& in, const std::string& want) {
        const FormatResult r = format_source(in);
        t.check(r.ok && r.text == want, name, r.ok ? r.text : r.error);
        if (!r.ok) return;
        const FormatResult again = format_source(r.text);
        t.check(again.ok && again.text == r.text, name + ": formatting the result changes nothing",
                again.ok ? again.text : again.error);
    };
    golden("bodies, members, blank lines, commas, match, inline lambda and if, chains, atoms",
           FORMAT_RULES_IN, FORMAT_RULES_OUT);
    golden("comments, long parameters, broken if and lambda, method chain, one long string",
           FORMAT_BREAKS_IN, FORMAT_BREAKS_OUT);
    {
        const FormatResult r = format_source("fn f() -> Int { 1 }\r\nlet x = f()\r\n");
        t.check(r.ok && r.text == "fn f() -> Int {\r\n    1\r\n}\r\n\r\nlet x = f()\r\n",
                "CRLF line breaks are kept", r.ok ? r.text : r.error);
    }
    {
        const FormatResult r = format_source("fn f() -> Int {\n    1 +\n}\n");
        t.check(!r.ok && r.error.find("syntax errors") != std::string::npos, "a file with a syntax error is not formatted",
                r.error);
    }
    {
        const FormatResult r = format_source(FORMAT_RULES_OUT);
        t.check(r.ok && r.text == FORMAT_RULES_OUT, "a formatted file is left unchanged", r.ok ? r.text : r.error);
    }
}

// ---- S4: a scripted session -----------------------------------------------------

std::string frame(const std::string& body) {
    std::ostringstream o;
    write_message(o, body);
    return o.str();
}

std::string did_open(const std::string& uri, const std::string& text) {
    Json doc = Json::object();
    doc.set("uri", uri);
    doc.set("languageId", "skarn");
    doc.set("version", 1);
    doc.set("text", text);
    Json params = Json::object();
    params.set("textDocument", std::move(doc));
    Json msg = Json::object();
    msg.set("jsonrpc", "2.0");
    msg.set("method", "textDocument/didOpen");
    msg.set("params", std::move(params));
    return msg.dump();
}

std::string did_change(const std::string& uri, const std::string& text) {
    Json doc = Json::object();
    doc.set("uri", uri);
    doc.set("version", 2);
    Json change = Json::object();
    change.set("text", text);
    Json changes = Json::array();
    changes.push(std::move(change));
    Json params = Json::object();
    params.set("textDocument", std::move(doc));
    params.set("contentChanges", std::move(changes));
    Json msg = Json::object();
    msg.set("jsonrpc", "2.0");
    msg.set("method", "textDocument/didChange");
    msg.set("params", std::move(params));
    return msg.dump();
}

void test_session(Suite& t) {
    t.section("session");
    const std::string uri = "file:///skarn-lsp-selftest/main.skn";
    std::string input;
    input += frame(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"capabilities":{}}})");
    input += frame(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
    input += frame(did_open(uri, "let a = 1\nprintln(undefinedName + a)\n"));
    input += frame(did_change(uri, "fn one() -> Int { 1 }\nlet a = one()\nprintln(a)\n"));
    const std::string doc = R"("textDocument":{"uri":")" + uri + R"("})";
    const std::string at_one = doc + R"(,"position":{"line":1,"character":9})";
    input += frame(R"({"jsonrpc":"2.0","id":3,"method":"textDocument/documentSymbol","params":{)" + doc + "}}");
    input += frame(R"({"jsonrpc":"2.0","id":4,"method":"textDocument/hover","params":{)" + at_one + "}}");
    input += frame(R"({"jsonrpc":"2.0","id":5,"method":"textDocument/definition","params":{)" + at_one + "}}");
    input += frame(R"({"jsonrpc":"2.0","id":8,"method":"textDocument/references","params":{)" + at_one +
                   R"(,"context":{"includeDeclaration":true}}})");
    input += frame(R"({"jsonrpc":"2.0","id":9,"method":"textDocument/rename","params":{)" + at_one + R"(,"newName":"uno"}})");
    input += frame(R"({"jsonrpc":"2.0","id":10,"method":"textDocument/prepareRename","params":{)" + doc +
                   R"(,"position":{"line":1,"character":1}}})");
    input += frame(R"({"jsonrpc":"2.0","id":12,"method":"textDocument/completion","params":{)" + doc +
                   R"(,"position":{"line":1,"character":9}}})");
    input += frame(R"({"jsonrpc":"2.0","id":13,"method":"textDocument/signatureHelp","params":{)" + doc +
                   R"(,"position":{"line":2,"character":8}}})");
    input += frame(R"({"jsonrpc":"2.0","id":14,"method":"textDocument/formatting","params":{)" + doc +
                   R"(,"options":{"tabSize":2,"insertSpaces":true}}})");
    input += frame(did_change(uri, "fn one() -> Int { 1 }\nlet a = one()\nfn two( -> Int\nprintln(a + )\n"));
    input += frame(R"({"jsonrpc":"2.0","id":6,"method":"textDocument/documentSymbol","params":{)" + doc + "}}");
    input += frame(R"({"jsonrpc":"2.0","id":7,"method":"textDocument/hover","params":{)" + at_one + "}}");
    input += frame(R"({"jsonrpc":"2.0","id":11,"method":"textDocument/rename","params":{)" + at_one + R"(,"newName":"uno"}})");
    input += frame(R"({"jsonrpc":"2.0","id":"x7","method":"textDocument/rangeFormatting","params":{}})");
    input += frame(R"({"jsonrpc":"2.0","id":2,"method":"shutdown"})");
    input += frame(R"({"jsonrpc":"2.0","method":"exit"})");

    std::istringstream in(input);
    std::ostringstream out, log;
    Server server(in, out, log);
    const int code = server.run();
    t.check(code == 0, "orderly shutdown + exit returns 0", log.str());

    std::istringstream replies(out.str());
    std::vector<Json> msgs;
    try {
        while (auto body = read_message(replies)) msgs.push_back(Json::parse(*body));
    } catch (const std::exception& e) {
        t.check(false, "server output is well-framed JSON", e.what());
        return;
    }
    t.check(msgs.size() == 18, "eighteen messages: init, 2 publishes, 9 answers, publish, 3 answers, error, shutdown",
            std::to_string(msgs.size()));
    if (msgs.size() != 18) return;

    const Json& caps = msgs[0].get("result").get("capabilities");
    t.check(msgs[0].get("id") == Json(1) && caps.get("textDocumentSync").get("change") == Json(1),
            "initialize advertises full text sync");
    t.check(caps.get("documentSymbolProvider") == Json(true) && caps.get("hoverProvider") == Json(true) &&
            caps.get("definitionProvider") == Json(true), "initialize advertises outline, hover, definition");
    t.check(caps.get("referencesProvider") == Json(true) && caps.get("documentHighlightProvider") == Json(true) &&
            caps.get("renameProvider").get("prepareProvider") == Json(true),
            "initialize advertises references, highlight, rename with prepare");
    t.check(caps.get("completionProvider").get("triggerCharacters") == Json::parse(R"(["."])"),
            "initialize advertises completion, triggered by '.'", caps.dump());
    t.check(caps.get("signatureHelpProvider").get("triggerCharacters") == Json::parse(R"(["(", ","])"),
            "initialize advertises signature help, triggered by '(' and ','", caps.dump());
    t.check(caps.get("documentFormattingProvider") == Json(true), "initialize advertises formatting", caps.dump());

    const Json& p1 = msgs[1].get("params");
    const Json& diags = p1.get("diagnostics");
    t.check(msgs[1].get("method") == Json("textDocument/publishDiagnostics") && p1.get("uri") == Json(uri) &&
            diags.size() == 1, "didOpen publishes one diagnostic on the client's URI", msgs[1].dump());
    if (diags.size() == 1) {
        const Json& range = diags.at(0).get("range");
        t.check(range.get("start").get("line") == Json(1) && range.get("start").get("character") == Json(8) &&
                range.get("end").get("character") == Json(21) && diags.at(0).get("severity") == Json(1) &&
                diags.at(0).get("source") == Json("skarn"), "diagnostic range, severity and source", diags.at(0).dump());
    }
    t.check(msgs[2].get("params").get("diagnostics").size() == 0 && msgs[2].get("params").get("uri") == Json(uri),
            "fixing the error publishes an empty list", msgs[2].dump());
    const Json& syms = msgs[3].get("result");
    t.check(msgs[3].get("id") == Json(3) && syms.size() == 1 && syms.at(0).get("name") == Json("one") &&
            syms.at(0).get("kind") == Json(12) && syms.at(0).get("selectionRange").get("start").get("character") == Json(3),
            "documentSymbol answers the outline", msgs[3].dump());
    const Json& hv = msgs[4].get("result").get("contents");
    t.check(msgs[4].get("id") == Json(4) && hv.get("kind") == Json("markdown") &&
            hv.get("value") == Json("```skarn\nfn one() -> Int\n```"), "hover answers markdown", msgs[4].dump());
    const Json& def = msgs[5].get("result");
    t.check(msgs[5].get("id") == Json(5) && def.get("uri") == Json(uri) &&
            def.get("range").get("start").get("line") == Json(0) && def.get("range").get("start").get("character") == Json(3),
            "definition answers a location on the client's URI", msgs[5].dump());
    const Json& refs = msgs[6].get("result");
    t.check(msgs[6].get("id") == Json(8) && refs.size() == 2 && refs.at(0).get("uri") == Json(uri),
            "references answers the declaration and the use", msgs[6].dump());
    const Json& edits = msgs[7].get("result").get("changes").get(uri);
    t.check(msgs[7].get("id") == Json(9) && edits.size() == 2 && edits.at(0).get("newText") == Json("uno"),
            "rename answers a WorkspaceEdit on the client's URI", msgs[7].dump());
    t.check(msgs[8].get("id") == Json(10) && msgs[8].get("error").get("code") == Json(-32803) &&
            msgs[8].get("error").get("message").as_string().find("nothing to rename") != std::string::npos,
            "prepareRename on a keyword answers RequestFailed with the reason", msgs[8].dump());
    {
        const Json& list = msgs[9].get("result");
        bool one = false;
        for (std::size_t i = 0; i < list.get("items").size(); ++i)
            one = one || (list.get("items").at(i).get("label") == Json("one") &&
                          list.get("items").at(i).get("kind") == Json(3) &&
                          list.get("items").at(i).get("detail") == Json("fn one() -> Int"));
        t.check(msgs[9].get("id") == Json(12) && list.get("isIncomplete") == Json(false) && one,
                "completion answers a CompletionList with the fn and its signature", msgs[9].dump());
    }
    {
        const Json& help = msgs[10].get("result");
        t.check(msgs[10].get("id") == Json(13) && help.get("signatures").size() == 1 &&
                help.get("signatures").at(0).get("label") == Json("fn println(T, ...)") &&
                help.get("signatures").at(0).get("parameters").at(0).get("label") == Json::parse("[11, 12]") &&
                help.get("activeSignature") == Json(0) && help.get("activeParameter") == Json(0),
                "signatureHelp answers the signature with parameter offsets", msgs[10].dump());
    }
    {
        const Json& fmt = msgs[11].get("result");
        t.check(msgs[11].get("id") == Json(14) && fmt.size() == 1 &&
                fmt.at(0).get("newText") == Json("fn one() -> Int {\n    1\n}\n\nlet a = one()\nprintln(a)\n") &&
                fmt.at(0).get("range").get("end").get("line") == Json(3),
                "formatting answers one edit replacing the document", msgs[11].dump());
    }
    t.check(msgs[12].get("method") == Json("textDocument/publishDiagnostics") &&
            msgs[12].get("params").get("diagnostics").size() == 2, "both syntax errors are published", msgs[12].dump());
    t.check(msgs[13].get("id") == Json(6) && msgs[13].get("result").size() == 1 &&
            msgs[13].get("result").at(0).get("name") == Json("one"),
            "with syntax errors, the outline shows the recovered items", msgs[13].dump());
    t.check(msgs[14].get("id") == Json(7) &&
            msgs[14].get("result").get("contents").get("value") == Json("```skarn\nfn one() -> Int\n```"),
            "with syntax errors, hover answers in the intact code", msgs[14].dump());
    t.check(msgs[15].get("id") == Json(11) && msgs[15].get("error").get("code") == Json(-32803) &&
            msgs[15].get("error").get("message") == Json("Fix the syntax errors in main.skn first."),
            "with syntax errors, rename is refused with the reason", msgs[15].dump());
    t.check(msgs[16].get("id") == Json("x7") && msgs[16].get("error").get("code") == Json(-32601),
            "unsupported request answers MethodNotFound with the string id", msgs[16].dump());
    t.check(msgs[17].get("id") == Json(2) && msgs[17].find("result") && msgs[17].get("result").is_null(),
            "shutdown answers a null result", msgs[17].dump());

    // exit without shutdown -> 1; end of input without exit -> 1
    std::istringstream in2(frame(R"({"jsonrpc":"2.0","method":"exit"})"));
    std::ostringstream out2, log2;
    t.check(Server(in2, out2, log2).run() == 1, "exit without shutdown returns 1");

    // a request before initialize is refused; a garbage body gets a ParseError response
    std::istringstream in3(frame(R"({"jsonrpc":"2.0","id":5,"method":"shutdown"})") + frame("{nope"));
    std::ostringstream out3, log3;
    Server(in3, out3, log3).run();
    std::istringstream r3(out3.str());
    const auto a = read_message(r3);
    const auto b = read_message(r3);
    t.check(a && Json::parse(*a).get("error").get("code") == Json(-32002), "request before initialize is refused");
    t.check(b && Json::parse(*b).get("error").get("code") == Json(-32700), "malformed JSON answers ParseError");
}

} // namespace

int run_selftest(std::ostream& out) {
    Suite t(out);
    try {
        test_json(t);
        test_transport(t);
        test_uris(t);
        test_analysis(t);
        test_queries(t);
        test_completion(t);
        test_qualified_completion(t);
        test_signature_help(t);
        test_formatting(t);
        test_session(t);
    } catch (const std::exception& e) {
        t.check(false, "unexpected exception", e.what());
    }
    return t.finish();
}

} // namespace lsp
