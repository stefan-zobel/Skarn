// =============================================================================
// Json.cpp -- see Json.h for the contract.
//
// Number formatting avoids floating-point std::to_chars / from_chars on purpose: libc++
// ships neither, and this file must build wherever the front end does. An integral value
// in the exactly-representable range prints through the INTEGER to_chars (which every
// library has); anything else goes through "%.17g", which round-trips a double. Parsing
// validates the JSON number grammar by hand and then hands the span to strtod.
// =============================================================================

#include "Json.h"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace lsp {

namespace {

const Json& null_json() {
    static const Json n;
    return n;
}

[[noreturn]] void kind_error(const char* want) {
    throw std::logic_error(std::string("JSON value is not ") + want);
}

void append_utf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

void dump_string(std::string& out, const std::string& s) {
    static const char* HEX = "0123456789abcdef";
    out += '"';
    for (const char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    out += "\\u00";
                    out += HEX[c >> 4];
                    out += HEX[c & 0xF];
                } else {
                    out += ch;   // UTF-8 bytes pass through unchanged
                }
        }
    }
    out += '"';
}

void dump_number(std::string& out, double d) {
    if (!std::isfinite(d)) { out += "null"; return; }   // JSON has no NaN / infinity
    constexpr double EXACT = 9007199254740992.0;        // 2^53
    if (d == std::floor(d) && std::fabs(d) < EXACT) {
        char buf[32];
        const auto res = std::to_chars(buf, buf + sizeof(buf), static_cast<int64_t>(d));
        out.append(buf, res.ptr);
        return;
    }
    char buf[40];
    const int n = std::snprintf(buf, sizeof(buf), "%.17g", d);
    out.append(buf, static_cast<std::size_t>(n > 0 ? n : 0));
}

class Parser {
public:
    explicit Parser(std::string_view text) : s_(text) {}

    Json parse_document() {
        Json v = parse_value(0);
        skip_ws();
        if (pos_ != s_.size()) fail("trailing characters after the JSON value");
        return v;
    }

private:
    static constexpr int MAX_DEPTH = 256;   // nesting cap: a hostile message cannot blow the stack

    [[noreturn]] void fail(const std::string& msg) const { throw JsonError(msg, pos_); }

    bool at_end() const noexcept { return pos_ >= s_.size(); }
    char peek() const noexcept { return at_end() ? '\0' : s_[pos_]; }

    void skip_ws() {
        while (!at_end()) {
            const char c = s_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++pos_;
            else break;
        }
    }

    void expect_word(std::string_view w) {
        if (s_.substr(pos_, w.size()) != w) fail("invalid literal");
        pos_ += w.size();
    }

    Json parse_value(int depth) {
        if (depth > MAX_DEPTH) fail("nesting too deep");
        skip_ws();
        if (at_end()) fail("unexpected end of input");
        switch (s_[pos_]) {
            case '{': return parse_object(depth);
            case '[': return parse_array(depth);
            case '"': return Json(parse_string());
            case 't': expect_word("true");  return Json(true);
            case 'f': expect_word("false"); return Json(false);
            case 'n': expect_word("null");  return Json();
            default:  return Json(parse_number());
        }
    }

    Json parse_object(int depth) {
        ++pos_;   // '{'
        Json obj = Json::object();
        skip_ws();
        if (peek() == '}') { ++pos_; return obj; }
        for (;;) {
            skip_ws();
            if (peek() != '"') fail("expected a member name");
            std::string key = parse_string();
            skip_ws();
            if (peek() != ':') fail("expected ':' after a member name");
            ++pos_;
            obj.set(std::move(key), parse_value(depth + 1));
            skip_ws();
            if (peek() == ',') { ++pos_; continue; }
            if (peek() == '}') { ++pos_; return obj; }
            fail("expected ',' or '}' in an object");
        }
    }

    Json parse_array(int depth) {
        ++pos_;   // '['
        Json arr = Json::array();
        skip_ws();
        if (peek() == ']') { ++pos_; return arr; }
        for (;;) {
            arr.push(parse_value(depth + 1));
            skip_ws();
            if (peek() == ',') { ++pos_; continue; }
            if (peek() == ']') { ++pos_; return arr; }
            fail("expected ',' or ']' in an array");
        }
    }

    uint32_t parse_hex4() {
        if (pos_ + 4 > s_.size()) fail("truncated \\u escape");
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = s_[pos_++];
            v <<= 4;
            if (c >= '0' && c <= '9')      v |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<uint32_t>(c - 'A' + 10);
            else fail("invalid hex digit in a \\u escape");
        }
        return v;
    }

    std::string parse_string() {
        ++pos_;   // opening quote
        std::string out;
        for (;;) {
            if (at_end()) fail("unterminated string");
            const char c = s_[pos_++];
            if (c == '"') return out;
            if (static_cast<unsigned char>(c) < 0x20) fail("control character in a string");
            if (c != '\\') { out += c; continue; }
            if (at_end()) fail("unterminated escape");
            const char e = s_[pos_++];
            switch (e) {
                case '"':  out += '"';  break;
                case '\\': out += '\\'; break;
                case '/':  out += '/';  break;
                case 'b':  out += '\b'; break;
                case 'f':  out += '\f'; break;
                case 'n':  out += '\n'; break;
                case 'r':  out += '\r'; break;
                case 't':  out += '\t'; break;
                case 'u': {
                    uint32_t cp = parse_hex4();
                    if (cp >= 0xD800 && cp <= 0xDBFF) {            // high surrogate: needs a low one
                        if (s_.substr(pos_, 2) != "\\u") fail("unpaired high surrogate");
                        pos_ += 2;
                        const uint32_t lo = parse_hex4();
                        if (lo < 0xDC00 || lo > 0xDFFF) fail("invalid low surrogate");
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        fail("unpaired low surrogate");
                    }
                    append_utf8(out, cp);
                    break;
                }
                default: fail("invalid escape character");
            }
        }
    }

    double parse_number() {
        const std::size_t start = pos_;
        auto digits = [&] {
            const std::size_t d0 = pos_;
            while (!at_end() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
            return pos_ - d0;
        };
        if (peek() == '-') ++pos_;
        if (peek() == '0') ++pos_;
        else if (digits() == 0) fail("invalid value");
        if (peek() == '.') { ++pos_; if (digits() == 0) fail("digit expected after '.'"); }
        if (peek() == 'e' || peek() == 'E') {
            ++pos_;
            if (peek() == '+' || peek() == '-') ++pos_;
            if (digits() == 0) fail("digit expected in the exponent");
        }
        const std::string span(s_.substr(start, pos_ - start));
        return std::strtod(span.c_str(), nullptr);
    }

    std::string_view s_;
    std::size_t      pos_ = 0;
};

} // namespace

bool Json::as_bool() const {
    if (kind_ != Kind::Bool) kind_error("a boolean");
    return bool_;
}

double Json::as_number() const {
    if (kind_ != Kind::Number) kind_error("a number");
    return num_;
}

const std::string& Json::as_string() const {
    if (kind_ != Kind::String) kind_error("a string");
    return str_;
}

std::size_t Json::size() const noexcept {
    return (kind_ == Kind::Array || kind_ == Kind::Object) ? items_.size() : 0;
}

const Json& Json::at(std::size_t i) const {
    if (kind_ != Kind::Array) kind_error("an array");
    if (i >= items_.size()) throw std::logic_error("JSON array index out of range");
    return items_[i];
}

Json& Json::push(Json v) {
    if (kind_ != Kind::Array) kind_error("an array");
    items_.push_back(std::move(v));
    return items_.back();
}

const Json* Json::find(std::string_view key) const noexcept {
    if (kind_ != Kind::Object) return nullptr;
    for (std::size_t i = 0; i < keys_.size(); ++i)
        if (keys_[i] == key) return &items_[i];
    return nullptr;
}

const Json& Json::get(std::string_view key) const noexcept {
    const Json* v = find(key);
    return v ? *v : null_json();
}

Json& Json::set(std::string key, Json v) {
    if (kind_ != Kind::Object) kind_error("an object");
    for (std::size_t i = 0; i < keys_.size(); ++i)
        if (keys_[i] == key) { items_[i] = std::move(v); return items_[i]; }
    keys_.push_back(std::move(key));
    items_.push_back(std::move(v));
    return items_.back();
}

const std::string& Json::key_at(std::size_t i) const {
    if (kind_ != Kind::Object) kind_error("an object");
    return keys_.at(i);
}

const Json& Json::value_at(std::size_t i) const {
    if (kind_ != Kind::Object) kind_error("an object");
    return items_.at(i);
}

void Json::dump_to(std::string& out) const {
    switch (kind_) {
        case Kind::Null:   out += "null"; break;
        case Kind::Bool:   out += bool_ ? "true" : "false"; break;
        case Kind::Number: dump_number(out, num_); break;
        case Kind::String: dump_string(out, str_); break;
        case Kind::Array:
            out += '[';
            for (std::size_t i = 0; i < items_.size(); ++i) {
                if (i) out += ',';
                items_[i].dump_to(out);
            }
            out += ']';
            break;
        case Kind::Object:
            out += '{';
            for (std::size_t i = 0; i < items_.size(); ++i) {
                if (i) out += ',';
                dump_string(out, keys_[i]);
                out += ':';
                items_[i].dump_to(out);
            }
            out += '}';
            break;
    }
}

std::string Json::dump() const {
    std::string out;
    dump_to(out);
    return out;
}

Json Json::parse(std::string_view text) {
    return Parser(text).parse_document();
}

bool operator==(const Json& a, const Json& b) {
    if (a.kind_ != b.kind_) return false;
    switch (a.kind_) {
        case Json::Kind::Null:   return true;
        case Json::Kind::Bool:   return a.bool_ == b.bool_;
        case Json::Kind::Number: return a.num_ == b.num_;
        case Json::Kind::String: return a.str_ == b.str_;
        case Json::Kind::Array:  return a.items_ == b.items_;
        case Json::Kind::Object: return a.keys_ == b.keys_ && a.items_ == b.items_;
    }
    return false;
}

} // namespace lsp
