// =============================================================================
// Format.cpp -- see Format.h.
// =============================================================================

#include "Format.h"

#include "Ast.h"      // svc::dump
#include "Lexer.h"    // svc::Lexer / svc::Comment
#include "Parser.h"   // svc::Parser / svc::ParseLayout

#include <algorithm>
#include <exception>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace lsp {

namespace {

using svc::TokKind;

constexpr int WIDTH  = 80;
constexpr int INDENT = 4;

// Columns a text takes: code points of its first line.
int width_of(const std::string& s) {
    int w = 0;
    for (unsigned char c : s) {
        if (c == '\n') break;
        if ((c & 0xC0) != 0x80) ++w;
    }
    return w;
}

// ---- the document ------------------------------------------------------------------
//
// A Wadler-style document. A Group prints on one line when it fits, else broken: a Line is then a
// line break (else a space), a Soft a line break (else nothing). A Hard always breaks and forces
// every Group around it to break. A Fresh wraps an expanded body: it starts on a new line anyway,
// so it forces nothing outside, its own Groups decide for themselves, and measuring a line stops
// there. A Suffix is a `//` comment, printed at the end of the line. A Start begins a new line
// unless the current one is still empty.

struct Doc;
using DocP = std::shared_ptr<const Doc>;

struct Doc {
    enum class K { Text, Line, Soft, Hard, Start, Suffix, Group, Nest, Fresh, Concat } k = K::Concat;
    std::string text;
    std::vector<DocP> kids;
    bool hard  = false;   // holds a Hard or a Suffix outside any Fresh: the Group must break
    bool multi = false;   // spans lines: holds a Fresh (or is hard)
};

DocP make(Doc::K k, std::string text = {}, std::vector<DocP> kids = {}) {
    auto d = std::make_shared<Doc>();
    d->k = k;
    d->text = std::move(text);
    d->kids = std::move(kids);
    if (k == Doc::K::Hard || k == Doc::K::Suffix) d->hard = true;
    if (k == Doc::K::Fresh) d->multi = true;
    if (k == Doc::K::Text && d->text.find('\n') != std::string::npos) d->multi = true;
    if (k != Doc::K::Fresh)
        for (const DocP& c : d->kids) { d->hard = d->hard || c->hard; d->multi = d->multi || c->multi; }
    d->multi = d->multi || d->hard;
    return d;
}
DocP text(std::string s)       { return make(Doc::K::Text, std::move(s)); }
DocP line()                    { return make(Doc::K::Line); }
DocP soft()                    { return make(Doc::K::Soft); }
DocP hard()                    { return make(Doc::K::Hard); }
DocP start()                   { return make(Doc::K::Start); }
DocP suffix(std::string s)     { return make(Doc::K::Suffix, std::move(s)); }
DocP cat(std::vector<DocP> v)  { return make(Doc::K::Concat, {}, std::move(v)); }
DocP group(DocP d)             { return make(Doc::K::Group, {}, { std::move(d) }); }
DocP nest(DocP d)              { return make(Doc::K::Nest, {}, { std::move(d) }); }
DocP fresh(DocP d)             { return make(Doc::K::Fresh, {}, { std::move(d) }); }

class Printer {
public:
    std::string run(const DocP& root) {
        std::vector<Cmd> stack{ { 0, false, root.get() } };
        while (!stack.empty()) {
            const Cmd c = stack.back();
            stack.pop_back();
            const Doc& d = *c.d;
            switch (d.k) {
            case Doc::K::Text:   put(d.text, c.indent); break;
            case Doc::K::Line:   if (c.flat) put(" ", c.indent); else newline(c.indent); break;
            case Doc::K::Soft:   if (!c.flat) newline(c.indent); break;
            case Doc::K::Hard:   newline(c.indent); break;
            case Doc::K::Start:  if (text_on_line_) newline(c.indent); break;
            case Doc::K::Suffix: suffix_ += d.text; break;
            case Doc::K::Concat:
                for (auto it = d.kids.rbegin(); it != d.kids.rend(); ++it) stack.push_back({ c.indent, c.flat, it->get() });
                break;
            case Doc::K::Nest:   stack.push_back({ c.indent + INDENT, c.flat, d.kids[0].get() }); break;
            case Doc::K::Fresh:  stack.push_back({ c.indent, false, d.kids[0].get() }); break;
            case Doc::K::Group: {
                const bool flat = c.flat || (!d.hard && fits(d.kids[0].get(), c.indent, stack, WIDTH - col_));
                stack.push_back({ c.indent, flat, d.kids[0].get() });
                break;
            }
            }
        }
        out_ += suffix_;
        while (!out_.empty() && (out_.back() == ' ' || out_.back() == '\n')) out_.pop_back();
        return out_ + "\n";
    }

private:
    struct Cmd { int indent; bool flat; const Doc* d; };

    // Whether `d` printed flat, followed by what `rest` prints up to its next line break, fits in `w`.
    static bool fits(const Doc* d, int indent, const std::vector<Cmd>& rest, int w) {
        std::vector<Cmd> st{ { indent, true, d } };
        std::size_t r = rest.size();
        while (w >= 0) {
            if (st.empty()) {
                if (r == 0) return true;
                st.push_back(rest[--r]);
                continue;
            }
            const Cmd c = st.back();
            st.pop_back();
            switch (c.d->k) {
            case Doc::K::Text:
                if (c.d->multi) return !c.flat ? true : false;
                w -= width_of(c.d->text);
                break;
            case Doc::K::Line:   if (c.flat) w -= 1; else return true; break;
            case Doc::K::Soft:   if (!c.flat) return true; break;
            case Doc::K::Hard:   return true;
            case Doc::K::Fresh:  return true;
            case Doc::K::Start: case Doc::K::Suffix: break;
            case Doc::K::Concat:
                for (auto it = c.d->kids.rbegin(); it != c.d->kids.rend(); ++it) st.push_back({ c.indent, c.flat, it->get() });
                break;
            case Doc::K::Nest:   st.push_back({ c.indent + INDENT, c.flat, c.d->kids[0].get() }); break;
            case Doc::K::Group:  st.push_back({ c.indent, c.flat && !c.d->hard, c.d->kids[0].get() }); break;
            }
        }
        return false;
    }

    void newline(int indent) {
        out_ += suffix_;
        suffix_.clear();
        while (!out_.empty() && out_.back() == ' ') out_.pop_back();
        out_ += '\n';
        out_.append(static_cast<std::size_t>(indent), ' ');
        col_ = indent;
        text_on_line_ = false;
    }

    void put(const std::string& s, int indent) {
        if (!suffix_.empty()) newline(indent);   // a `//` comment ended the line
        if (s == " " && !text_on_line_) return;
        out_ += s;
        const std::size_t nl = s.rfind('\n');
        col_ = nl == std::string::npos ? col_ + width_of(s) : width_of(s.substr(nl + 1));
        text_on_line_ = true;
    }

    std::string out_;
    std::string suffix_;
    int col_ = 0;
    bool text_on_line_ = false;
};

// ---- the elements -------------------------------------------------------------------

struct Note {
    std::string text;
    int  blank_before = 0;   // blank lines before it (at most one kept)
};

// One printed token: its source spelling, or a whole interpolated string. The comments around it:
// `lead` on their own lines before it, `trail` after it on its line.
struct El {
    TokKind     kind = TokKind::Eof;
    std::string spell;
    uint32_t    begin = 0, end = 0;
    std::vector<Note> lead;
    int lead_gap = 0;            // blank lines between the last lead comment and the element
    std::string trail;           // " /* a */" pieces
    std::string trail_line;      // a trailing `//` comment, if any
    int blank_before = 0;        // blank lines before it (or before its first lead comment)
};

bool is_open(TokKind k)  { return k == TokKind::LParen || k == TokKind::LBracket || k == TokKind::LBrace; }
bool is_close(TokKind k) { return k == TokKind::RParen || k == TokKind::RBracket || k == TokKind::RBrace; }
bool is_name(TokKind k)  { return k == TokKind::LIdent || k == TokKind::UIdent; }

// Ends a value: an operator after it is binary.
bool ends_value(TokKind k) {
    switch (k) {
    case TokKind::Int: case TokKind::Double: case TokKind::Str: case TokKind::UIdent: case TokKind::LIdent:
    case TokKind::Underscore: case TokKind::RParen: case TokKind::RBracket: case TokKind::RBrace:
    case TokKind::KwTrue: case TokKind::KwFalse: case TokKind::KwSelf: case TokKind::Question:
    case TokKind::InterpStrEnd:
        return true;
    default:
        return false;
    }
}

// Binary operators an operator chain breaks before, by precedence (lowest first); -1 = none.
int chain_level(TokKind k) {
    switch (k) {
    case TokKind::Pipe:   return 0;
    case TokKind::OrOr:   return 1;
    case TokKind::AndAnd: return 2;
    case TokKind::EqEq: case TokKind::NotEq: case TokKind::Lt: case TokKind::Le: case TokKind::Gt: case TokKind::Ge:
        return 3;
    case TokKind::BitOr:  return 4;
    case TokKind::BitXor: return 5;
    case TokKind::BitAnd: return 6;
    case TokKind::Shl: case TokKind::Shr: case TokKind::UShr: return 7;
    case TokKind::Plus: case TokKind::Minus: return 8;
    case TokKind::Star: case TokKind::Slash: case TokKind::Percent: return 9;
    default: return -1;
    }
}

// An assignment-level separator a statement or arm is split at: `=`, `op=`, `=>`.
bool is_assign(TokKind k) {
    switch (k) {
    case TokKind::Assign: case TokKind::FatArrow:
    case TokKind::PlusEq: case TokKind::MinusEq: case TokKind::StarEq: case TokKind::SlashEq: case TokKind::PercentEq:
    case TokKind::BitAndEq: case TokKind::BitOrEq: case TokKind::BitXorEq:
    case TokKind::ShlEq: case TokKind::ShrEq: case TokKind::UShrEq:
        return true;
    default:
        return false;
    }
}

using Brace = svc::ParseLayout::Brace;

class Formatter {
public:
    Formatter(const std::string& src, const std::vector<svc::Token>& toks, const std::vector<svc::Comment>& comments,
              const svc::ParseLayout& layout)
        : src_(src) {
        build_elements(toks, comments, layout);
    }

    std::string run() { return Printer().run(top()); }

private:
    // ---- elements from tokens + comments ------------------------------------------------

    void build_elements(const std::vector<svc::Token>& toks, const std::vector<svc::Comment>& comments,
                        const svc::ParseLayout& layout) {
        std::vector<std::size_t> el_of(toks.size(), SIZE_MAX);
        for (std::size_t i = 0; i < toks.size(); ++i) {
            const svc::Token& t = toks[i];
            if (t.kind == TokKind::Eof) break;
            if (t.kind == TokKind::StmtEnd && t.begin == t.end) continue;   // a line break's statement end
            El e;
            e.kind = t.kind; e.begin = t.begin; e.end = t.end;
            el_of[i] = els_.size();
            if (t.kind == TokKind::InterpStrBegin) {   // the whole interpolated string, verbatim
                int depth = 0;
                std::size_t j = i;
                for (; j < toks.size(); ++j) {
                    if (toks[j].kind == TokKind::InterpStrBegin) ++depth;
                    else if (toks[j].kind == TokKind::InterpStrEnd && --depth == 0) break;
                }
                e.kind = TokKind::Str;
                e.end = toks[j].end;
                i = j;
            }
            e.spell = src_.substr(e.begin, e.end - e.begin);
            els_.push_back(std::move(e));
        }
        // the comments: trailing when on the line of the element before them, else leading
        uint32_t prev_end = 0;   // end of the element or comment before
        std::vector<Note> pending;
        auto blanks = [&](uint32_t from, uint32_t to) {
            int nl = 0;
            for (uint32_t p = from; p < to && p < src_.size(); ++p) if (src_[p] == '\n') ++nl;
            return nl;
        };
        auto trimmed = [](std::string s) {
            while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
            return s;
        };
        std::size_t ci = 0;
        for (std::size_t e = 0; e <= els_.size(); ++e) {
            const uint32_t at = e < els_.size() ? els_[e].begin : static_cast<uint32_t>(src_.size());
            for (; ci < comments.size() && comments[ci].begin < at; ++ci) {
                const svc::Comment& c = comments[ci];
                if (e > 0 && c.begin < els_[e - 1].end) continue;   // inside an interpolated string
                const std::string txt = trimmed(src_.substr(c.begin, c.end - c.begin));
                const int nl = blanks(prev_end, c.begin);
                if (e > 0 && nl == 0 && pending.empty()) {   // on the line of the element before
                    if (txt.rfind("//", 0) == 0) els_[e - 1].trail_line = txt;
                    else els_[e - 1].trail += " " + txt;
                } else {
                    pending.push_back(Note{ txt, e == 0 && pending.empty() ? 0 : std::min(1, std::max(0, nl - 1)) });
                }
                prev_end = c.end;
            }
            const int nl = e == 0 && pending.empty() ? 0 : blanks(prev_end, at);
            if (e < els_.size()) {
                El& el = els_[e];
                el.lead = std::move(pending);
                pending.clear();
                if (!el.lead.empty()) {
                    el.blank_before = el.lead.front().blank_before;
                    el.lead.front().blank_before = 0;
                    // the blank lines between the last comment and the element
                    el.lead_gap = std::min(1, std::max(0, nl - 1));
                } else {
                    el.blank_before = std::min(1, std::max(0, nl - 1));
                }
                prev_end = el.end;
            } else {
                tail_ = std::move(pending);
                tail_gap_ = std::min(1, std::max(0, nl - 1));
            }
        }
        // the parser's layout, by element
        for (std::size_t s : layout.starts)
            if (s < el_of.size() && el_of[s] != SIZE_MAX) starts_.insert(el_of[s]);
        for (const auto& [t, b] : layout.braces)
            if (t < el_of.size() && el_of[t] != SIZE_MAX) braces_[el_of[t]] = b;
        // bracket pairs
        match_.assign(els_.size(), SIZE_MAX);
        std::vector<std::size_t> open;
        for (std::size_t i = 0; i < els_.size(); ++i) {
            if (is_open(els_[i].kind)) open.push_back(i);
            else if (is_close(els_[i].kind) && !open.empty()) { match_[open.back()] = i; match_[i] = open.back(); open.pop_back(); }
        }
    }

    // ---- small helpers -------------------------------------------------------------------

    TokKind kind(std::size_t i) const { return i < els_.size() ? els_[i].kind : TokKind::Eof; }

    // The element after the unit at `i`: a bracket group is one unit.
    std::size_t skip(std::size_t i) const {
        return is_open(kind(i)) && match_[i] != SIZE_MAX ? match_[i] + 1 : i + 1;
    }

    Brace brace(std::size_t i) const {
        const auto it = braces_.find(i);
        return it == braces_.end() ? static_cast<Brace>(255) : it->second;
    }
    bool is_block(std::size_t i) const { return kind(i) == TokKind::LBrace && braces_.count(i) && brace(i) == Brace::Block; }

    // The comments of `e` that come before it (own lines) and the token with its trailing comments.
    DocP lead_of(const El& e) const {
        if (e.lead.empty()) return cat({});
        std::vector<DocP> v{ start() };
        for (std::size_t i = 0; i < e.lead.size(); ++i) {
            if (i > 0 && e.lead[i].blank_before) v.push_back(hard());
            v.push_back(text(e.lead[i].text));
            v.push_back(hard());
        }
        if (e.lead_gap) v.push_back(hard());
        return cat(std::move(v));
    }
    DocP tok(std::size_t i, bool with_lead = true) const {
        const El& e = els_[i];
        std::vector<DocP> v;
        if (with_lead && !e.lead.empty()) v.push_back(lead_of(e));
        v.push_back(text(e.spell));
        if (detached_.count(i)) return cat(std::move(v));
        if (!e.trail.empty()) v.push_back(text(e.trail));
        if (!e.trail_line.empty()) v.push_back(suffix(" " + e.trail_line));
        return cat(std::move(v));
    }
    // A dropped comma's comments still have to be printed.
    DocP comments_of(std::size_t i) const {
        const El& e = els_[i];
        std::vector<DocP> v;
        if (!e.lead.empty()) v.push_back(lead_of(e));
        if (!e.trail.empty()) v.push_back(text(e.trail));
        if (!e.trail_line.empty()) v.push_back(suffix(" " + e.trail_line));
        return cat(std::move(v));
    }

    // Whether a space goes between the units ending at `a` and starting at `b`.
    bool space_between(std::size_t a, std::size_t b) const {
        const TokKind ka = kind(a), kb = kind(b);
        const TokKind before_a = a > 0 ? kind(a - 1) : TokKind::Eof;
        switch (kb) {
        case TokKind::RParen: case TokKind::RBracket: case TokKind::Comma: case TokKind::Dot:
        case TokKind::ColonColon: case TokKind::Question: case TokKind::StmtEnd:
        case TokKind::DotDot: case TokKind::DotDotLess:
            return false;
        case TokKind::Colon:   // `name: Type`; `enum E : Int` keeps both spaces
            return is_enum_repr_colon(b);
        default: break;
        }
        switch (ka) {
        case TokKind::LParen: case TokKind::LBracket: case TokKind::Dot: case TokKind::ColonColon:
        case TokKind::Hash: case TokKind::DotDot: case TokKind::DotDotLess: case TokKind::Not:
            return false;
        case TokKind::Minus:   // unary minus
            if (!(a > 0 && ends_value(before_a))) return false;
            break;
        case TokKind::KwFn:    // `fn(` lambda / fn type
            if (kb == TokKind::LParen) return false;
            break;
        case TokKind::KwImpl:  // `impl[T]`
            if (kb == TokKind::LBracket) return false;
            break;
        default: break;
        }
        if (kb == TokKind::LParen || kb == TokKind::LBracket)   // a call, an index, generic arguments
            return !(is_name(ka) || ka == TokKind::RParen || ka == TokKind::RBracket || ka == TokKind::KwSelf ||
                     ka == TokKind::Str || ka == TokKind::Underscore);
        if (kb == TokKind::LBrace && (ka == TokKind::Hash || ka == TokKind::ColonColon)) return false;
        return true;
    }

    bool is_enum_repr_colon(std::size_t c) const {
        std::size_t j = c;
        if (j > 0 && kind(j - 1) == TokKind::RBracket && match_[j - 1] != SIZE_MAX) j = match_[j - 1];
        return j >= 2 && kind(j - 1) == TokKind::UIdent && kind(j - 2) == TokKind::KwEnum;
    }

    // ---- constructs: `if`, `match`, `while`, `for`, `loop`, `fn` ---------------------------

    // The `{` that opens the body of the construct at `kw` (Block, or Match for `match`), searched at
    // depth 0 before `hi`; SIZE_MAX when there is none.
    std::size_t body_of(std::size_t kw, std::size_t hi) const {
        const TokKind k = kind(kw);
        if (k == TokKind::KwFn) return fn_body(kw, hi);
        for (std::size_t j = kw + 1; j < hi; j = skip(j)) {
            if (kind(j) != TokKind::LBrace) continue;
            if (k == TokKind::KwMatch ? brace(j) == Brace::Match : is_block(j)) return j;
            return SIZE_MAX;
        }
        return SIZE_MAX;
    }

    // `fn name(..) -> T {` / `fn(..) -> T {`: the body's `{`, or SIZE_MAX (a fn type, a required
    // trait method). A return type is names, `::`, `dyn`, `fn`, `->` and bracket groups.
    std::size_t fn_body(std::size_t kw, std::size_t hi) const {
        std::size_t j = kw + 1;
        if (is_name(kind(j))) ++j;
        if (kind(j) == TokKind::LBracket) j = skip(j);
        if (kind(j) != TokKind::LParen) return SIZE_MAX;
        j = skip(j);
        if (kind(j) == TokKind::Arrow) {
            ++j;
            while (j < hi) {
                const TokKind k = kind(j);
                if (is_name(k) || k == TokKind::ColonColon || k == TokKind::KwDyn || k == TokKind::KwFn ||
                    k == TokKind::Arrow || k == TokKind::LParen || k == TokKind::LBracket)
                    j = skip(j);
                else break;
            }
        }
        return j < hi && is_block(j) ? j : SIZE_MAX;
    }

    bool is_construct(std::size_t i, std::size_t hi) const {
        switch (kind(i)) {
        case TokKind::KwIf: case TokKind::KwMatch: case TokKind::KwWhile: case TokKind::KwFor:
        case TokKind::KwLoop: case TokKind::KwFn:
            return body_of(i, hi) != SIZE_MAX;
        default:
            return false;
        }
    }

    // The element after the whole construct at `i` (an `if` with its `else` chain).
    std::size_t construct_end(std::size_t i, std::size_t hi) const {
        std::size_t j = skip(body_of(i, hi));
        if (kind(i) == TokKind::KwIf)
            while (kind(j) == TokKind::KwElse && j + 1 < hi) {
                if (kind(j + 1) == TokKind::KwIf && is_construct(j + 1, hi)) return construct_end(j + 1, hi);
                if (!is_block(j + 1)) break;
                j = skip(j + 1);
            }
        return j;
    }

    // The element after the unit at `i`: a bracket group or a whole construct is one unit.
    std::size_t unit_end(std::size_t i, std::size_t hi) const {
        return is_construct(i, hi) ? construct_end(i, hi) : skip(i);
    }

    DocP construct(std::size_t i, std::size_t hi) const {
        const std::size_t body = body_of(i, hi);
        const bool statement = starts_.count(i) > 0;
        switch (kind(i)) {
        case TokKind::KwIf: {
            if (statement) return if_chain(i, hi, false);
            return group(if_chain(i, hi, true));
        }
        case TokKind::KwMatch:
            return cat({ tok(i), text(" "), seq(i + 1, body), text(" "), match_body(body) });
        case TokKind::KwFn: {
            const bool lambda = kind(i + 1) == TokKind::LParen;
            DocP head = plain(i, body);
            if (!lambda) return cat({ head, text(" "), block(body, false) });
            return group(cat({ head, text(" "), block(body, true) }));
        }
        default:   // while / for / loop
            return cat({ plain_or_seq(i, body), text(" "), block(body, false) });
        }
    }

    DocP plain_or_seq(std::size_t lo, std::size_t hi) const {
        // `while x > 0` / `for x in xs`: the keyword, then the header as an expression
        if (hi == lo + 1) return tok(lo);
        return cat({ tok(lo), text(" "), seq(lo + 1, hi) });
    }

    // `if c { a } else if d { b } else { c }`. Inline: the blocks' breaks belong to one group.
    DocP if_chain(std::size_t i, std::size_t hi, bool inline_ok) const {
        std::vector<DocP> v;
        std::size_t kw = i;
        for (;;) {
            const std::size_t body = body_of(kw, hi);
            v.push_back(tok(kw));
            v.push_back(text(" "));
            v.push_back(seq(kw + 1, body));
            v.push_back(text(" "));
            v.push_back(block(body, inline_ok, /*own_group=*/false));
            std::size_t j = skip(body);
            if (kind(j) != TokKind::KwElse || j + 1 >= hi) break;
            v.push_back(text(" "));
            v.push_back(tok(j));
            v.push_back(text(" "));
            if (kind(j + 1) == TokKind::KwIf && is_construct(j + 1, hi)) { kw = j + 1; continue; }
            if (!is_block(j + 1)) break;
            v.push_back(block(j + 1, inline_ok, false));
            break;
        }
        return cat(std::move(v));
    }

    // ---- braces ----------------------------------------------------------------------------

    // The statements (or arms, members) of the body at `open`, split at the parser's starts.
    std::vector<std::pair<std::size_t, std::size_t>> parts(std::size_t open) const {
        std::vector<std::pair<std::size_t, std::size_t>> out;
        const std::size_t close = match_[open];
        std::size_t cur = SIZE_MAX;
        for (std::size_t j = open + 1; j < close; j = skip(j)) {
            if (starts_.count(j)) {
                if (cur != SIZE_MAX) out.emplace_back(cur, j);
                cur = j;
            } else if (cur == SIZE_MAX) {
                cur = j;
            }
        }
        if (cur != SIZE_MAX) out.emplace_back(cur, close);
        return out;
    }

    // The elements of the comma list in (open, close), with any trailing comma left out; `commas`
    // gets the index of the comma after each element (SIZE_MAX for none).
    std::vector<std::pair<std::size_t, std::size_t>> elements(std::size_t open, std::vector<std::size_t>& commas) const {
        std::vector<std::pair<std::size_t, std::size_t>> out;
        const std::size_t close = match_[open];
        std::size_t from = open + 1;
        for (std::size_t j = open + 1; j < close; j = skip(j))
            if (kind(j) == TokKind::Comma) {
                out.emplace_back(from, j);
                commas.push_back(j);
                from = j + 1;
            }
        if (from < close) { out.emplace_back(from, close); commas.push_back(SIZE_MAX); }
        return out;
    }

    // Blank lines before a part: as written, at most one.
    DocP gap(std::size_t first) const { return els_[first].blank_before ? hard() : cat({}); }

    // The comments before the closer of a body, on their own lines.
    DocP closer_lead(std::size_t close) const {
        const El& e = els_[close];
        if (e.lead.empty()) return cat({});
        return cat({ hard(), lead_of_inner(e) });
    }
    DocP lead_of_inner(const El& e) const {
        std::vector<DocP> v;
        for (std::size_t i = 0; i < e.lead.size(); ++i) {
            if (i > 0) v.push_back(hard());
            if (i > 0 && e.lead[i].blank_before) v.push_back(hard());
            v.push_back(text(e.lead[i].text));
        }
        return cat(std::move(v));
    }
    // A closer, and the comments after it on its line -- outside a group, which they must not break.
    DocP closer(std::size_t close) const { return text(els_[close].spell); }
    DocP after(std::size_t close) const {
        if (detached_.count(close)) return cat({});   // the statement it ends prints them
        const El& e = els_[close];
        std::vector<DocP> v;
        if (!e.trail.empty()) v.push_back(text(e.trail));
        if (!e.trail_line.empty()) v.push_back(suffix(" " + e.trail_line));
        return cat(std::move(v));
    }

    // A block. Inline (`{ x }` when it fits) only where allowed and for one statement that is one line.
    DocP block(std::size_t open, bool inline_ok, bool own_group = true) const {
        const std::size_t close = match_[open];
        const auto ps = parts(open);
        if (ps.empty() && els_[close].lead.empty()) return cat({ tok(open), closer(close), after(close) });
        if (inline_ok && ps.size() == 1 && els_[close].lead.empty() && els_[ps[0].first].lead.empty() &&
            els_[open].trail_line.empty()) {
            DocP body = statement(ps[0].first, ps[0].second);
            if (!body->multi) {
                DocP d = cat({ tok(open), nest(cat({ line(), body })), line(), closer(close) });
                return cat({ own_group ? group(d) : d, after(close) });
            }
        }
        std::vector<DocP> v;
        for (std::size_t p = 0; p < ps.size(); ++p) {
            v.push_back(hard());
            if (p > 0) v.push_back(gap(ps[p].first));
            v.push_back(statement(ps[p].first, ps[p].second));
        }
        v.push_back(closer_lead(close));
        return cat({ tok(open), fresh(cat({ nest(cat(std::move(v))), hard() })), closer(close), after(close) });
    }

    DocP match_body(std::size_t open) const {
        const std::size_t close = match_[open];
        const auto ps = parts(open);
        std::vector<DocP> v;
        for (std::size_t p = 0; p < ps.size(); ++p) {
            std::size_t lo = ps[p].first, hi = ps[p].second;
            DocP dropped = cat({});
            if (hi > lo && kind(hi - 1) == TokKind::Comma) { dropped = comments_of(hi - 1); --hi; }
            v.push_back(hard());
            if (p > 0) v.push_back(gap(lo));
            v.push_back(statement(lo, hi));
            if (p + 1 < ps.size()) v.push_back(text(","));
            v.push_back(dropped);
        }
        v.push_back(closer_lead(close));
        return cat({ tok(open), fresh(cat({ nest(cat(std::move(v))), hard() })), closer(close), after(close) });
    }

    DocP members(std::size_t open) const {
        const std::size_t close = match_[open];
        const auto ps = parts(open);
        if (ps.empty() && els_[close].lead.empty()) return cat({ tok(open), closer(close), after(close) });
        std::vector<DocP> v;
        for (std::size_t p = 0; p < ps.size(); ++p) {
            v.push_back(hard());
            if (p > 0) v.push_back(gap(ps[p].first));
            v.push_back(statement(ps[p].first, ps[p].second));
        }
        v.push_back(closer_lead(close));
        return cat({ tok(open), fresh(cat({ nest(cat(std::move(v))), hard() })), closer(close), after(close) });
    }

    // Struct fields / enum variants: one per line, a comma after each but the last.
    DocP fields(std::size_t open) const {
        const std::size_t close = match_[open];
        std::vector<std::size_t> commas;
        const auto es = elements(open, commas);
        if (es.empty() && els_[close].lead.empty()) return cat({ tok(open), closer(close), after(close) });
        std::vector<DocP> v;
        for (std::size_t p = 0; p < es.size(); ++p) {
            v.push_back(hard());
            if (p > 0 && es[p].first < es[p].second) v.push_back(gap(es[p].first));
            v.push_back(seq(es[p].first, es[p].second));
            if (commas[p] != SIZE_MAX) {
                if (p + 1 < es.size()) v.push_back(tok(commas[p], false));
                else v.push_back(comments_of(commas[p]));
            }
        }
        v.push_back(closer_lead(close));
        return cat({ tok(open), fresh(cat({ nest(cat(std::move(v))), hard() })), closer(close), after(close) });
    }

    // A comma list in `( )`, `[ ]` or a literal `{ }`: on one line when it fits, else one element
    // per line and the closer alone. `pad`: `{ a, b }` has a space inside.
    DocP list(std::size_t open, bool pad) const {
        const std::size_t close = match_[open];
        std::vector<std::size_t> commas;
        auto es = elements(open, commas);
        // `(x,)` is a one-element tuple: its comma is not a trailing one
        const bool keep_last = kind(open) == TokKind::LParen && es.size() == 1 && commas[0] != SIZE_MAX;
        if (es.empty()) {
            if (els_[close].lead.empty()) return cat({ tok(open), closer(close), after(close) });
            return cat({ tok(open), fresh(cat({ nest(cat({ hard(), lead_of_inner(els_[close]) })), hard() })), closer(close),
                         after(close) });
        }
        // one string: breaking would not make it shorter
        if (es.size() == 1 && es[0].second == es[0].first + 1 && kind(es[0].first) == TokKind::Str &&
            commas[0] == SIZE_MAX && els_[close].lead.empty() && els_[es[0].first].lead.empty())
            return cat({ tok(open), tok(es[0].first), closer(close), after(close) });
        std::vector<DocP> v;
        for (std::size_t p = 0; p < es.size(); ++p) {
            v.push_back(p == 0 ? (pad ? line() : soft()) : line());
            v.push_back(seq(es[p].first, es[p].second));
            if (commas[p] != SIZE_MAX) {
                if (p + 1 < es.size() || keep_last) v.push_back(tok(commas[p], false));
                else v.push_back(comments_of(commas[p]));
            }
        }
        if (!els_[close].lead.empty()) v.push_back(cat({ hard(), lead_of_inner(els_[close]) }));
        return cat({ group(cat({ tok(open), nest(cat(std::move(v))), pad ? line() : soft(), closer(close) })), after(close) });
    }

    DocP bracket(std::size_t open) const {
        if (kind(open) != TokKind::LBrace) return list(open, false);
        const auto it = braces_.find(open);
        if (it == braces_.end()) {   // a literal or pattern; a `use` list has no inner space
            const bool tight = open > 0 && kind(open - 1) == TokKind::ColonColon;
            return list(open, !tight);
        }
        switch (it->second) {
        case Brace::Block:   return block(open, true);
        case Brace::Match:   return match_body(open);
        case Brace::Members: return members(open);
        case Brace::Fields:  return fields(open);
        }
        return list(open, true);
    }

    // ---- statements and expressions ----------------------------------------------------------

    // A statement (or arm, member, top-level item); its first token prints the comments before it.
    // The comments at its end go after it, outside its groups: a comment ending the statement's line
    // must not break the statement.
    DocP statement(std::size_t lo, std::size_t hi) const {
        if (lo >= hi) return cat({});
        const std::size_t last = hi - 1;
        detached_.insert(last);
        DocP body = seq(lo, hi);
        detached_.erase(last);
        return cat({ body, after(last) });
    }

    // An expression / statement fragment: split at a depth-0 assignment (`=`, `op=`, `=>`), else
    // at the lowest-precedence operator chain, else printed unit by unit.
    DocP seq(std::size_t lo, std::size_t hi) const {
        if (lo >= hi) return cat({});
        // assignment
        for (std::size_t j = lo; j < hi; j = unit_end(j, hi))
            if (is_assign(kind(j)) && j > lo)
                return cat({ seq(lo, j), text(" "), tok(j), text(" "), seq(j + 1, hi) });
        // operator chain: the lowest precedence present at depth 0
        int level = 99;
        std::size_t prev = SIZE_MAX;
        for (std::size_t j = lo; j < hi; ) {
            const std::size_t next = unit_end(j, hi);
            const int l = chain_level(kind(j));
            if (l >= 0 && prev != SIZE_MAX && ends_value(kind(prev))) level = std::min(level, l);
            prev = next - 1;
            j = next;
        }
        if (level != 99) {
            std::vector<DocP> rest;
            std::size_t from = lo;
            DocP first;
            prev = SIZE_MAX;
            for (std::size_t j = lo; j < hi; ) {
                const std::size_t next = unit_end(j, hi);
                if (chain_level(kind(j)) == level && prev != SIZE_MAX && ends_value(kind(prev))) {
                    DocP operand = seq(from, j);
                    if (!first) first = operand; else rest.push_back(operand);
                    rest.push_back(line());
                    rest.push_back(tok(j));
                    rest.push_back(text(" "));
                    from = j + 1;
                }
                prev = next - 1;
                j = next;
            }
            rest.push_back(seq(from, hi));
            return group(cat({ first, nest(cat(std::move(rest))) }));
        }
        return plain(lo, hi);
    }

    // Unit by unit: tokens, bracket groups, constructs; a method chain `a.b(..).c(..)` of two or more
    // calls breaks before each `.` when it does not fit.
    DocP plain(std::size_t lo, std::size_t hi) const {
        std::vector<std::size_t> dots;   // `.name(` at depth 0
        for (std::size_t j = lo; j < hi; j = unit_end(j, hi))
            if (kind(j) == TokKind::Dot && j + 2 < hi && is_name(kind(j + 1)) && kind(j + 2) == TokKind::LParen && j > lo)
                dots.push_back(j);
        std::vector<DocP> v;
        std::vector<DocP>* out = &v;
        std::vector<DocP> chained;
        const bool chain = dots.size() >= 2;
        std::size_t last = SIZE_MAX;
        for (std::size_t j = lo; j < hi; ) {
            const std::size_t next = unit_end(j, hi);
            if (last != SIZE_MAX) {
                if (chain && kind(j) == TokKind::Dot && std::find(dots.begin(), dots.end(), j) != dots.end()) {
                    out = &chained;
                    out->push_back(soft());
                } else if (space_between(last, j)) {
                    out->push_back(text(" "));
                }
            }
            DocP unit;
            if (is_construct(j, hi)) unit = construct(j, hi);
            else if (is_open(kind(j)) && match_[j] != SIZE_MAX) unit = bracket(j);
            else unit = tok(j);
            out->push_back(unit);
            last = next - 1;
            j = next;
        }
        if (!chain) return cat(std::move(v));
        return group(cat({ cat(std::move(v)), nest(cat(std::move(chained))) }));
    }

    // ---- the file -------------------------------------------------------------------------

    enum class Unit { Import, Item, Stmt };

    Unit unit_kind(std::size_t i) const {
        switch (kind(i)) {
        case TokKind::KwImport: case TokKind::KwUse: return Unit::Import;
        case TokKind::KwStruct: case TokKind::KwEnum: case TokKind::KwTrait: case TokKind::KwImpl:
        case TokKind::KwConst: case TokKind::KwPub: case TokKind::KwTransparent:
            return Unit::Item;
        case TokKind::KwFn: return kind(i + 1) == TokKind::LIdent ? Unit::Item : Unit::Stmt;
        default: return Unit::Stmt;
        }
    }

    DocP top() const {
        std::vector<std::pair<std::size_t, std::size_t>> units;
        std::size_t cur = SIZE_MAX;
        for (std::size_t j = 0; j < els_.size(); j = skip(j)) {
            if (starts_.count(j) || cur == SIZE_MAX) {
                if (cur != SIZE_MAX) units.emplace_back(cur, j);
                cur = j;
            }
        }
        if (cur != SIZE_MAX) units.emplace_back(cur, els_.size());
        std::vector<DocP> v;
        for (std::size_t u = 0; u < units.size(); ++u) {
            if (u > 0) {
                const Unit a = unit_kind(units[u - 1].first), b = unit_kind(units[u].first);
                int blank;
                if (a == Unit::Import && b == Unit::Import) blank = els_[units[u].first].blank_before;
                else if (a == Unit::Import) blank = 2;
                else if (a == Unit::Item || b == Unit::Item) blank = 1;
                else blank = els_[units[u].first].blank_before;
                v.push_back(hard());
                for (int k = 0; k < blank; ++k) v.push_back(hard());
            }
            v.push_back(statement(units[u].first, units[u].second));
        }
        if (!tail_.empty()) {
            if (!units.empty()) { v.push_back(hard()); if (tail_.front().blank_before || tail_gap_) v.push_back(hard()); }
            for (std::size_t i = 0; i < tail_.size(); ++i) {
                if (i > 0) { v.push_back(hard()); if (tail_[i].blank_before) v.push_back(hard()); }
                v.push_back(text(tail_[i].text));
            }
        }
        return cat(std::move(v));
    }

    const std::string& src_;
    std::vector<El> els_;
    std::vector<std::size_t> match_;
    std::set<std::size_t> starts_;
    std::map<std::size_t, Brace> braces_;
    std::vector<Note> tail_;   // comments after the last token
    mutable std::set<std::size_t> detached_;   // elements whose trailing comments statement() prints
    int tail_gap_ = 0;
};

// ---- the self-check ------------------------------------------------------------------------

struct Parsed {
    std::vector<svc::Token> toks;
    std::vector<svc::Comment> comments;
    svc::ParseLayout layout;
    std::string dump;
};

// Strict lexing and parsing; throws on a syntax error.
Parsed parse(const std::string& src) {
    Parsed p;
    p.toks = svc::Lexer(src).tokenize_with_comments(p.comments);
    svc::Parser parser(p.toks);
    parser.set_layout(&p.layout);
    p.dump = svc::dump(parser.parse_program());
    return p;
}

// The tokens that must survive formatting unchanged: all but statement ends and commas.
std::vector<std::string> spellings(const std::string& src, const Parsed& p) {
    std::vector<std::string> out;
    for (const svc::Token& t : p.toks)
        if (t.kind != TokKind::StmtEnd && t.kind != TokKind::Comma && t.kind != TokKind::Eof)
            out.push_back(src.substr(t.begin, t.end - t.begin));
    return out;
}
std::vector<std::string> comment_texts(const std::string& src, const Parsed& p) {
    std::vector<std::string> out;
    for (const svc::Comment& c : p.comments) {
        std::string s = src.substr(c.begin, c.end - c.begin);
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
        out.push_back(std::move(s));
    }
    return out;
}

} // namespace

FormatResult format_source(const std::string& source) {
    FormatResult r;
    Parsed in;
    try {
        in = parse(source);
    } catch (const std::exception&) {
        r.error = "The file has syntax errors; fix them before formatting.";
        return r;
    }
    std::string out;
    try {
        out = Formatter(source, in.toks, in.comments, in.layout).run();
    } catch (const std::exception& e) {
        r.error = std::string("The formatter failed: ") + e.what();
        return r;
    }
    // Keep the file's line breaks: CRLF when its first one is (a copied raw string keeps its own).
    const std::size_t first_nl = source.find('\n');
    if (first_nl != std::string::npos && first_nl > 0 && source[first_nl - 1] == '\r') {
        std::string crlf;
        crlf.reserve(out.size() + out.size() / 16);
        for (std::size_t i = 0; i < out.size(); ++i) {
            if (out[i] == '\n' && (i == 0 || out[i - 1] != '\r')) crlf += '\r';
            crlf += out[i];
        }
        out = std::move(crlf);
    }
    Parsed check;
    try {
        check = parse(out);
    } catch (const std::exception& e) {
        r.error = std::string("The formatter's result does not parse (") + e.what() + "); nothing was changed.";
        return r;
    }
    if (check.dump != in.dump) {
        r.error = "The formatter would change the program's structure; nothing was changed.";
        return r;
    }
    if (spellings(out, check) != spellings(source, in)) {
        r.error = "The formatter would change a token; nothing was changed.";
        return r;
    }
    if (comment_texts(out, check) != comment_texts(source, in)) {
        r.error = "The formatter would change a comment; nothing was changed.";
        return r;
    }
    r.ok = true;
    r.text = std::move(out);
    return r;
}

} // namespace lsp
