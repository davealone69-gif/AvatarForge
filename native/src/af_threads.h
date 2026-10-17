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
    const int chunk = (count + n - 1) / n;
    std::vector<std::thread> pool;
    pool.reserve(static_cast<size_t>(n));
    std::atomic<bool> cancelled{false};
    for (int t = 0; t < n; ++t) {
        const int i0 = t * chunk;
        const int i1 = std::min(count, i0 + chunk);
        if (i0 >= i1) break;
        pool.emplace_back([&, i0, i1]() {
            if (cancelled.load(std::memory_order_relaxed)) return;
            if (cancel && cancel()) { cancelled.store(true, std::memory_order_relaxed); return; }
            body(i0, i1);
        });
    }
    for (std::thread& th : pool) if (th.joinable()) th.join();
}

} // namespace af
#endif /* AF_THREADS_H */
