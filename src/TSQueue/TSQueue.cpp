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

template<typename T>
void TSQueue<T>::wait_and_pop()
{
    std::unique_lock<std::mutex> lk(mut);
    data_cond.wait(lk, [this](){return !queue.empty();});
    std::shared_ptr<T> res(std::make_shared<T>(queue.front()));
    queue.pop();
}




