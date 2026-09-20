// =============================================================================
// Server.cpp -- see Server.h for the supported protocol surface.
// =============================================================================

#include "Server.h"

#include "Complete.h"
#include "Format.h"
#include "Query.h"
#include "Transport.h"

#include <exception>
#include <istream>
#include <ostream>
#include <set>
#include <stdexcept>

namespace lsp {

namespace {

// JSON-RPC / LSP error codes.
constexpr int PARSE_ERROR           = -32700;
constexpr int INVALID_REQUEST       = -32600;
constexpr int METHOD_NOT_FOUND      = -32601;
constexpr int INVALID_PARAMS        = -32602;
constexpr int INTERNAL_ERROR        = -32603;
constexpr int SERVER_NOT_INITIALIZED = -32002;
constexpr int REQUEST_FAILED        = -32803;

Json position_json(const Position& p) {
    Json j = Json::object();
    j.set("line", p.line);
    j.set("character", p.character);
    return j;
}

Json range_json(const Range& r) {
    Json j = Json::object();
    j.set("start", position_json(r.start));
    j.set("end", position_json(r.end));
    return j;
}

Json symbol_json(const Symbol& s) {
    Json j = Json::object();
    j.set("name", s.name);
    if (!s.detail.empty()) j.set("detail", s.detail);
    j.set("kind", static_cast<int>(s.kind));
    j.set("range", range_json(s.range));
    j.set("selectionRange", range_json(s.selection));
    if (!s.children.empty()) {
        Json kids = Json::array();
        for (const Symbol& c : s.children) kids.push(symbol_json(c));
        j.set("children", std::move(kids));
    }
    return j;
}

// A `Position` parameter; throws on a missing or malformed field.
Position position_of(const Json& params) {
    const Json& p = params.get("position");
    return Position{ static_cast<int>(p.get("line").as_number()), static_cast<int>(p.get("character").as_number()) };
}

// The URI of a TEXT DOCUMENT parameter, as a normalized path; throws on a missing field so
// the caller's catch-all reports it.
std::string doc_path(const Json& params, std::string* uri_out = nullptr) {
    const std::string& uri = params.get("textDocument").get("uri").as_string();
    const std::string path = uri_to_path(uri);
    if (path.empty()) throw std::logic_error("not a file URI: " + uri);
    if (uri_out) *uri_out = uri;
    return path;
}

} // namespace

Server::Server(std::istream& in, std::ostream& out, std::ostream& log) : in_(in), out_(out), log_(log) {}

int Server::run() {
    for (;;) {
        std::optional<std::string> body;
        try {
            body = read_message(in_);
        } catch (const TransportError& e) {
            log_ << "skarn_lsp: transport error: " << e.what() << "\n";
            return 1;
        }
        if (!body) return shutdown_ ? 0 : 1;   // the client closed the stream

        Json msg;
        try {
            msg = Json::parse(*body);
        } catch (const JsonError& e) {
            send_error(Json(), PARSE_ERROR, e.what());
            continue;
        }
        handle(msg);
        if (exit_) return shutdown_ ? 0 : 1;
    }
}

void Server::handle(const Json& msg) {
    const Json& method = msg.get("method");
    if (!method.is_string()) return;   // a response to a server->client request; we send none
    const Json* id = msg.find("id");
    if (id) handle_request(*id, method.as_string(), msg.get("params"));
    else    handle_notification(method.as_string(), msg.get("params"));
}

void Server::handle_request(const Json& id, const std::string& method, const Json& params) {
    try {
        if (method == "initialize") {
            initialized_ = true;
            Json sync = Json::object();
            sync.set("openClose", true);
            sync.set("change", 1);                          // TextDocumentSyncKind.Full
            Json save = Json::object();
            save.set("includeText", false);
            sync.set("save", std::move(save));
            Json caps = Json::object();
            caps.set("textDocumentSync", std::move(sync));
            caps.set("documentSymbolProvider", true);
            caps.set("hoverProvider", true);
            caps.set("definitionProvider", true);
            caps.set("referencesProvider", true);
            caps.set("documentHighlightProvider", true);
            Json rename_opts = Json::object();
            rename_opts.set("prepareProvider", true);
            caps.set("renameProvider", std::move(rename_opts));
            Json completion_opts = Json::object();
            Json triggers = Json::array();
            triggers.push(".");
            completion_opts.set("triggerCharacters", std::move(triggers));
            caps.set("completionProvider", std::move(completion_opts));
            Json signature_opts = Json::object();
            Json sig_triggers = Json::array();
            sig_triggers.push("(");
            sig_triggers.push(",");
            signature_opts.set("triggerCharacters", std::move(sig_triggers));
            Json sig_retriggers = Json::array();
            sig_retriggers.push(")");
            signature_opts.set("retriggerCharacters", std::move(sig_retriggers));
            caps.set("signatureHelpProvider", std::move(signature_opts));
            caps.set("documentFormattingProvider", true);
            Json info = Json::object();
            info.set("name", "skarn_lsp");
            info.set("version", "0.2.0");
            Json result = Json::object();
            result.set("capabilities", std::move(caps));
            result.set("serverInfo", std::move(info));
            send_result(id, std::move(result));
            return;
        }
        if (!initialized_) {
            send_error(id, SERVER_NOT_INITIALIZED, "the server has not been initialized");
            return;
        }
        if (shutdown_) {
            send_error(id, INVALID_REQUEST, "shutdown has already been requested");
            return;
        }
        if (method == "shutdown") {
            shutdown_ = true;
            send_result(id, Json());
            return;
        }
        if (method == "textDocument/documentSymbol") { send_result(id, document_symbols_json(doc_path(params))); return; }
        if (method == "textDocument/hover")          { send_result(id, hover_json(params)); return; }
        if (method == "textDocument/definition")     { send_result(id, definition_json(params)); return; }
        if (method == "textDocument/references") {
            const std::string path = doc_path(params);
            const bool with_decl = params.get("context").get("includeDeclaration").is_bool() &&
                                   params.get("context").get("includeDeclaration").as_bool();
            Json list = Json::array();
            for (const Location& l : references(entry_analyses(), path, position_of(params), with_decl))
                list.push(location_json(l.path, l.range));
            send_result(id, std::move(list));
            return;
        }
        if (method == "textDocument/documentHighlight") {
            const std::string path = doc_path(params);
            Json list = Json::array();
            if (const AnalysisResult* a = covering(path))
                for (const Highlight& h : highlights(*a, path, position_of(params))) {
                    Json j = Json::object();
                    j.set("range", range_json(h.range));
                    j.set("kind", h.write ? 3 : 2);   // DocumentHighlightKind Write / Read
                    list.push(std::move(j));
                }
            send_result(id, std::move(list));
            return;
        }
        if (method == "textDocument/completion") {
            const std::string path = doc_path(params);
            Json items = Json::array();
            if (const AnalysisResult* a = covering(path))
                for (const CompletionItem& c : complete(*a, path, position_of(params))) {
                    Json j = Json::object();
                    j.set("label", c.label);
                    j.set("kind", static_cast<int>(c.kind));
                    if (!c.detail.empty()) j.set("detail", c.detail);
                    j.set("sortText", c.sort);
                    items.push(std::move(j));
                }
            Json result = Json::object();
            result.set("isIncomplete", false);
            result.set("items", std::move(items));
            send_result(id, std::move(result));
            return;
        }
        if (method == "textDocument/signatureHelp") {
            const std::string path = doc_path(params);
            const AnalysisResult* a = covering(path);
            const std::optional<SignatureHelp> help = a ? signature_help(*a, path, position_of(params)) : std::nullopt;
            if (!help) { send_result(id, Json()); return; }
            Json sigs = Json::array();
            for (const SignatureInfo& s : help->signatures) {
                Json ps = Json::array();
                for (const auto& [start, end] : s.params) {   // offsets into the label (ASCII)
                    Json range = Json::array();
                    range.push(static_cast<int>(start));
                    range.push(static_cast<int>(end));
                    Json p = Json::object();
                    p.set("label", std::move(range));
                    ps.push(std::move(p));
                }
                Json j = Json::object();
                j.set("label", s.label);
                j.set("parameters", std::move(ps));
                sigs.push(std::move(j));
            }
            Json result = Json::object();
            result.set("signatures", std::move(sigs));
            result.set("activeSignature", help->active_signature);
            result.set("activeParameter", help->active_parameter);
            send_result(id, std::move(result));
            return;
        }
        if (method == "textDocument/formatting") {   // one edit replacing the whole document
            const std::string path = doc_path(params);
            const auto it = docs_.find(path);
            if (it == docs_.end()) { send_result(id, Json::array()); return; }
            const std::string& text = it->second;
            const FormatResult f = format_source(text);
            if (!f.ok) { send_error(id, REQUEST_FAILED, f.error); return; }
            Json edits = Json::array();
            if (f.text != text) {
                Range all;
                for (char c : text) {
                    if (c == '\n') { ++all.end.line; all.end.character = 0; }
                    else ++all.end.character;
                }
                Json e = Json::object();
                e.set("range", range_json(all));
                e.set("newText", f.text);
                edits.push(std::move(e));
            }
            send_result(id, std::move(edits));
            return;
        }
        if (method == "textDocument/prepareRename") {
            const PrepareRename p = prepare_rename(entry_analyses(), doc_path(params), position_of(params));
            if (!p.error.empty()) { send_error(id, REQUEST_FAILED, p.error); return; }
            Json result = Json::object();
            result.set("range", range_json(p.range));
            result.set("placeholder", p.placeholder);
            send_result(id, std::move(result));
            return;
        }
        if (method == "textDocument/rename") {
            const RenameResult r = rename(entry_analyses(), doc_path(params), position_of(params),
                                          params.get("newName").as_string());
            if (!r.error.empty()) { send_error(id, REQUEST_FAILED, r.error); return; }
            Json changes = Json::object();
            for (const auto& [path, edits] : r.changes) {
                Json list = Json::array();
                for (const TextEdit& e : edits) {
                    Json j = Json::object();
                    j.set("range", range_json(e.range));
                    j.set("newText", e.new_text);
                    list.push(std::move(j));
                }
                changes.set(uri_of(path), std::move(list));
            }
            Json result = Json::object();
            result.set("changes", std::move(changes));
            send_result(id, std::move(result));
            return;
        }
        send_error(id, METHOD_NOT_FOUND, "unsupported method: " + method);
    } catch (const std::logic_error& e) {
        send_error(id, INVALID_PARAMS, e.what());
    } catch (const std::exception& e) {
        send_error(id, INTERNAL_ERROR, e.what());
    }
}

void Server::handle_notification(const std::string& method, const Json& params) {
    if (method == "exit") { exit_ = true; return; }
    if (!initialized_ || shutdown_) return;
    try {
        if (method == "textDocument/didOpen") {
            std::string uri;
            const std::string path = doc_path(params, &uri);
            open_uri_[path] = uri;
            docs_[path] = params.get("textDocument").get("text").as_string();
            refresh();
        } else if (method == "textDocument/didChange") {
            const std::string path = doc_path(params);
            const Json& changes = params.get("contentChanges");
            if (changes.size() == 0) return;
            // Full sync: every change carries the whole document; the last one is current.
            docs_[path] = changes.at(changes.size() - 1).get("text").as_string();
            refresh();
        } else if (method == "textDocument/didSave") {
            refresh();   // a saved file may be a module that other entries read from disk
        } else if (method == "textDocument/didClose") {
            const std::string path = doc_path(params);
            docs_.erase(path);
            open_uri_.erase(path);
            refresh();
        }
        // initialized, $/cancelRequest, workspace notifications, ...: nothing to do.
    } catch (const std::exception& e) {
        log_ << "skarn_lsp: " << method << ": " << e.what() << "\n";
    }
}

void Server::refresh() {
    std::map<std::string, AnalysisResult> results;
    std::set<std::string> imported;
    for (const auto& [path, text] : docs_) {
        AnalysisResult r = analyze(path, text, docs_);
        imported.insert(r.modules.begin(), r.modules.end());
        results.emplace(path, std::move(r));
    }

    std::vector<std::string> entries;
    for (const auto& [path, r] : results)
        if (!imported.count(path)) entries.push_back(path);
    if (entries.empty())   // every open document imports another (a cycle): check them all
        for (const auto& [path, r] : results) entries.push_back(path);
    analyses_ = std::move(results);
    entries_  = entries;
    const auto& results_ref = analyses_;

    std::map<std::string, std::vector<Diagnostic>> merged;
    for (const std::string& entry : entries)
        for (const auto& [path, diags] : results_ref.at(entry).by_path) {
            std::vector<Diagnostic>& dst = merged[path];
            for (const Diagnostic& d : diags) {
                bool seen = false;
                for (const Diagnostic& e : dst) if (e == d) { seen = true; break; }
                if (!seen) dst.push_back(d);
            }
        }

    std::set<std::string> paths;
    for (const auto& [p, d] : merged) paths.insert(p);
    for (const auto& [p, d] : published_) paths.insert(p);

    for (const std::string& path : paths) {
        static const std::vector<Diagnostic> none;
        const auto mit = merged.find(path);
        const std::vector<Diagnostic>& now = mit != merged.end() ? mit->second : none;
        const auto pit = published_.find(path);
        if (pit != published_.end() ? pit->second == now : now.empty()) continue;

        Json list = Json::array();
        for (const Diagnostic& d : now) list.push(diagnostic_json(d));
        Json params = Json::object();
        params.set("uri", uri_of(path));
        params.set("diagnostics", std::move(list));
        Json msg = Json::object();
        msg.set("jsonrpc", "2.0");
        msg.set("method", "textDocument/publishDiagnostics");
        msg.set("params", std::move(params));
        send(msg);

        if (now.empty()) published_.erase(path);
        else             published_[path] = now;
    }
}

Json Server::diagnostic_json(const Diagnostic& d) const {
    Json j = Json::object();
    j.set("range", range_json(d.range));
    j.set("severity", static_cast<int>(d.severity));
    j.set("source", "skarn");
    j.set("message", d.message);
    if (!d.related.empty()) {
        Json rel = Json::array();
        for (const RelatedInfo& r : d.related) {
            Json loc = Json::object();
            loc.set("uri", uri_of(r.path));
            loc.set("range", range_json(r.range));
            Json item = Json::object();
            item.set("location", std::move(loc));
            item.set("message", r.message);
            rel.push(std::move(item));
        }
        j.set("relatedInformation", std::move(rel));
    }
    return j;
}

std::string Server::uri_of(const std::string& path) const {
    const auto it = open_uri_.find(path);
    return it != open_uri_.end() ? it->second : path_to_uri(path);
}

const AnalysisResult* Server::covering(const std::string& path) const {
    if (const auto it = analyses_.find(path); it != analyses_.end() && it->second.module_of.count(path)) {
        for (const std::string& e : entries_) if (e == path) return &it->second;
    }
    for (const std::string& e : entries_) {
        const auto it = analyses_.find(e);
        if (it != analyses_.end() && it->second.module_of.count(path)) return &it->second;
    }
    return nullptr;
}

Json Server::document_symbols_json(const std::string& path) {
    const AnalysisResult* a = covering(path);
    if (a && a->program) last_outline_[path] = document_symbols(*a, path);
    Json list = Json::array();
    if (const auto it = last_outline_.find(path); it != last_outline_.end())
        for (const Symbol& s : it->second) list.push(symbol_json(s));
    return list;
}

Json Server::hover_json(const Json& params) const {
    const std::string path = doc_path(params);
    const AnalysisResult* a = covering(path);
    if (!a) return Json();
    const std::optional<std::string> text = hover(*a, path, position_of(params));
    if (!text) return Json();
    Json contents = Json::object();
    contents.set("kind", "markdown");
    contents.set("value", *text);
    Json result = Json::object();
    result.set("contents", std::move(contents));
    return result;
}

std::vector<const AnalysisResult*> Server::entry_analyses() const {
    std::vector<const AnalysisResult*> out;
    for (const std::string& e : entries_)
        if (const auto it = analyses_.find(e); it != analyses_.end()) out.push_back(&it->second);
    return out;
}

Json Server::location_json(const std::string& path, const Range& range) const {
    Json j = Json::object();
    j.set("uri", uri_of(path));
    j.set("range", range_json(range));
    return j;
}

Json Server::definition_json(const Json& params) const {
    const std::string path = doc_path(params);
    const AnalysisResult* a = covering(path);
    if (!a) return Json();
    const std::optional<Location> loc = definition(*a, path, position_of(params));
    if (!loc) return Json();
    Json result = Json::object();
    result.set("uri", uri_of(loc->path));
    result.set("range", range_json(loc->range));
    return result;
}

void Server::send(const Json& msg) {
    write_message(out_, msg.dump());
}

void Server::send_result(const Json& id, Json result) {
    Json msg = Json::object();
    msg.set("jsonrpc", "2.0");
    msg.set("id", id);
    msg.set("result", std::move(result));
    send(msg);
}

void Server::send_error(const Json& id, int code, const std::string& message) {
    Json err = Json::object();
    err.set("code", code);
    err.set("message", message);
    Json msg = Json::object();
    msg.set("jsonrpc", "2.0");
    msg.set("id", id);
    msg.set("error", std::move(err));
    send(msg);
}

} // namespace lsp
