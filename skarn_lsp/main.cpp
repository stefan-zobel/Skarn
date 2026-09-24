// =============================================================================
// skarn_lsp -- a Language Server Protocol server for Skarn.
//
//   skarn_lsp [--stdio]        serve the protocol on stdin/stdout (what an editor starts)
//   skarn_lsp --selftest       run the built-in test suite and exit
//   skarn_lsp --format <file>  print the file in Skarn's fixed format (Format.h) to stdout
//
// Type errors, parse errors and the checker's warnings appear in the editor while the
// file is edited, with the same wording skarnvm prints. Nothing is compiled to
// bytecode or run: an analysis is the front half of the compiler only.
//
// stdout belongs to the protocol. Two things keep it clean: on Windows both standard
// streams are switched to binary mode (text mode would expand "\n" inside a frame and
// break its Content-Length), and std::cout is redirected to stderr after the protocol
// stream has taken over stdout's buffer, so a stray print anywhere in the front end
// lands in the log instead of in the middle of a message.
//
// Arguments an editor client adds on its own (`--stdio`, `--clientProcessId=N`) are
// accepted and ignored.
//
// Exit codes: 0 after an orderly shutdown + exit (or a green self-test), 1 otherwise,
// 2 on bad usage.
// =============================================================================

#include "Format.h"
#include "SelfTest.h"
#include "Server.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace {

void usage(std::ostream& out) {
    out << "usage: skarn_lsp [--stdio]        serve the Language Server Protocol on stdin/stdout\n"
           "       skarn_lsp --selftest       run the self-test suite\n"
           "       skarn_lsp --format <file>  print the file formatted to stdout\n";
}

// --format: exit 0 with the formatted text on stdout, 1 with the reason on stderr.
int format_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) { std::cerr << "skarn_lsp: cannot read '" << path << "'\n"; return 1; }
    std::ostringstream text;
    text << in.rdbuf();
    std::string src = text.str();
    if (src.rfind("\xEF\xBB\xBF", 0) == 0) src.erase(0, 3);   // a UTF-8 BOM
    const lsp::FormatResult r = lsp::format_source(src);
    if (!r.ok) { std::cerr << "skarn_lsp: " << path << ": " << r.error << "\n"; return 1; }
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    std::cout << r.text;
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--selftest") return lsp::run_selftest(std::cout);
        if (arg == "--format") {
            if (i + 1 >= argc) { usage(std::cerr); return 2; }
            return format_file(argv[i + 1]);
        }
        if (arg == "--help" || arg == "-h") { usage(std::cout); return 0; }
        if (arg == "--stdio" || arg.rfind("--clientProcessId", 0) == 0) continue;
        std::cerr << "skarn_lsp: unknown argument '" << arg << "'\n";
        usage(std::cerr);
        return 2;
    }

#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    std::ostream protocol(std::cout.rdbuf());   // the real stdout, for framed messages only
    std::cout.rdbuf(std::cerr.rdbuf());         // anything else printed goes to the log

    lsp::Server server(std::cin, protocol, std::cerr);
    return server.run();
}
