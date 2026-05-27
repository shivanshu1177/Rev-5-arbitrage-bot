#pragma once
#include <cstdint>
#include <cstddef>
#include "constants.hpp"

namespace arb {

enum class Venue : uint8_t { C2C1 = 1, C2C2 = 2 };

// Binary replay format: each record is exactly 64 bytes, one cache line.
// Memory layout (verified by static_assert below):
//   offset  0: uint64_t timestamp_ns      (8 bytes)
//   offset  8: uint8_t  venue             (1 byte)
//   offset  9: [compiler pad]             (1 byte, to align pair_id)
//   offset 10: uint16_t pair_id           (2 bytes)
//   offset 12: float    best_bid          (4 bytes)
//   offset 16: float    best_ask          (4 bytes)
//   offset 20: float    bid_qty           (4 bytes)
//   offset 24: float    ask_qty           (4 bytes)
//   offset 28: uint32_t exchange_ts_ms    (4 bytes, Unix ms from WS payload; 0 = unknown)
//   offset 32: uint8_t  _pad[32]          (32 bytes, explicit fill to 64)
struct alignas(64) MarketTick {
    uint64_t timestamp_ns;
    uint8_t  venue;
    uint8_t  _implicit_pad;   // explicit field so Python struct can mirror layout
    uint16_t pair_id;
    float    best_bid;
    float    best_ask;
    float    bid_qty;
    float    ask_qty;
    uint32_t exchange_ts_ms;  // Unix ms from exchange WS payload; 0 = unknown (backtest/omitted)
    uint8_t  _pad[32];        // 32 bytes used + 32 pad = 64 total
};
static_assert(sizeof(MarketTick)  == 64,  "MarketTick must be exactly 64 bytes");
static_assert(alignof(MarketTick) == 64,  "MarketTick must be 64-byte aligned");
static_assert(offsetof(MarketTick, venue)          == 8);
static_assert(offsetof(MarketTick, pair_id)        == 10);
static_assert(offsetof(MarketTick, best_bid)       == 12);
static_assert(offsetof(MarketTick, exchange_ts_ms) == 28);
static_assert(offsetof(MarketTick, _pad)           == 32);

// One arb opportunity fired when both legs can execute
struct OrderRequest {
    uint64_t timestamp_ns;
    uint8_t  buy_venue;    // 1 = C2C1, 2 = C2C2
    uint8_t  sell_venue;
    uint16_t pair_id;
    float    buy_price;    // asks[buy_idx]   — what we pay
    float    sell_price;   // bids[sell_idx]  — what we receive
    float    qty;
    float    net_profit;   // per-unit profit after fees
};

// One completed trade recorded for post-loop metrics
struct TradeRecord {
    uint64_t timestamp_ns;
    uint16_t pair_id;
    uint8_t  buy_venue;
    uint8_t  sell_venue;
    float    buy_price;
    float    sell_price;
    float    qty;
    float    net_pnl;    // total profit = net_profit * qty
};

// Backtest simulation parameters — model the gap between ideal and real execution.
// All values configurable in config/default.json under "sim".
struct SimParams {
    float fill_probability    = 0.70f;  // fraction of signals that actually fill (REST lag + flickering)
    float latency_penalty_bps = 2.0f;   // price moves this many bps against us before fill
    float leg_failure_prob    = 0.05f;  // probability sell leg fails (inventory risk)
    float leg_fail_loss_bps   = 50.0f;  // extra loss bps on emergency sell when leg fails
    float tds_rate            = 0.01f;  // 1% TDS on sell value (Indian income tax)
    float gst_on_fee_rate     = 0.18f;  // 18% GST on trading fee (Indian GST)
    float fee_rate            = constants::FEE_RATE;  // per-leg fee, overridable for scenario testing
};

struct Config {
    struct SymbolInfo {
        uint16_t pair_id;
        char     name[16];
    };
    struct VenueInfo {
        char exchange_id[16];   // "c2c1" or "c2c2"
        char base_url[256];     // https://coinswitch.co
    };

    SymbolInfo symbols[constants::PAIRS_PER_VENUE];
    uint16_t   symbol_count       = 0;
    float      fee_rate           = constants::FEE_RATE;
    char       data_file[256]     = {};
    float      initial_balance    = 50000.0f;  // single USDT balance (unified wallet)
    float      min_profit_usd     = constants::MIN_PROFIT_USD;
    float      max_position_pct        = constants::MAX_POSITION_PCT;
    uint64_t   max_stale_ns            = constants::MAX_STALE_NS;
    uint32_t   max_exchange_ts_skew_ms = 100;  // reject signal if venues' exchange-ts differ by more
    uint32_t   order_timeout_ms        = 500;  // mark order TIMEOUT after this many ms
    char       api_key[128]            = {};          // one key shared across both venues
    char       private_key_hex[128] = {};        // Ed25519 private key, 64-char hex string
    VenueInfo  venues[2]          = {};          // [0]=C2C1, [1]=C2C2
    char       ws_url[256]        = {};
    SimParams  sim                = {};          // backtest realism parameters
};

} // namespace arb
