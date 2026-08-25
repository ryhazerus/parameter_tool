#include "json.hpp"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace pt::json {
namespace {

constexpr int kMaxDepth = 64;  // manifests are 3 deep; this only guards malformed input

void AppendEscaped(std::string& out, std::string_view s) {
    for (char raw : s) {
        const unsigned char c = static_cast<unsigned char>(raw);
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
                    char buf[7];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    // Bytes >= 0x20 pass through, so valid UTF-8 input stays valid UTF-8 output.
                    out += static_cast<char>(c);
                }
        }
    }
}

void SerializeInto(const Value& v, std::string& out, int indent) {
    const std::string pad(static_cast<std::size_t>(indent) * 2, ' ');
    const std::string pad_inner(static_cast<std::size_t>(indent + 1) * 2, ' ');

    switch (v.type()) {
        case Type::kNull: out += "null"; break;
        case Type::kBool: out += *v.AsBool() ? "true" : "false"; break;
        case Type::kInt:  out += std::to_string(*v.AsInt()); break;
        case Type::kDouble: {
            const double d = *v.AsDouble();
            if (!std::isfinite(d)) {
                out += "null";  // JSON has no inf/nan
            } else {
                char buf[40];
                std::snprintf(buf, sizeof(buf), "%.17g", d);
                out += buf;
            }
            break;
        }
        case Type::kString:
            out += '"';
            AppendEscaped(out, *v.AsString());
            out += '"';
            break;
        case Type::kArray: {
            const Array& a = *v.AsArray();
            if (a.empty()) { out += "[]"; break; }
            out += "[\n";
            for (std::size_t i = 0; i < a.size(); ++i) {
                out += pad_inner;
                SerializeInto(a[i], out, indent + 1);
                if (i + 1 < a.size()) out += ',';
                out += '\n';
            }
            out += pad;
            out += ']';
            break;
        }
        case Type::kObject: {
            const Object& o = *v.AsObject();
            if (o.empty()) { out += "{}"; break; }
            out += "{\n";
            for (std::size_t i = 0; i < o.size(); ++i) {
                out += pad_inner;
                out += '"';
                AppendEscaped(out, o[i].first);
                out += "\": ";
                SerializeInto(o[i].second, out, indent + 1);
                if (i + 1 < o.size()) out += ',';
                out += '\n';
            }
            out += pad;
            out += '}';
            break;
        }
    }
}

class Parser {
public:
    Parser(std::string_view t, std::string* err) : t_(t), err_(err) {}

    bool ParseDocument(Value& out) {
        SkipWs();
        if (!ParseValue(out, 0)) return false;
        SkipWs();
        if (pos_ != t_.size()) return Fail("trailing content after JSON value");
        return true;
    }

private:
    bool Fail(const std::string& msg) {
        if (err_ && err_->empty()) *err_ = msg + " at offset " + std::to_string(pos_);
        return false;
    }

    void SkipWs() {
        while (pos_ < t_.size()) {
            const char c = t_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++pos_;
            else break;
        }
    }

    bool Eof() const { return pos_ >= t_.size(); }
    char Peek() const { return t_[pos_]; }

    bool Literal(std::string_view lit) {
        if (t_.substr(pos_, lit.size()) != lit) return false;
        pos_ += lit.size();
        return true;
    }

    bool ParseValue(Value& out, int depth) {
        if (depth > kMaxDepth) return Fail("nesting too deep");
        if (Eof()) return Fail("unexpected end of input");

        switch (Peek()) {
            case '{': return ParseObject(out, depth);
            case '[': return ParseArray(out, depth);
            case '"': {
                std::string s;
                if (!ParseString(s)) return false;
                out = Value(std::move(s));
                return true;
            }
            case 't': if (!Literal("true"))  return Fail("invalid literal"); out = Value(true);  return true;
            case 'f': if (!Literal("false")) return Fail("invalid literal"); out = Value(false); return true;
            case 'n': if (!Literal("null"))  return Fail("invalid literal"); out = Value();      return true;
            default:  return ParseNumber(out);
        }
    }

    bool ParseObject(Value& out, int depth) {
        ++pos_;  // '{'
        Object obj;
        SkipWs();
        if (!Eof() && Peek() == '}') { ++pos_; out = Value(std::move(obj)); return true; }
        for (;;) {
            SkipWs();
            if (Eof() || Peek() != '"') return Fail("expected object key");
            std::string key;
            if (!ParseString(key)) return false;
            SkipWs();
            if (Eof() || Peek() != ':') return Fail("expected ':'");
            ++pos_;
            SkipWs();
            Value v;
            if (!ParseValue(v, depth + 1)) return false;
            obj.emplace_back(std::move(key), std::move(v));
            SkipWs();
            if (Eof()) return Fail("unterminated object");
            if (Peek() == ',') { ++pos_; continue; }
            if (Peek() == '}') { ++pos_; break; }
            return Fail("expected ',' or '}'");
        }
        out = Value(std::move(obj));
        return true;
    }

    bool ParseArray(Value& out, int depth) {
        ++pos_;  // '['
        Array arr;
        SkipWs();
        if (!Eof() && Peek() == ']') { ++pos_; out = Value(std::move(arr)); return true; }
        for (;;) {
            SkipWs();
            Value v;
            if (!ParseValue(v, depth + 1)) return false;
            arr.push_back(std::move(v));
            SkipWs();
            if (Eof()) return Fail("unterminated array");
            if (Peek() == ',') { ++pos_; continue; }
            if (Peek() == ']') { ++pos_; break; }
            return Fail("expected ',' or ']'");
        }
        out = Value(std::move(arr));
        return true;
    }

    static void AppendUtf8(std::string& s, std::uint32_t cp) {
        if (cp <= 0x7f) {
            s += static_cast<char>(cp);
        } else if (cp <= 0x7ff) {
            s += static_cast<char>(0xc0 | (cp >> 6));
            s += static_cast<char>(0x80 | (cp & 0x3f));
        } else if (cp <= 0xffff) {
            s += static_cast<char>(0xe0 | (cp >> 12));
            s += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
            s += static_cast<char>(0x80 | (cp & 0x3f));
        } else {
            s += static_cast<char>(0xf0 | (cp >> 18));
            s += static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
            s += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
            s += static_cast<char>(0x80 | (cp & 0x3f));
        }
    }

    bool ParseHex4(std::uint32_t& out) {
        if (pos_ + 4 > t_.size()) return Fail("truncated \\u escape");
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = t_[pos_ + static_cast<std::size_t>(i)];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<std::uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<std::uint32_t>(c - 'A' + 10);
            else return Fail("bad hex digit in \\u escape");
        }
        pos_ += 4;
        out = v;
        return true;
    }

    bool ParseString(std::string& out) {
        ++pos_;  // opening quote
        out.clear();
        for (;;) {
            if (Eof()) return Fail("unterminated string");
            const unsigned char c = static_cast<unsigned char>(t_[pos_]);
            if (c == '"') { ++pos_; return true; }
            if (c < 0x20) return Fail("unescaped control character in string");
            if (c != '\\') { out += static_cast<char>(c); ++pos_; continue; }

            ++pos_;  // backslash
            if (Eof()) return Fail("unterminated escape");
            const char e = t_[pos_++];
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
                    std::uint32_t cp = 0;
                    if (!ParseHex4(cp)) return false;
                    if (cp >= 0xd800 && cp <= 0xdbff) {  // high surrogate, expect the low half
                        if (pos_ + 1 < t_.size() && t_[pos_] == '\\' && t_[pos_ + 1] == 'u') {
                            pos_ += 2;
                            std::uint32_t lo = 0;
                            if (!ParseHex4(lo)) return false;
                            if (lo >= 0xdc00 && lo <= 0xdfff) {
                                cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                            } else {
                                cp = 0xfffd;  // unpaired; substitute rather than reject
                            }
                        } else {
                            cp = 0xfffd;
                        }
                    } else if (cp >= 0xdc00 && cp <= 0xdfff) {
                        cp = 0xfffd;  // stray low surrogate
                    }
                    AppendUtf8(out, cp);
                    break;
                }
                default: return Fail("invalid escape character");
            }
        }
    }

    bool ParseNumber(Value& out) {
        const std::size_t start = pos_;
        if (!Eof() && Peek() == '-') ++pos_;
        if (Eof() || Peek() < '0' || Peek() > '9') return Fail("invalid number");
        if (Peek() == '0') {
            ++pos_;  // a leading zero must stand alone: "0" or "0.5", never "01"
            if (!Eof() && Peek() >= '0' && Peek() <= '9') return Fail("leading zero in number");
        } else {
            while (!Eof() && Peek() >= '0' && Peek() <= '9') ++pos_;
        }

        bool is_double = false;
        if (!Eof() && Peek() == '.') {
            is_double = true;
            ++pos_;
            if (Eof() || Peek() < '0' || Peek() > '9') return Fail("invalid fraction");
            while (!Eof() && Peek() >= '0' && Peek() <= '9') ++pos_;
        }
        if (!Eof() && (Peek() == 'e' || Peek() == 'E')) {
            is_double = true;
            ++pos_;
            if (!Eof() && (Peek() == '+' || Peek() == '-')) ++pos_;
            if (Eof() || Peek() < '0' || Peek() > '9') return Fail("invalid exponent");
            while (!Eof() && Peek() >= '0' && Peek() <= '9') ++pos_;
        }

        const std::string text(t_.substr(start, pos_ - start));
        if (is_double) {
            out = Value(std::strtod(text.c_str(), nullptr));
        } else {
            errno = 0;
            char* end = nullptr;
            const long long v = std::strtoll(text.c_str(), &end, 10);
            if (errno == ERANGE) {
                out = Value(std::strtod(text.c_str(), nullptr));  // too big for int64; keep as double
            } else {
                out = Value(static_cast<std::int64_t>(v));
            }
        }
        return true;
    }

    std::string_view t_;
    std::size_t pos_ = 0;
    std::string* err_;
};

}  // namespace

std::optional<bool> Value::AsBool() const {
    if (type_ != Type::kBool) return std::nullopt;
    return bool_;
}
std::optional<std::int64_t> Value::AsInt() const {
    if (type_ != Type::kInt) return std::nullopt;
    return int_;
}
std::optional<double> Value::AsDouble() const {
    if (type_ == Type::kDouble) return double_;
    if (type_ == Type::kInt) return static_cast<double>(int_);
    return std::nullopt;
}
const std::string* Value::AsString() const { return type_ == Type::kString ? &str_ : nullptr; }
const Array* Value::AsArray() const { return type_ == Type::kArray ? &arr_ : nullptr; }
const Object* Value::AsObject() const { return type_ == Type::kObject ? &obj_ : nullptr; }

const Value* Value::Find(std::string_view key) const {
    if (type_ != Type::kObject) return nullptr;
    for (const auto& kv : obj_) {
        if (kv.first == key) return &kv.second;
    }
    return nullptr;
}

std::string Serialize(const Value& v) {
    std::string out;
    SerializeInto(v, out, 0);
    out += '\n';
    return out;
}

std::optional<Value> Parse(std::string_view text, std::string* error) {
    std::string local;
    Parser p(text, error ? error : &local);
    Value v;
    if (!p.ParseDocument(v)) return std::nullopt;
    return v;
}

std::string EscapeString(std::string_view s) {
    std::string out;
    AppendEscaped(out, s);
    return out;
}

}  // namespace pt::json
