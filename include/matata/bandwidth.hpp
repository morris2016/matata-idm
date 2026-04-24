#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>

namespace matata {

// Simple blocking token-bucket rate limiter shared across worker threads.
// `bytesPerSec <= 0` means unlimited (acquire is a no-op).
class RateLimiter {
public:
    explicit RateLimiter(int64_t bytesPerSec = 0);

    // Block until `bytes` tokens are available, then consume them.
    // Safe to call from any thread.
    void acquire(int64_t bytes);

    // Change the rate at runtime. Threads already waiting will pick up
    // the new rate on their next refill check.
    void setRate(int64_t bytesPerSec);
    int64_t rate() const { return m_rate.load(); }

private:
    std::atomic<int64_t>                  m_rate;  // bytes/sec; 0 = unlimited
    std::mutex                            m_mu;
    double                                m_tokens = 0.0;
    std::chrono::steady_clock::time_point m_last;
};

}
