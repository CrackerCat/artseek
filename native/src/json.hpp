#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace artseek {

class Json {
public:
    enum class Kind { Null, Bool, Int, Double, String, Array, Object };
    using Array = std::vector<Json>;
    using Object = std::vector<std::pair<std::string, Json>>;

    Json() : kind_(Kind::Null) {}
    Json(std::nullptr_t) : kind_(Kind::Null) {}
    Json(bool b) : kind_(Kind::Bool), bool_(b) {}
    Json(int v) : kind_(Kind::Int), int_(v) {}
    Json(int64_t v) : kind_(Kind::Int), int_(v) {}
    Json(uint32_t v) : kind_(Kind::Int), int_(static_cast<int64_t>(v)) {}
    Json(uint64_t v) : kind_(Kind::Int), int_(static_cast<int64_t>(v)) {}
    Json(double v) : kind_(Kind::Double), double_(v) {}
    Json(const char* s) : kind_(Kind::String), str_(s ? s : "") {}
    Json(std::string s) : kind_(Kind::String), str_(std::move(s)) {}
    Json(std::string_view s) : kind_(Kind::String), str_(s) {}
    Json(Array a) : kind_(Kind::Array), arr_(std::move(a)) {}
    Json(Object o) : kind_(Kind::Object), obj_(std::move(o)) {}

    static Json array() { return Json(Array{}); }
    static Json object() { return Json(Object{}); }
    static Json object(std::initializer_list<std::pair<std::string, Json>> init) {
        Object o;
        o.reserve(init.size());
        for (const auto& kv : init) {
            o.emplace_back(kv.first, kv.second);
        }
        return Json(std::move(o));
    }

    Kind kind() const { return kind_; }
    bool isNull() const { return kind_ == Kind::Null; }
    bool isBool() const { return kind_ == Kind::Bool; }
    bool isInt() const { return kind_ == Kind::Int; }
    bool isDouble() const { return kind_ == Kind::Double; }
    bool isNumber() const { return kind_ == Kind::Int || kind_ == Kind::Double; }
    bool isString() const { return kind_ == Kind::String; }
    bool isArray() const { return kind_ == Kind::Array; }
    bool isObject() const { return kind_ == Kind::Object; }

    bool asBool(bool def = false) const {
        return kind_ == Kind::Bool ? bool_ : def;
    }
    int64_t asInt(int64_t def = 0) const {
        if (kind_ == Kind::Int) return int_;
        if (kind_ == Kind::Double) return static_cast<int64_t>(double_);
        return def;
    }
    double asDouble(double def = 0.0) const {
        if (kind_ == Kind::Double) return double_;
        if (kind_ == Kind::Int) return static_cast<double>(int_);
        return def;
    }
    const std::string& asString() const {
        static const std::string kEmpty;
        return kind_ == Kind::String ? str_ : kEmpty;
    }
    std::string asString(std::string def) const {
        return kind_ == Kind::String ? str_ : std::move(def);
    }
    const Array& asArray() const {
        static const Array kEmpty;
        return kind_ == Kind::Array ? arr_ : kEmpty;
    }
    Array& asArrayMut() {
        if (kind_ != Kind::Array) {
            *this = array();
        }
        return arr_;
    }
    const Object& asObject() const {
        static const Object kEmpty;
        return kind_ == Kind::Object ? obj_ : kEmpty;
    }
    Object& asObjectMut() {
        if (kind_ != Kind::Object) {
            *this = object();
        }
        return obj_;
    }

    size_t size() const {
        if (kind_ == Kind::Array) return arr_.size();
        if (kind_ == Kind::Object) return obj_.size();
        if (kind_ == Kind::String) return str_.size();
        return 0;
    }

    bool has(std::string_view key) const {
        if (kind_ != Kind::Object) return false;
        for (const auto& kv : obj_) {
            if (kv.first == key) return true;
        }
        return false;
    }

    const Json& get(std::string_view key) const {
        static const Json kNull;
        if (kind_ != Kind::Object) return kNull;
        for (const auto& kv : obj_) {
            if (kv.first == key) return kv.second;
        }
        return kNull;
    }

    Json& set(std::string key, Json val) {
        if (kind_ != Kind::Object) {
            *this = object();
        }
        for (auto& kv : obj_) {
            if (kv.first == key) {
                kv.second = std::move(val);
                return kv.second;
            }
        }
        obj_.emplace_back(std::move(key), std::move(val));
        return obj_.back().second;
    }

    void push(Json val) {
        if (kind_ != Kind::Array) {
            *this = array();
        }
        arr_.push_back(std::move(val));
    }

    std::string dump() const {
        std::string out;
        out.reserve(256);
        dumpTo(out, -1, 0);
        return out;
    }

    std::string dumpPretty(int indent = 2) const {
        std::string out;
        out.reserve(512);
        dumpTo(out, indent, 0);
        return out;
    }

    static std::optional<Json> parse(std::string_view input, std::string* err = nullptr) {
        Parser p(input);
        auto val = p.parseValue(0);
        if (!val) {
            if (err) *err = p.err;
            return std::nullopt;
        }
        p.skipWs();
        if (p.pos != p.in.size()) {
            if (err) *err = "trailing characters after JSON value";
            return std::nullopt;
        }
        return val;
    }

private:
    Kind kind_ = Kind::Null;
    bool bool_ = false;
    int64_t int_ = 0;
    double double_ = 0.0;
    std::string str_;
    Array arr_;
    Object obj_;

    static void appendEscaped(std::string& out, std::string_view s) {
        out.push_back('"');
        for (unsigned char c : s) {
            switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (c < 0x20) {
                        char buf[8];
                        std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                        out += buf;
                    } else {
                        out.push_back(static_cast<char>(c));
                    }
                    break;
            }
        }
        out.push_back('"');
    }

    static void appendIndent(std::string& out, int n) {
        for (int i = 0; i < n; ++i) out.push_back(' ');
    }

    void dumpTo(std::string& out, int indent, int level) const {
        switch (kind_) {
            case Kind::Null:
                out += "null";
                return;
            case Kind::Bool:
                out += bool_ ? "true" : "false";
                return;
            case Kind::Int: {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(int_));
                out += buf;
                return;
            }
            case Kind::Double: {
                if (!std::isfinite(double_)) {
                    out += "null";
                    return;
                }
                char buf[64];
                std::snprintf(buf, sizeof(buf), "%.15g", double_);
                // Ensure negative zero isn't weird, and valid JSON number
                out += buf;
                return;
            }
            case Kind::String:
                appendEscaped(out, str_);
                return;
            case Kind::Array:
                out.push_back('[');
                if (!arr_.empty()) {
                    for (size_t i = 0; i < arr_.size(); ++i) {
                        if (i > 0) out.push_back(',');
                        if (indent >= 0) {
                            out.push_back('\n');
                            appendIndent(out, (level + 1) * indent);
                        }
                        arr_[i].dumpTo(out, indent, level + 1);
                    }
                    if (indent >= 0) {
                        out.push_back('\n');
                        appendIndent(out, level * indent);
                    }
                }
                out.push_back(']');
                return;
            case Kind::Object:
                out.push_back('{');
                if (!obj_.empty()) {
                    for (size_t i = 0; i < obj_.size(); ++i) {
                        if (i > 0) out.push_back(',');
                        if (indent >= 0) {
                            out.push_back('\n');
                            appendIndent(out, (level + 1) * indent);
                        }
                        appendEscaped(out, obj_[i].first);
                        out.push_back(':');
                        if (indent >= 0) out.push_back(' ');
                        obj_[i].second.dumpTo(out, indent, level + 1);
                    }
                    if (indent >= 0) {
                        out.push_back('\n');
                        appendIndent(out, level * indent);
                    }
                }
                out.push_back('}');
                return;
        }
    }

    struct Parser {
        std::string_view in;
        size_t pos = 0;
        std::string err;

        explicit Parser(std::string_view s) : in(s) {}

        void skipWs() {
            while (pos < in.size()) {
                char c = in[pos];
                if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                    ++pos;
                } else {
                    break;
                }
            }
        }

        static void appendUtf8(std::string& out, uint32_t cp) {
            if (cp <= 0x7F) {
                out.push_back(static_cast<char>(cp));
            } else if (cp <= 0x7FF) {
                out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            } else if (cp <= 0xFFFF) {
                out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            } else if (cp <= 0x10FFFF) {
                out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            }
        }

        bool parseHex4(uint32_t& out) {
            if (pos + 4 > in.size()) {
                err = "truncated unicode escape";
                return false;
            }
            uint32_t v = 0;
            for (int i = 0; i < 4; ++i) {
                char c = in[pos++];
                v <<= 4;
                if (c >= '0' && c <= '9') v |= static_cast<uint32_t>(c - '0');
                else if (c >= 'a' && c <= 'f') v |= static_cast<uint32_t>(10 + c - 'a');
                else if (c >= 'A' && c <= 'F') v |= static_cast<uint32_t>(10 + c - 'A');
                else {
                    err = "invalid hex in unicode escape";
                    return false;
                }
            }
            out = v;
            return true;
        }

        std::optional<std::string> parseString() {
            if (pos >= in.size() || in[pos] != '"') {
                err = "expected '\"'";
                return std::nullopt;
            }
            ++pos;
            std::string out;
            while (pos < in.size()) {
                unsigned char c = static_cast<unsigned char>(in[pos++]);
                if (c == '"') return out;
                if (c < 0x20) {
                    err = "unescaped control character in string";
                    return std::nullopt;
                }
                if (c != '\\') {
                    out.push_back(static_cast<char>(c));
                    continue;
                }
                if (pos >= in.size()) {
                    err = "unterminated escape";
                    return std::nullopt;
                }
                char e = in[pos++];
                switch (e) {
                    case '"':  out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/':  out.push_back('/'); break;
                    case 'b':  out.push_back('\b'); break;
                    case 'f':  out.push_back('\f'); break;
                    case 'n':  out.push_back('\n'); break;
                    case 'r':  out.push_back('\r'); break;
                    case 't':  out.push_back('\t'); break;
                    case 'u': {
                        uint32_t cp = 0;
                        if (!parseHex4(cp)) return std::nullopt;
                        if (cp >= 0xD800 && cp <= 0xDBFF) {
                            if (pos + 2 <= in.size() && in[pos] == '\\' && in[pos + 1] == 'u') {
                                pos += 2;
                                uint32_t low = 0;
                                if (!parseHex4(low)) return std::nullopt;
                                if (low >= 0xDC00 && low <= 0xDFFF) {
                                    cp = 0x10000 + (((cp - 0xD800) << 10) | (low - 0xDC00));
                                }
                            }
                        }
                        appendUtf8(out, cp);
                        break;
                    }
                    default:
                        err = "invalid escape character";
                        return std::nullopt;
                }
            }
            err = "unterminated string";
            return std::nullopt;
        }

        std::optional<Json> parseNumber() {
            size_t start = pos;
            bool isFloat = false;
            if (pos < in.size() && in[pos] == '-') ++pos;
            if (pos >= in.size()) {
                err = "incomplete number";
                return std::nullopt;
            }
            if (in[pos] == '0') {
                ++pos;
            } else if (in[pos] >= '1' && in[pos] <= '9') {
                while (pos < in.size() && in[pos] >= '0' && in[pos] <= '9') ++pos;
            } else {
                err = "invalid number";
                return std::nullopt;
            }
            if (pos < in.size() && in[pos] == '.') {
                isFloat = true;
                ++pos;
                if (pos >= in.size() || in[pos] < '0' || in[pos] > '9') {
                    err = "invalid fraction in number";
                    return std::nullopt;
                }
                while (pos < in.size() && in[pos] >= '0' && in[pos] <= '9') ++pos;
            }
            if (pos < in.size() && (in[pos] == 'e' || in[pos] == 'E')) {
                isFloat = true;
                ++pos;
                if (pos < in.size() && (in[pos] == '+' || in[pos] == '-')) ++pos;
                if (pos >= in.size() || in[pos] < '0' || in[pos] > '9') {
                    err = "invalid exponent in number";
                    return std::nullopt;
                }
                while (pos < in.size() && in[pos] >= '0' && in[pos] <= '9') ++pos;
            }
            std::string tok(in.substr(start, pos - start));
            if (!isFloat) {
                char* endp = nullptr;
                errno = 0;
                long long v = std::strtoll(tok.c_str(), &endp, 10);
                if (errno == 0 && endp && *endp == '\0') {
                    return Json(static_cast<int64_t>(v));
                }
            }
            char* endp = nullptr;
            double d = std::strtod(tok.c_str(), &endp);
            if (!endp || *endp != '\0') {
                err = "failed to parse number";
                return std::nullopt;
            }
            return Json(d);
        }

        std::optional<Json> parseValue(int depth) {
            if (depth > 256) {
                err = "JSON nesting too deep";
                return std::nullopt;
            }
            skipWs();
            if (pos >= in.size()) {
                err = "unexpected end of input";
                return std::nullopt;
            }
            char c = in[pos];
            if (c == 'n') {
                if (in.substr(pos, 4) == "null") { pos += 4; return Json(nullptr); }
                err = "invalid token";
                return std::nullopt;
            }
            if (c == 't') {
                if (in.substr(pos, 4) == "true") { pos += 4; return Json(true); }
                err = "invalid token";
                return std::nullopt;
            }
            if (c == 'f') {
                if (in.substr(pos, 5) == "false") { pos += 5; return Json(false); }
                err = "invalid token";
                return std::nullopt;
            }
            if (c == '"') {
                auto s = parseString();
                if (!s) return std::nullopt;
                return Json(std::move(*s));
            }
            if (c == '[') {
                ++pos;
                Json arr = Json::array();
                skipWs();
                if (pos < in.size() && in[pos] == ']') {
                    ++pos;
                    return arr;
                }
                while (true) {
                    auto elem = parseValue(depth + 1);
                    if (!elem) return std::nullopt;
                    arr.push(std::move(*elem));
                    skipWs();
                    if (pos >= in.size()) {
                        err = "unterminated array";
                        return std::nullopt;
                    }
                    if (in[pos] == ']') {
                        ++pos;
                        return arr;
                    }
                    if (in[pos] != ',') {
                        err = "expected ',' or ']' in array";
                        return std::nullopt;
                    }
                    ++pos;
                }
            }
            if (c == '{') {
                ++pos;
                Json obj = Json::object();
                skipWs();
                if (pos < in.size() && in[pos] == '}') {
                    ++pos;
                    return obj;
                }
                while (true) {
                    skipWs();
                    auto k = parseString();
                    if (!k) return std::nullopt;
                    skipWs();
                    if (pos >= in.size() || in[pos] != ':') {
                        err = "expected ':' in object";
                        return std::nullopt;
                    }
                    ++pos;
                    auto v = parseValue(depth + 1);
                    if (!v) return std::nullopt;
                    obj.set(std::move(*k), std::move(*v));
                    skipWs();
                    if (pos >= in.size()) {
                        err = "unterminated object";
                        return std::nullopt;
                    }
                    if (in[pos] == '}') {
                        ++pos;
                        return obj;
                    }
                    if (in[pos] != ',') {
                        err = "expected ',' or '}' in object";
                        return std::nullopt;
                    }
                    ++pos;
                }
            }
            if (c == '-' || (c >= '0' && c <= '9')) {
                return parseNumber();
            }
            err = "unexpected character";
            return std::nullopt;
        }
    };
};

inline std::string hexAddr(uint64_t addr) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%llx", static_cast<unsigned long long>(addr));
    return std::string(buf);
}

inline std::optional<uint64_t> parseAddr(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    if (s.empty()) return std::nullopt;
    int base = 10;
    if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s.remove_prefix(2);
    }
    if (s.empty()) return std::nullopt;
    std::string tmp(s);
    char* endp = nullptr;
    errno = 0;
    unsigned long long v = std::strtoull(tmp.c_str(), &endp, base);
    if (errno != 0 || !endp || *endp != '\0') return std::nullopt;
    return static_cast<uint64_t>(v);
}

} // namespace artseek
