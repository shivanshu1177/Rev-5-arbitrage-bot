#pragma once
#include "common/types.hpp"
#include "common/constants.hpp"
#include <cstring>

namespace arb {

// Tracks the live best bid/ask and top-of-book quantity for every venue+pair.
// Total size: 200*4*4 + 200*8 = ~4.8 KB — fits comfortably in M2 L1 data cache (128 KB).
//
// Index mapping (constexpr, branch-free):
//   C2C1 venue (1) → indices   0 ..  99
//   C2C2 venue (2) → indices 100 .. 199
//   formula: (venue - 1) * PAIRS_PER_VENUE + pair_id
struct alignas(64) MarketState {
    float    bids[constants::MAX_INSTRUMENTS];              // best bid price per index
    float    asks[constants::MAX_INSTRUMENTS];              // best ask price per index
    float    bid_qty[constants::MAX_INSTRUMENTS];           // top-of-book bid quantity
    float    ask_qty[constants::MAX_INSTRUMENTS];           // top-of-book ask quantity
    uint64_t last_ts_ns[constants::MAX_INSTRUMENTS];  // tick.timestamp_ns of last update per slot (0 = never seen)
    uint64_t max_stale_ns = constants::MAX_STALE_NS;  // configurable threshold; set from Config after reset()

    void reset() noexcept {
        std::memset(bids,        0, sizeof(bids));
        std::memset(asks,        0, sizeof(asks));
        std::memset(bid_qty,     0, sizeof(bid_qty));
        std::memset(ask_qty,     0, sizeof(ask_qty));
        std::memset(last_ts_ns,  0, sizeof(last_ts_ns));
    }

    [[nodiscard]] static constexpr uint16_t map_to_index(
        uint8_t venue, uint16_t pair_id) noexcept
    {
        // Hard runtime guards — survive -DNDEBUG, unlike assert().
        // All hot-path callers validate before calling; these clamp on malformed
        // config-sourced inputs so the returned index is always in [0, MAX_INSTRUMENTS).
        if (venue < 1u || venue > 2u)              [[unlikely]] venue   = 1;
        if (pair_id >= constants::PAIRS_PER_VENUE) [[unlikely]] pair_id = 0;
        return static_cast<uint16_t>((venue - 1u) * constants::PAIRS_PER_VENUE + pair_id);
    }

    void update(const MarketTick& tick) noexcept {
        // Belt-and-suspenders guard: process_tick() validates before calling update(),
        // but assert() is stripped by -DNDEBUG in release builds.  This if-guard
        // is never taken on valid data and costs nothing on the happy path.
        if (tick.venue < 1u || tick.venue > 2u ||
            tick.pair_id >= constants::PAIRS_PER_VENUE) [[unlikely]] return;
        const uint16_t idx = map_to_index(tick.venue, tick.pair_id);
        bids[idx]       = tick.best_bid;
        asks[idx]       = tick.best_ask;
        bid_qty[idx]    = tick.bid_qty;
        ask_qty[idx]    = tick.ask_qty;
        last_ts_ns[idx] = tick.timestamp_ns;
    }
};

} // namespace arb
