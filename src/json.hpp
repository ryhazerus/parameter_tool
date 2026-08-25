#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// A deliberately small JSON reader/writer. We own both ends of the format (snapshot manifests),
// so this covers exactly what JSON requires and nothing more: no comments, no trailing commas.
namespace pt::json {

class Value;
using Object = std::vector<std::pair<std::string, Value>>;  // insertion-ordered
using Array = std::vector<Value>;

enum class Type { kNull, kBool, kInt, kDouble, kString, kArray, kObject };

class Value {
public:
    Value() : type_(Type::kNull) {}
    Value(std::nullptr_t) : type_(Type::kNull) {}
    Value(bool b) : type_(Type::kBool), bool_(b) {}
    Value(std::int64_t i) : type_(Type::kInt), int_(i) {}
    Value(int i) : type_(Type::kInt), int_(i) {}
    Value(double d) : type_(Type::kDouble), double_(d) {}
    Value(std::string s) : type_(Type::kString), str_(std::move(s)) {}
    Value(const char* s) : type_(Type::kString), str_(s) {}
    Value(Array a) : type_(Type::kArray), arr_(std::move(a)) {}
    Value(Object o) : type_(Type::kObject), obj_(std::move(o)) {}

    Type type() const { return type_; }
    bool IsNull() const { return type_ == Type::kNull; }

    // Typed accessors. Each returns nullopt/nullptr when the node is a different type, so callers
    // reading a hand-edited manifest get a clean error instead of garbage.
    std::optional<bool> AsBool() const;
    std::optional<std::int64_t> AsInt() const;
    std::optional<double> AsDouble() const;
    const std::string* AsString() const;
    const Array* AsArray() const;
    const Object* AsObject() const;

    // Object member lookup; nullptr when absent or when this is not an object.
    const Value* Find(std::string_view key) const;

private:
    Type type_;
    bool bool_ = false;
    std::int64_t int_ = 0;
    double double_ = 0.0;
    std::string str_;
    Array arr_;
    Object obj_;
};

// Serializes with two-space indentation and a trailing newline.
std::string Serialize(const Value& v);

// Parses a complete document. On failure returns nullopt and sets `error`.
std::optional<Value> Parse(std::string_view text, std::string* error);

// Escapes a string's contents (without surrounding quotes) per RFC 8259.
std::string EscapeString(std::string_view s);

}  // namespace pt::json
