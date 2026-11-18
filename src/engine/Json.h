// A small JSON value with a strict parser and a compact serializer: just
// enough for the plug-in protocol, without a Qt (or any) dependency.
//
// The serializer never emits a raw newline, so one value is always one line
// (JSON lines). Strings are UTF-8; non-ASCII passes through unescaped. The
// parser accepts standard JSON only (no comments, no trailing commas),
// understands every escape including \uXXXX with surrogate pairs, and refuses
// to nest deeper than 64 levels.
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace pn {

class JsonValue {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    JsonValue() = default;
    static JsonValue boolean(bool b);
    static JsonValue number(double d);
    static JsonValue string(std::string s);
    static JsonValue array();
    static JsonValue object();

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isBool() const { return type_ == Type::Bool; }
    bool isNumber() const { return type_ == Type::Number; }
    bool isString() const { return type_ == Type::String; }
    bool isArray() const { return type_ == Type::Array; }
    bool isObject() const { return type_ == Type::Object; }

    // Lenient accessors: the fallback is returned if the type does not match.
    bool asBool(bool fallback = false) const { return isBool() ? bool_ : fallback; }
    double asNumber(double fallback = 0.0) const { return isNumber() ? number_ : fallback; }
    const std::string &asString() const;  // empty for non-strings
    const std::vector<JsonValue> &items() const { return items_; }
    // Object members in insertion order (empty for non-objects).
    const std::vector<std::pair<std::string, JsonValue>> &members() const { return members_; }

    // Objects: the value for `key`, or nullptr (also for non-objects).
    const JsonValue *find(const std::string &key) const;

    // Building. set() on an object replaces an existing key; push() appends to an array.
    JsonValue &set(const std::string &key, JsonValue value);
    JsonValue &push(JsonValue value);

    std::string dump() const;  // compact, single line

    // On failure `error` gets a message, and `offset` (if given) the byte offset where parsing stopped.
    static bool parse(const std::string &text, JsonValue &out, std::string *error = nullptr,
                      std::size_t *offset = nullptr);

private:
    void dumpTo(std::string &out) const;

    Type type_ = Type::Null;
    bool bool_ = false;
    double number_ = 0.0;
    std::string string_;
    std::vector<JsonValue> items_;                           // array elements
    std::vector<std::pair<std::string, JsonValue>> members_;  // object members, in insertion order
};

}  // namespace pn
