#pragma once
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// A small JSON reader, for the replies of the web services band3 asks
// (src/Net/rhythmverse.h). Numbers are doubles; \u escapes become UTF-8.

namespace band3::json {

class Value {
public:
    using Array = std::vector<Value>;
    using Object = std::map<std::string, Value, std::less<>>;

    Value() = default;
    explicit Value(bool b) : v_(b) {}
    explicit Value(double d) : v_(d) {}
    explicit Value(std::string s) : v_(std::move(s)) {}
    // not the bool constructor's
    explicit Value(const char* s) : v_(std::string(s)) {}
    explicit Value(Array a) : v_(std::make_shared<Array>(std::move(a))) {}
    explicit Value(Object o) : v_(std::make_shared<Object>(std::move(o))) {}

    bool IsNull() const { return std::holds_alternative<std::monostate>(v_); }
    bool IsString() const { return std::holds_alternative<std::string>(v_); }
    bool IsNumber() const { return std::holds_alternative<double>(v_); }
    bool IsArray() const { return std::holds_alternative<std::shared_ptr<Array>>(v_); }
    bool IsObject() const { return std::holds_alternative<std::shared_ptr<Object>>(v_); }

    // a member of an object; a null Value for anything else, or a missing key
    const Value& operator[](std::string_view key) const;
    // an array's items; none for anything else
    const Array& Items() const;
    // an object's members; none for anything else
    const Object& Members() const;

    // a string as it is, a number in its shortest form, true/false; "" otherwise
    std::string Text() const;
    // a number, or a string holding one ("4"); fallback otherwise
    double Number(double fallback = 0) const;
    bool Bool(bool fallback = false) const;

private:
    std::variant<std::monostate, bool, double, std::string, std::shared_ptr<Array>,
                 std::shared_ptr<Object>>
        v_;
};

// nullopt unless `text` is one JSON value (with whitespace around it allowed)
std::optional<Value> Parse(std::string_view text);

}
