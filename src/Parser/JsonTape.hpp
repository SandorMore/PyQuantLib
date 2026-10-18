#pragma once

// Stage 2 of the JSON parser: walks the structural indexes produced by
// json_detail::index_document() and builds values through a Sink, so the same
// validation and decoding code can produce JsonValue trees or Python objects.
//
// A Sink provides:
//   using Value, Array, Object, Key;
//   Value make_null(); Value make_bool(bool);
//   Value make_double(double);
//   Value make_integer(std::string_view text, uint64_t magnitude, bool negative, bool exact);
//       `exact` is true when `magnitude` holds the full value (at most 19 digits).
//   Value make_string(std::string& s);  Key make_key(std::string& s);
//       `s` is a scratch buffer and may be moved from.
//   Array new_array(); void push(Array&, Value&&); Value finish(Array&&);
//   Object new_object(); void insert(Object&, Key&&, Value&&); Value finish(Object&&);

#include "Parser.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cstring>
#include <string>
#include <string_view>

namespace json_detail
{

constexpr size_t MAX_DEPTH = 1024;

constexpr std::array<bool, 256> make_terminator_table() {
    std::array<bool, 256> t{};
    for (char c : {' ', '\t', '\n', '\r', ',', ']', '}', ':'}) {
        t[static_cast<unsigned char>(c)] = true;
    }
    return t;
}
inline constexpr auto TERMINATOR = make_terminator_table();

inline int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

inline void append_utf8(std::string& out, uint32_t cp) {
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

// Converts an already validated JSON number to double.
inline double parse_double(std::string_view text, size_t pos) {
    double value = 0.0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec == std::errc::result_out_of_range && result.ptr == text.data() + text.size()) {
        // Underflow rounds to zero like json.loads; overflow is rejected.
        const bool negative = text.front() == '-';
        size_t i = negative ? 1 : 0;
        // Decimal exponent of the leading significant digit.
        long long magnitude = 0;
        while (i < text.size() && text[i] == '0') ++i;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9') { ++magnitude; ++i; }
        if (i < text.size() && text[i] == '.') {
            ++i;
            if (magnitude == 0) {
                while (i < text.size() && text[i] == '0') { --magnitude; ++i; }
            }
            while (i < text.size() && text[i] >= '0' && text[i] <= '9') ++i;
        }
        long long exponent = 0;
        if (i < text.size()) {
            ++i; // 'e' or 'E'
            const bool negative_exp = text[i] == '-';
            if (text[i] == '-' || text[i] == '+') ++i;
            for (; i < text.size(); ++i) {
                exponent = std::min<long long>(exponent * 10 + (text[i] - '0'), 1'000'000'000LL);
            }
            if (negative_exp) exponent = -exponent;
        }
        if (magnitude + exponent < 0) return negative ? -0.0 : 0.0;
        throw JsonParseError("Number out of range", pos);
    }
    if (result.ec != std::errc() || result.ptr != text.data() + text.size()) {
        throw JsonParseError("Invalid number format", pos);
    }
    return value;
}

// Converts a validated integer literal to double.
inline double integer_to_double(std::string_view text, uint64_t magnitude, bool negative, bool exact) {
    if (!exact) return parse_double(text, 0);
    const double v = static_cast<double>(magnitude);
    return negative ? -v : v;
}

template <class Sink>
class TapeWalker {
public:
    using Value = typename Sink::Value;

    TapeWalker(std::string_view json, IndexView idx, Sink& sink)
        : buf_(json.data()), len_(json.size()), idx_(idx.data), count_(idx.size), sink_(sink) {}

    Value parse_document() {
        if (count_ == 0) {
            throw JsonParseError("Empty document", 0);
        }
        Value root = parse_value(0);
        if (cur_ < count_) {
            throw JsonParseError("Unexpected data after JSON value", idx_[cur_]);
        }
        return root;
    }

private:
    const char* buf_;
    size_t len_;
    const uint32_t* idx_;
    size_t count_;
    size_t cur_ = 0;
    Sink& sink_;
    std::string scratch_;

    char peek() const { return cur_ < count_ ? buf_[idx_[cur_]] : '\0'; }
    size_t peek_pos() const { return cur_ < count_ ? idx_[cur_] : len_; }

    Value parse_value(size_t depth) {
        if (cur_ >= count_) {
            throw JsonParseError("Unexpected end of input", len_);
        }
        const size_t pos = idx_[cur_++];
        switch (buf_[pos]) {
            case '{': return parse_object(pos, depth);
            case '[': return parse_array(pos, depth);
            case '"':
                parse_string(pos, scratch_);
                return sink_.make_string(scratch_);
            case 't': parse_literal(pos, "true"); return sink_.make_bool(true);
            case 'f': parse_literal(pos, "false"); return sink_.make_bool(false);
            case 'n': parse_literal(pos, "null"); return sink_.make_null();
            case '-':
            case '0': case '1': case '2': case '3': case '4':
            case '5': case '6': case '7': case '8': case '9':
                return parse_number(pos);
            default:
                throw JsonParseError("Unexpected character", pos);
        }
    }

    Value parse_object(size_t pos, size_t depth) {
        if (depth >= MAX_DEPTH) throw JsonParseError("Maximum nesting depth exceeded", pos);
        auto obj = sink_.new_object();
        if (peek() == '}') {
            ++cur_;
            return sink_.finish(std::move(obj));
        }
        while (true) {
            if (peek() != '"') {
                throw JsonParseError("Expected string key in object", peek_pos());
            }
            parse_string(idx_[cur_++], scratch_);
            auto key = sink_.make_key(scratch_);
            if (peek() != ':') {
                throw JsonParseError("Expected ':'", peek_pos());
            }
            ++cur_;
            sink_.insert(obj, std::move(key), parse_value(depth + 1));

            const char c = peek();
            if (c == ',') {
                ++cur_;
            } else if (c == '}') {
                ++cur_;
                break;
            } else {
                throw JsonParseError("Expected ',' or '}' in object", peek_pos());
            }
        }
        return sink_.finish(std::move(obj));
    }

    Value parse_array(size_t pos, size_t depth) {
        if (depth >= MAX_DEPTH) throw JsonParseError("Maximum nesting depth exceeded", pos);
        auto arr = sink_.new_array();
        if (peek() == ']') {
            ++cur_;
            return sink_.finish(std::move(arr));
        }
        while (true) {
            sink_.push(arr, parse_value(depth + 1));

            const char c = peek();
            if (c == ',') {
                ++cur_;
            } else if (c == ']') {
                ++cur_;
                break;
            } else {
                throw JsonParseError("Expected ',' or ']' in array", peek_pos());
            }
        }
        return sink_.finish(std::move(arr));
    }

    bool at_terminator(size_t pos) const {
        return pos >= len_ || TERMINATOR[static_cast<unsigned char>(buf_[pos])];
    }

    void parse_literal(size_t pos, std::string_view word) {
        if (pos + word.size() > len_ || std::memcmp(buf_ + pos, word.data(), word.size()) != 0 ||
            !at_terminator(pos + word.size())) {
            throw JsonParseError(std::string("Expected '") + std::string(word) + "'", pos);
        }
    }

    Value parse_number(size_t pos) {
        const char* const start = buf_ + pos;
        const char* const end = buf_ + len_;
        const char* p = start;
        auto is_digit = [&](const char* q) { return q < end && *q >= '0' && *q <= '9'; };

        const bool negative = *p == '-';
        if (negative) ++p;

        const char* int_start = p;
        uint64_t magnitude = 0;
        if (p < end && *p == '0') {
            ++p;
        } else if (is_digit(p)) {
            while (is_digit(p)) {
                magnitude = magnitude * 10 + static_cast<uint64_t>(*p - '0');
                ++p;
            }
        } else {
            throw JsonParseError("Invalid number", pos);
        }
        const size_t int_digits = static_cast<size_t>(p - int_start);

        bool integral = true;
        if (p < end && *p == '.') {
            integral = false;
            ++p;
            if (!is_digit(p)) throw JsonParseError("Expected digit after decimal point", p - buf_);
            while (is_digit(p)) ++p;
        }
        if (p < end && (*p == 'e' || *p == 'E')) {
            integral = false;
            ++p;
            if (p < end && (*p == '+' || *p == '-')) ++p;
            if (!is_digit(p)) throw JsonParseError("Expected digit in exponent", p - buf_);
            while (is_digit(p)) ++p;
        }
        if (!at_terminator(static_cast<size_t>(p - buf_))) {
            throw JsonParseError("Invalid number", pos);
        }

        const std::string_view text(start, static_cast<size_t>(p - start));
        if (integral) {
            // Up to 19 digits always fit in uint64_t.
            return sink_.make_integer(text, magnitude, negative, int_digits <= 19);
        }
        return sink_.make_double(parse_double(text, pos));
    }

    // Offset of the first '"', '\' or control character at or after `p`, or len_.
    size_t scan_string(size_t p) const {
        if constexpr (std::endian::native == std::endian::little) {
            constexpr uint64_t ones = 0x0101010101010101ULL;
            constexpr uint64_t highs = 0x8080808080808080ULL;
            while (p + 8 <= len_) {
                uint64_t v;
                std::memcpy(&v, buf_ + p, 8);
                const uint64_t q = v ^ (ones * '"');
                const uint64_t b = v ^ (ones * '\\');
                // Each term flags its lowest matching byte exactly, so the lowest
                // set bit of the union is the first special byte.
                const uint64_t hit = ((q - ones) & ~q) | ((b - ones) & ~b) | ((v - ones * 0x20) & ~v);
                const uint64_t mask = hit & highs;
                if (mask) return p + static_cast<size_t>(std::countr_zero(mask) >> 3);
                p += 8;
            }
        }
        while (p < len_) {
            const auto c = static_cast<unsigned char>(buf_[p]);
            if (c == '"' || c == '\\' || c < 0x20) return p;
            ++p;
        }
        return len_;
    }

    uint32_t parse_hex4(size_t p) const {
        if (p + 4 > len_) throw JsonParseError("Invalid unicode escape", p);
        uint32_t cp = 0;
        for (size_t i = 0; i < 4; ++i) {
            const int h = hex_value(buf_[p + i]);
            if (h < 0) throw JsonParseError("Invalid unicode escape", p + i);
            cp = (cp << 4) | static_cast<uint32_t>(h);
        }
        return cp;
    }

    // Decodes the string whose opening quote is at `pos` into `out`.
    void parse_string(size_t pos, std::string& out) {
        out.clear();
        size_t run = pos + 1;
        while (true) {
            const size_t p = scan_string(run);
            if (p >= len_) throw JsonParseError("Unterminated string", pos);
            out.append(buf_ + run, p - run);

            const char c = buf_[p];
            if (c == '"') return;
            if (c != '\\') throw JsonParseError("Unescaped control character in string", p);

            if (p + 1 >= len_) throw JsonParseError("Unterminated string", pos);
            run = p + 2;
            switch (buf_[p + 1]) {
                case '"':  out += '"';  break;
                case '\\': out += '\\'; break;
                case '/':  out += '/';  break;
                case 'b':  out += '\b'; break;
                case 'f':  out += '\f'; break;
                case 'n':  out += '\n'; break;
                case 'r':  out += '\r'; break;
                case 't':  out += '\t'; break;
                case 'u': {
                    uint32_t cp = parse_hex4(p + 2);
                    run = p + 6;
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        if (run + 6 > len_ || buf_[run] != '\\' || buf_[run + 1] != 'u') {
                            throw JsonParseError("Unpaired surrogate in unicode escape", p);
                        }
                        const uint32_t low = parse_hex4(run + 2);
                        if (low < 0xDC00 || low > 0xDFFF) {
                            throw JsonParseError("Invalid low surrogate in unicode escape", run);
                        }
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                        run += 6;
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        throw JsonParseError("Unpaired surrogate in unicode escape", p);
                    }
                    append_utf8(out, cp);
                    break;
                }
                default:
                    throw JsonParseError("Invalid escape sequence", p + 1);
            }
        }
    }
};

} // namespace json_detail
