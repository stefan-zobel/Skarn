#pragma once

// =============================================================================
// Json.h -- the small JSON value the language server speaks.
//
// Exactly what the Language Server Protocol needs and nothing more: null, bool,
// number (a double), string, array and object. Objects keep INSERTION order, so a
// message is written in the order it was built -- that keeps the self-test's expected
// output readable and makes a dump reproducible. Strings are UTF-8 in and out; the
// parser decodes every escape, including \uXXXX surrogate pairs, to UTF-8.
//
// Deliberately no third-party library: this is the one JSON reader/writer in the
// C++ side of the repository, and the protocol needs only a sliver of the format.
// =============================================================================

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace lsp {

// Malformed JSON text. `offset` is the byte position in the input where parsing gave up.
class JsonError : public std::runtime_error {
public:
    JsonError(const std::string& msg, std::size_t offset)
        : std::runtime_error(msg + " (at byte " + std::to_string(offset) + ")"), offset(offset) {}
    std::size_t offset;
};

class Json {
public:
    enum class Kind : uint8_t { Null, Bool, Number, String, Array, Object };

    Json() = default;                                    // null
    Json(std::nullptr_t) {}                              // null
    Json(bool b) : kind_(Kind::Bool), bool_(b) {}
    Json(double d) : kind_(Kind::Number), num_(d) {}
    Json(int i) : kind_(Kind::Number), num_(static_cast<double>(i)) {}
    Json(int64_t i) : kind_(Kind::Number), num_(static_cast<double>(i)) {}
    Json(const char* s) : kind_(Kind::String), str_(s) {}
    Json(std::string s) : kind_(Kind::String), str_(std::move(s)) {}
    Json(std::string_view s) : kind_(Kind::String), str_(s) {}

    static Json array()  { Json j; j.kind_ = Kind::Array;  return j; }
    static Json object() { Json j; j.kind_ = Kind::Object; return j; }

    Kind kind() const noexcept { return kind_; }
    bool is_null()   const noexcept { return kind_ == Kind::Null; }
    bool is_bool()   const noexcept { return kind_ == Kind::Bool; }
    bool is_number() const noexcept { return kind_ == Kind::Number; }
    bool is_string() const noexcept { return kind_ == Kind::String; }
    bool is_array()  const noexcept { return kind_ == Kind::Array; }
    bool is_object() const noexcept { return kind_ == Kind::Object; }

    // Typed reads. Each throws std::logic_error on a kind mismatch -- a protocol handler
    // catches it and answers the request with an error instead of crashing.
    bool               as_bool()   const;
    double             as_number() const;
    const std::string& as_string() const;

    // Array access.
    std::size_t size() const noexcept;                   // elements (array) or members (object)
    const Json& at(std::size_t i) const;                 // array element
    Json&       push(Json v);                            // append to an array; returns the element

    // Object access. `find` returns nullptr when the key is absent (or this is not an object);
    // `get` returns a shared null instead, so `msg.get("params").get("uri")` never throws.
    const Json* find(std::string_view key) const noexcept;
    const Json& get(std::string_view key) const noexcept;
    Json&       set(std::string key, Json v);            // insert or replace; returns the value
    const std::string& key_at(std::size_t i) const;      // i-th member name (object)
    const Json&        value_at(std::size_t i) const;    // i-th member value (object)

    std::string dump() const;                            // compact serialization
    static Json parse(std::string_view text);            // throws JsonError

    friend bool operator==(const Json& a, const Json& b);

private:
    void dump_to(std::string& out) const;

    Kind                     kind_ = Kind::Null;
    bool                     bool_ = false;
    double                   num_  = 0.0;
    std::string              str_;
    std::vector<Json>        items_;   // array elements, or object member VALUES
    std::vector<std::string> keys_;    // object member names, parallel to items_
};

} // namespace lsp
