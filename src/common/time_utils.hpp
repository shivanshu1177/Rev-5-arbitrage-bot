#pragma once
#include <cstdint>
#include <mach/mach_time.h>

namespace time_utils {

namespace detail {
    // Set once at startup via init(). Not thread-safe to write — read is safe after init.
    inline mach_timebase_info_data_t g_timebase{};
    inline bool g_is_unity = false;  // true when numer == denom (M1/M2 always true)
} // namespace detail

// Call once before the hot loop (cold path). Not noexcept — mach_timebase_info can fail.
inline void init() {
    mach_timebase_info(&detail::g_timebase);
    // On Apple M1/M2, the Mach timebase is 1:1 (1 tick = 1 ns), so the multiply/divide
    // is redundant. We detect this at startup to keep now_ns() branch-free at runtime.
    detail::g_is_unity = (detail::g_timebase.numer == detail::g_timebase.denom);
}

// Hot path: reads ARM64 CNTVCT_EL0 via vDSO — no kernel crossing on M1/M2.
// On M1/M2 with unity timebase: one 'mrs' instruction + a conditional branch
// that the predictor will learn is always taken.
[[nodiscard]] inline uint64_t now_ns() noexcept {
    const uint64_t ticks = mach_absolute_time();
    if (detail::g_is_unity) [[likely]] {
        return ticks;
    }
    // Non-M2 fallback (Intel Mac, or edge case): scale using timebase info.
    // Avoids overflow by splitting into high/low parts.
    return (ticks / detail::g_timebase.denom) * detail::g_timebase.numer
         + (ticks % detail::g_timebase.denom) * detail::g_timebase.numer
           / detail::g_timebase.denom;
}

} // namespace time_utils
