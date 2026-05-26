#pragma once
#include <cstddef>
#include <cstdint>

namespace constants {

inline constexpr uint16_t MAX_INSTRUMENTS  = 200;   // 2 venues × 100 pairs
inline constexpr uint16_t PAIRS_PER_VENUE  = 100;
inline constexpr float    FEE_RATE         = 0.0004f;  // 4 bps maker/taker
inline constexpr float    MIN_PROFIT_USD   = 0.50f;    // minimum net profit to fire a signal (absolute)
inline constexpr float    MIN_PROFIT_BPS   = 1.0f;     // minimum net profit as bps of mid-price (relative)
inline constexpr float    MAX_POSITION_PCT = 0.10f;    // max 10% of venue USDT balance per trade
inline constexpr size_t   RING_BUFFER_SIZE = 65536;    // SPSC buffer capacity (must be power of 2)
inline constexpr size_t   MAX_TRADES       = 2'000'000;
inline constexpr uint64_t MAX_STALE_NS     = 5'000'000'000ULL;  // 5 s: reject cross-venue signal if other venue silent longer than this

} // namespace constants
