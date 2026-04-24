#include "matata/bandwidth.hpp"

#include <windows.h>

#include <algorithm>
#include <thread>

namespace matata {

RateLimiter::RateLimiter(int64_t bytesPerSec)
    : m_rate(bytesPerSec), m_last(std::chrono::steady_clock::now()) {
    if (bytesPerSec > 0) m_tokens = (double)bytesPerSec; // one second of burst
}

void RateLimiter::setRate(int64_t bytesPerSec) {
    m_rate.store(bytesPerSec);
}

void RateLimiter::acquire(int64_t bytes) {
    if (m_rate.load() <= 0) return;
    if (bytes <= 0) return;

    while (true) {
        int64_t rate = m_rate.load();
        if (rate <= 0) return;

        std::unique_lock<std::mutex> lk(m_mu);

        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - m_last).count();
        m_last = now;
        m_tokens = std::min<double>(m_tokens + elapsed * (double)rate,
                                    (double)rate);  // cap burst at 1s
        if (m_tokens >= (double)bytes) {
            m_tokens -= (double)bytes;
            return;
        }

        double needed = (double)bytes - m_tokens;
        double waitSec = needed / (double)rate;
        lk.unlock();

        // Sleep at most 100ms per pass so rate changes are picked up quickly.
        DWORD ms = (DWORD)std::min<double>(waitSec * 1000.0, 100.0);
        if (ms == 0) ms = 1;
        Sleep(ms);
    }
}

}
