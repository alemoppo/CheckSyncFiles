#pragma once

// Minimal JSON value model + writer + strict parser, sized exactly for the
// BVSS context header: objects, arrays, UTF-8 strings, integers, booleans.
// No floats, no null members in our schema (the parser still accepts `null`
// and skips it so hand-edited files degrade gracefully).
//
// The parser is strict: trailing garbage, unterminated constructs, invalid
// escapes or non-integer numbers are errors. Unknown object fields are
// SKIPPED (not rejected) so older readers tolerate additive header growth;
// unknown schema versions are rejected by SessionStore, not here.

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace bv {
namespace session {
namespace json {

struct Value;
using Object = std::map<std::string, Value>;
using Array = std::vector<Value>;

struct Value {
    enum class Type { Null, Bool, Int, String, Array, Object };
    Type type = Type::Null;
    bool boolean = false;
    int64_t integer = 0;
    std::string str;
    std::shared_ptr<Array> arr;
    std::shared_ptr<Object> obj;

    static Value Null() { return Value{}; }
    static Value Bool(bool b) {
        Value v;
        v.type = Type::Bool;
        v.boolean = b;
        return v;
    }
    static Value Int(int64_t i) {
        Value v;
        v.type = Type::Int;
        v.integer = i;
        return v;
    }
    static Value String(const std::string& s) {
        Value v;
        v.type = Type::String;
        v.str = s;
        return v;
    }
    static Value MakeArray() {
        Value v;
        v.type = Type::Array;
        v.arr = std::make_shared<Array>();
        return v;
    }
    static Value MakeObject() {
        Value v;
        v.type = Type::Object;
        v.obj = std::make_shared<Object>();
        return v;
    }

    bool isNull() const { return type == Type::Null; }
    // Typed accessors with defaults (missing/wrong-typed reads never crash;
    // SessionStore treats missing required fields as CorruptHeader).
    int64_t asInt(int64_t def = 0) const { return type == Type::Int ? integer : def; }
    bool asBool(bool def = false) const { return type == Type::Bool ? boolean : def; }
    const std::string& asString(const std::string& def = Empty()) const {
        return type == Type::String ? str : def;
    }
    const Value* find(const std::string& key) const;

   private:
    static const std::string& Empty() {
        static const std::string e;
        return e;
    }
};

// Serializes `v` to compact JSON (UTF-8 out; non-ASCII bytes pass through,
// controls/quotes/backslash are escaped, other bytes verbatim).
std::string Write(const Value& v);

// Parses `text` (UTF-8). Returns true and fills `out` on success; on failure
// returns false with a human-readable `error` (narrow).
bool Parse(const std::string& text, Value& out, std::string& error);

} // namespace json
} // namespace session
} // namespace bv
