#pragma once
// The hot-path event dispatcher. Everything called from process_tick() must be noexcept.
// The OrderHandler concept enforces that the handler's submit() is also noexcept.
//
// In backtest mode: OrderHandler = BacktestOrderHandler (records trades to a buffer).
// In live mode:     OrderHandler = LiveOrderHandler     (pushes to order sender).
//
// The branch on sig.valid is intentional: arb events are extremely rare (~0.01% of ticks),
// so the branch predictor learns quickly to always predict "not valid". The [[unlikely]]
// annotation communicates this to the code generator for layout/prefetch hints.
#include "market_state.hpp"
#include "signal_eval.hpp"
#include "virtual_ledger.hpp"
#include "order_builder.hpp"
#include "common/types.hpp"
#include "common/constants.hpp"

namespace arb {

template<typename T>
concept OrderHandler = requires(T& h, const OrderRequest& req) {
    { h.submit(req) } noexcept;
};

template<OrderHandler H>
inline void process_tick(
    const MarketTick& tick,
    MarketState&      state,
    VirtualLedger&    ledger,
    OrderTemplate*    buy_templates,   // MAX_INSTRUMENTS BUY-side pre-formatted templates
    OrderTemplate*    sell_templates,  // MAX_INSTRUMENTS SELL-side pre-formatted templates
    H&                handler) noexcept
{
    // Validate venue and pair_id before any array access.
    // assert() is stripped by -DNDEBUG in release builds; these if-guards are the
    // only protection in production.  Invalid ticks are silently dropped.
    if (tick.venue < 1u || tick.venue > 2u ||
        tick.pair_id >= constants::PAIRS_PER_VENUE) [[unlikely]] return;

    // Update the live state unconditionally (always runs).
    state.update(tick);

    // Evaluate the arb signal using the freshly updated state.
    const ArbSignal sig = evaluate_signal(tick, state);

    // Only proceed if the signal is profitable above the minimum threshold.
    if (sig.valid) [[unlikely]] {
        const float buy_price  = state.asks[sig.buy_idx];
        const float sell_price = state.bids[sig.sell_idx];

        // Top-of-book depth: cap fill qty to what's available at the best price.
        const float buy_depth  = state.ask_qty[sig.buy_idx];
        const float sell_depth = state.bid_qty[sig.sell_idx];

        // Imbalance gate: skip if one leg's depth is < 20% of the other.
        // Thin side = high execution risk (likely to partially fill or miss entirely).
        {
            const float mn = (buy_depth < sell_depth) ? buy_depth : sell_depth;
            const float mx = (buy_depth > sell_depth) ? buy_depth : sell_depth;
            if (mx <= 0.0f || mn / mx < 0.20f) return;
        }

        // Exchange-timestamp skew gate: reject if one venue's price is older than
        // the other by more than max_exchange_ts_skew_ms (default 100ms).
        // Gate is skipped when either slot has exchange_ts_ms == 0 (backtest replay
        // or exchange does not include a timestamp in the WS payload).
        {
            const uint32_t ts_b = state.exchange_ts_ms[sig.buy_idx];
            const uint32_t ts_s = state.exchange_ts_ms[sig.sell_idx];
            if (ts_b != 0 && ts_s != 0) {
                const uint32_t skew = ts_b > ts_s ? ts_b - ts_s : ts_s - ts_b;
                if (skew > state.max_exchange_ts_skew_ms) [[unlikely]] return;
            }
        }

        // Constraint check only — does NOT touch quote_balance.
        // tick.pair_id is the same for both legs (buy and sell are the same coin).
        const float qty = ledger.try_arb(
            tick.pair_id,
            buy_price,
            buy_depth, sell_depth,
            constants::MAX_POSITION_PCT);

        if (qty > 0.0f) [[likely]] {
            // Last-look: re-read state to confirm spread is still profitable.
            // Guards against market movement between signal eval and order send.
            const float ll_ask = state.asks[sig.buy_idx];
            const float ll_bid = state.bids[sig.sell_idx];
            const float ll_fee = constants::FEE_RATE * (ll_ask + ll_bid);
            if (ll_bid - ll_ask - ll_fee < constants::MIN_PROFIT_USD) [[unlikely]] return;

            // Last-look passed — commit ledger, then patch + submit.
            ledger.apply_arb(qty, buy_price, sell_price);

            // Patch both pre-formatted order bodies with current prices and qty.
            patch_order(buy_templates[sig.buy_idx],   buy_price,  qty);
            patch_order(sell_templates[sig.sell_idx], sell_price, qty);

            // Notify the handler (backtest: record; live: send both legs to exchange).
            handler.submit(OrderRequest{
                tick.timestamp_ns,
                sig.buy_venue,
                sig.sell_venue,
                tick.pair_id,
                buy_price,
                sell_price,
                qty,
                sig.net_profit
            });
        }
    }
}

} // namespace arb
