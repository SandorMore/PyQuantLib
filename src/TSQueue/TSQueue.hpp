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

    void push(T);
    void wait_and_pop(T&);
    std::shared_ptr<T> wait_and_pop();
    bool try_pop(T& value);
    std::shared_ptr<T> try_pop();
    bool empty() const;
private:
    mutable std::mutex mut;
    std::deque<T> queue;
    std::condition_variable data_cond;
};
