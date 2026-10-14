#pragma once

#include <immintrin.h>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <exception>
#include <memory>
#include <new>
#include <bitset>
#include <string>
#include <string_view>
#include <variant>
#include <vector>
#include <unordered_map>
#include <stdexcept>
#include <charconv>

#if defined(_MSC_VER)
  #include <intrin.h>
#endif

enum class DIA {
    NONE,
    SSE4_2,
    AVX2,
    AVX512,
    NEON
};

namespace detail {

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))

inline DIA detect_x86_msvc() {
    int r[4];                       // eax, ebx, ecx, edx
    __cpuid(r, 0);
    const int max_leaf = r[0];
    if (max_leaf < 1) return DIA::NONE;

    __cpuid(r, 1);
    const bool sse42    = r[2] & (1 << 20);
    const bool osxsave  = r[2] & (1 << 27);
    const bool avx      = r[2] & (1 << 28);

    uint64_t xcr0 = 0;
    if (osxsave) xcr0 = _xgetbv(0);
    const bool os_ymm = (xcr0 & 0x06) == 0x06;   // XMM + YMM state enabled
    const bool os_zmm = (xcr0 & 0xE6) == 0xE6;   // + opmask, ZMM_Hi256, Hi16_ZMM

    bool avx2 = false, avx512 = false;
    if (max_leaf >= 7) {
        __cpuidex(r, 7, 0);
        avx2 = r[1] & (1 << 5);
        const bool f  = r[1] & (1 << 16);
        const bool dq = r[1] & (1 << 17);
        const bool bw = r[1] & (1 << 30);
        const bool vl = r[1] & (1 << 31);
        avx512 = f && dq && bw && vl;
    }

    if (avx512 && os_zmm && avx && os_ymm) return DIA::AVX512;
    if (avx2   && avx && os_ymm)           return DIA::AVX2;
    if (sse42)                             return DIA::SSE4_2;
    return DIA::NONE;
}

#endif

} // namespace detail

inline DIA run_diagnostics()
{
#if defined(__aarch64__) || defined(_M_ARM64)
    return DIA::NEON;   // NEON is mandatory on AArch64

#elif defined(__x86_64__) || defined(__i386__)
  #if defined(__GNUC__) || defined(__clang__)
    __builtin_cpu_init();
    // GCC/Clang's runtime also verifies OS register-state support (XGETBV)
    if (__builtin_cpu_supports("avx512f")  && __builtin_cpu_supports("avx512dq") &&
        __builtin_cpu_supports("avx512bw") && __builtin_cpu_supports("avx512vl"))
        return DIA::AVX512;
    if (__builtin_cpu_supports("avx2"))   return DIA::AVX2;
    if (__builtin_cpu_supports("sse4.2")) return DIA::SSE4_2;
    return DIA::NONE;
  #elif defined(_MSC_VER)
    return detail::detect_x86_msvc();
  #endif

#else
    return DIA::NONE;
#endif
}

inline const char* to_string(DIA d) {
    switch (d) {
        case DIA::AVX512: return "AVX-512";
        case DIA::AVX2:   return "AVX2";
        case DIA::SSE4_2: return "SSE4.2";
        case DIA::NEON:   return "NEON";
        default:          return "scalar";
    }
}

DIA active_arch();

void init();

// JSON Value Types
enum class JsonType {
    Null,
    Bool,
    Number,
    String,
    Array,
    Object
};

// Forward declarations
class JsonValue;
class JsonDocument;

// JSON Value - variant type holding different JSON types
class JsonValue {
public:
    using Object = std::unordered_map<std::string, JsonValue>;
    using Array = std::vector<JsonValue>;

    JsonValue() : type_(JsonType::Null) {}
    JsonValue(std::nullptr_t) : type_(JsonType::Null) {}
    JsonValue(bool b) : type_(JsonType::Bool), bool_val_(b) {}
    JsonValue(double n) : type_(JsonType::Number), number_val_(n) {}
    JsonValue(std::string s) : type_(JsonType::String), string_val_(std::move(s)) {}
    JsonValue(Array a) : type_(JsonType::Array), array_val_(std::make_shared<Array>(std::move(a))) {}
    JsonValue(Object o) : type_(JsonType::Object), object_val_(std::make_shared<Object>(std::move(o))) {}

    JsonType type() const { return type_; }

    bool is_null() const { return type_ == JsonType::Null; }
    bool is_bool() const { return type_ == JsonType::Bool; }
    bool is_number() const { return type_ == JsonType::Number; }
    bool is_string() const { return type_ == JsonType::String; }
    bool is_array() const { return type_ == JsonType::Array; }
    bool is_object() const { return type_ == JsonType::Object; }

    bool as_bool() const {
        if (!is_bool()) throw std::runtime_error("Not a boolean");
        return bool_val_;
    }

    double as_number() const {
        if (!is_number()) throw std::runtime_error("Not a number");
        return number_val_;
    }

    const std::string& as_string() const {
        if (!is_string()) throw std::runtime_error("Not a string");
        return string_val_;
    }

    const Array& as_array() const {
        if (!is_array()) throw std::runtime_error("Not an array");
        return *array_val_;
    }

    const Object& as_object() const {
        if (!is_object()) throw std::runtime_error("Not an object");
        return *object_val_;
    }

    // Array/Object access
    JsonValue& operator[](size_t index) {
        if (!is_array()) throw std::runtime_error("Not an array");
        return (*array_val_)[index];
    }

    JsonValue& operator[](const std::string& key) {
        if (!is_object()) throw std::runtime_error("Not an object");
        return (*object_val_)[key];
    }

private:
    JsonType type_;
    bool bool_val_ = false;
    double number_val_ = 0.0;
    std::string string_val_;
    std::shared_ptr<Array> array_val_;
    std::shared_ptr<Object> object_val_;
};

// SIMD scanning state for AVX2
namespace avx2
{
    struct State
    {
        uint64_t prev_escaped = 0;
        uint64_t prev_in_string = 0;
        uint64_t prev_scalar = 0;
    };

    // Find structural characters using AVX2
    inline void find_structural_bits(const uint8_t* data, size_t len,
                                     uint64_t& structural_chars,
                                     State& state);

    // Classify characters for JSON parsing
    inline __m256i classify_chars(const __m256i input);

} // namespace avx2

// JSON Parser Exception
class JsonParseError : public std::runtime_error {
public:
    JsonParseError(const std::string& msg, size_t pos)
        : std::runtime_error("JSON parse error at position " + std::to_string(pos) + ": " + msg)
        , position_(pos) {}

    size_t position() const { return position_; }

private:
    size_t position_;
};

// Main JSON Document class
class JsonDocument {
public:
    JsonDocument() = default;

    // Parse JSON from string
    static JsonDocument parse(std::string_view json);

    // Get root value
    const JsonValue& root() const { return root_; }
    JsonValue& root() { return root_; }

    // Check if document is valid
    bool is_valid() const { return !root_.is_null() || explicit_null_; }

private:
    JsonValue root_;
    bool explicit_null_ = false;

    // Parser state
    struct Parser {
        std::string_view json;
        size_t pos = 0;

        Parser(std::string_view j) : json(j) {}

        // Skip whitespace
        void skip_whitespace();

        // Parse value (entry point)
        JsonValue parse_value();

        // Parse specific types
        JsonValue parse_null();
        JsonValue parse_bool();
        JsonValue parse_number();
        JsonValue parse_string();
        JsonValue parse_array();
        JsonValue parse_object();

        // Helpers
        char peek() const { return pos < json.size() ? json[pos] : '\0'; }
        char advance() { return pos < json.size() ? json[pos++] : '\0'; }
        void expect(char c);
        bool match(std::string_view str);
    };
};
