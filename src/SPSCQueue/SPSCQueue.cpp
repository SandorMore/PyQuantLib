#include "SPSCQueue.hpp"
#include <assert.h>

bool TEST1()
{
    bool passed = false;
    SPSCQueue<int> queue{4};

    assert(!queue.try_pop());

    assert(queue.try_emplace(10));
    assert(queue.try_emplace(20));
    assert(queue.try_emplace(30));
    assert(!queue.try_emplace(40));

    assert(queue.try_pop() == 10);
    assert(queue.try_pop() == 20);
    assert(queue.try_pop() == 30);
    assert(!queue.try_pop());

    assert(queue.try_emplace(50));
    assert(queue.try_emplace(60));
    assert(queue.try_pop() == 50);
    assert(queue.try_pop() == 60);
}