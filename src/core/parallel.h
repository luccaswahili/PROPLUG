// Parallel-for over an index range on a persistent thread pool.
// Hosts render several frames at once on their own threads; each call's thread also works on its
// own range, so concurrent callers never wait on each other and nothing is created per call.
#pragma once
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace lm {

class ThreadPool {
public:
    static ThreadPool& get() {
        static ThreadPool pool;
        return pool;
    }

    int size() const { return int(workers_.size()) + 1; }

    // Runs body(lo, hi) over [begin, end) in chunks of `chunk`, on the pool and the calling thread.
    void run(int begin, int end, int chunk, const std::function<void(int, int)>& body) {
        auto job = std::make_shared<Job>();
        job->body = &body;
        job->next = begin;
        job->end = end;
        job->chunk = chunk;
        {
            std::lock_guard<std::mutex> lock(m_);
            jobs_.push_back(job);
        }
        cv_.notify_all();
        work(*job);
        // wait for helpers still inside a chunk of this job
        while (job->working.load(std::memory_order_acquire) > 0) std::this_thread::yield();
        retire(job);
    }

    ~ThreadPool() {
        {
            std::lock_guard<std::mutex> lock(m_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& t : workers_) t.join();
    }

private:
    struct Job {
        const std::function<void(int, int)>* body = nullptr;
        std::atomic<int> next{0};
        int end = 0, chunk = 1;
        std::atomic<int> working{0};
    };

    ThreadPool() {
        int n = std::max(0, int(std::thread::hardware_concurrency()) - 1);
        for (int i = 0; i < n; ++i) workers_.emplace_back([this] { loop(); });
    }

    static void work(Job& job) {
        for (;;) {
            int lo = job.next.fetch_add(job.chunk, std::memory_order_relaxed);
            if (lo >= job.end) return;
            (*job.body)(lo, std::min(job.end, lo + job.chunk));
        }
    }

    void retire(const std::shared_ptr<Job>& job) {
        std::lock_guard<std::mutex> lock(m_);
        jobs_.erase(std::remove(jobs_.begin(), jobs_.end(), job), jobs_.end());
    }

    void loop() {
        for (;;) {
            std::shared_ptr<Job> job;
            {
                std::unique_lock<std::mutex> lock(m_);
                cv_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
                if (stop_) return;
                // first job that still has work; drop exhausted ones from the queue
                while (!jobs_.empty() && jobs_.front()->next.load(std::memory_order_relaxed) >= jobs_.front()->end)
                    jobs_.pop_front();
                if (jobs_.empty()) continue;
                job = jobs_.front();
                job->working.fetch_add(1, std::memory_order_acq_rel);
            }
            work(*job);
            job->working.fetch_sub(1, std::memory_order_acq_rel);
        }
    }

    std::vector<std::thread> workers_;
    std::deque<std::shared_ptr<Job>> jobs_;
    std::mutex m_;
    std::condition_variable cv_;
    bool stop_ = false;
};

template <class F>
void parallelFor(int begin, int end, F&& f) {
    const int n = end - begin;
    if (n <= 0) return;
    ThreadPool& pool = ThreadPool::get();
    if (pool.size() == 1 || n < 16) {
        for (int i = begin; i < end; ++i) f(i);
        return;
    }
    const int chunk = std::max(1, n / (pool.size() * 4));
    const std::function<void(int, int)> body = [&f](int lo, int hi) {
        for (int i = lo; i < hi; ++i) f(i);
    };
    pool.run(begin, end, chunk, body);
}

}  // namespace lm
