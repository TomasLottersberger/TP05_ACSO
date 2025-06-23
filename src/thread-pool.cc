/**
 * File: thread-pool.cc
 * --------------------
 * Presents the implementation of the ThreadPool class.
 */

#include "thread-pool.h"
using namespace std;

ThreadPool::ThreadPool(size_t numThreads) : wts(numThreads), done(false) {
    
    dt = thread(&ThreadPool::dispatcher, this);
    
    for (size_t i = 0; i < numThreads; ++i) {
        wts[i].available = true;
        wts[i].ts = thread(&ThreadPool::worker, this, static_cast<int>(i));
    }
}

void ThreadPool::schedule(const function<void(void)>& thunk) {
    {
        lock_guard<mutex> lock(queueLock);
        taskQueue.push(thunk);
    }
    taskCount.signal(); // notify dispatcher of new task
}

void ThreadPool::dispatcher() {
    while (true) {
        taskCount.wait();
        if (done && taskQueue.empty()) break;

        function<void(void)> next;
        {
            lock_guard<mutex> lock(queueLock);
            if (taskQueue.empty()) continue;
            next = move(taskQueue.front());
            taskQueue.pop();
        }

        for (auto& w : wts) { // assign to an available worker
            if (w.available) {
                w.available = false;
                w.thunk = move(next);
                busyCount.fetch_add(1, memory_order_relaxed);
                w.sem.signal();
                break;
            }
        }
    }
}

void ThreadPool::worker(int id) {
    worker_t& w = wts[id];
    while (true) {
        w.sem.wait();
        if (done) break;
        // execute assigned task
        w.thunk();
        busyCount.fetch_sub(1, memory_order_relaxed);
        {
            unique_lock<mutex> lock(waitLock);
            w.available = true;
            if (busyCount.load(memory_order_relaxed) == 0 && taskQueue.empty()) {
                waitCv.notify_all();
            }
        }
    }
}

void ThreadPool::wait() {
    unique_lock<mutex> lock(waitLock);
    waitCv.wait(lock, [this] {
        return busyCount.load(memory_order_relaxed) == 0 && taskQueue.empty();
    });
}

ThreadPool::~ThreadPool() {
    wait();
    done = true;
    taskCount.signal(); // wake dispatcher
    for (auto& w : wts) {
        w.sem.signal();  // wake workers
    }

    if (dt.joinable()) dt.join(); // join dispatcher
    
    for (auto& w : wts) { // join workers
        if (w.ts.joinable()) w.ts.join();
    }
}
