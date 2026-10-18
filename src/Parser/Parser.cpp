#include "Parser.hpp"
#include "JsonTape.hpp"
#include <iostream>
#include <cstring>
#include <cstddef>
#include <algorithm>
#include <bit>
#include <limits>

#if defined(__x86_64__) || defined(_M_X64)
  #include <immintrin.h>
  #define PQL_X86_64 1
  #if defined(__GNUC__) || defined(__clang__)
    // Lets the AVX2 kernel exist without building the whole module with -mavx2;
    // it is only called after runtime detection says the CPU supports it.
    #define PQL_TARGET_AVX2 __attribute__((target("avx2,pclmul")))
  #else
    #define PQL_TARGET_AVX2
  #endif
#endif

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
// Stage 1: structural indexing (simdjson-style)
//
// The input is processed in 64-byte blocks. Each block is classified into
// bitmasks (one bit per byte), string regions are resolved with a prefix XOR
// over the unescaped quotes, and the positions of everything stage 2 needs to
// look at are written to an index array.
// ============================================================================

namespace {

constexpr size_t BLOCK = 64;
constexpr uint64_t ODD_BITS = 0xAAAAAAAAAAAAAAAAULL;

struct BlockMasks {
    uint64_t op;        // { } [ ] , :
    uint64_t ws;        // space \t \n \r
    uint64_t quote;     // "
    uint64_t backslash; // '\'
};

struct Stage1State {
    uint64_t next_is_escaped = 0; // last byte of previous block was an escaping backslash
    uint64_t prev_in_string = 0;  // all ones if the previous block ended inside a string
    uint64_t prev_scalar = 0;     // last byte of previous block was a non-quote scalar byte
};

// Bits of characters escaped by a backslash. Runs of backslashes alternate
// escape/escaped, so only odd-length runs escape the following character.
inline uint64_t escaped_bits(uint64_t backslash, Stage1State& s) {
    if (!backslash) {
        uint64_t escaped = s.next_is_escaped;
        s.next_is_escaped = 0;
        return escaped;
    }
    uint64_t potential_escape = backslash & ~s.next_is_escaped;
    uint64_t maybe_escaped = potential_escape << 1;
    uint64_t escape_and_terminal_code = ((maybe_escaped | ODD_BITS) - potential_escape) ^ ODD_BITS;
    uint64_t escaped = escape_and_terminal_code ^ (backslash | s.next_is_escaped);
    uint64_t escape = escape_and_terminal_code & backslash;
    s.next_is_escaped = escape >> 63;
    return escaped;
}

inline uint64_t prefix_xor_scalar(uint64_t x) {
    x ^= x << 1;
    x ^= x << 2;
    x ^= x << 4;
    x ^= x << 8;
    x ^= x << 16;
    x ^= x << 32;
    return x;
}

// Combines the classified masks into the structural bitmap of one block.
// `in_string` covers the opening quote and string contents, not the closing quote.
inline uint64_t structural_bits(const BlockMasks& m, uint64_t quote, uint64_t in_string, Stage1State& s) {
    uint64_t scalar = ~(m.op | m.ws);
    uint64_t nonquote_scalar = scalar & ~quote;
    uint64_t follows_nonquote_scalar = (nonquote_scalar << 1) | s.prev_scalar;
    s.prev_scalar = nonquote_scalar >> 63;
    uint64_t scalar_start = scalar & ~follows_nonquote_scalar;
    uint64_t string_tail = in_string ^ quote;
    return (m.op | scalar_start) & ~string_tail;
}

inline uint32_t* flatten_bits(uint32_t* out, uint32_t base, uint64_t bits) {
    if (!bits) return out;
    const int count = std::popcount(bits);
    // Unconditional writes keep the common case branch-free; the buffer has slack.
    for (int i = 0; i < 8; ++i) {
        out[i] = base + static_cast<uint32_t>(std::countr_zero(bits));
        bits &= bits - 1;
    }
    if (count > 8) {
        for (int i = 8; i < 16; ++i) {
            out[i] = base + static_cast<uint32_t>(std::countr_zero(bits));
            bits &= bits - 1;
        }
        for (int i = 16; bits; ++i) {
            out[i] = base + static_cast<uint32_t>(std::countr_zero(bits));
            bits &= bits - 1;
        }
    }
    return out + count;
}

inline BlockMasks classify_scalar(const uint8_t* p) {
    BlockMasks m{0, 0, 0, 0};
    for (size_t i = 0; i < BLOCK; ++i) {
        const uint64_t bit = 1ULL << i;
        switch (p[i]) {
            case '{': case '}': case '[': case ']': case ',': case ':': m.op |= bit; break;
            case ' ': case '\t': case '\n': case '\r': m.ws |= bit; break;
            case '"': m.quote |= bit; break;
            case '\\': m.backslash |= bit; break;
            default: break;
        }
    }
    return m;
}

inline uint32_t* process_block_scalar(const uint8_t* p, uint32_t base, uint32_t* out, Stage1State& s) {
    const BlockMasks m = classify_scalar(p);
    const uint64_t quote = m.quote & ~escaped_bits(m.backslash, s);
    const uint64_t in_string = prefix_xor_scalar(quote) ^ s.prev_in_string;
    s.prev_in_string = static_cast<uint64_t>(static_cast<int64_t>(in_string) >> 63);
    return flatten_bits(out, base, structural_bits(m, quote, in_string, s));
}

#if defined(PQL_X86_64)

PQL_TARGET_AVX2 inline uint64_t movemask64(__m256i lo, __m256i hi) {
    return static_cast<uint32_t>(_mm256_movemask_epi8(lo)) |
           (static_cast<uint64_t>(static_cast<uint32_t>(_mm256_movemask_epi8(hi))) << 32);
}

PQL_TARGET_AVX2 inline BlockMasks classify_avx2(const uint8_t* p) {
    // Nibble lookup tables: a byte is whitespace iff table[low nibble] == byte,
    // and an operator iff table[low nibble] == (byte | 0x20), which folds [ ] onto { }.
    // Bytes >= 0x80 shuffle to 0 and never match.
    const __m256i ws_table = _mm256_setr_epi8(
        ' ', 100, 100, 100, 17, 100, 113, 2, 100, '\t', '\n', 112, 100, '\r', 100, 100,
        ' ', 100, 100, 100, 17, 100, 113, 2, 100, '\t', '\n', 112, 100, '\r', 100, 100);
    const __m256i op_table = _mm256_setr_epi8(
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, ':', '{', ',', '}', 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, ':', '{', ',', '}', 0, 0);
    const __m256i lower = _mm256_set1_epi8(0x20);
    const __m256i quote = _mm256_set1_epi8('"');
    const __m256i backslash = _mm256_set1_epi8('\\');

    const __m256i lo = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p));
    const __m256i hi = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p + 32));

    BlockMasks m;
    m.ws = movemask64(_mm256_cmpeq_epi8(lo, _mm256_shuffle_epi8(ws_table, lo)),
                      _mm256_cmpeq_epi8(hi, _mm256_shuffle_epi8(ws_table, hi)));
    m.op = movemask64(_mm256_cmpeq_epi8(_mm256_or_si256(lo, lower), _mm256_shuffle_epi8(op_table, lo)),
                      _mm256_cmpeq_epi8(_mm256_or_si256(hi, lower), _mm256_shuffle_epi8(op_table, hi)));
    m.quote = movemask64(_mm256_cmpeq_epi8(lo, quote), _mm256_cmpeq_epi8(hi, quote));
    m.backslash = movemask64(_mm256_cmpeq_epi8(lo, backslash), _mm256_cmpeq_epi8(hi, backslash));
    return m;
}

PQL_TARGET_AVX2 inline uint64_t prefix_xor_clmul(uint64_t x) {
    // Carry-less multiply by all ones computes the running XOR in one instruction.
    const __m128i all_ones = _mm_set1_epi8(static_cast<char>(0xFF));
    const __m128i r = _mm_clmulepi64_si128(_mm_set_epi64x(0, static_cast<int64_t>(x)), all_ones, 0);
    return static_cast<uint64_t>(_mm_cvtsi128_si64(r));
}

PQL_TARGET_AVX2 inline uint32_t* process_block_avx2(const uint8_t* p, uint32_t base, uint32_t* out, Stage1State& s) {
    const BlockMasks m = classify_avx2(p);
    const uint64_t quote = m.quote & ~escaped_bits(m.backslash, s);
    const uint64_t in_string = prefix_xor_clmul(quote) ^ s.prev_in_string;
    s.prev_in_string = static_cast<uint64_t>(static_cast<int64_t>(in_string) >> 63);
    return flatten_bits(out, base, structural_bits(m, quote, in_string, s));
}

PQL_TARGET_AVX2 uint32_t* stage1_avx2(const uint8_t* data, size_t len, uint32_t* out, Stage1State& s) {
    size_t i = 0;
    for (; i + BLOCK <= len; i += BLOCK) {
        out = process_block_avx2(data + i, static_cast<uint32_t>(i), out, s);
    }
    if (i < len) {
        alignas(32) uint8_t tail[BLOCK];
        std::memset(tail, ' ', BLOCK);
        std::memcpy(tail, data + i, len - i);
        out = process_block_avx2(tail, static_cast<uint32_t>(i), out, s);
    }
    return out;
}

#endif

uint32_t* stage1_scalar(const uint8_t* data, size_t len, uint32_t* out, Stage1State& s) {
    size_t i = 0;
    for (; i + BLOCK <= len; i += BLOCK) {
        out = process_block_scalar(data + i, static_cast<uint32_t>(i), out, s);
    }
    if (i < len) {
        uint8_t tail[BLOCK];
        std::memset(tail, ' ', BLOCK);
        std::memcpy(tail, data + i, len - i);
        out = process_block_scalar(tail, static_cast<uint32_t>(i), out, s);
    }
    return out;
}

bool simd_available() {
#if defined(PQL_X86_64)
    const DIA arch = active_arch();
    return arch == DIA::AVX2 || arch == DIA::AVX512;
#else
    return false;
#endif
}

// Index buffer reused across parses on the same thread, so parsing a stream of
// small messages does not allocate it every time.
struct IndexBuffer {
    std::unique_ptr<uint32_t[]> data;
    size_t capacity = 0;

    uint32_t* reserve(size_t n) {
        if (n > capacity) {
            data = std::make_unique_for_overwrite<uint32_t[]>(n);
            capacity = n;
        }
        return data.get();
    }

    void trim() {
        constexpr size_t keep = size_t(1) << 20; // 4 MB
        if (capacity > keep) {
            data.reset();
            capacity = 0;
        }
    }
};

// Returns the number of indexes written to `buf`.
size_t run_stage1(std::string_view json, IndexBuffer& buf, bool use_simd) {
    if (json.size() > std::numeric_limits<uint32_t>::max() - BLOCK) {
        throw JsonParseError("Document too large (max 4 GB)", 0);
    }
    // At most one index per byte, plus slack for the unconditional writes in flatten_bits.
    uint32_t* out = buf.reserve(json.size() + BLOCK);
    const auto* data = reinterpret_cast<const uint8_t*>(json.data());
    Stage1State state;

    uint32_t* end;
#if defined(PQL_X86_64)
    if (use_simd && simd_available()) {
        end = stage1_avx2(data, json.size(), out, state);
    } else {
        end = stage1_scalar(data, json.size(), out, state);
    }
#else
    (void)use_simd;
    end = stage1_scalar(data, json.size(), out, state);
#endif

    if (state.prev_in_string) {
        throw JsonParseError("Unterminated string", json.size());
    }
    return static_cast<size_t>(end - out);
}

thread_local IndexBuffer t_index_buffer;

// Builds JsonValue trees for JsonDocument::parse.
struct JsonValueSink {
    using Value = JsonValue;
    using Array = JsonValue::Array;
    using Object = JsonValue::Object;
    using Key = std::string;

    Value make_null() { return JsonValue(nullptr); }
    Value make_bool(bool b) { return JsonValue(b); }
    Value make_double(double d) { return JsonValue(d); }
    Value make_integer(std::string_view text, uint64_t magnitude, bool negative, bool exact) {
        return JsonValue(json_detail::integer_to_double(text, magnitude, negative, exact));
    }
    Value make_string(std::string& s) { return JsonValue(std::move(s)); }
    Key make_key(std::string& s) { return std::move(s); }

    Array new_array() { return {}; }
    void push(Array& a, Value&& v) { a.push_back(std::move(v)); }
    Value finish(Array&& a) { return JsonValue(std::move(a)); }

    Object new_object() { return {}; }
    // Duplicate keys: the last one wins, like Python's json module.
    void insert(Object& o, Key&& k, Value&& v) { o.insert_or_assign(std::move(k), std::move(v)); }
    Value finish(Object&& o) { return JsonValue(std::move(o)); }
};

} // namespace

json_detail::IndexView json_detail::index_document(std::string_view json) {
    const size_t count = run_stage1(json, t_index_buffer, true);
    return {t_index_buffer.data.get(), count};
}

void json_detail::release_index_buffer() {
    t_index_buffer.trim();
}

void json_detail::find_structural_indexes(std::string_view json, std::vector<uint32_t>& out, bool use_simd) {
    IndexBuffer buf;
    const size_t n = run_stage1(json, buf, use_simd);
    out.assign(buf.data.get(), buf.data.get() + n);
}

JsonDocument JsonDocument::parse(std::string_view json) {
    struct Release {
        ~Release() { json_detail::release_index_buffer(); }
    } release;

    JsonValueSink sink;
    JsonDocument doc;
    doc.root_ = json_detail::TapeWalker<JsonValueSink>(json, json_detail::index_document(json), sink).parse_document();
    doc.explicit_null_ = doc.root_.is_null();
    return doc;
}
