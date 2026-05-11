/*
 * Copyright (c) 2026
 */
#include "mini_json.h"

#include <cctype>
#include <charconv>

namespace usbip::mini_json
{
namespace
{

class Parser
{
public:
        Parser(std::string_view text) : text_(text) {}

        Value parse_root(ParseError &err)
        {
                skip_ws();
                auto v = parse_value(err);
                if (!err.message.empty()) {
                        return {};
                }
                skip_ws();
                if (pos_ != text_.size()) {
                        err = { "trailing characters after root value", pos_ };
                        return {};
                }
                return v;
        }

private:
        void skip_ws()
        {
                while (pos_ < text_.size()) {
                        auto c = text_[pos_];
                        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                                ++pos_;
                        } else if (c == '/' && pos_ + 1 < text_.size() && text_[pos_+1] == '/') {
                                pos_ += 2;
                                while (pos_ < text_.size() && text_[pos_] != '\n') {
                                        ++pos_;
                                }
                        } else if (c == '/' && pos_ + 1 < text_.size() && text_[pos_+1] == '*') {
                                pos_ += 2;
                                while (pos_ + 1 < text_.size() &&
                                       !(text_[pos_] == '*' && text_[pos_+1] == '/')) {
                                        ++pos_;
                                }
                                if (pos_ + 1 < text_.size()) {
                                        pos_ += 2;
                                }
                        } else {
                                break;
                        }
                }
        }

        Value parse_value(ParseError &err)
        {
                skip_ws();
                if (pos_ >= text_.size()) {
                        err = { "unexpected end of input", pos_ };
                        return {};
                }

                auto c = text_[pos_];
                if (c == '{') return parse_object(err);
                if (c == '[') return parse_array(err);
                if (c == '"') return parse_string(err);
                if (c == 't' || c == 'f') return parse_bool(err);
                if (c == 'n') return parse_null(err);
                if (c == '-' || (c >= '0' && c <= '9')) return parse_number(err);

                err = { "unexpected token", pos_ };
                return {};
        }

        Value parse_object(ParseError &err)
        {
                ObjectMap obj;
                ++pos_; // consume {
                skip_ws();
                if (pos_ < text_.size() && text_[pos_] == '}') {
                        ++pos_;
                        return Value(std::move(obj));
                }
                while (true) {
                        skip_ws();
                        if (pos_ >= text_.size() || text_[pos_] != '"') {
                                err = { "expected string key in object", pos_ };
                                return {};
                        }
                        auto key_val = parse_string(err);
                        if (!err.message.empty()) return {};

                        skip_ws();
                        if (pos_ >= text_.size() || text_[pos_] != ':') {
                                err = { "expected ':' after key", pos_ };
                                return {};
                        }
                        ++pos_;
                        auto val = parse_value(err);
                        if (!err.message.empty()) return {};

                        obj.insert_or_assign(std::string(key_val.as_string()), std::move(val));

                        skip_ws();
                        if (pos_ >= text_.size()) {
                                err = { "unterminated object", pos_ };
                                return {};
                        }
                        if (text_[pos_] == ',') {
                                ++pos_;
                                continue;
                        }
                        if (text_[pos_] == '}') {
                                ++pos_;
                                break;
                        }
                        err = { "expected ',' or '}' in object", pos_ };
                        return {};
                }
                return Value(std::move(obj));
        }

        Value parse_array(ParseError &err)
        {
                ArrayVec arr;
                ++pos_;
                skip_ws();
                if (pos_ < text_.size() && text_[pos_] == ']') {
                        ++pos_;
                        return Value(std::move(arr));
                }
                while (true) {
                        auto val = parse_value(err);
                        if (!err.message.empty()) return {};
                        arr.push_back(std::move(val));

                        skip_ws();
                        if (pos_ >= text_.size()) {
                                err = { "unterminated array", pos_ };
                                return {};
                        }
                        if (text_[pos_] == ',') {
                                ++pos_;
                                continue;
                        }
                        if (text_[pos_] == ']') {
                                ++pos_;
                                break;
                        }
                        err = { "expected ',' or ']' in array", pos_ };
                        return {};
                }
                return Value(std::move(arr));
        }

        Value parse_string(ParseError &err)
        {
                if (pos_ >= text_.size() || text_[pos_] != '"') {
                        err = { "expected string", pos_ };
                        return {};
                }
                ++pos_;
                std::string out;
                while (pos_ < text_.size()) {
                        auto c = text_[pos_++];
                        if (c == '"') {
                                return Value(std::move(out));
                        }
                        if (c != '\\') {
                                out.push_back(c);
                                continue;
                        }
                        if (pos_ >= text_.size()) break;
                        auto e = text_[pos_++];
                        switch (e) {
                        case '"': out.push_back('"'); break;
                        case '\\': out.push_back('\\'); break;
                        case '/': out.push_back('/'); break;
                        case 'b': out.push_back('\b'); break;
                        case 'f': out.push_back('\f'); break;
                        case 'n': out.push_back('\n'); break;
                        case 'r': out.push_back('\r'); break;
                        case 't': out.push_back('\t'); break;
                        default:
                                err = { "unsupported escape in string", pos_ - 1 };
                                return {};
                        }
                }
                err = { "unterminated string", pos_ };
                return {};
        }

        Value parse_bool(ParseError &err)
        {
                if (text_.compare(pos_, 4, "true") == 0) {
                        pos_ += 4;
                        return Value(true);
                }
                if (text_.compare(pos_, 5, "false") == 0) {
                        pos_ += 5;
                        return Value(false);
                }
                err = { "invalid literal", pos_ };
                return {};
        }

        Value parse_null(ParseError &err)
        {
                if (text_.compare(pos_, 4, "null") == 0) {
                        pos_ += 4;
                        return Value(nullptr);
                }
                err = { "invalid literal", pos_ };
                return {};
        }

        Value parse_number(ParseError &err)
        {
                auto start = pos_;
                if (text_[pos_] == '-') ++pos_;
                while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
                        ++pos_;
                }
                std::int64_t v{};
                auto [ptr, ec] = std::from_chars(text_.data() + start, text_.data() + pos_, v);
                if (ec != std::errc{}) {
                        err = { "bad integer", start };
                        return {};
                }
                return Value(v);
        }

        std::string_view text_;
        std::size_t      pos_ = 0;
};

} // namespace

namespace
{

void append_escaped_string(std::string &out, std::string_view s)
{
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
                                // minimal unicode escape not supported; use space
                                out.push_back(' ');
                        } else {
                                out.push_back(static_cast<char>(c));
                        }
                }
        }
        out.push_back('"');
}

void serialize_to(const Value &v, std::string &out)
{
        switch (v.kind()) {
        case Value::Kind::Null:
                out += "null";
                return;
        case Value::Kind::Bool:
                out += v.as_bool(false) ? "true" : "false";
                return;
        case Value::Kind::Int: {
                char buf[32];
                const auto n = v.as_int(0);
                const auto r = std::to_chars(buf, buf + sizeof(buf), n);
                if (r.ec == std::errc{}) {
                        out.append(buf, r.ptr);
                } else {
                        out += "0";
                }
                return;
        }
        case Value::Kind::String:
                append_escaped_string(out, v.as_string());
                return;
        case Value::Kind::Array: {
                out.push_back('[');
                const auto *a = v.as_array();
                if (a) {
                        for (std::size_t i = 0; i < a->size(); ++i) {
                                if (i) {
                                        out.push_back(',');
                                }
                                serialize_to((*a)[i], out);
                        }
                }
                out.push_back(']');
                return;
        }
        case Value::Kind::Object: {
                out.push_back('{');
                const auto *o = v.as_object();
                if (o) {
                        bool first = true;
                        for (const auto &kv : *o) {
                                if (!first) {
                                        out.push_back(',');
                                }
                                first = false;
                                append_escaped_string(out, kv.first);
                                out.push_back(':');
                                serialize_to(kv.second, out);
                        }
                }
                out.push_back('}');
                return;
        }
        default:
                out += "null";
                return;
        }
}

} // namespace

Value parse(std::string_view text, ParseError *err)
{
        Parser p(text);
        ParseError local;
        auto v = p.parse_root(local);
        if (err) {
                *err = local;
        }
        if (!local.message.empty()) {
                return {};
        }
        return v;
}

std::string serialize(const Value &v)
{
        std::string out;
        serialize_to(v, out);
        return out;
}

} // namespace usbip::mini_json
