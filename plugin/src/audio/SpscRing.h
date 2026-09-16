#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <thread>
#include <type_traits>
#include <vector>

namespace gbtts {

// Lock-free single-producer / single-consumer ring for trivially copyable T.
//
// Capacity is a power of two; indices are monotonically increasing size_t
// counters masked on access, so "full" and "empty" never alias and no slot is
// wasted. Exactly one thread may call the write side and exactly one thread
// may call the read side AT A TIME - handing the read side between threads is
// allowed when the hand-over is serialised by the caller (TtsAudio does it
// under its reader spinlock).
template <typename T>
class SpscRing
{
public:
    explicit SpscRing(size_t capacityPow2)
    {
        size_t cap = 1;
        while (cap < capacityPow2) cap <<= 1;
        m_buf.resize(cap);
        m_mask = cap - 1;
    }

    size_t capacity() const { return m_mask + 1; }

    size_t readAvailable() const
    {
        return m_w.load(std::memory_order_acquire) - m_r.load(std::memory_order_acquire);
    }

    size_t writeAvailable() const { return capacity() - readAvailable(); }

    // ---- producer --------------------------------------------------------
    size_t write(const T *src, size_t n)
    {
        const size_t w = m_w.load(std::memory_order_relaxed);
        const size_t r = m_r.load(std::memory_order_acquire);
        n = std::min(n, capacity() - (w - r));
        copyIn(w, src, n);
        m_w.store(w + n, std::memory_order_release);
        return n;
    }

    // ---- consumer --------------------------------------------------------
    size_t read(T *dst, size_t n)
    {
        const size_t r = m_r.load(std::memory_order_relaxed);
        const size_t w = m_w.load(std::memory_order_acquire);
        n = std::min(n, w - r);
        copyOut(r, dst, n);
        m_r.store(r + n, std::memory_order_release);
        return n;
    }

    // Look at the element `offset` places after the read head without
    // consuming. Returns false when fewer elements are available.
    bool peek(size_t offset, T &out) const
    {
        const size_t r = m_r.load(std::memory_order_relaxed);
        const size_t w = m_w.load(std::memory_order_acquire);
        if (offset >= w - r) return false;
        out = m_buf[(r + offset) & m_mask];
        return true;
    }

    size_t discard(size_t n)
    {
        const size_t r = m_r.load(std::memory_order_relaxed);
        const size_t w = m_w.load(std::memory_order_acquire);
        n = std::min(n, w - r);
        m_r.store(r + n, std::memory_order_release);
        return n;
    }

private:
    void copyIn(size_t w, const T *src, size_t n)
    {
        const size_t pos = w & m_mask;
        const size_t first = std::min(n, capacity() - pos);
        std::memcpy(&m_buf[pos], src, first * sizeof(T));
        if (n > first) std::memcpy(&m_buf[0], src + first, (n - first) * sizeof(T));
    }

    void copyOut(size_t r, T *dst, size_t n) const
    {
        const size_t pos = r & m_mask;
        const size_t first = std::min(n, capacity() - pos);
        std::memcpy(dst, &m_buf[pos], first * sizeof(T));
        if (n > first) std::memcpy(dst + first, &m_buf[0], (n - first) * sizeof(T));
    }

    std::vector<T> m_buf;
    size_t m_mask = 0;
    alignas(64) std::atomic<size_t> m_w{0};
    alignas(64) std::atomic<size_t> m_r{0};
};

// Minimal spinlock for the audio-thread reader hand-over. Critical sections are
// a memcpy of at most one TeamSpeak block, so spinning is cheaper than any
// kernel object and can never block an audio thread for long.
class SpinLock
{
public:
    void lock()
    {
        for (int spins = 0; m_flag.test_and_set(std::memory_order_acquire); ++spins) {
            if (spins > 64) std::this_thread::yield();
        }
    }
    void unlock() { m_flag.clear(std::memory_order_release); }

private:
    std::atomic_flag m_flag = ATOMIC_FLAG_INIT;
};

} // namespace gbtts
