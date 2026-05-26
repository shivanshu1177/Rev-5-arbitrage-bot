#pragma once
// ARM NEON SIMD placeholder for future batch signal evaluation.
// Current hot path evaluates one tick at a time (scalar ARM64 FCMP + CSEL).
// Future: vectorize evaluate_signal over 4 pairs simultaneously using float32x4_t,
// processing 4 bids/asks in one NEON instruction (vsubq_f32, vmulq_f32, vcgtq_f32).
#include <arm_neon.h>
#include <cstdint>

namespace simd {

// Load 4 consecutive floats from a 16-byte aligned address.
[[nodiscard]] inline float32x4_t load4(const float* __restrict__ p) noexcept {
    return vld1q_f32(p);
}

// Element-wise a > b comparison. Returns lane mask: 0xFFFFFFFF where true, 0 where false.
[[nodiscard]] inline uint32x4_t gt4(float32x4_t a, float32x4_t b) noexcept {
    return vcgtq_f32(a, b);
}

// Element-wise subtract: a - b
[[nodiscard]] inline float32x4_t sub4(float32x4_t a, float32x4_t b) noexcept {
    return vsubq_f32(a, b);
}

// Multiply: a * b
[[nodiscard]] inline float32x4_t mul4(float32x4_t a, float32x4_t b) noexcept {
    return vmulq_f32(a, b);
}

// Horizontal max across all 4 lanes
[[nodiscard]] inline float hmax4(float32x4_t v) noexcept {
    float32x2_t hi = vget_high_f32(v);
    float32x2_t lo = vget_low_f32(v);
    float32x2_t mx = vpmax_f32(lo, hi);
    mx = vpmax_f32(mx, mx);
    return vget_lane_f32(mx, 0);
}

} // namespace simd
