/*
 * af_json.h -- a small, bounded JSON reader for the glTF importer.
 *
 * Why hand-rolled: the engine ships in an APK with no third-party dependencies, and glTF's JSON
 * chunk is the only JSON the app ever reads. What it needs from a parser is therefore narrow but
 * specific:
 *
 *   - it must never crash or allocate unbounded memory on hostile input, because a .glb is a file
 *     the user can drop into the app's import folder and it is not trusted;
 *   - it must keep numbers as double (accessor offsets and byte lengths are exact integers, and
 *     rounding one to float would mis-address a buffer);
 *   - it must report *where* it failed, so the importer's error message names an offset in the
 *     file rather than "invalid JSON".
 *
 * So: a DOM built by a recursive-descent parser with an explicit depth limit, a node budget and
 * bounds-checked slices. No exceptions, no I/O, no locale dependence. RFC 8259 semantics,
 * including \u escapes, surrogate pairs and the number grammar; duplicate object keys resolve to
 * the last one, as the spec allows.
 *
 * The DOM is a value type. `Json` is a struct with a tag plus storage, and arrays/objects hold
 * their children inline; the parser fills it bottom-up. Copying a `Json` copies the subtree, which
 * is fine because nothing in the importer copies a parsed document.
 */
#ifndef AF_JSON_H
#define AF_JSON_H

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace af {
namespace json {

class Json {
public:
    enum Type : uint8_t { Null, Bool, Number, String, Array, Object };

    Json() = default;

    Type type() const { return type_; }
    bool isNull() const { return type_ == Null; }
    bool isBool() const { return type_ == Bool; }
    bool isNumber() const { return type_ == Number; }
    bool isString() const { return type_ == String; }
    bool isArray() const { return type_ == Array; }
    bool isObject() const { return type_ == Object; }

    /* ---- scalars, with defaults so a missing key and a wrong type read the same ---- */
    bool asBool(bool fallback = false) const { return type_ == Bool ? boolean_ : fallback; }
    double asNumber(double fallback = 0.0) const { return type_ == Number ? number_ : fallback; }
    int asInt(int fallback = 0) const {
        if (type_ != Number) return fallback;
        const double v = number_;
        if (!(v >= -2147483648.0 && v <= 2147483647.0)) return fallback;
        return static_cast<int>(v);
    }
    /* Rounds toward the nearest integer, for fields the spec types as integers but a writer may
       have emitted as 3.0. Returns `fallback` when the value is not integral. */
    bool asIndex(size_t* out) const {
        if (type_ != Number) return false;
        const double v = number_;
        if (v < 0.0 || v > 9.007199254740992e15 ||
            v > static_cast<double>(std::numeric_limits<size_t>::max())) return false;
        const double r = static_cast<double>(static_cast<uint64_t>(v + 0.5));
        if (r != v) return false;
        *out = static_cast<size_t>(v);
        return true;
    }
    const std::string& asString() const { return string_; }
    std::string stringOr(const char* fallback) const {
        return type_ == String ? string_ : std::string(fallback ? fallback : "");
    }

    /* ---- containers ---- */
    size_t size() const { return items_.size(); }
    const Json& at(size_t i) const { return items_[i]; }
    /* Object lookup. Returns nullptr when absent or when this is not an object. O(n) with n small
       (glTF objects have tens of keys); no hash table to keep the DOM value-type. */
    const Json* find(const char* key) const {
        if (type_ != Object) return nullptr;
        for (const auto& kv : members_) {
            if (kv.first == key) return &kv.second;
        }
        return nullptr;
    }
    const Json& operator[](const char* key) const {
        static const Json kNull;
        const Json* v = find(key);
        return v ? *v : kNull;
    }
    const std::vector<std::pair<std::string, Json>>& members() const { return members_; }
    const std::vector<Json>& items() const { return items_; }

    /* Convenience: `doc["buffers"].at(0)["byteLength"].asInt()` with the null object swallowing
       out-of-range indexing. Out-of-range `at()` on a malformed document would be a crash, so the
       importer prefers `atOrNull`. */
    const Json& atOrNull(size_t i) const {
        static const Json kNull;
        if (type_ != Array || i >= items_.size()) return kNull;
        return items_[i];
    }

    /* ---- construction (used by the parser and by tests that build documents) ---- */
    void setBool(bool b) { type_ = Bool; boolean_ = b; }
    void setNumber(double n) { type_ = Number; number_ = n; }
    void setString(std::string s) { type_ = String; string_ = std::move(s); }
    void setNull() { type_ = Null; }
    void makeArray() { type_ = Array; }
    void makeObject() { type_ = Object; }
    void push(Json v) { items_.push_back(std::move(v)); }
    void add(std::string key, Json v) { members_.emplace_back(std::move(key), std::move(v)); }

    /* Parses `text`. On failure returns false and, when `error` is non-null, fills it with a
       message that includes the byte offset. The output is left as a null value. */
    static bool parse(const std::string& text, Json* out, std::string* error);

private:
    Type type_ = Null;
    bool boolean_ = false;
    double number_ = 0.0;
    std::string string_;
    std::vector<Json> items_;                            /* Array */
    std::vector<std::pair<std::string, Json>> members_;   /* Object, insertion order */
};

} /* namespace json */
} /* namespace af */

#endif /* AF_JSON_H */
