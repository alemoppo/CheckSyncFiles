#include "Session/SessionJson.h"

#include <cctype>
#include <cstdio>

namespace bv {
namespace session {
namespace json {

const Value* Value::find(const std::string& key) const {
    if (type != Type::Object || !obj) return nullptr;
    const auto it = obj->find(key);
    return it == obj->end() ? nullptr : &it->second;
}

namespace {

void WriteEscaped(const std::string& s, std::string& out) {
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out.push_back('"');
}

void WriteValue(const Value& v, std::string& out) {
    switch (v.type) {
        case Value::Type::Null: out += "null"; break;
        case Value::Type::Bool: out += (v.boolean ? "true" : "false"); break;
        case Value::Type::Int: out += std::to_string(v.integer); break;
        case Value::Type::String: WriteEscaped(v.str, out); break;
        case Value::Type::Array:
            out.push_back('[');
            if (v.arr) {
                for (size_t i = 0; i < v.arr->size(); ++i) {
                    if (i) out.push_back(',');
                    WriteValue((*v.arr)[i], out);
                }
            }
            out.push_back(']');
            break;
        case Value::Type::Object:
            out.push_back('{');
            if (v.obj) {
                bool first = true;
                for (const auto& kv : *v.obj) {
                    if (!first) out.push_back(',');
                    first = false;
                    WriteEscaped(kv.first, out);
                    out.push_back(':');
                    WriteValue(kv.second, out);
                }
            }
            out.push_back('}');
            break;
    }
}

struct Parser {
    const char* p = nullptr;
    const char* end = nullptr;
    std::string error;

    bool fail(const std::string& msg) {
        error = msg;
        return false;
    }
    void skipWs() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
    }
    bool consume(char c) {
        if (p < end && *p == c) {
            ++p;
            return true;
        }
        return false;
    }
    bool parseValue(Value& out) {
        skipWs();
        if (p >= end) return fail("unexpected end of input");
        switch (*p) {
            case '{': return parseObject(out);
            case '[': return parseArray(out);
            case '"': {
                std::string s;
                if (!parseString(s)) return false;
                out = Value::String(s);
                return true;
            }
            case 't':
                if (end - p >= 4 && p[0] == 't' && p[1] == 'r' && p[2] == 'u' && p[3] == 'e') {
                    p += 4;
                    out = Value::Bool(true);
                    return true;
                }
                return fail("invalid literal (expected true)");
            case 'f':
                if (end - p >= 5 && p[0] == 'f' && p[1] == 'a' && p[2] == 'l' && p[3] == 's' &&
                    p[4] == 'e') {
                    p += 5;
                    out = Value::Bool(false);
                    return true;
                }
                return fail("invalid literal (expected false)");
            case 'n':
                if (end - p >= 4 && p[0] == 'n' && p[1] == 'u' && p[2] == 'l' && p[3] == 'l') {
                    p += 4;
                    out = Value::Null();
                    return true;
                }
                return fail("invalid literal (expected null)");
            default: return parseNumber(out);
        }
    }
    bool parseNumber(Value& out) {
        const char* start = p;
        if (p < end && *p == '-') ++p;
        const char* digits = p;
        while (p < end && *p >= '0' && *p <= '9') ++p;
        if (p == digits) return fail("invalid number");
        // Our schema is integers-only: reject fractions/exponents loudly.
        if (p < end && (*p == '.' || *p == 'e' || *p == 'E'))
            return fail("non-integer numbers are not supported");
        try {
            out = Value::Int(std::stoll(std::string(start, p)));
        } catch (...) {
            return fail("number out of range");
        }
        return true;
    }
    static void AppendUtf8(std::string& s, unsigned cp) {
        if (cp < 0x80) {
            s.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            s.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            s.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            s.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            s.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    bool parseHex4(unsigned& cp) {
        if (end - p < 4) return false;
        cp = 0;
        for (int i = 0; i < 4; ++i) {
            char c = p[i];
            cp <<= 4;
            if (c >= '0' && c <= '9') cp |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') cp |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') cp |= static_cast<unsigned>(c - 'A' + 10);
            else return false;
        }
        p += 4;
        return true;
    }
    bool parseString(std::string& out) {
        // *p == '"'
        ++p;
        while (p < end) {
            char c = *p++;
            if (c == '"') return true;
            if (c == '\\') {
                if (p >= end) break;
                char e = *p++;
                switch (e) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': {
                        unsigned cp = 0;
                        if (!parseHex4(cp)) return fail("invalid \\u escape");
                        // Surrogate pair.
                        if (cp >= 0xD800 && cp <= 0xDBFF) {
                            if (end - p < 6 || p[0] != '\\' || p[1] != 'u') {
                                AppendUtf8(out, 0xFFFD);
                                break;
                            }
                            p += 2;
                            unsigned lo = 0;
                            if (!parseHex4(lo) || lo < 0xDC00 || lo > 0xDFFF)
                                return fail("invalid low surrogate");
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                            return fail("lone low surrogate");
                        }
                        AppendUtf8(out, cp);
                        break;
                    }
                    default: return fail("invalid escape");
                }
            } else if (static_cast<unsigned char>(c) < 0x20) {
                return fail("unescaped control character in string");
            } else {
                out.push_back(c);
            }
        }
        return fail("unterminated string");
    }
    bool parseObject(Value& out) {
        // *p == '{'
        ++p;
        Value obj = Value::MakeObject();
        skipWs();
        if (consume('}')) {
            out = obj;
            return true;
        }
        while (true) {
            skipWs();
            if (p >= end || *p != '"') return fail("expected string key");
            std::string key;
            if (!parseString(key)) return false;
            skipWs();
            if (!consume(':')) return fail("expected ':'");
            Value val;
            if (!parseValue(val)) return false;
            (*obj.obj)[key] = val; // last wins on duplicates
            skipWs();
            if (consume('}')) {
                out = obj;
                return true;
            }
            if (!consume(',')) return fail("expected ',' or '}'");
        }
    }
    bool parseArray(Value& out) {
        // *p == '['
        ++p;
        Value arr = Value::MakeArray();
        skipWs();
        if (consume(']')) {
            out = arr;
            return true;
        }
        while (true) {
            Value val;
            if (!parseValue(val)) return false;
            arr.arr->push_back(val);
            skipWs();
            if (consume(']')) {
                out = arr;
                return true;
            }
            if (!consume(',')) return fail("expected ',' or ']'");
        }
    }
};

} // namespace

std::string Write(const Value& v) {
    std::string out;
    WriteValue(v, out);
    return out;
}

bool Parse(const std::string& text, Value& out, std::string& error) {
    Parser ps;
    ps.p = text.data();
    ps.end = text.data() + text.size();
    if (!ps.parseValue(out)) {
        error = ps.error;
        return false;
    }
    ps.skipWs();
    if (ps.p != ps.end) {
        error = "trailing characters after JSON value";
        return false;
    }
    return true;
}

} // namespace json
} // namespace session
} // namespace bv
