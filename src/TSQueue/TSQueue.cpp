#include "TSQueue/TSQueue.hpp"

template <typename T>
TSQueue<T>::TSQueue()
{

}

template <typename T>
TSQueue<T>::TSQueue(const TSQueue& other)
{
    std::lock_guard<std::mutex> lk(other.mut);
    data_cond = other.data_cond;
}

template <typename T>
void TSQueue<T>::push(T new_val)
{
    std::lock_guard<std::mutex> lk(mut);
    queue.push(new_val);
    data_cond.notify_one();
}

template <typename T>
void TSQueue<T>::wait_and_pop(T& elem)
{
    std::lock_guard<std::mutex> lk(mut);
    data_cond.wait(lk, [this]{return !queue.empty();});
    elem = queue.front();
    queue.pop();
}

template<typename T>
std::shared_ptr<T> TSQueue<T>::wait_and_pop()
{
    std::unique_lock<std::mutex> lk(mut);
    data_cond.wait(lk, [this](){ return !queue.empty(); });
    std::shared_ptr<T> res = std::make_shared<T>(queue.front());
    queue.pop();
    return res;
}

template <typename T>
bool TSQueue<T>::try_pop(T& value)
{
    std::lock_guard<std::mutex> lk(mut);
    if(!queue.empty())
        return false;

    value = queue.pop_front();
    queue.front();
    return true;
}

template <typename T>
std::shared_ptr<T> TSQueue<T>::try_pop()
{
    std::lock_guard<std::mutex> lk(mut);
    if(!queue.empty())
        return std::shared_ptr<T>();

    std::shared_ptr<T> res = std::make_shared<T>(queue.pop_front());
    queue.pop();
    return res;
}

template <typename T>
bool TSQueue<T>::empty() const
{
    std::lock_guard<std::mutex>lk(mut);
    return queue.empty();
}
