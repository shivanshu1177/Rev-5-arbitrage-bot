#include "cold_path/metrics.hpp"
#include <cmath>
#include <cstdio>
#include <algorithm>

namespace arb {

// Welford's online algorithm for mean and variance in a single pass.
MetricsSummary compute_metrics(const TradeRecord* records,
                               size_t             count,
                               float              initial_balance,
                               size_t             attempts_count)
{
    MetricsSummary m{};
    m.trade_count    = static_cast<int64_t>(count);
    m.attempts_count = static_cast<int64_t>(attempts_count);
    m.effective_fill_rate = (attempts_count > 0)
        ? static_cast<double>(count) / static_cast<double>(attempts_count) : 0.0;

    if (count == 0) {
        m.final_balance = initial_balance;
        return m;
    }

    double balance = initial_balance;
    double peak    = initial_balance;
    int64_t wins   = 0;

    // Welford state for per-trade PnL
    double wf_mean = 0.0;
    double wf_M2   = 0.0;

    for (size_t i = 0; i < count; ++i) {
        const double pnl = static_cast<double>(records[i].net_pnl);
        m.total_pnl += pnl;
        balance     += pnl;

        if (pnl > 0.0) ++wins;
        if (pnl < 0.0) ++m.leg_failure_count;  // negative-PnL trade → leg failure or tax drag

        // Track equity peak and drawdown.
        if (balance > peak) peak = balance;
        const double dd = (peak - balance) / peak;
        if (dd > m.max_drawdown_pct) m.max_drawdown_pct = dd;

        // Welford online update.
        const double n     = static_cast<double>(i + 1);
        const double delta = pnl - wf_mean;
        wf_mean           += delta / n;
        wf_M2             += delta * (pnl - wf_mean);
    }

    m.final_balance        = balance;
    m.win_rate             = static_cast<double>(wins) / static_cast<double>(count);
    m.avg_profit_per_trade = m.total_pnl / static_cast<double>(count);

    // Sharpe = mean / std (information ratio).
    const double variance = (count > 1) ? (wf_M2 / static_cast<double>(count - 1)) : 0.0;
    const double std_dev  = std::sqrt(variance);
    m.sharpe_ratio = (std_dev > 1e-12) ? (wf_mean / std_dev) : 0.0;

    return m;
}

void print_summary(const MetricsSummary& m) {
    std::printf("\n=== Backtest Results ===\n");
    std::printf("  Trades recorded  : %lld\n",  (long long)m.trade_count);
    if (m.attempts_count > 0) {
        std::printf("  Signals fired    : %lld\n",  (long long)m.attempts_count);
        std::printf("  Fill rate        : %.1f%%\n", m.effective_fill_rate * 100.0);
    }
    std::printf("  Net PnL          : $%.2f\n", m.total_pnl);
    std::printf("  Avg per trade    : $%.4f\n", m.avg_profit_per_trade);
    std::printf("  Win rate         : %.1f%%\n", m.win_rate * 100.0);
    if (m.leg_failure_count > 0) {
        std::printf("  Leg failures     : %lld (negative-PnL trades)\n", (long long)m.leg_failure_count);
    }
    std::printf("  Sharpe ratio     : %.3f\n",  m.sharpe_ratio);
    std::printf("  Max drawdown     : %.2f%%\n", m.max_drawdown_pct * 100.0);
    std::printf("  Final balance    : $%.2f\n",  m.final_balance);
    std::printf("========================\n");
}

} // namespace arb
