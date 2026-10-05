#include <atomic>
#include <cstdint>
#include <memory>
#include <exception>
#include <concepts>
#include <type_traits>

template <typename T>
    requires std::is_copy_constructible_v<T> || std::is_move_constructible_v<T>
class SPSCQueue final 
{
public:
    explicit SPSCQueue(size_t capacity_) :



private:
    static constexpr size_t cache_line{64};
    constexpr size_t capacity;
    alignas(cache_line) std::atomic<size_t> head{0};
    alignas(cache_line) std::atomic<size_t> tail{0};

    std::unique_ptr<T[]> data;
};