#pragma once
// Single-Producer Single-Consumer lock-free ring buffer.
// Used only in live mode: WebSocket thread pushes ticks, engine thread pops them.
// Not used by the backtest target (FileSource feeds ticks directly).
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace arb {

template<typename T, size_t N>
struct SPSCRingBuffer {
    static_assert((N & (N - 1)) == 0, "N must be a power of 2");
    static_assert(N >= 2, "N must be at least 2");

    // Data array sits first: large enough to dominate cache, both pos fields trail it.
    T data[N];

    // Producer state — on its own 64-byte cache line to prevent false sharing.
    alignas(64) std::atomic<size_t> write_pos{0};
    char _pad_w[64 - sizeof(std::atomic<size_t>)];

    // Consumer state — on its own cache line.
    alignas(64) std::atomic<size_t> read_pos{0};
    char _pad_r[64 - sizeof(std::atomic<size_t>)];

    // Producer thread: returns false if buffer is full (caller should spin/yield).
    bool push(const T& item) noexcept {
        const size_t w = write_pos.load(std::memory_order_relaxed);
        // Acquire read_pos to synchronize with consumer's last store.
        if (w - read_pos.load(std::memory_order_acquire) >= N) {
            return false;  // full
        }
        data[w & (N - 1)] = item;
        // Release: make the data visible before advancing write_pos.
        write_pos.store(w + 1, std::memory_order_release);
        return true;
    }

    // Consumer thread: returns false if buffer is empty.
    bool pop(T& out) noexcept {
        const size_t r = read_pos.load(std::memory_order_relaxed);
        // Acquire write_pos to synchronize with producer's last store.
        if (r == write_pos.load(std::memory_order_acquire)) {
            return false;  // empty
        }
        out = data[r & (N - 1)];
        // Release: allow producer to reuse this slot.
        read_pos.store(r + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] size_t size_approx() const noexcept {
        const size_t w = write_pos.load(std::memory_order_relaxed);
        const size_t r = read_pos.load(std::memory_order_relaxed);
        return w - r;
    }

    [[nodiscard]] bool empty() const noexcept {
        return write_pos.load(std::memory_order_relaxed)
            == read_pos.load(std::memory_order_relaxed);
    }
};

} // namespace arb
