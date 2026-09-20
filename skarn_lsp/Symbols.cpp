// =============================================================================
// Symbols.cpp -- see Symbols.h.
// =============================================================================

#include "Symbols.h"

#include "Ast.h"      // svc::Program and the item / type nodes
#include "Naming.h"   // svc::short_name

#include <cctype>

namespace lsp {

namespace {

std::string generics_text(const std::vector<svc::GenericParam>& gs) {
    if (gs.empty()) return {};
    std::string s = "[";
    for (std::size_t i = 0; i < gs.size(); ++i) {
        if (i) s += ", ";
        s += gs[i].name;
        for (std::size_t b = 0; b < gs[i].bounds.size(); ++b) {
            const svc::BoundRef& br = gs[i].bounds[b];
            s += b == 0 ? ": " : " + ";
            s += svc::short_name(br.trait);
            if (!br.args.empty()) {
                s += '[';
                for (std::size_t a = 0; a < br.args.size(); ++a) {
                    if (a) s += ", ";
                    s += render_type(*br.args[a]);
                }
                s += ']';
            }
        }
    }
    return s + "]";
}

std::string params_text(const std::vector<svc::Param>& ps, bool has_self, bool self_mut) {
    std::string s = "(";
    bool first = true;
    if (has_self) { s += self_mut ? "mut self" : "self"; first = false; }
    for (const svc::Param& p : ps) {
        if (!first) s += ", ";
        first = false;
        if (p.is_mut) s += "mut ";
        s += p.name;
        if (p.type) s += ": " + render_type(*p.type);
    }
    return s + ")";
}

std::string fields_text(const std::vector<svc::Field>& fs, bool is_tuple) {
    std::string s;
    if (is_tuple) {
        if (fs.empty()) return s;
        s += '(';
        for (std::size_t i = 0; i < fs.size(); ++i) {
            if (i) s += ", ";
            s += render_type(*fs[i].type);
        }
        return s + ')';
    }
    s += " { ";
    for (std::size_t i = 0; i < fs.size(); ++i) {
        if (i) s += ", ";
        s += fs[i].name + ": " + render_type(*fs[i].type);
    }
    return s + (fs.empty() ? "}" : " }");
}

// A whole-line range for line `line` (1-based).
Range line_range(const std::string& text, uint32_t line) {
    const int l0 = static_cast<int>(line) - 1;
    return Range{ Position{ l0, 0 }, Position{ l0, static_cast<int>(line_text(text, line).size()) } };
}

uint32_t line_count(const std::string& text) {
    uint32_t n = 1;
    for (const char c : text) if (c == '\n') ++n;
    if (!text.empty() && text.back() == '\n') --n;
    return n;
}

} // namespace

std::string drop_module_paths(const std::string& s) {
    auto ident = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
    std::string out;
    for (std::size_t i = 0; i < s.size();) {
        const bool word_start = i == 0 || !ident(s[i - 1]);
        if (word_start && (std::islower(static_cast<unsigned char>(s[i])) || s[i] == '_')) {
            std::size_t j = i;
            while (j < s.size() && ident(s[j])) ++j;
            if (s.compare(j, 2, "::") == 0) { i = j + 2; continue; }   // a module segment
            out.append(s, i, j - i);
            i = j;
            continue;
        }
        out += s[i++];
    }
    return out;
}

std::string render_type(const svc::Type& t) {
    switch (t.kind) {
    case svc::TypeKind::Named: {
        const auto& nt = static_cast<const svc::NamedType&>(t);
        std::string s = svc::short_name(nt.name);
        if (!nt.args.empty()) {
            s += '[';
            for (std::size_t i = 0; i < nt.args.size(); ++i) {
                if (i) s += ", ";
                s += render_type(*nt.args[i]);
            }
            s += ']';
        }
        return s;
    }
    case svc::TypeKind::Fn: {
        const auto& ft = static_cast<const svc::FnType&>(t);
        std::string s = "fn(";
        for (std::size_t i = 0; i < ft.params.size(); ++i) {
            if (i) s += ", ";
            s += render_type(*ft.params[i]);
        }
        s += ")";
        if (ft.ret) s += " -> " + render_type(*ft.ret);
        return s;
    }
    case svc::TypeKind::Tuple: {
        const auto& tt = static_cast<const svc::TupleType&>(t);
        std::string s = "(";
        for (std::size_t i = 0; i < tt.elems.size(); ++i) {
            if (i) s += ", ";
            s += render_type(*tt.elems[i]);
        }
        return s + ")";
    }
    case svc::TypeKind::Dyn: {
        const auto& dt = static_cast<const svc::DynType&>(t);
        std::string s = "dyn " + svc::short_name(dt.trait);
        if (!dt.args.empty()) {
            s += '[';
            for (std::size_t i = 0; i < dt.args.size(); ++i) {
                if (i) s += ", ";
                s += render_type(*dt.args[i]);
            }
            s += ']';
        }
        return s;
    }
    }
    return "?";
}

std::string render_generics(const std::vector<svc::GenericParam>& gs) {
    return generics_text(gs);
}

std::string render_method(const svc::Method& m) {
    std::string s = "fn " + m.name + generics_text(m.generics) + params_text(m.params, m.has_self, m.self_mut);
    if (m.ret) s += " -> " + render_type(*m.ret);
    return s;
}

std::string render_item(const svc::Item& item) {
    switch (item.kind) {
    case svc::ItemKind::Fn: {
        const auto& f = static_cast<const svc::FnItem&>(item);
        std::string s = "fn " + svc::short_name(f.name) + generics_text(f.generics) + params_text(f.params, false, false);
        if (f.ret) s += " -> " + render_type(*f.ret);
        return s;
    }
    case svc::ItemKind::Struct: {
        const auto& st = static_cast<const svc::StructItem&>(item);
        return std::string(st.is_transparent ? "transparent struct " : "struct ") + svc::short_name(st.name) +
               generics_text(st.generics) + fields_text(st.fields, st.is_tuple);
    }
    case svc::ItemKind::Enum: {
        const auto& en = static_cast<const svc::EnumItem&>(item);
        std::string s = "enum " + svc::short_name(en.name) + generics_text(en.generics);
        if (en.is_int_backed) s += " : " + en.repr;
        return s;
    }
    case svc::ItemKind::Trait: {
        const auto& tr = static_cast<const svc::TraitDecl&>(item);
        std::string s = "trait " + svc::short_name(tr.name) + generics_text(tr.generics);
        for (std::size_t i = 0; i < tr.supertraits.size(); ++i)
            s += (i == 0 ? ": " : " + ") + svc::short_name(tr.supertraits[i]);
        return s;
    }
    case svc::ItemKind::Impl: {
        const auto& im = static_cast<const svc::ImplDecl&>(item);
        std::string s = "impl" + generics_text(im.generics) + " ";
        if (!im.is_inherent) {
            s += svc::short_name(im.trait_name);
            if (!im.trait_args.empty()) {
                s += '[';
                for (std::size_t i = 0; i < im.trait_args.size(); ++i) {
                    if (i) s += ", ";
                    s += render_type(*im.trait_args[i]);
                }
                s += ']';
            }
            s += " for ";
        }
        if (im.target) s += render_type(*im.target);
        return s;
    }
    case svc::ItemKind::Const: {
        const auto& c = static_cast<const svc::ConstItem&>(item);
        std::string s = "const " + svc::short_name(c.name);
        if (c.type) s += ": " + render_type(*c.type);
        return s;
    }
    case svc::ItemKind::Stmt: case svc::ItemKind::Import: case svc::ItemKind::Use:
        break;
    }
    return {};
}

std::vector<Symbol> document_symbols(const AnalysisResult& analysis, const std::string& path) {
    std::vector<Symbol> out;
    if (!analysis.program) return out;
    const auto mit = analysis.module_of.find(path);
    const auto tit = analysis.texts.find(path);
    if (mit == analysis.module_of.end() || tit == analysis.texts.end()) return out;
    const std::string& module = mit->second;
    const std::string& text = tit->second;

    std::vector<const svc::Item*> items;   // this file's items, in source order
    const svc::Program& prog = *analysis.program;
    for (std::size_t i = prog.prelude_item_count; i < prog.items.size(); ++i)
        if (prog.items[i]->module_prefix == module) items.push_back(prog.items[i].get());

    const uint32_t last_line = line_count(text);
    for (std::size_t i = 0; i < items.size(); ++i) {
        const svc::Item& it = *items[i];
        Symbol s;
        switch (it.kind) {
        case svc::ItemKind::Fn:     s.kind = SymbolKind::Function;  break;
        case svc::ItemKind::Struct: s.kind = SymbolKind::Struct;    break;
        case svc::ItemKind::Enum:   s.kind = SymbolKind::Enum;      break;
        case svc::ItemKind::Trait:  s.kind = SymbolKind::Interface; break;
        case svc::ItemKind::Impl:   s.kind = SymbolKind::Namespace; break;
        case svc::ItemKind::Const:  s.kind = SymbolKind::Constant;  break;
        case svc::ItemKind::Stmt: case svc::ItemKind::Import: case svc::ItemKind::Use:
            continue;
        }
        // From the declaration's first line to the line before whatever item comes next.
        uint32_t end_line = last_line;
        if (i + 1 < items.size() && items[i + 1]->line > it.line) end_line = items[i + 1]->line - 1;
        while (end_line > it.line && line_text(text, end_line).find_first_not_of(" \t") == std::string::npos)
            --end_line;   // trailing blank lines belong to no one
        s.range = Range{ Position{ static_cast<int>(it.line) - 1, 0 }, line_range(text, end_line).end };

        if (it.kind == svc::ItemKind::Impl) {
            s.name      = render_item(it);
            s.selection = ident_range(text, it.line, it.col);   // the `impl` keyword
        } else {
            s.name      = [&] {
                switch (it.kind) {
                case svc::ItemKind::Fn:     return svc::short_name(static_cast<const svc::FnItem&>(it).name);
                case svc::ItemKind::Struct: return svc::short_name(static_cast<const svc::StructItem&>(it).name);
                case svc::ItemKind::Enum:   return svc::short_name(static_cast<const svc::EnumItem&>(it).name);
                case svc::ItemKind::Trait:  return svc::short_name(static_cast<const svc::TraitDecl&>(it).name);
                default:                    return svc::short_name(static_cast<const svc::ConstItem&>(it).name);
                }
            }();
            s.detail    = render_item(it);
            s.selection = ident_range(text, it.name_line, it.name_col);
        }

        auto add_methods = [&](const std::vector<svc::Method>& ms) {
            for (const svc::Method& m : ms) {
                Symbol c;
                c.name      = m.name;
                c.detail    = render_method(m);
                c.kind      = SymbolKind::Method;
                c.range     = line_range(text, m.line);
                c.selection = ident_range(text, m.line, m.col);
                s.children.push_back(std::move(c));
            }
        };
        if (it.kind == svc::ItemKind::Enum) {
            for (const svc::EnumVariant& v : static_cast<const svc::EnumItem&>(it).variants) {
                Symbol c;
                c.name      = svc::short_name(v.name);
                c.kind      = SymbolKind::EnumMember;
                c.range     = line_range(text, v.line);
                c.selection = ident_range(text, v.line, v.col);
                s.children.push_back(std::move(c));
            }
        } else if (it.kind == svc::ItemKind::Trait) {
            add_methods(static_cast<const svc::TraitDecl&>(it).methods);
        } else if (it.kind == svc::ItemKind::Impl) {
            add_methods(static_cast<const svc::ImplDecl&>(it).methods);
        }
        out.push_back(std::move(s));
    }
    return out;
}

} // namespace lsp
