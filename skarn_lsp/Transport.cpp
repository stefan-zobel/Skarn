// =============================================================================
// Transport.cpp -- see Transport.h for the framing contract.
// =============================================================================

#include "Transport.h"

#include <cctype>
#include <cstddef>
#include <istream>
#include <ostream>

namespace lsp {

namespace {

constexpr std::size_t MAX_HEADER_LINE = 8 * 1024;          // a header line longer than this is garbage
constexpr std::size_t MAX_BODY        = 64 * 1024 * 1024;  // a sane upper bound for one message

// Reads one header line without its terminator. Returns false on end of input before any
// character was read; sets `partial` when input ended mid-line.
bool read_header_line(std::istream& in, std::string& line, bool& partial) {
    line.clear();
    partial = false;
    for (;;) {
        const int c = in.get();
        if (c == std::char_traits<char>::eof()) {
            partial = !line.empty();
            return !line.empty();
        }
        if (c == '\n') {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            return true;
        }
        line += static_cast<char>(c);
        if (line.size() > MAX_HEADER_LINE) throw TransportError("header line too long");
    }
}

bool iequals(const std::string& a, const char* b) {
    std::size_t i = 0;
    for (; i < a.size() && b[i]; ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return i == a.size() && b[i] == '\0';
}

} // namespace

std::optional<std::string> read_message(std::istream& in) {
    std::string line;
    bool partial = false;
    bool any_header = false;
    std::optional<std::size_t> length;

    for (;;) {
        const bool got = read_header_line(in, line, partial);
        if (!got) {
            if (!any_header) return std::nullopt;                  // clean EOF between messages
            throw TransportError("end of input inside a header block");
        }
        if (partial) throw TransportError("end of input inside a header line");
        if (line.empty()) {
            if (!any_header) continue;                             // stray blank line between frames
            break;                                                 // end of the header block
        }
        any_header = true;
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) throw TransportError("malformed header line: " + line);
        const std::string name = line.substr(0, colon);
        if (iequals(name, "Content-Length")) {
            std::size_t pos = colon + 1;
            while (pos < line.size() && line[pos] == ' ') ++pos;
            if (pos == line.size()) throw TransportError("empty Content-Length");
            std::size_t n = 0;
            for (; pos < line.size(); ++pos) {
                const char c = line[pos];
                if (c < '0' || c > '9') throw TransportError("invalid Content-Length: " + line);
                n = n * 10 + static_cast<std::size_t>(c - '0');
                if (n > MAX_BODY) throw TransportError("Content-Length too large");
            }
            length = n;
        }
        // Any other header (Content-Type) is accepted and ignored.
    }

    if (!length) throw TransportError("header block without Content-Length");
    std::string body(*length, '\0');
    if (*length > 0) {
        in.read(body.data(), static_cast<std::streamsize>(*length));
        if (static_cast<std::size_t>(in.gcount()) != *length)
            throw TransportError("end of input inside a message body");
    }
    return body;
}

void write_message(std::ostream& out, const std::string& body) {
    out << "Content-Length: " << body.size() << "\r\n\r\n" << body;
    out.flush();
}

} // namespace lsp
