#pragma once
// Shared simulation-realism order handler used by both backtester and live --paper mode.
// Applies fill probability, latency penalty, leg-failure simulation, and Indian taxes.
// When a fill is rejected, reverses the ledger delta that try_arb() already applied.
//
// xorshift32 RNG: fast, no heap, good statistical properties for this use case.
#include "hot_path/virtual_ledger.hpp"
#include "common/types.hpp"
#include "common/constants.hpp"

namespace arb {

struct BacktestOrderHandler {
    TradeRecord*        records;
    size_t*             count;
    size_t*             attempts;   // total handler calls (before fill probability)
    arb::VirtualLedger* ledger;
    arb::SimParams      sim;
    uint32_t            rng = 0xBEEF1234u;

    uint32_t next_rand() noexcept {
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        return rng;
    }
    float rand01() noexcept {
        return static_cast<float>(next_rand()) / 4294967296.0f;
    }

    // The exact net delta that try_arb() applied to quote_balance.
    // MUST use constants::FEE_RATE to match try_arb() — not the sim override.
    // sim.fee_rate is used only for per-trade P&L recording below.
    float ledger_delta(float bp, float sp, float qty) const noexcept {
        const float fee = constants::FEE_RATE * (bp + sp);
        return qty * (sp - bp - fee);
    }

    void submit(const OrderRequest& req) noexcept {
        ++(*attempts);

        // 1. Fill probability: model REST lag and top-of-book flickering.
        if (rand01() >= sim.fill_probability) {
            // Signal fired but trade didn't execute — reverse the ledger update.
            ledger->quote_balance -= ledger_delta(req.buy_price, req.sell_price, req.qty);
            return;
        }

        // 2. Latency penalty: by the time our order hits the exchange, price has moved.
        const float pen = sim.latency_penalty_bps / 10000.0f;
        const float b   = req.buy_price  * (1.0f + pen);   // pay more to buy
        const float s   = req.sell_price * (1.0f - pen);   // receive less on sell

        // Adjust ledger from the original prices to the post-latency prices.
        ledger->quote_balance -= ledger_delta(req.buy_price, req.sell_price, req.qty);
        ledger->quote_balance += ledger_delta(b, s, req.qty);

        // 3. Leg failure: sell leg doesn't fill, must emergency-sell at worse price.
        const bool  leg_failed = (rand01() < sim.leg_failure_prob);
        const float s_actual   = leg_failed
            ? s * (1.0f - sim.leg_fail_loss_bps / 10000.0f)
            : s;

        if (leg_failed) {
            ledger->quote_balance -= ledger_delta(b, s, req.qty);
            ledger->quote_balance += ledger_delta(b, s_actual, req.qty);
        }

        // 4. Tax deductions (India).
        const float fee_per_unit = sim.fee_rate * (b + s_actual);
        const float tds  = req.qty * s_actual * sim.tds_rate;            // 1% TDS on sell value
        const float gst  = req.qty * fee_per_unit * sim.gst_on_fee_rate; // 18% GST on fee
        // Deduct taxes from the balance (ledger tracked gross; taxes are additional costs).
        ledger->quote_balance -= (tds + gst);

        const float gross_pnl = req.qty * (s_actual - b - fee_per_unit);
        const float net_pnl   = gross_pnl - tds - gst;

        if (*count >= constants::MAX_TRADES) return;
        TradeRecord& r  = records[(*count)++];
        r.timestamp_ns  = req.timestamp_ns;
        r.pair_id       = req.pair_id;
        r.buy_venue     = req.buy_venue;
        r.sell_venue    = req.sell_venue;
        r.buy_price     = b;
        r.sell_price    = s_actual;
        r.qty           = req.qty;
        r.net_pnl       = net_pnl;
    }
};

} // namespace arb
