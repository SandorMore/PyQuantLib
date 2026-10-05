#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <exception>
#include <concepts>
#include <type_traits>
#include <bit>
#include <stdexcept>
#include <optional>
#include <utility>

template <typename T>
    requires std::is_copy_constructible_v<T> || std::is_move_constructible_v<T>
class SPSCQueue final 
{
public:
    explicit SPSCQueue(size_t capacity_) 
        : capacity{capacity_}, capacity_mask{capacity_ - 1}, buffer{std::make_unique<T[]>(capacity_)}
    {
        if(capacity < 2 || !std::has_single_bit(capacity))
        {
            throw std::invalid_argument("capacity must be a power of two and at least 2");
        }
    }

    SPSCQueue(const SPSCQueue&) = delete;
    SPSCQueue& operator=(const SPSCQueue&) = delete;
    SPSCQueue(SPSCQueue&&) noexcept = delete;
    SPSCQueue& operator=(SPSCQueue&&) noexcept = delete;

    template <typename... Args>
    bool try_emplace(Args&&... args)
    {
        const size_t t = tail.load(std::memory_order_relaxed);
        const size_t h = head.load(std::memory_order_acquire);

        if(((t + 1) & capacity_mask) == h)
            return false;

        buffer[t] = T(std::forward<Args>(args)...);
        tail.store((t + 1) & capacity_mask, std::memory_order_release);
        return true;
    }

    [[nodiscard]] std::optional<T> try_pop()
    {
        const size_t h = head.load(std::memory_order_relaxed);
        const size_t t = tail.load(std::memory_order_acquire);

        if(t == h)
            return std::nullopt;

        T result = std::move(buffer[h]);
        head.store((h + 1) & capacity_mask, std::memory_order_release);
        return std::optional<T>{std::move(result)};
    }

private:
    static constexpr size_t cache_line{64};
    
    const size_t capacity_mask;
    const size_t capacity;

    alignas(cache_line) std::atomic<size_t> head{0};
    alignas(cache_line) std::atomic<size_t> tail{0};

    const std::unique_ptr<T[]> buffer;
};