/*
 * af_threads.h -- minimal, dependency-free work distribution for the kernel.
 *
 * The kernel deliberately does not own a thread pool: callers (the Android app) already have
 * a scheduler, and spawning std::thread per stage keeps the ABI surface and the memory
 * footprint honest. Every use is "fork-join inside one API call", so the engine handle stays
 * single-threaded from the caller's point of view.
 */
#ifndef AF_THREADS_H
#define AF_THREADS_H

#include <thread>
#include <vector>
#include <atomic>
#include <functional>
#include <algorithm>
#include <exception>
#include <mutex>

namespace af {

/** Number of worker threads to use, clamped so we never oversubscribe a big.LITTLE CPU. */
inline int defaultThreadCount() {
    unsigned hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 4;
    return static_cast<int>(std::max(1u, std::min(hw, 8u)));
}

/**
 * Runs `body(i0, i1)` over disjoint index ranges, one range per thread.
 * `cancel` is polled between chunks so a user-initiated cancel aborts the stage promptly.
 * Exceptions from `body` are captured and rethrown on the calling thread.
 */
inline void parallelFor(int count, int threads, const std::function<void(int, int)>& body,
                        const std::function<bool()>& cancel = nullptr) {
    if (count <= 0) return;
    if (threads <= 1 || count == 1) {
        if (cancel && cancel()) return;
        body(0, count);
        return;
    }

    const int n = std::min(threads, count);
    const int chunk = (count - 1) / n + 1;  // avoids overflow in count + n - 1
    std::vector<std::thread> pool;
    pool.reserve(static_cast<size_t>(n));
    std::atomic<bool> stopped{false};
    std::mutex exceptionMutex;
    std::exception_ptr workerException;

    auto run = [&](int i0, int i1) {
        if (stopped.load(std::memory_order_acquire)) return;
        try {
            if (cancel && cancel()) {
                stopped.store(true, std::memory_order_release);
                return;
            }
            if (!stopped.load(std::memory_order_acquire)) body(i0, i1);
        } catch (...) {
            {
                std::lock_guard<std::mutex> lock(exceptionMutex);
                if (!workerException) workerException = std::current_exception();
            }
            stopped.store(true, std::memory_order_release);
        }
    };

    try {
        for (int t = 0; t < n; ++t) {
            if (stopped.load(std::memory_order_acquire)) break;
            const int i0 = t * chunk;
            const int i1 = std::min(count, i0 + chunk);
            if (i0 >= i1) break;
            pool.emplace_back(run, i0, i1);
        }
    } catch (...) {
        /* std::thread's destructor terminates the process if it is still joinable. If worker
           creation fails part-way through, stop and join the workers already started first. */
        stopped.store(true, std::memory_order_release);
        for (std::thread& th : pool) if (th.joinable()) th.join();
        throw;
    }

    for (std::thread& th : pool) if (th.joinable()) th.join();
    if (workerException) std::rethrow_exception(workerException);
}

} // namespace af
#endif /* AF_THREADS_H */
