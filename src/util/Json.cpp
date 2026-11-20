#include "util/Json.hpp"

#include <cctype>
#include <cstdint>
#include <cerrno>
#include <cstdio>
#include <cstdlib>

namespace json {

std::optional<std::string> Object::getString(const std::string& key) const {
    auto it = fields.find(key);
    if (it == fields.end() || it->second.type != Value::Type::String) return std::nullopt;
    return it->second.string;
}

std::optional<double> Object::getNumber(const std::string& key) const {
    auto it = fields.find(key);
    if (it == fields.end()) return std::nullopt;
    if (it->second.type == Value::Type::Number) return it->second.number;
    if (it->second.type == Value::Type::String && !it->second.string.empty()) {
        const char* begin = it->second.string.c_str();
        char* end = nullptr;
        errno = 0;
        const double v = std::strtod(begin, &end);
        if (errno == 0 && end != begin && *end == '\0') return v;
    }
    return std::nullopt;
}

std::optional<bool> Object::getBool(const std::string& key) const {
    auto it = fields.find(key);
    if (it == fields.end() || it->second.type != Value::Type::Bool) return std::nullopt;
    return it->second.boolean;
}

namespace {

class Parser {
public:
    explicit Parser(std::string_view text) : s_(text) {}

    ParseResult parse() {
        ParseResult result;
        Object obj;
        skipWs();
        if (!consume('{')) return fail("expected '{'");
        skipWs();
        if (consume('}')) {
            skipWs();
            if (pos_ != s_.size()) return fail("trailing characters");
            result.object = std::move(obj);
            return result;
        }
        while (true) {
            skipWs();
            if (peek() != '"') return fail("expected string key");
            std::string key;
            if (!parseString(key)) return fail(error_);
            skipWs();
            if (!consume(':')) return fail("expected ':'");
            skipWs();
            Value value;
            if (!parseValue(value)) return fail(error_);
            obj.fields[std::move(key)] = std::move(value);
            skipWs();
            if (consume(',')) continue;
            if (consume('}')) break;
            return fail("expected ',' or '}'");
        }
        skipWs();
        if (pos_ != s_.size()) return fail("trailing characters");
        result.object = std::move(obj);
        return result;
    }

private:
    ParseResult fail(const std::string& msg) {
        ParseResult r;
        r.error = msg + " at offset " + std::to_string(pos_);
        return r;
    }

    char peek() const { return pos_ < s_.size() ? s_[pos_] : '\0'; }

    bool consume(char c) {
        if (peek() == c) {
            ++pos_;
            return true;
        }
        return false;
    }

    void skipWs() {
        while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r')) {
            ++pos_;
        }
    }

    bool parseValue(Value& out) {
        const char c = peek();
        if (c == '"') {
            out.type = Value::Type::String;
            return parseString(out.string);
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            out.type = Value::Type::Number;
            return parseNumber(out.number);
        }
        if (matchLiteral("true")) {
            out.type = Value::Type::Bool;
            out.boolean = true;
            return true;
        }
        if (matchLiteral("false")) {
            out.type = Value::Type::Bool;
            out.boolean = false;
            return true;
        }
        if (matchLiteral("null")) {
            out.type = Value::Type::Null;
            return true;
        }
        if (c == '{' || c == '[') {
            error_ = "nested objects/arrays are not supported";
            return false;
        }
        error_ = "unexpected character";
        return false;
    }

    bool matchLiteral(std::string_view lit) {
        if (s_.substr(pos_, lit.size()) == lit) {
            pos_ += lit.size();
            return true;
        }
        return false;
    }

    bool parseNumber(double& out) {
        const std::size_t start = pos_;
        while (pos_ < s_.size()) {
            const char c = s_[pos_];
            if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E') {
                ++pos_;
            } else {
                break;
            }
        }
        const std::string token(s_.substr(start, pos_ - start));
        char* end = nullptr;
        errno = 0;
        out = std::strtod(token.c_str(), &end);
        if (errno != 0 || end != token.c_str() + token.size() || token.empty()) {
            error_ = "invalid number";
            return false;
        }
        return true;
    }

    static void appendUtf8(std::string& out, std::uint32_t cp) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    bool parseHex4(std::uint32_t& out) {
        if (pos_ + 4 > s_.size()) return false;
        out = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = s_[pos_++];
            out <<= 4;
            if (c >= '0' && c <= '9') out |= static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') out |= static_cast<std::uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= static_cast<std::uint32_t>(c - 'A' + 10);
            else return false;
        }
        return true;
    }

    bool parseString(std::string& out) {
        if (!consume('"')) {
            error_ = "expected '\"'";
            return false;
        }
        while (pos_ < s_.size()) {
            const char c = s_[pos_++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) {
                error_ = "control character in string";
                return false;
            }
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (pos_ >= s_.size()) break;
            const char esc = s_[pos_++];
            switch (esc) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    std::uint32_t cp = 0;
                    if (!parseHex4(cp)) {
                        error_ = "invalid \\u escape";
                        return false;
                    }
                    if (cp >= 0xD800 && cp <= 0xDBFF && s_.substr(pos_, 2) == "\\u") {
                        pos_ += 2;
                        std::uint32_t low = 0;
                        if (!parseHex4(low) || low < 0xDC00 || low > 0xDFFF) {
                            error_ = "invalid surrogate pair";
                            return false;
                        }
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default:
                    error_ = "invalid escape";
                    return false;
            }
        }
        error_ = "unterminated string";
        return false;
    }

    std::string_view s_;
    std::size_t pos_ = 0;
    std::string error_;
};

}  // namespace

ParseResult parseFlatObject(std::string_view text) { return Parser(text).parse(); }

std::string quote(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (const char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
                    out += buf;
                } else {
                    out.push_back(c);
                }
        }
    }
    out.push_back('"');
    return out;
}

}  // namespace json
