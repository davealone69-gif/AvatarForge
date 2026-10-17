/*
 * af_json.cpp -- recursive-descent JSON parser behind af_json.h.
 *
 * Structure: a `Cursor` over the byte range, one function per production, and a shared error path
 * that records the offset once. Everything is iterative where the input can be hostile (white
 * space runs, long strings) and recursive only where the nesting is, with MAX_DEPTH guarding that.
 */
#include "af_json.h"

#include <cstdlib>
#include <cstring>

namespace af {
namespace json {
namespace {

constexpr int MAX_DEPTH = 64;          /* deeper than any glTF document, shallow enough to bound stack */
constexpr size_t MAX_NODES = 4u << 20; /* 4 M values: an 80 MB glTF, far past anything the app opens */

struct Cursor {
    const char* p;
    const char* end;
    size_t pos = 0;      /* byte offset from the start of the document, for error messages */
    size_t nodes = 0;    /* values produced so far */
    std::string* error;

    bool fail(const char* what) {
        if (error && error->empty()) {
            char buf[256];
            std::snprintf(buf, sizeof(buf), "JSON: %s at byte %zu", what, pos);
            *error = buf;
        }
        return false;
    }

    bool eof() const { return p >= end; }
    char peek() const { return p < end ? *p : '\0'; }

    void skipWhitespace() {
        while (p < end) {
            const char c = *p;
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++p;
                ++pos;
            } else {
                break;
            }
        }
    }

    bool literal(const char* text) {
        const size_t n = std::strlen(text);
        if (static_cast<size_t>(end - p) < n || std::memcmp(p, text, n) != 0) return false;
        p += n;
        pos += n;
        return true;
    }
};

bool parseValue(Cursor& c, Json* out, int depth);

/* ---- strings ------------------------------------------------------------------------- */
/* Appends one \u escape's UTF-16 code unit, combining surrogate pairs into UTF-8. */
bool appendCodePoint(std::string* s, uint32_t cp) {
    if (cp <= 0x7F) {
        s->push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        s->push_back(static_cast<char>(0xC0 | (cp >> 6)));
        s->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        s->push_back(static_cast<char>(0xE0 | (cp >> 12)));
        s->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        s->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0x10FFFF) {
        s->push_back(static_cast<char>(0xF0 | (cp >> 18)));
        s->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        s->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        s->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        return false;
    }
    return true;
}

bool parseHex4(Cursor& c, uint32_t* out) {
    if (c.end - c.p < 4) return c.fail("truncated \\u escape");
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
        const char h = c.p[i];
        v <<= 4;
        if (h >= '0' && h <= '9') v |= static_cast<uint32_t>(h - '0');
        else if (h >= 'a' && h <= 'f') v |= static_cast<uint32_t>(h - 'a' + 10);
        else if (h >= 'A' && h <= 'F') v |= static_cast<uint32_t>(h - 'A' + 10);
        else return c.fail("bad hex digit in \\u escape");
    }
    c.p += 4;
    c.pos += 4;
    *out = v;
    return true;
}

bool parseString(Cursor& c, std::string* out) {
    /* caller has consumed the opening quote */
    while (true) {
        if (c.eof()) return c.fail("unterminated string");
        const char ch = *c.p;
        if (ch == '"') {
            ++c.p;
            ++c.pos;
            return true;
        }
        if (static_cast<unsigned char>(ch) < 0x20) return c.fail("raw control character in string");
        if (ch != '\\') {
            out->push_back(ch);
            ++c.p;
            ++c.pos;
            continue;
        }
        ++c.p;
        ++c.pos;
        if (c.eof()) return c.fail("truncated escape");
        const char esc = *c.p;
        ++c.p;
        ++c.pos;
        switch (esc) {
            case '"': out->push_back('"'); break;
            case '\\': out->push_back('\\'); break;
            case '/': out->push_back('/'); break;
            case 'b': out->push_back('\b'); break;
            case 'f': out->push_back('\f'); break;
            case 'n': out->push_back('\n'); break;
            case 'r': out->push_back('\r'); break;
            case 't': out->push_back('\t'); break;
            case 'u': {
                uint32_t cp = 0;
                if (!parseHex4(c, &cp)) return false;
                if (cp >= 0xD800 && cp <= 0xDBFF) {           /* high surrogate: need the low half */
                    if (c.end - c.p < 2 || c.p[0] != '\\' || c.p[1] != 'u') {
                        return c.fail("lone high surrogate");
                    }
                    c.p += 2;
                    c.pos += 2;
                    uint32_t lo = 0;
                    if (!parseHex4(c, &lo)) return false;
                    if (lo < 0xDC00 || lo > 0xDFFF) return c.fail("bad low surrogate");
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    return c.fail("lone low surrogate");
                }
                if (!appendCodePoint(out, cp)) return c.fail("code point out of range");
                break;
            }
            default:
                return c.fail("unknown escape");
        }
    }
}

/* ---- numbers ------------------------------------------------------------------------- */
bool parseNumber(Cursor& c, double* out) {
    const char* start = c.p;
    if (c.peek() == '-') {
        ++c.p;
        ++c.pos;
    }
    if (c.eof()) return c.fail("truncated number");
    if (c.peek() == '0') {
        ++c.p;
        ++c.pos;
    } else if (c.peek() >= '1' && c.peek() <= '9') {
        while (!c.eof() && *c.p >= '0' && *c.p <= '9') {
            ++c.p;
            ++c.pos;
        }
    } else {
        return c.fail("malformed number");
    }
    if (!c.eof() && c.peek() == '.') {
        ++c.p;
        ++c.pos;
        if (c.eof() || *c.p < '0' || *c.p > '9') return c.fail("malformed fraction");
        while (!c.eof() && *c.p >= '0' && *c.p <= '9') {
            ++c.p;
            ++c.pos;
        }
    }
    if (!c.eof() && (c.peek() == 'e' || c.peek() == 'E')) {
        ++c.p;
        ++c.pos;
        if (!c.eof() && (*c.p == '+' || *c.p == '-')) {
            ++c.p;
            ++c.pos;
        }
        if (c.eof() || *c.p < '0' || *c.p > '9') return c.fail("malformed exponent");
        while (!c.eof() && *c.p >= '0' && *c.p <= '9') {
            ++c.p;
            ++c.pos;
        }
    }
    /* strtod is locale-sensitive for the decimal separator; C locales for parsing are not
       guaranteed here, so classify and convert by hand with a bounded exponent check. The
       manual splice also rejects 1e999 before it becomes inf. */
    std::string token(start, static_cast<size_t>(c.p - start));
    errno = 0;
    char* endp = nullptr;
    const double v = std::strtod(token.c_str(), &endp);
    if (endp != token.c_str() + token.size()) return c.fail("malformed number");
    if (v != v || v > 1.7e308 || v < -1.7e308) return c.fail("number out of range");
    *out = v;
    return true;
}

/* ---- values -------------------------------------------------------------------------- */
bool parseObject(Cursor& c, Json* out, int depth) {
    out->makeObject();
    ++c.p; /* '{' */
    ++c.pos;
    c.skipWhitespace();
    if (c.eof()) return c.fail("unterminated object");
    if (c.peek() == '}') {
        ++c.p;
        ++c.pos;
        return true;
    }
    while (true) {
        c.skipWhitespace();
        if (c.eof() || c.peek() != '"') return c.fail("expected object key");
        ++c.p;
        ++c.pos;
        std::string key;
        if (!parseString(c, &key)) return false;
        c.skipWhitespace();
        if (c.eof() || c.peek() != ':') return c.fail("expected ':'");
        ++c.p;
        ++c.pos;
        Json child;
        if (!parseValue(c, &child, depth + 1)) return false;
        out->add(std::move(key), std::move(child));
        c.skipWhitespace();
        if (c.eof()) return c.fail("unterminated object");
        if (c.peek() == ',') {
            ++c.p;
            ++c.pos;
            continue;
        }
        if (c.peek() == '}') {
            ++c.p;
            ++c.pos;
            return true;
        }
        return c.fail("expected ',' or '}'");
    }
}

bool parseArray(Cursor& c, Json* out, int depth) {
    out->makeArray();
    ++c.p; /* '[' */
    ++c.pos;
    c.skipWhitespace();
    if (c.eof()) return c.fail("unterminated array");
    if (c.peek() == ']') {
        ++c.p;
        ++c.pos;
        return true;
    }
    while (true) {
        Json child;
        if (!parseValue(c, &child, depth + 1)) return false;
        out->push(std::move(child));
        c.skipWhitespace();
        if (c.eof()) return c.fail("unterminated array");
        if (c.peek() == ',') {
            ++c.p;
            ++c.pos;
            continue;
        }
        if (c.peek() == ']') {
            ++c.p;
            ++c.pos;
            return true;
        }
        return c.fail("expected ',' or ']'");
    }
}

bool parseValue(Cursor& c, Json* out, int depth) {
    if (depth > MAX_DEPTH) return c.fail("nesting too deep");
    if (++c.nodes > MAX_NODES) return c.fail("too many values");
    c.skipWhitespace();
    if (c.eof()) return c.fail("unexpected end of input");
    const char ch = c.peek();
    switch (ch) {
        case '{':
            return parseObject(c, out, depth);
        case '[':
            return parseArray(c, out, depth);
        case '"': {
            ++c.p;
            ++c.pos;
            std::string s;
            if (!parseString(c, &s)) return false;
            out->setString(std::move(s));
            return true;
        }
        case 't':
            if (!c.literal("true")) return c.fail("expected 'true'");
            out->setBool(true);
            return true;
        case 'f':
            if (!c.literal("false")) return c.fail("expected 'false'");
            out->setBool(false);
            return true;
        case 'n':
            if (!c.literal("null")) return c.fail("expected 'null'");
            out->setNull();
            return true;
        default: {
            double v = 0.0;
            if (!parseNumber(c, &v)) return false;
            out->setNumber(v);
            return true;
        }
    }
}

} /* namespace */

bool Json::parse(const std::string& text, Json* out, std::string* error) {
    if (error) error->clear();
    if (!out) return false;
    *out = Json();
    Cursor c{text.data(), text.data() + text.size(), 0, 0, error};
    /* A UTF-8 BOM is legal in a file and illegal in JSON; strip it rather than fail, because
       exporters do write it. */
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) {
        c.p += 3;
        c.pos += 3;
    }
    Json value;
    if (!parseValue(c, &value, 0)) return false;
    c.skipWhitespace();
    if (!c.eof()) return c.fail("trailing bytes after the document");
    *out = std::move(value);
    return true;
}

} /* namespace json */
} /* namespace af */
