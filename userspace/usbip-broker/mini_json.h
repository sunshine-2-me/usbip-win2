/*
 * Copyright (c) 2026
 *
 * Minimal recursive-descent JSON parser limited to what the broker needs:
 *  - objects, arrays, strings (with \" \\ \/ \b \f \n \r \t escapes),
 *    integers (signed 64-bit), true/false/null.
 *  - // line comments and /* block comments are accepted to keep policy files
 *    self-documenting.
 *
 * Floating-point numbers, scientific notation and \uXXXX escapes are not
 * supported because the policy schema does not need them.
 */
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace usbip::mini_json
{

class Value;
using ObjectMap = std::map<std::string, Value>;
using ArrayVec  = std::vector<Value>;

class Value
{
public:
        enum class Kind { Null, Bool, Int, String, Array, Object };

        Value() = default;
        Value(std::nullptr_t)        : kind_(Kind::Null) {}
        Value(bool b)                : kind_(Kind::Bool),   bool_(b) {}
        Value(std::int64_t i)        : kind_(Kind::Int),    int_(i) {}
        Value(std::string s)         : kind_(Kind::String), str_(std::move(s)) {}
        Value(ArrayVec a)            : kind_(Kind::Array),  arr_(std::make_shared<ArrayVec>(std::move(a))) {}
        Value(ObjectMap o)           : kind_(Kind::Object), obj_(std::make_shared<ObjectMap>(std::move(o))) {}

        Kind kind() const noexcept { return kind_; }

        bool is_null()   const noexcept { return kind_ == Kind::Null; }
        bool is_bool()   const noexcept { return kind_ == Kind::Bool; }
        bool is_int()    const noexcept { return kind_ == Kind::Int; }
        bool is_string() const noexcept { return kind_ == Kind::String; }
        bool is_array()  const noexcept { return kind_ == Kind::Array; }
        bool is_object() const noexcept { return kind_ == Kind::Object; }

        bool as_bool(bool def = false) const noexcept { return is_bool() ? bool_ : def; }
        std::int64_t as_int(std::int64_t def = 0) const noexcept { return is_int() ? int_ : def; }

        std::string_view as_string(std::string_view def = {}) const noexcept
        {
                return is_string() ? std::string_view(str_) : def;
        }

        const ArrayVec*  as_array()  const noexcept { return is_array()  ? arr_.get() : nullptr; }
        const ObjectMap* as_object() const noexcept { return is_object() ? obj_.get() : nullptr; }

        // Convenience: object lookup by key. Returns null Value if not found / not object.
        const Value& at(std::string_view key) const noexcept
        {
                static const Value null_value;
                if (auto o = as_object()) {
                        auto it = o->find(std::string(key));
                        if (it != o->end()) {
                                return it->second;
                        }
                }
                return null_value;
        }

private:
        Kind kind_ = Kind::Null;
        bool bool_ = false;
        std::int64_t int_ = 0;
        std::string str_;
        std::shared_ptr<ArrayVec>  arr_;
        std::shared_ptr<ObjectMap> obj_;
};

struct ParseError
{
        std::string message;
        std::size_t offset = 0;
};

/*
 * Parse a UTF-8 JSON document. On success returns the root Value and clears
 * *err. On failure returns a null Value and sets *err if non-null.
 */
Value parse(std::string_view text, ParseError *err = nullptr);

/** Serialize to strict JSON (compact, no comments). */
std::string serialize(const Value &v);

} // namespace usbip::mini_json
