#pragma once
#include "common/types.hpp"
#include <cstddef>
#include <cstdint>

namespace arb {

struct MetricsSummary {
    // PnL
    double  total_pnl;            // gross PnL recorded in TradeRecord.net_pnl (includes TDS+GST already)
    double  final_balance;        // initial_balance + total_pnl
    double  avg_profit_per_trade;

    // Risk
    double  sharpe_ratio;         // mean/std of per-trade returns (information ratio)
    double  max_drawdown_pct;     // largest peak-to-trough drawdown as % of initial balance
    double  win_rate;             // fraction of recorded trades with net_pnl > 0

    // Realism stats
    int64_t trade_count;          // trades actually recorded (after fill probability rejection)
    int64_t attempts_count;       // signals fired that reached the handler (before fill rejection)
    double  effective_fill_rate;  // trade_count / attempts_count
    int64_t leg_failure_count;    // trades where sell leg failed (net_pnl < 0 AND attempt was profitable)
};

// attempts_count: total times handler.submit() was called (including rejections).
// Pass it separately since the handler tracks it before applying fill probability.
MetricsSummary compute_metrics(const TradeRecord* records,
                               size_t             count,
                               float              initial_balance,
                               size_t             attempts_count = 0);

void print_summary(const MetricsSummary& m);

} // namespace arb
