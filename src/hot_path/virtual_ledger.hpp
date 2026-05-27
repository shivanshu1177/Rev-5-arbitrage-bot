#pragma once
// Unified-wallet virtual ledger — models a single CoinSwitch account that trades on
// both c2c1 and c2c2. Both venues share the same USDT balance and the same coin
// inventory per pair (buying on c2c1 and selling on c2c2 nets zero coin change).
//
// Trade flow for one arb round-trip:
//   BUY  on c2c1: spend USDT → receive coin (quote_balance -= qty*buy_price)
//   SELL on c2c2: spend coin → receive USDT (quote_balance += qty*sell_price)
//   Net: quote_balance += qty*(sell_price - buy_price - fee_both_legs)
//        coin_balance[pair_id] unchanged (coin received = coin sold)
//
// try_arb() attempts both legs simultaneously with four branchless constraints:
//   USDT budget, coin inventory, buy-side top-of-book depth, sell-side depth.
// On constraint failure, returns 0 and leaves balances unchanged.
#include "common/constants.hpp"
#include "common/types.hpp"
#include <bit>
#include <cstring>
#include <cstdint>

namespace arb {

struct VirtualLedger {
    float quote_balance;                                     // single USDT for the whole account
    float coin_balance[constants::PAIRS_PER_VENUE];          // per pair_id (not per venue)

    void reset() noexcept {
        quote_balance = 0.0f;
        std::memset(coin_balance, 0, sizeof(coin_balance));
    }

    void seed_quote(float amount) noexcept {
        quote_balance = amount;
    }

    void seed_base(uint16_t pair_id, float qty) noexcept {
        if (pair_id < constants::PAIRS_PER_VENUE) {
            coin_balance[pair_id] = qty;
        }
    }

    // Attempt both legs of an arb trade with unified-wallet semantics.
    // Returns executable qty (> 0) or 0.0f if any constraint is unsatisfied.
    // Does NOT modify quote_balance — call apply_arb() only after last-look passes.
    // sell_price is not needed for constraints (only buy_price drives the budget calc).
    // All min operations are branchless (bit_cast pattern → ARM64 CSEL).
    [[nodiscard]] float try_arb(
        uint16_t pair_id,
        float    buy_price,
        float    buy_depth,   // top-of-book qty available at best ask (from MarketState.ask_qty)
        float    sell_depth,  // top-of-book qty available at best bid (from MarketState.bid_qty)
        float    max_pct) noexcept
    {
        // Insolvency guard: when quote_balance <= 0, qty_budget is negative.
        // Negative floats have sign-bit 1, making bit_cast<uint32_t> enormous —
        // the branchless CMP+CSEL min below would silently discard the constraint
        // and allow unlimited trading on an insolvent account.  Return early.
        if (quote_balance <= 0.0f) [[unlikely]] return 0.0f;

        // Constraint 1: USDT budget (fraction of account balance)
        const float qty_budget = (buy_price > 0.0f)
            ? (quote_balance * max_pct / buy_price) : 0.0f;
        // Constraint 2: coin inventory (must have coin to sell on c2c2 simultaneously)
        const float qty_inv = (pair_id < constants::PAIRS_PER_VENUE)
            ? coin_balance[pair_id] : 0.0f;

        // 4-way branchless min: budget, inventory, buy depth, sell depth.
        // IEEE 754 positive floats sort identically as uint32 → CMP+CSEL on ARM64.
        float qty = qty_budget;
        { const uint32_t a = std::bit_cast<uint32_t>(qty);
          const uint32_t b = std::bit_cast<uint32_t>(qty_inv);
          qty = std::bit_cast<float>(a < b ? a : b); }
        { const uint32_t a = std::bit_cast<uint32_t>(qty);
          const uint32_t b = std::bit_cast<uint32_t>(buy_depth);
          qty = std::bit_cast<float>(a < b ? a : b); }
        { const uint32_t a = std::bit_cast<uint32_t>(qty);
          const uint32_t b = std::bit_cast<uint32_t>(sell_depth);
          qty = std::bit_cast<float>(a < b ? a : b); }

        // coin_balance[pair_id] is unchanged: qty bought on c2c1 = qty sold on c2c2.
        return (qty > 0.0f) ? qty : 0.0f;  // FCMP+CSEL
    }

    // Commit the ledger after last-look passes. Called only when try_arb() returned > 0
    // and the spread is still profitable. Matches BacktestOrderHandler::ledger_delta()
    // exactly so sim_handler.hpp reversal logic stays correct.
    void apply_arb(float qty, float buy_price, float sell_price) noexcept {
        const float fee = constants::FEE_RATE * (buy_price + sell_price);
        quote_balance += qty * (sell_price - buy_price - fee);
    }
};

} // namespace arb
