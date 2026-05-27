#pragma once
// Cold-path order state machine. Tracks both legs of every arb trade from send
// to fill/rejection, and detects timeout conditions that require emergency hedging.
//
// VirtualLedger is a hot-path type; including it here is safe because cold-path
// code may include hot-path headers (the reverse is forbidden).
#include "hot_path/virtual_ledger.hpp"
#include "common/types.hpp"
#include "common/time_utils.hpp"
#include "cold_path/logger.hpp"
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace arb {

enum class OrderState : uint8_t {
    PENDING, FILLED, PARTIAL, REJECTED, TIMEOUT, CANCELLED
};

struct TrackedOrder {
    char       exchange_oid[64];  // order_id string from exchange ("" if unknown)
    uint64_t   local_id;
    uint64_t   send_time_ns;
    uint64_t   last_update_ns;
    float      price;
    float      quantity;
    float      filled_qty;
    uint16_t   pair_id;
    uint8_t    venue;
    OrderState state;
};

struct ArbitrageRecord {
    uint64_t buy_local_id;
    uint64_t sell_local_id;
    uint16_t pair_id;
    uint8_t  buy_venue;
    uint8_t  sell_venue;
    float    buy_price;
    float    sell_price;
    float    qty;
};

class OrderTracker {
public:
    // Register both legs of one arb trade. oid strings may be "" if send failed.
    void track_arb(const OrderRequest& req,
                   const char* buy_oid, const char* sell_oid) noexcept
    {
        const uint64_t b_id = register_order(buy_oid,  req.buy_venue,  req.pair_id,
                                             req.buy_price,  req.qty);
        const uint64_t s_id = register_order(sell_oid, req.sell_venue, req.pair_id,
                                             req.sell_price, req.qty);
        arb_pairs_.push_back(ArbitrageRecord{
            b_id, s_id,
            req.pair_id, req.buy_venue, req.sell_venue,
            req.buy_price, req.sell_price, req.qty
        });
    }

    // Update order state when exchange confirmation arrives.
    void update(uint64_t local_id, OrderState new_state, float filled_qty) noexcept {
        auto it = orders_.find(local_id);
        if (it == orders_.end()) return;
        it->second.state       = new_state;
        it->second.filled_qty  = filled_qty;
        it->second.last_update_ns = time_utils::now_ns();
    }

    // Update by exchange order_id string (used by Order Updates WebSocket callback).
    // O(1) via oid_to_local_ reverse map populated in register_order().
    void update_by_exchange_oid(const char* oid,
                                OrderState  new_state,
                                float       filled_qty) noexcept {
        if (!oid || !oid[0]) return;
        auto it = oid_to_local_.find(oid);
        if (it == oid_to_local_.end()) return;
        update(it->second, new_state, filled_qty);
    }

    // Walk all PENDING orders; mark TIMEOUT when older than timeout_ms.
    // Returns ArbitrageRecords where at least one leg timed out.
    std::vector<ArbitrageRecord> check_timeouts(uint32_t timeout_ms) noexcept {
        const uint64_t now_ns     = time_utils::now_ns();
        const uint64_t threshold  = static_cast<uint64_t>(timeout_ms) * 1'000'000ULL;
        std::vector<ArbitrageRecord> timed_out;

        for (auto& [id, ord] : orders_) {
            if (ord.state == OrderState::PENDING &&
                now_ns - ord.send_time_ns > threshold) {
                ord.state         = OrderState::TIMEOUT;
                ord.last_update_ns= now_ns;
            }
        }

        for (const auto& rec : arb_pairs_) {
            auto bi = orders_.find(rec.buy_local_id);
            auto si = orders_.find(rec.sell_local_id);
            if (bi == orders_.end() || si == orders_.end()) continue;
            if (bi->second.state == OrderState::TIMEOUT ||
                si->second.state == OrderState::TIMEOUT) {
                timed_out.push_back(rec);
            }
        }
        return timed_out;
    }

    // Handle leg failures detected after check_timeouts().
    // Reverses ledger entries for unhedged positions and logs the event.
    // Returns true if a manual hedge order is required.
    bool handle_leg_failure(const ArbitrageRecord& rec,
                            VirtualLedger& ledger) noexcept
    {
        auto bi = orders_.find(rec.buy_local_id);
        auto si = orders_.find(rec.sell_local_id);
        if (bi == orders_.end() || si == orders_.end()) return false;

        const OrderState bs = bi->second.state;
        const OrderState ss = si->second.state;

        if (bs == OrderState::FILLED && ss == OrderState::FILLED) return false;

        if (bs == OrderState::FILLED &&
            (ss == OrderState::TIMEOUT || ss == OrderState::REJECTED)) {
            // Long coin — buy leg filled, sell leg failed.
            // Reverse expected sell proceeds and apply emergency-sell penalty (50 bps).
            const float pen_sell = rec.sell_price * (1.0f - 50.0f / 10000.0f);
            const float fee      = constants::FEE_RATE * (rec.buy_price + pen_sell);
            ledger.quote_balance -= rec.qty * (rec.sell_price - rec.buy_price - fee);
            ledger.quote_balance += rec.qty * (pen_sell      - rec.buy_price - fee);
            spdlog::error("Leg failure: BUY filled but SELL timed out, pair={} — hedge required",
                          rec.pair_id);
            return true;
        }

        if (ss == OrderState::FILLED &&
            (bs == OrderState::TIMEOUT || bs == OrderState::REJECTED)) {
            // Sold coin we don't have — sell leg filled, buy leg failed.
            const float fee = constants::FEE_RATE * (rec.buy_price + rec.sell_price);
            ledger.quote_balance -= rec.qty * (rec.sell_price - rec.buy_price - fee);
            spdlog::error("Leg failure: SELL filled but BUY timed out, pair={} — hedge required",
                          rec.pair_id);
            return true;
        }

        // Both legs failed: reverse the apply_arb() delta that was committed.
        const float fee = constants::FEE_RATE * (rec.buy_price + rec.sell_price);
        ledger.quote_balance -= rec.qty * (rec.sell_price - rec.buy_price - fee);
        spdlog::warn("Both legs failed, pair={} — ledger reversed", rec.pair_id);
        return false;
    }

    size_t pending_count() const noexcept {
        size_t n = 0;
        for (const auto& [id, ord] : orders_)
            if (ord.state == OrderState::PENDING) ++n;
        return n;
    }

private:
    std::unordered_map<uint64_t, TrackedOrder> orders_;
    std::unordered_map<std::string, uint64_t>  oid_to_local_;  // exchange_oid → local_id
    std::vector<ArbitrageRecord>               arb_pairs_;
    uint64_t next_id_ = 1;

    uint64_t register_order(const char* oid, uint8_t venue, uint16_t pair_id,
                            float price, float qty) noexcept
    {
        const uint64_t id = next_id_++;
        TrackedOrder ord{};
        if (oid && oid[0]) {
            std::strncpy(ord.exchange_oid, oid, 63);
            oid_to_local_.emplace(oid, id);
        }
        ord.local_id      = id;
        ord.send_time_ns  = time_utils::now_ns();
        ord.last_update_ns= ord.send_time_ns;
        ord.price         = price;
        ord.quantity      = qty;
        ord.filled_qty    = 0.0f;
        ord.pair_id       = pair_id;
        ord.venue         = venue;
        ord.state         = OrderState::PENDING;
        orders_.emplace(id, ord);
        return id;
    }
};

} // namespace arb
