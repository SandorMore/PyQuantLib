#pragma once

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

// JSON Value - compact tagged union (40 bytes). The variant index order must
// match JsonType so type() is a plain cast.
class JsonValue {
public:
    using Object = std::unordered_map<std::string, JsonValue>;
    using Array = std::vector<JsonValue>;

    JsonValue() = default;
    JsonValue(std::nullptr_t) {}
    JsonValue(bool b) : data_(b) {}
    JsonValue(double n) : data_(n) {}
    JsonValue(std::string s) : data_(std::move(s)) {}
    JsonValue(Array a) : data_(std::make_shared<Array>(std::move(a))) {}
    JsonValue(Object o) : data_(std::make_shared<Object>(std::move(o))) {}

    JsonType type() const { return static_cast<JsonType>(data_.index()); }

    bool is_null() const { return type() == JsonType::Null; }
    bool is_bool() const { return type() == JsonType::Bool; }
    bool is_number() const { return type() == JsonType::Number; }
    bool is_string() const { return type() == JsonType::String; }
    bool is_array() const { return type() == JsonType::Array; }
    bool is_object() const { return type() == JsonType::Object; }

    bool as_bool() const {
        if (!is_bool()) throw std::runtime_error("Not a boolean");
        return std::get<bool>(data_);
    }

    double as_number() const {
        if (!is_number()) throw std::runtime_error("Not a number");
        return std::get<double>(data_);
    }

    const std::string& as_string() const {
        if (!is_string()) throw std::runtime_error("Not a string");
        return std::get<std::string>(data_);
    }

    const Array& as_array() const {
        if (!is_array()) throw std::runtime_error("Not an array");
        return *std::get<std::shared_ptr<Array>>(data_);
    }

    const Object& as_object() const {
        if (!is_object()) throw std::runtime_error("Not an object");
        return *std::get<std::shared_ptr<Object>>(data_);
    }

    // Array/Object access
    JsonValue& operator[](size_t index) {
        if (!is_array()) throw std::runtime_error("Not an array");
        return (*std::get<std::shared_ptr<Array>>(data_))[index];
    }

    JsonValue& operator[](const std::string& key) {
        if (!is_object()) throw std::runtime_error("Not an object");
        return (*std::get<std::shared_ptr<Object>>(data_))[key];
    }

private:
    std::variant<std::nullptr_t, bool, double, std::string,
                 std::shared_ptr<Array>, std::shared_ptr<Object>> data_;
};

// SIMD structural indexing (stage 1): the offset of every structural character
// ({}[],:), every opening quote and the first byte of every other scalar, outside
// strings. Uses AVX2 when the CPU supports it, a scalar kernel otherwise.
namespace json_detail
{
    struct IndexView {
        const uint32_t* data;
        size_t size;
    };

    // Indexes `json` into a thread-local buffer. The view stays valid until the
    // next call on this thread or release_index_buffer().
    IndexView index_document(std::string_view json);

    // Frees the thread-local buffer if a large document made it grow.
    void release_index_buffer();

    // Copying variant for tests and benchmarks; `use_simd = false` forces the scalar kernel.
    void find_structural_indexes(std::string_view json, std::vector<uint32_t>& out, bool use_simd);
} // namespace json_detail

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
};
