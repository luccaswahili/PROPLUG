// Minimal parallel-for over an index range using std::thread.
#pragma once
#include <algorithm>
#include <thread>
#include <vector>

namespace lm {

template <class F>
void parallelFor(int begin, int end, F&& f) {
    int n = end - begin;
    if (n <= 0) return;
    int threads = std::max(1, std::min<int>(int(std::thread::hardware_concurrency()), n / 8 + 1));
    if (threads == 1) {
        for (int i = begin; i < end; ++i) f(i);
        return;
    }
    std::vector<std::thread> pool;
    int chunk = (n + threads - 1) / threads;
    for (int t = 0; t < threads; ++t) {
        int lo = begin + t * chunk, hi = std::min(end, lo + chunk);
        if (lo >= hi) break;
        pool.emplace_back([lo, hi, &f] { for (int i = lo; i < hi; ++i) f(i); });
    }
    for (auto& th : pool) th.join();
}

}  // namespace lm
