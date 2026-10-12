#include <queue>
#include <mutex>
#include <memory>
#include <condition_variable>

template <typename T>
class TSQueue final
{
public:
    TSQueue();
    TSQueue(const TSQueue&);

    void push(T new_val);
    void wait_and_pop();
private:
    mutable std::mutex mut;
    std::deque<T> queue;
    std::condition_variable data_cond;
};
