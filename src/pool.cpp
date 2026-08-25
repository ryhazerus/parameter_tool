#include "pool.hpp"

#include <algorithm>
#include <atomic>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

namespace pt {

unsigned DefaultWorkers() {
    const unsigned hw = std::thread::hardware_concurrency();
    if (hw == 0) return 1;
    return std::min(hw, 8u);
}

unsigned ClampWorkers(long requested) {
    if (requested < 1) return 1;
    if (requested > 64) return 64;
    return static_cast<unsigned>(requested);
}

void ParallelFor(std::size_t n, unsigned workers, const std::function<void(std::size_t)>& fn) {
    if (n == 0) return;

    if (workers <= 1 || n == 1) {
        for (std::size_t i = 0; i < n; ++i) fn(i);
        return;
    }

    const unsigned count = static_cast<unsigned>(std::min<std::size_t>(workers, n));
    std::atomic<std::size_t> next{0};
    std::atomic<bool> failed{false};
    std::mutex err_mu;
    std::exception_ptr first_error;

    const auto run = [&] {
        for (;;) {
            if (failed.load(std::memory_order_relaxed)) return;
            const std::size_t i = next.fetch_add(1, std::memory_order_relaxed);
            if (i >= n) return;
            try {
                fn(i);
            } catch (...) {
                std::lock_guard<std::mutex> g(err_mu);
                if (!first_error) first_error = std::current_exception();
                failed.store(true, std::memory_order_relaxed);
                return;
            }
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(count);
    for (unsigned t = 0; t < count; ++t) threads.emplace_back(run);
    for (auto& t : threads) t.join();

    if (first_error) std::rethrow_exception(first_error);
}

}  // namespace pt
