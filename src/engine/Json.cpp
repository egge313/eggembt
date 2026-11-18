#include "engine/Json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace pn {

// ---------------------------------------------------------------------------
// Building and access
// ---------------------------------------------------------------------------

JsonValue JsonValue::boolean(bool b)
{
    JsonValue v;
    v.type_ = Type::Bool;
    v.bool_ = b;
    return v;
}

JsonValue JsonValue::number(double d)
{
    JsonValue v;
    v.type_ = Type::Number;
    v.number_ = d;
    return v;
}

JsonValue JsonValue::string(std::string s)
{
    JsonValue v;
    v.type_ = Type::String;
    v.string_ = std::move(s);
    return v;
}

JsonValue JsonValue::array()
{
    JsonValue v;
    v.type_ = Type::Array;
    return v;
}

JsonValue JsonValue::object()
{
    JsonValue v;
    v.type_ = Type::Object;
    return v;
}

const std::string &JsonValue::asString() const
{
    static const std::string kEmpty;
    return isString() ? string_ : kEmpty;
}

const JsonValue *JsonValue::find(const std::string &key) const
{
    if (!isObject())
        return nullptr;
    for (const auto &m : members_)
        if (m.first == key)
            return &m.second;
    return nullptr;
}

JsonValue &JsonValue::set(const std::string &key, JsonValue value)
{
    if (type_ != Type::Object) {
        *this = object();
    }
    for (auto &m : members_) {
        if (m.first == key) {
            m.second = std::move(value);
            return *this;
        }
    }
    members_.emplace_back(key, std::move(value));
    return *this;
}

JsonValue &JsonValue::push(JsonValue value)
{
    if (type_ != Type::Array)
        *this = array();
    items_.push_back(std::move(value));
    return *this;
}

// ---------------------------------------------------------------------------
// Serializing
// ---------------------------------------------------------------------------

namespace {

void appendEscaped(std::string &out, const std::string &s)
{
    out += '"';
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
                std::snprintf(buf, sizeof buf, "\\u%04x", c);
                out += buf;
            } else {
                out += static_cast<char>(c);  // UTF-8 passes through
            }
        }
    }
    out += '"';
}

}  // namespace

void JsonValue::dumpTo(std::string &out) const
{
    switch (type_) {
    case Type::Null:
        out += "null";
        break;
    case Type::Bool:
        out += bool_ ? "true" : "false";
        break;
    case Type::Number: {
        char buf[40];
        if (std::isfinite(number_) && number_ == std::floor(number_) && std::fabs(number_) < 1e15)
            std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(number_));
        else if (std::isfinite(number_))
            std::snprintf(buf, sizeof buf, "%.17g", number_);
        else
            std::snprintf(buf, sizeof buf, "null");  // JSON has no NaN or infinity
        out += buf;
        break;
    }
    case Type::String:
        appendEscaped(out, string_);
        break;
    case Type::Array:
        out += '[';
        for (std::size_t i = 0; i < items_.size(); ++i) {
            if (i)
                out += ',';
            items_[i].dumpTo(out);
        }
        out += ']';
        break;
    case Type::Object:
        out += '{';
        for (std::size_t i = 0; i < members_.size(); ++i) {
            if (i)
                out += ',';
            appendEscaped(out, members_[i].first);
            out += ':';
            members_[i].second.dumpTo(out);
        }
        out += '}';
        break;
    }
}

std::string JsonValue::dump() const
{
    std::string out;
    dumpTo(out);
    return out;
}

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------

namespace {

class Parser {
public:
    explicit Parser(const std::string &text) : s_(text) {}

    std::size_t position() const { return pos_; }

    bool parseDocument(JsonValue &out, std::string *error)
    {
        skipWs();
        if (!parseValue(out, 0))
            return fail(error);
        skipWs();
        if (pos_ != s_.size()) {
            message_ = "unexpected characters after the value";
            return fail(error);
        }
        return true;
    }

private:
    bool fail(std::string *error)
    {
        if (error)
            *error = message_ + " at offset " + std::to_string(pos_);
        return false;
    }

    bool err(const char *m)
    {
        message_ = m;
        return false;
    }

    void skipWs()
    {
        while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r'))
            ++pos_;
    }

    bool literal(const char *word)
    {
        const std::size_t n = std::char_traits<char>::length(word);
        if (s_.compare(pos_, n, word) != 0)
            return false;
        pos_ += n;
        return true;
    }

    bool parseValue(JsonValue &out, int depth)
    {
        if (depth >= 64)  // levels 0..63: at most 64 nested containers
            return err("nesting too deep");
        if (pos_ >= s_.size())
            return err("unexpected end of input");
        const char c = s_[pos_];
        switch (c) {
        case '{':
            return parseObject(out, depth);
        case '[':
            return parseArray(out, depth);
        case '"': {
            std::string str;
            if (!parseString(str))
                return false;
            out = JsonValue::string(std::move(str));
            return true;
        }
        case 't':
            if (!literal("true"))
                return err("invalid literal");
            out = JsonValue::boolean(true);
            return true;
        case 'f':
            if (!literal("false"))
                return err("invalid literal");
            out = JsonValue::boolean(false);
            return true;
        case 'n':
            if (!literal("null"))
                return err("invalid literal");
            out = JsonValue();
            return true;
        default:
            if (c == '-' || (c >= '0' && c <= '9'))
                return parseNumber(out);
            return err("unexpected character");
        }
    }

    bool parseNumber(JsonValue &out)
    {
        const std::size_t start = pos_;
        if (s_[pos_] == '-')
            ++pos_;
        if (pos_ >= s_.size())
            return err("invalid number");
        if (s_[pos_] == '0') {
            ++pos_;
        } else if (s_[pos_] >= '1' && s_[pos_] <= '9') {
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9')
                ++pos_;
        } else {
            return err("invalid number");
        }
        if (pos_ < s_.size() && s_[pos_] == '.') {
            ++pos_;
            if (pos_ >= s_.size() || s_[pos_] < '0' || s_[pos_] > '9')
                return err("invalid number");
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9')
                ++pos_;
        }
        if (pos_ < s_.size() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < s_.size() && (s_[pos_] == '+' || s_[pos_] == '-'))
                ++pos_;
            if (pos_ >= s_.size() || s_[pos_] < '0' || s_[pos_] > '9')
                return err("invalid number");
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9')
                ++pos_;
        }
        out = JsonValue::number(std::strtod(s_.substr(start, pos_ - start).c_str(), nullptr));
        return true;
    }

    static void appendUtf8(std::string &out, unsigned long cp)
    {
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

    bool hex4(unsigned long &value)
    {
        if (pos_ + 4 > s_.size())
            return err("incomplete \\u escape");
        value = 0;
        for (int i = 0; i < 4; ++i) {
            const char h = s_[pos_++];
            value <<= 4;
            if (h >= '0' && h <= '9')
                value |= static_cast<unsigned long>(h - '0');
            else if (h >= 'a' && h <= 'f')
                value |= static_cast<unsigned long>(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F')
                value |= static_cast<unsigned long>(h - 'A' + 10);
            else
                return err("invalid \\u escape");
        }
        return true;
    }

    bool parseString(std::string &out)
    {
        ++pos_;  // opening quote
        while (true) {
            if (pos_ >= s_.size())
                return err("unterminated string");
            const unsigned char c = static_cast<unsigned char>(s_[pos_++]);
            if (c == '"')
                return true;
            if (c < 0x20)
                return err("control character in string");
            if (c != '\\') {
                out += static_cast<char>(c);
                continue;
            }
            if (pos_ >= s_.size())
                return err("unterminated escape");
            const char e = s_[pos_++];
            switch (e) {
            case '"':  out += '"'; break;
            case '\\': out += '\\'; break;
            case '/':  out += '/'; break;
            case 'b':  out += '\b'; break;
            case 'f':  out += '\f'; break;
            case 'n':  out += '\n'; break;
            case 'r':  out += '\r'; break;
            case 't':  out += '\t'; break;
            case 'u': {
                unsigned long cp = 0;
                if (!hex4(cp))
                    return false;
                if (cp >= 0xD800 && cp <= 0xDBFF) {  // high surrogate: a low one must follow
                    if (pos_ + 2 > s_.size() || s_[pos_] != '\\' || s_[pos_ + 1] != 'u')
                        return err("unpaired surrogate");
                    pos_ += 2;
                    unsigned long low = 0;
                    if (!hex4(low))
                        return false;
                    if (low < 0xDC00 || low > 0xDFFF)
                        return err("unpaired surrogate");
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    return err("unpaired surrogate");
                }
                appendUtf8(out, cp);
                break;
            }
            default:
                return err("invalid escape");
            }
        }
    }

    bool parseArray(JsonValue &out, int depth)
    {
        ++pos_;  // [
        out = JsonValue::array();
        skipWs();
        if (pos_ < s_.size() && s_[pos_] == ']') {
            ++pos_;
            return true;
        }
        while (true) {
            skipWs();
            JsonValue item;
            if (!parseValue(item, depth + 1))
                return false;
            out.push(std::move(item));
            skipWs();
            if (pos_ >= s_.size())
                return err("unterminated array");
            if (s_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (s_[pos_] == ']') {
                ++pos_;
                return true;
            }
            return err("expected ',' or ']'");
        }
    }

    bool parseObject(JsonValue &out, int depth)
    {
        ++pos_;  // {
        out = JsonValue::object();
        skipWs();
        if (pos_ < s_.size() && s_[pos_] == '}') {
            ++pos_;
            return true;
        }
        while (true) {
            skipWs();
            if (pos_ >= s_.size() || s_[pos_] != '"')
                return err("expected a string key");
            std::string key;
            if (!parseString(key))
                return false;
            skipWs();
            if (pos_ >= s_.size() || s_[pos_] != ':')
                return err("expected ':'");
            ++pos_;
            skipWs();
            JsonValue value;
            if (!parseValue(value, depth + 1))
                return false;
            out.set(key, std::move(value));
            skipWs();
            if (pos_ >= s_.size())
                return err("unterminated object");
            if (s_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (s_[pos_] == '}') {
                ++pos_;
                return true;
            }
            return err("expected ',' or '}'");
        }
    }

    const std::string &s_;
    std::size_t pos_ = 0;
    std::string message_;
};

}  // namespace

bool JsonValue::parse(const std::string &text, JsonValue &out, std::string *error, std::size_t *offset)
{
    JsonValue result;
    Parser parser(text);
    if (!parser.parseDocument(result, error)) {
        if (offset)
            *offset = parser.position();
        return false;
    }
    out = std::move(result);
    return true;
}

}  // namespace pn
