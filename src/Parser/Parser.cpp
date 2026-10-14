#include "Parser.hpp"
#include <iostream>
#include <cstring>
#include <cstddef>
#include <cctype>
#include <algorithm>

DIA active_arch()
{
    const static DIA arch = run_diagnostics();
    return arch;
}

void init()
{
    switch (active_arch())
    {
    case DIA::NONE:
        std::cerr << "No SIMD support detected\n";
        break;
    case DIA::AVX512:
        std::cout << "AVX-512 support detected\n";
        break;
    case DIA::AVX2:
        std::cout << "AVX2 support detected\n";
        break;
    case DIA::SSE4_2:
        std::cout << "SSE4.2 support detected\n";
        break;
    case DIA::NEON:
        std::cout << "NEON support detected\n";
        break;
    default:
        break;
    }
}

// ============================================================================
// AVX2 SIMD Implementation
// ============================================================================

namespace avx2 {

#if defined(__AVX2__) || (defined(_MSC_VER) && defined(__AVX2__))

// Classify 32 bytes at once for JSON structural characters
inline __m256i classify_chars(const __m256i input) {
    // Detect structural characters: {}[],:
    const __m256i left_brace   = _mm256_set1_epi8('{');
    const __m256i right_brace  = _mm256_set1_epi8('}');
    const __m256i left_bracket = _mm256_set1_epi8('[');
    const __m256i right_bracket= _mm256_set1_epi8(']');
    const __m256i colon        = _mm256_set1_epi8(':');
    const __m256i comma        = _mm256_set1_epi8(',');

    __m256i structural = _mm256_cmpeq_epi8(input, left_brace);
    structural = _mm256_or_si256(structural, _mm256_cmpeq_epi8(input, right_brace));
    structural = _mm256_or_si256(structural, _mm256_cmpeq_epi8(input, left_bracket));
    structural = _mm256_or_si256(structural, _mm256_cmpeq_epi8(input, right_bracket));
    structural = _mm256_or_si256(structural, _mm256_cmpeq_epi8(input, colon));
    structural = _mm256_or_si256(structural, _mm256_cmpeq_epi8(input, comma));

    return structural;
}

// Detect whitespace characters
inline __m256i detect_whitespace(const __m256i input) {
    const __m256i space  = _mm256_set1_epi8(' ');
    const __m256i tab    = _mm256_set1_epi8('\t');
    const __m256i newline= _mm256_set1_epi8('\n');
    const __m256i cr     = _mm256_set1_epi8('\r');

    __m256i ws = _mm256_cmpeq_epi8(input, space);
    ws = _mm256_or_si256(ws, _mm256_cmpeq_epi8(input, tab));
    ws = _mm256_or_si256(ws, _mm256_cmpeq_epi8(input, newline));
    ws = _mm256_or_si256(ws, _mm256_cmpeq_epi8(input, cr));

    return ws;
}

// Detect quote characters
inline __m256i detect_quotes(const __m256i input) {
    const __m256i quote = _mm256_set1_epi8('"');
    return _mm256_cmpeq_epi8(input, quote);
}

// Detect backslash (escape character)
inline __m256i detect_backslash(const __m256i input) {
    const __m256i backslash = _mm256_set1_epi8('\\');
    return _mm256_cmpeq_epi8(input, backslash);
}

// Calculate escaped characters using prefix XOR technique (simdjson-style)
inline uint64_t compute_escaped(uint64_t backslashes, uint64_t& prev_escaped) {
    // Find escaped characters by tracking odd runs of backslashes
    uint64_t starts_with_escape = backslashes & ~(backslashes << 1);
    uint64_t even_starts = starts_with_escape & 0xAAAAAAAAAAAAAAAAULL;
    uint64_t odd_starts = starts_with_escape & ~even_starts;

    uint64_t even_carries = backslashes + even_starts;
    uint64_t odd_carries = backslashes + odd_starts;

    uint64_t even_escaped = even_carries ^ backslashes;
    uint64_t odd_escaped = odd_carries ^ backslashes;

    uint64_t escaped = odd_escaped & ~even_escaped;

    // Handle escape from previous block
    if (prev_escaped) {
        escaped ^= 1ULL;
    }

    prev_escaped = (backslashes >> 63) & 1;
    return escaped;
}

// Find strings using quote tracking
inline uint64_t find_strings(uint64_t quotes, uint64_t escaped, uint64_t& prev_in_string) {
    // Remove escaped quotes
    uint64_t unescaped_quotes = quotes & ~escaped;

    // Toggle string state at each unescaped quote using prefix XOR
    uint64_t string_mask = unescaped_quotes ^ prev_in_string;

    // Propagate the string state
    uint64_t in_string = string_mask;
    in_string ^= in_string << 1;
    in_string ^= in_string << 2;
    in_string ^= in_string << 4;
    in_string ^= in_string << 8;
    in_string ^= in_string << 16;
    in_string ^= in_string << 32;

    prev_in_string = in_string >> 63;
    return in_string;
}

void find_structural_bits(const uint8_t* data, size_t len,
                          uint64_t& structural_chars,
                          State& state) {
    structural_chars = 0;

    if (len < 32) {
        // Fall back to scalar processing for small buffers
        return;
    }

    // Process 32 bytes at a time
    __m256i chunk = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data));

    // Detect structural characters, quotes, backslashes, whitespace
    __m256i structural = classify_chars(chunk);
    __m256i quotes_vec = detect_quotes(chunk);
    __m256i backslash_vec = detect_backslash(chunk);
    __m256i ws_vec = detect_whitespace(chunk);

    // Convert to bitmasks
    uint32_t structural_mask = _mm256_movemask_epi8(structural);
    uint32_t quotes_mask = _mm256_movemask_epi8(quotes_vec);
    uint32_t backslash_mask = _mm256_movemask_epi8(backslash_vec);
    uint32_t ws_mask = _mm256_movemask_epi8(ws_vec);

    // Calculate escaped characters
    uint64_t escaped = compute_escaped(backslash_mask, state.prev_escaped);

    // Find string regions
    uint64_t in_string = find_strings(quotes_mask, escaped, state.prev_in_string);

    // Structural characters are only valid outside strings and non-whitespace
    structural_chars = structural_mask & ~in_string & ~ws_mask;
}

#else

// Scalar fallback
inline __m256i classify_chars(const __m256i input) {
    return _mm256_setzero_si256();
}

void find_structural_bits(const uint8_t* data, size_t len,
                          uint64_t& structural_chars,
                          State& state) {
    structural_chars = 0;
}

#endif

} // namespace avx2

// ============================================================================
// JSON Parser Implementation
// ============================================================================

void JsonDocument::Parser::skip_whitespace() {
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) {
        ++pos;
    }
}

void JsonDocument::Parser::expect(char c) {
    if (peek() != c) {
        throw JsonParseError(std::string("Expected '") + c + "'", pos);
    }
    advance();
}

bool JsonDocument::Parser::match(std::string_view str) {
    if (json.substr(pos, str.size()) == str) {
        pos += str.size();
        return true;
    }
    return false;
}

JsonValue JsonDocument::Parser::parse_null() {
    if (!match("null")) {
        throw JsonParseError("Expected 'null'", pos);
    }
    return JsonValue(nullptr);
}

JsonValue JsonDocument::Parser::parse_bool() {
    if (match("true")) {
        return JsonValue(true);
    }
    if (match("false")) {
        return JsonValue(false);
    }
    throw JsonParseError("Expected boolean", pos);
}

JsonValue JsonDocument::Parser::parse_number() {
    size_t start = pos;

    // Handle negative sign
    if (peek() == '-') {
        advance();
    }

    // Parse integer part
    if (peek() == '0') {
        advance();
    } else if (peek() >= '1' && peek() <= '9') {
        while (peek() >= '0' && peek() <= '9') {
            advance();
        }
    } else {
        throw JsonParseError("Invalid number", pos);
    }

    // Parse fractional part
    if (peek() == '.') {
        advance();
        if (peek() < '0' || peek() > '9') {
            throw JsonParseError("Expected digit after decimal point", pos);
        }
        while (peek() >= '0' && peek() <= '9') {
            advance();
        }
    }

    // Parse exponent
    if (peek() == 'e' || peek() == 'E') {
        advance();
        if (peek() == '+' || peek() == '-') {
            advance();
        }
        if (peek() < '0' || peek() > '9') {
            throw JsonParseError("Expected digit in exponent", pos);
        }
        while (peek() >= '0' && peek() <= '9') {
            advance();
        }
    }

    // Convert to double
    std::string_view num_str = json.substr(start, pos - start);
    double value = 0.0;

    auto result = std::from_chars(num_str.data(), num_str.data() + num_str.size(), value);
    if (result.ec != std::errc()) {
        throw JsonParseError("Invalid number format", start);
    }

    return JsonValue(value);
}

JsonValue JsonDocument::Parser::parse_string() {
    expect('"');

    std::string result;
    result.reserve(32); // Reserve some space

    while (peek() != '"' && peek() != '\0') {
        if (peek() == '\\') {
            advance();
            char escaped = advance();
            switch (escaped) {
                case '"':  result += '"';  break;
                case '\\': result += '\\'; break;
                case '/':  result += '/';  break;
                case 'b':  result += '\b'; break;
                case 'f':  result += '\f'; break;
                case 'n':  result += '\n'; break;
                case 'r':  result += '\r'; break;
                case 't':  result += '\t'; break;
                case 'u': {
                    // Unicode escape (simplified - just handle basic case)
                    char hex[5] = {0};
                    for (int i = 0; i < 4; ++i) {
                        hex[i] = advance();
                        if (!std::isxdigit(static_cast<unsigned char>(hex[i]))) {
                            throw JsonParseError("Invalid unicode escape", pos - 1);
                        }
                    }
                    // For simplicity, just add placeholder (full Unicode support would be more complex)
                    result += "\\u";
                    result += hex;
                    break;
                }
                default:
                    throw JsonParseError("Invalid escape sequence", pos - 1);
            }
        } else {
            result += advance();
        }
    }

    expect('"');
    return JsonValue(std::move(result));
}

JsonValue JsonDocument::Parser::parse_array() {
    expect('[');
    skip_whitespace();

    JsonValue::Array arr;

    if (peek() == ']') {
        advance();
        return JsonValue(std::move(arr));
    }

    while (true) {
        skip_whitespace();
        arr.push_back(parse_value());
        skip_whitespace();

        if (peek() == ',') {
            advance();
            skip_whitespace();
        } else if (peek() == ']') {
            advance();
            break;
        } else {
            throw JsonParseError("Expected ',' or ']' in array", pos);
        }
    }

    return JsonValue(std::move(arr));
}

JsonValue JsonDocument::Parser::parse_object() {
    expect('{');
    skip_whitespace();

    JsonValue::Object obj;

    if (peek() == '}') {
        advance();
        return JsonValue(std::move(obj));
    }

    while (true) {
        skip_whitespace();

        // Parse key
        if (peek() != '"') {
            throw JsonParseError("Expected string key in object", pos);
        }
        JsonValue key_val = parse_string();
        std::string key = key_val.as_string();

        skip_whitespace();
        expect(':');
        skip_whitespace();

        // Parse value
        JsonValue value = parse_value();
        obj[key] = std::move(value);

        skip_whitespace();

        if (peek() == ',') {
            advance();
            skip_whitespace();
        } else if (peek() == '}') {
            advance();
            break;
        } else {
            throw JsonParseError("Expected ',' or '}' in object", pos);
        }
    }

    return JsonValue(std::move(obj));
}

JsonValue JsonDocument::Parser::parse_value() {
    skip_whitespace();

    char c = peek();

    switch (c) {
        case 'n':
            return parse_null();
        case 't':
        case 'f':
            return parse_bool();
        case '"':
            return parse_string();
        case '[':
            return parse_array();
        case '{':
            return parse_object();
        case '-':
        case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
            return parse_number();
        default:
            throw JsonParseError("Unexpected character", pos);
    }
}

JsonDocument JsonDocument::parse(std::string_view json) {
    JsonDocument doc;
    Parser parser(json);

    parser.skip_whitespace();
    if (parser.pos >= json.size()) {
        throw JsonParseError("Empty document", 0);
    }

    doc.root_ = parser.parse_value();

    parser.skip_whitespace();
    if (parser.pos < json.size()) {
        throw JsonParseError("Unexpected data after JSON value", parser.pos);
    }

    if (doc.root_.is_null()) {
        doc.explicit_null_ = true;
    }

    return doc;
}
