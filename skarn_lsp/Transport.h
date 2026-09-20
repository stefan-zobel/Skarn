#pragma once

// =============================================================================
// Transport.h -- the LSP base protocol: Content-Length framed messages.
//
//   Content-Length: 52\r\n
//   \r\n
//   {"jsonrpc":"2.0","method":"initialized","params":{}}
//
// A header block (lines ended by CRLF, a bare LF is tolerated) terminated by an empty
// line, then exactly Content-Length bytes of UTF-8 JSON. Only Content-Length is
// required; any other header (Content-Type) is read and ignored. The streams are
// injected so the self-test can drive a whole session through std::stringstream --
// the real server binds them to stdin/stdout, which main() switches to binary mode
// first (a text-mode stdout on Windows would turn "\r\n" into "\r\r\n" and corrupt the
// length the client relies on).
// =============================================================================

#include <iosfwd>
#include <optional>
#include <stdexcept>
#include <string>

namespace lsp {

// A malformed or truncated frame. The server cannot resynchronise after one (the next
// byte's meaning is unknown), so it stops.
class TransportError : public std::runtime_error {
public:
    explicit TransportError(const std::string& msg) : std::runtime_error(msg) {}
};

// Reads one message body. Returns std::nullopt on a clean end of input BEFORE any header
// byte (the client went away); throws TransportError on anything else that is wrong.
std::optional<std::string> read_message(std::istream& in);

// Writes one framed message and flushes, so the client sees it immediately.
void write_message(std::ostream& out, const std::string& body);

} // namespace lsp
