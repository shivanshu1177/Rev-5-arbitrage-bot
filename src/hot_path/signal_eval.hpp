#pragma once
#include "market_state.hpp"
#include "common/constants.hpp"
#include "common/types.hpp"
#include <bit>
#include <cstdint>

namespace arb {

// Result of one signal evaluation. 'valid' is set without any conditional branch.
struct ArbSignal {
    uint16_t buy_idx;    // MarketState index of the venue to buy on
    uint16_t sell_idx;   // MarketState index of the venue to sell on
    uint8_t  buy_venue;  // 1=C2C1 or 2=C2C2
    uint8_t  sell_venue;
    uint8_t  valid;      // 1 if profitable above threshold, 0 otherwise (branchless)
    uint8_t  _pad;
    float    net_profit; // per-unit profit after both legs' fees
};

// Evaluate whether the tick creates a cross-venue arbitrage opportunity.
//
// Called after MarketState::update() has already written the new tick.
// Checks both directions for the same pair between the two venues.
//
// Branchless design:
//   Stage 1 — direction selection: (p1 >= p2) compiles to FCMP + CSEL on ARM64, no branch.
//   Stage 2 — sign-bit validity: std::bit_cast to uint32_t, shift sign bit, XOR → 1 if profit>0.
//   Stage 3 — threshold check: compare uint32 reps of two positive IEEE754 floats.
//             (positive IEEE754 floats sort identically to their uint32 representations)
//
// Guard: if other_venue bid == 0.0f (no tick seen yet), all profits are negative → valid = 0.
[[nodiscard]] inline ArbSignal evaluate_signal(
    const MarketTick& tick, const MarketState& state) noexcept
{
    const uint16_t my_idx = MarketState::map_to_index(tick.venue, tick.pair_id);

    // Branchless venue flip: C2C1(1) ↔ C2C2(2) via 3 - venue.
    const uint8_t  other_v   = static_cast<uint8_t>(3u - tick.venue);
    const uint16_t other_idx = MarketState::map_to_index(other_v, tick.pair_id);

    // Staleness gate: suppress if other venue slot was never written or data is too old.
    // uint64_t underflow if other's ts > tick's ts is safe: result exceeds MAX_STALE_NS, suppressing.
    if (state.last_ts_ns[other_idx] == 0 ||
        tick.timestamp_ns - state.last_ts_ns[other_idx] > state.max_stale_ns) {
        return ArbSignal{};
    }

    const float bid_here  = state.bids[my_idx];
    const float ask_here  = state.asks[my_idx];
    const float bid_other = state.bids[other_idx];
    const float ask_other = state.asks[other_idx];

    // Direction 1: BUY on tick's venue (pay ask_here), SELL on other venue (receive bid_other).
    const float fee1 = constants::FEE_RATE * (ask_here + bid_other);
    const float p1   = bid_other - ask_here - fee1;

    // Direction 2: BUY on other venue (pay ask_other), SELL on tick's venue (receive bid_here).
    const float fee2 = constants::FEE_RATE * (ask_other + bid_here);
    const float p2   = bid_here - ask_other - fee2;

    // Stage 1: pick the better direction (ARM64: FCMP + CSEL, zero branches).
    const bool  use_dir1 = (p1 >= p2);
    const float best     = use_dir1 ? p1 : p2;

    const uint16_t b_idx = use_dir1 ? my_idx    : other_idx;
    const uint16_t s_idx = use_dir1 ? other_idx : my_idx;
    const uint8_t  b_v   = use_dir1 ? tick.venue : other_v;
    const uint8_t  s_v   = use_dir1 ? other_v    : tick.venue;

    // Stage 2: sign-bit validity check (integer pipeline).
    // IEEE 754 positive float → sign bit = 0 → (bits >> 31) = 0 → XOR 1 = 1 (valid).
    // Negative or -0 → sign bit = 1 → (bits >> 31) = 1 → XOR 1 = 0 (invalid).
    const uint32_t bits     = std::bit_cast<uint32_t>(best);
    const uint32_t sign_inv = (bits >> 31u) ^ 1u;  // 1 if best > 0 (or +0), else 0

    // Stage 3: threshold check — adaptive floor = max(MIN_PROFIT_USD, mid × MIN_PROFIT_BPS).
    // At BTC ($77k): bps_floor = $7.70, replaces $0.50. At SOL ($85): $0.50 still applies.
    const float mid       = (bid_here + bid_other + ask_here + ask_other) * 0.25f;
    const float bps_floor = mid * constants::MIN_PROFIT_BPS / 10000.0f;
    const float floor_val = (bps_floor > constants::MIN_PROFIT_USD) ? bps_floor : constants::MIN_PROFIT_USD;
    const uint32_t thresh = std::bit_cast<uint32_t>(floor_val);
    const uint8_t  valid  = static_cast<uint8_t>(sign_inv & (bits > thresh ? 1u : 0u));

    return ArbSignal{b_idx, s_idx, b_v, s_v, valid, 0, best};
}

} // namespace arb
