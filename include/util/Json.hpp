#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace json {

struct Value {
    enum class Type { Null, Bool, Number, String };
    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string string;
};

// A flat JSON object (string / number / bool / null values). Every command the
// gateway accepts is a flat object, so nested values are rejected at parse time.
class Object {
public:
    bool has(const std::string& key) const { return fields.count(key) != 0; }
    std::optional<std::string> getString(const std::string& key) const;
    // Accepts numbers and numeric strings ("101.25").
    std::optional<double> getNumber(const std::string& key) const;
    std::optional<bool> getBool(const std::string& key) const;

    std::unordered_map<std::string, Value> fields;
};

struct ParseResult {
    std::optional<Object> object;
    std::string error;
};

ParseResult parseFlatObject(std::string_view text);

// Returns s as a quoted, escaped JSON string literal.
std::string quote(std::string_view s);

}  // namespace json
