#pragma once

// =============================================================================
// Server.h -- the language server's message loop.
//
// Supported: initialize / initialized / shutdown / exit, the text-document
// notifications didOpen / didChange (FULL sync) / didSave / didClose, and the requests
// textDocument/documentSymbol (outline), hover, definition, references, documentHighlight,
// prepareRename, rename, completion (triggered by `.` and by typing a name), signatureHelp
// (triggered by `(` and `,`) and formatting.
// Every change re-checks the open documents and publishes textDocument/publishDiagnostics
// for each file whose diagnostics changed -- including an empty list for a file whose
// last problem just went away. The requests read the analyses of that last check. An
// unknown request gets MethodNotFound; an unknown notification is ignored, as the
// protocol requires.
//
// A file with syntax errors still has a tree (the parser drops what it cannot read), so the
// queries answer from it. Only an analysis without a program (a missing module) leaves them
// null or empty, and the outline then answers with the file's last good outline rather than
// an empty one. A refused rename answers the RequestFailed error with the reason, which the
// editor shows.
//
// Which open documents are analyzed as ENTRY programs: every open document that no
// other open document imports. A module is checked through its importer, because it
// resolves its own imports relative to the ENTRY's directory (the static_vmrun rule)
// and would report spurious "cannot resolve module" errors checked on its own. A
// module opened WITHOUT its importer is checked as an entry -- the best available.
//
// Each handler runs under a catch-all, so one malformed message is answered with an
// error (or dropped, for a notification) instead of killing the server.
// =============================================================================

#include "Analyze.h"
#include "Json.h"
#include "Symbols.h"

#include <iosfwd>
#include <map>
#include <string>
#include <vector>

namespace lsp {

class Server {
public:
    // `in`/`out` carry the framed protocol; `log` receives human-readable diagnostics
    // about the server itself (stderr in production, a string stream in the self-test).
    Server(std::istream& in, std::ostream& out, std::ostream& log);

    // Runs until `exit` or the end of input. Returns the process exit code: 0 after an
    // orderly shutdown + exit, 1 otherwise (the protocol's rule for a missing shutdown).
    int run();

private:
    void handle(const Json& msg);
    void handle_request(const Json& id, const std::string& method, const Json& params);
    void handle_notification(const std::string& method, const Json& params);

    void send(const Json& msg);
    void send_result(const Json& id, Json result);
    void send_error(const Json& id, int code, const std::string& message);

    void refresh();   // re-check every open document and publish what changed
    Json diagnostic_json(const Diagnostic& d) const;
    std::string uri_of(const std::string& path) const;

    // The analysis that checked the file at `path`: its own, when it is an entry, else
    // the one of the entry that imports it. Null when no analysis covers it.
    const AnalysisResult* covering(const std::string& path) const;
    Json document_symbols_json(const std::string& path);
    Json hover_json(const Json& params) const;
    Json definition_json(const Json& params) const;
    std::vector<const AnalysisResult*> entry_analyses() const;
    Json location_json(const std::string& path, const Range& range) const;

    std::istream& in_;
    std::ostream& out_;
    std::ostream& log_;

    bool initialized_ = false;
    bool shutdown_    = false;
    bool exit_        = false;

    std::map<std::string, std::string> open_uri_;   // normalized path -> the URI the client used
    Overlay                             docs_;       // normalized path -> current text
    std::map<std::string, std::vector<Diagnostic>> published_;   // normalized path -> last sent
    std::map<std::string, AnalysisResult> analyses_;             // entry path -> its last analysis
    std::vector<std::string> entries_;                           // the entries of the last refresh
    std::map<std::string, std::vector<Symbol>> last_outline_;    // normalized path -> last good outline
};

} // namespace lsp
