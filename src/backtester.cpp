// Backtest entry point.
// Replays a binary file of MarketTick records through the same hot-path core
// that will later be used for live trading. No network I/O, no real orders.
//
// Flow:
//   1. Load config, set up logger
//   2. mmap the tick file via FileSource
//   3. Pre-allocate MarketState, VirtualLedger, OrderTemplate[] on the stack
//      (one heap alloc for the TradeRecord log — done before the hot loop)
//   4. HOT LOOP: process_tick() for every tick
//   5. compute_metrics() + print_summary()
//
// Simulation realism (configurable via config/default.json "sim" section):
//   fill_probability    — fraction of signals that actually fill (REST lag, flickering)
//   latency_penalty_bps — price moves N bps against us before fill
//   leg_failure_prob    — probability sell leg doesn't fill (inventory risk)
//   leg_fail_loss_bps   — extra bps loss on emergency sell when leg fails
//   tds_rate            — 1% TDS on sell value (Indian income tax)
//   gst_on_fee_rate     — 18% GST on trading fee (Indian GST)
#include "hot_path/engine.hpp"
#include "hot_path/market_state.hpp"
#include "hot_path/virtual_ledger.hpp"
#include "hot_path/order_builder.hpp"
#include "cold_path/config_loader.hpp"
#include "cold_path/sim_handler.hpp"
#include "cold_path/file_source.hpp"
#include "cold_path/metrics.hpp"
#include "cold_path/logger.hpp"
#include "common/types.hpp"
#include "common/constants.hpp"
#include "common/time_utils.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

namespace arb {

} // namespace arb

// ── Per-tick latency accumulator (mirrors live.cpp) ───────────────────────────

struct LatencyStats {
    uint64_t min_ns = UINT64_MAX;
    uint64_t max_ns = 0;
    uint64_t sum_ns = 0;
    uint64_t count  = 0;

    static constexpr int      N_BUCKETS = 7;
    static constexpr uint64_t BOUNDS[6] = {1'000, 10'000, 100'000, 1'000'000, 10'000'000, 100'000'000};
    uint64_t buckets[N_BUCKETS] = {};

    void record(uint64_t ns) noexcept {
        if (ns < min_ns) min_ns = ns;
        if (ns > max_ns) max_ns = ns;
        sum_ns += ns;
        ++count;
        int b = 0;
        while (b < N_BUCKETS - 1 && ns >= BOUNDS[b]) ++b;
        ++buckets[b];
    }
    double avg_us() const noexcept {
        return count ? (static_cast<double>(sum_ns) / count / 1000.0) : 0.0;
    }
    void print_histogram(const char* label) const noexcept {
        if (!count) return;
        static const char* const names[N_BUCKETS] = {
            "<1us", "1-10us", "10-100us", "100us-1ms", "1-10ms", "10-100ms", ">100ms"
        };
        std::printf("  %s histogram (%llu samples):\n", label,
                    static_cast<unsigned long long>(count));
        for (int i = 0; i < N_BUCKETS; ++i) {
            if (!buckets[i]) continue;
            std::printf("    %-12s  %6llu  (%5.1f%%)\n", names[i],
                        static_cast<unsigned long long>(buckets[i]),
                        100.0 * buckets[i] / count);
        }
        const char *p50 = names[N_BUCKETS-1], *p95 = names[N_BUCKETS-1], *p99 = names[N_BUCKETS-1];
        uint64_t cum = 0;
        bool got50 = false, got95 = false, got99 = false;
        for (int i = 0; i < N_BUCKETS; ++i) {
            cum += buckets[i];
            const double pct = static_cast<double>(cum) / count;
            if (!got50 && pct >= 0.50) { p50 = names[i]; got50 = true; }
            if (!got95 && pct >= 0.95) { p95 = names[i]; got95 = true; }
            if (!got99 && pct >= 0.99) { p99 = names[i]; got99 = true; }
        }
        std::printf("    p50 %-12s  p95 %-12s  p99 %-12s\n", p50, p95, p99);
    }
    void write_json(FILE* f, const char* label) const noexcept {
        std::fprintf(f, "  \"%s\": {\n", label);
        std::fprintf(f, "    \"count\": %llu,\n",   static_cast<unsigned long long>(count));
        std::fprintf(f, "    \"min_us\": %.3f,\n",  count ? min_ns / 1000.0 : 0.0);
        std::fprintf(f, "    \"avg_us\": %.3f,\n",  avg_us());
        std::fprintf(f, "    \"max_us\": %.3f,\n",  count ? max_ns / 1000.0 : 0.0);
        std::fprintf(f, "    \"buckets\": [%llu,%llu,%llu,%llu,%llu,%llu,%llu]\n",
                     static_cast<unsigned long long>(buckets[0]),
                     static_cast<unsigned long long>(buckets[1]),
                     static_cast<unsigned long long>(buckets[2]),
                     static_cast<unsigned long long>(buckets[3]),
                     static_cast<unsigned long long>(buckets[4]),
                     static_cast<unsigned long long>(buckets[5]),
                     static_cast<unsigned long long>(buckets[6]));
        std::fprintf(f, "  }");
    }
};

// ── Cold-path helpers ─────────────────────────────────────────────────────────

static void init_all_templates(
    arb::OrderTemplate*  buy_templates,
    arb::OrderTemplate*  sell_templates,
    const arb::Config&   cfg) noexcept
{
    for (int v = 1; v <= 2; ++v) {
        for (uint16_t i = 0; i < cfg.symbol_count; ++i) {
            const uint16_t pair_id = cfg.symbols[i].pair_id;
            const uint16_t idx     = arb::MarketState::map_to_index(
                                         static_cast<uint8_t>(v), pair_id);
            arb::init_template(buy_templates[idx],  pair_id,
                               static_cast<uint8_t>(v), cfg.symbols[i].name, "BUY");
            arb::init_template(sell_templates[idx], pair_id,
                               static_cast<uint8_t>(v), cfg.symbols[i].name, "SELL");
        }
    }
}

static void init_ledger(
    arb::VirtualLedger& ledger,
    const arb::Config&  cfg) noexcept
{
    ledger.reset();
    // Unified wallet: one USDT balance for the whole account (not per venue).
    ledger.seed_quote(cfg.initial_balance);

    // Pre-seed coin inventory per pair so sell legs can execute from the first tick.
    // 1% of initial balance worth (in coin units — approximate for backtest).
    const float coin_seed = cfg.initial_balance * 0.01f;
    for (uint16_t i = 0; i < cfg.symbol_count; ++i) {
        ledger.seed_base(cfg.symbols[i].pair_id, coin_seed);
    }
}

// ── Per-trade log helpers ─────────────────────────────────────────────────────

static const char* venue_name(uint8_t v) noexcept {
    return v == 1 ? "C2C1" : v == 2 ? "C2C2" : "????";
}

static const char* pair_name(uint16_t pair_id, const arb::Config& cfg) noexcept {
    for (uint16_t i = 0; i < cfg.symbol_count; ++i)
        if (cfg.symbols[i].pair_id == pair_id) return cfg.symbols[i].name;
    return "???";
}

static void print_trade_log(
    const arb::TradeRecord*    trades,
    size_t                     count,
    const arb::Config&         cfg,
    const arb::MetricsSummary& metrics) noexcept
{
    const float lat_bps       = static_cast<float>(cfg.sim.latency_penalty_bps) / 10000.0f;
    float       total_lat_cost = 0.0f;

    // ── stdout: trade table ───────────────────────────────────────────────────
    if (count == 0) {
        std::printf("\n(no trades recorded)\n");
    } else {
        std::printf("\n=== Per-Trade Log ===\n");
        std::printf("  %-4s  %-19s  %-8s  %-4s->%-4s  %-10s  %-10s  %-8s  %-10s  %-10s  %-10s  %s\n",
            "#", "Date/Time (UTC)", "Symbol",
            "Buy", "Sell", "BuyPrice", "SellPrice", "Qty",
            "LatencyCst", "GrossPnL", "NetPnL", "CumPnL");

        float cum_pnl = 0.0f;
        for (size_t i = 0; i < count; ++i) {
            const auto& t = trades[i];
            const time_t sec = static_cast<time_t>(t.timestamp_ns / 1'000'000'000ULL);
            char dt[20];
            struct tm* utc = std::gmtime(&sec);
            std::strftime(dt, sizeof(dt), "%Y-%m-%d %H:%M:%S", utc);
            const float fee      = cfg.sim.fee_rate * (t.buy_price + t.sell_price);
            const float gross    = t.qty * (t.sell_price - t.buy_price - fee);
            const float lat_cost = t.qty * (t.buy_price + t.sell_price) * lat_bps;
            cum_pnl             += t.net_pnl;
            total_lat_cost      += lat_cost;
            std::printf("  %-4zu  %-19s  %-8s  %-4s->%-4s  %10.2f  %10.2f  %8.5f  %10.2f  %+10.2f  %+10.2f  %+.2f\n",
                i + 1, dt,
                pair_name(t.pair_id, cfg),
                venue_name(t.buy_venue), venue_name(t.sell_venue),
                t.buy_price, t.sell_price, t.qty,
                lat_cost, gross, t.net_pnl, cum_pnl);
        }
        std::printf("=====================\n");
    }

    // ── stdout: summary metrics ───────────────────────────────────────────────
    std::printf("\n=== Backtest Results ===\n");
    std::printf("  Trades recorded  : %lld\n",   metrics.trade_count);
    if (metrics.attempts_count > 0)
        std::printf("  Signals fired    : %lld\n", metrics.attempts_count);
    std::printf("  Fill rate        : %.1f%%\n",  metrics.effective_fill_rate * 100.0);
    std::printf("  Net PnL          : $%.2f\n",   metrics.total_pnl);
    std::printf("  Avg per trade    : $%.4f\n",   metrics.avg_profit_per_trade);
    std::printf("  Win rate         : %.1f%%\n",  metrics.win_rate * 100.0);
    if (metrics.leg_failure_count > 0)
        std::printf("  Leg failures     : %lld (negative-PnL trades)\n", metrics.leg_failure_count);
    std::printf("  Sharpe ratio     : %.3f\n",    metrics.sharpe_ratio);
    std::printf("  Max drawdown     : %.2f%%\n",  metrics.max_drawdown_pct * 100.0);
    std::printf("  Final balance    : $%.2f\n",   metrics.final_balance);
    std::printf("  Latency penalty  : %.0f bps\n", cfg.sim.latency_penalty_bps);
    std::printf("  Total lat. cost  : $%.2f\n",   total_lat_cost);
    std::printf("  Avg lat. cost    : $%.4f\n",
                metrics.trade_count > 0 ? total_lat_cost / metrics.trade_count : 0.0f);
    std::printf("========================\n");

    // ── CSV: trade rows + summary footer ─────────────────────────────────────
    FILE* csv = std::fopen("backtest_trades.csv", "w");
    if (!csv) { std::fprintf(stderr, "Warning: could not write backtest_trades.csv\n"); return; }

    std::fprintf(csv, "trade,datetime_utc,symbol,buy_exchange,sell_exchange,"
                      "buy_price,sell_price,qty,latency_cost,gross_pnl,net_pnl,cumulative_pnl\n");
    float cum_pnl      = 0.0f;
    float csv_lat_cost = 0.0f;
    for (size_t i = 0; i < count; ++i) {
        const auto& t = trades[i];
        const time_t sec = static_cast<time_t>(t.timestamp_ns / 1'000'000'000ULL);
        char dt[20];
        std::strftime(dt, sizeof(dt), "%Y-%m-%d %H:%M:%S", std::gmtime(&sec));
        const float fee      = cfg.sim.fee_rate * (t.buy_price + t.sell_price);
        const float gross    = t.qty * (t.sell_price - t.buy_price - fee);
        const float lat_cost = t.qty * (t.buy_price + t.sell_price) * lat_bps;
        cum_pnl             += t.net_pnl;
        csv_lat_cost        += lat_cost;
        std::fprintf(csv, "%zu,%s,%s,%s,%s,%.6f,%.6f,%.8f,%.4f,%.4f,%.4f,%.4f\n",
            i + 1, dt,
            pair_name(t.pair_id, cfg),
            venue_name(t.buy_venue), venue_name(t.sell_venue),
            t.buy_price, t.sell_price, t.qty,
            lat_cost, gross, t.net_pnl, cum_pnl);
    }

    std::fprintf(csv, "\nmetric,value\n");
    std::fprintf(csv, "trades_recorded,%lld\n",    metrics.trade_count);
    std::fprintf(csv, "signals_fired,%lld\n",      metrics.attempts_count);
    std::fprintf(csv, "fill_rate_pct,%.2f\n",      metrics.effective_fill_rate * 100.0);
    std::fprintf(csv, "net_pnl,%.2f\n",            metrics.total_pnl);
    std::fprintf(csv, "avg_per_trade,%.4f\n",      metrics.avg_profit_per_trade);
    std::fprintf(csv, "win_rate_pct,%.2f\n",       metrics.win_rate * 100.0);
    std::fprintf(csv, "leg_failures,%lld\n",       metrics.leg_failure_count);
    std::fprintf(csv, "sharpe_ratio,%.3f\n",       metrics.sharpe_ratio);
    std::fprintf(csv, "max_drawdown_pct,%.2f\n",   metrics.max_drawdown_pct * 100.0);
    std::fprintf(csv, "latency_penalty_bps,%.0f\n", cfg.sim.latency_penalty_bps);
    std::fprintf(csv, "total_latency_cost,%.4f\n", csv_lat_cost);
    std::fprintf(csv, "initial_balance,%.2f\n",    (double)cfg.initial_balance);
    std::fprintf(csv, "final_balance,%.2f\n",      metrics.final_balance);

    std::fclose(csv);
    std::printf("Trade log written to: backtest_trades.csv\n");
}

// ── Backtest latency profile writer ──────────────────────────────────────────

static void write_latency_profile(
    const arb::TradeRecord* trades, size_t count,
    const arb::Config& cfg, double ticks_per_s,
    size_t tick_count, const LatencyStats& lat) noexcept
{
    FILE* f = std::fopen("latency_profile_backtest.json", "w");
    if (!f) return;
    const float lat_bps = static_cast<float>(cfg.sim.latency_penalty_bps) / 10000.0f;
    uint64_t buckets[6] = {};
    float total = 0.0f;
    for (size_t i = 0; i < count; ++i) {
        const auto& t = trades[i];
        const float cost = t.qty * (t.buy_price + t.sell_price) * lat_bps;
        total += cost;
        if      (cost <   5.0f) buckets[0]++;
        else if (cost <  10.0f) buckets[1]++;
        else if (cost <  20.0f) buckets[2]++;
        else if (cost <  50.0f) buckets[3]++;
        else if (cost < 100.0f) buckets[4]++;
        else                    buckets[5]++;
    }
    std::fprintf(f,
        "{\n"
        "  \"mode\": \"backtest\",\n"
        "  \"tick_count\": %zu,\n"
        "  \"trade_count\": %zu,\n"
        "  \"latency_penalty_bps\": %.1f,\n"
        "  \"total_latency_cost_usd\": %.4f,\n"
        "  \"avg_latency_cost_usd\": %.4f,\n"
        "  \"hot_loop_ticks_per_sec\": %.0f,\n",
        tick_count,
        count,
        (double)cfg.sim.latency_penalty_bps,
        (double)total,
        count > 0 ? (double)(total / count) : 0.0,
        ticks_per_s);
    lat.write_json(f, "engine_latency");
    std::fprintf(f,
        ",\n"
        "  \"cost_buckets\": {\n"
        "    \"labels\": [\"<$5\",\"$5-10\",\"$10-20\",\"$20-50\",\"$50-100\",\">$100\"],\n"
        "    \"counts\": [%llu,%llu,%llu,%llu,%llu,%llu]\n"
        "  }\n"
        "}\n",
        buckets[0], buckets[1], buckets[2], buckets[3], buckets[4], buckets[5]);
    std::fclose(f);
    std::printf("Latency profile  : latency_profile_backtest.json\n");
    std::system("python3 tools/generate_charts.py "
                "--input latency_profile_backtest.json "
                "--out latency_histogram_backtest.png 2>/dev/null && "
                "echo 'Histogram        : latency_histogram_backtest.png'");
}

// ── main ──────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    time_utils::init();
    logger::setup("backtest.log");

    // ── Argument parsing ──────────────────────────────────────────────────────
    const char* config_path      = "config/default.json";
    const char* data_file_override = nullptr;
    float       tds_override       = -1.0f;   // -1 = not set, use config value
    float       fee_override       = -1.0f;
    float       spread_bps         = 0.0f;

    for (int i = 1; i < argc; ++i) {
        if      (std::strcmp(argv[i], "--data-file")  == 0 && i+1 < argc) data_file_override = argv[++i];
        else if (std::strcmp(argv[i], "--tds-rate")   == 0 && i+1 < argc) tds_override  = std::strtof(argv[++i], nullptr);
        else if (std::strcmp(argv[i], "--fee-rate")   == 0 && i+1 < argc) fee_override  = std::strtof(argv[++i], nullptr);
        else if (std::strcmp(argv[i], "--spread-bps") == 0 && i+1 < argc) spread_bps    = std::strtof(argv[++i], nullptr);
        else if (argv[i][0] != '-')                                        config_path   = argv[i];
    }

    arb::Config cfg{};
    try {
        cfg = arb::load_config(config_path);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Config error: %s\n", e.what());
        return EXIT_FAILURE;
    }

    // Apply CLI overrides (after config load so they take precedence).
    if (data_file_override)  std::strncpy(cfg.data_file, data_file_override, sizeof(cfg.data_file) - 1);
    if (tds_override  >= 0)  cfg.sim.tds_rate  = tds_override;
    if (fee_override  >= 0)  cfg.sim.fee_rate   = fee_override;

    spdlog::info("Loaded config: {} symbols, balance=${:.0f} (unified wallet)",
                 cfg.symbol_count, cfg.initial_balance);
    spdlog::info("Sim params: fill_prob={:.0f}%, latency={}bps, leg_fail={:.0f}%, tds={:.2f}%, fee={:.4f}%, spread={:.1f}bps",
                 cfg.sim.fill_probability * 100.0f,
                 cfg.sim.latency_penalty_bps,
                 cfg.sim.leg_failure_prob * 100.0f,
                 cfg.sim.tds_rate * 100.0f,
                 cfg.sim.fee_rate * 100.0f,
                 spread_bps);

    arb::FileSource src;
    if (!src.open(cfg.data_file)) {
        std::fprintf(stderr, "Failed to open data file: %s\n", cfg.data_file);
        return EXIT_FAILURE;
    }
    const size_t tick_count = src.count();
    spdlog::info("Loaded {} ticks ({:.1f} MB) from {}",
                 tick_count, tick_count * 64.0 / 1024.0 / 1024.0, cfg.data_file);

    // Single heap allocation before the hot loop.
    auto* trade_buf = new(std::nothrow) arb::TradeRecord[constants::MAX_TRADES];
    if (!trade_buf) {
        std::fprintf(stderr, "Failed to allocate trade buffer\n");
        return EXIT_FAILURE;
    }
    size_t trade_count   = 0;
    size_t attempt_count = 0;
    LatencyStats lat_engine;

    arb::MarketState   state;
    arb::VirtualLedger ledger;
    arb::OrderTemplate buy_templates[constants::MAX_INSTRUMENTS];
    arb::OrderTemplate sell_templates[constants::MAX_INSTRUMENTS];

    state.reset();
    state.max_stale_ns            = cfg.max_stale_ns;
    state.max_exchange_ts_skew_ms = cfg.max_exchange_ts_skew_ms;
    init_all_templates(buy_templates, sell_templates, cfg);
    init_ledger(ledger, cfg);

    arb::BacktestOrderHandler handler{
        trade_buf, &trade_count, &attempt_count, &ledger, cfg.sim
    };
    const arb::MarketTick* ticks = src.data();

    spdlog::info("Starting hot loop over {} ticks...", tick_count);

    // ── HOT LOOP ──────────────────────────────────────────────────────────────
    const uint64_t t0 = time_utils::now_ns();

    if (spread_bps > 0.0f) {
        // Scenario mode: copy each venue-2 tick and apply synthetic spread offset.
        // The mmap source is const/read-only; copying one cache line per tick is cheap.
        const float spread_scale = spread_bps / 10000.0f;
        for (size_t i = 0; i < tick_count; ++i) {
            arb::MarketTick t = ticks[i];
            if (t.venue == 2) {
                const float offset = (t.best_bid + t.best_ask) * 0.5f * spread_scale;
                t.best_bid += offset;
                t.best_ask += offset;
            }
            const uint64_t t0_tick = time_utils::now_ns();
            arb::process_tick(t, state, ledger, buy_templates, sell_templates, handler);
            lat_engine.record(time_utils::now_ns() - t0_tick);
        }
    } else {
        for (size_t i = 0; i < tick_count; ++i) {
            const uint64_t t0_tick = time_utils::now_ns();
            arb::process_tick(ticks[i], state, ledger, buy_templates, sell_templates, handler);
            lat_engine.record(time_utils::now_ns() - t0_tick);
        }
    }

    const uint64_t t1 = time_utils::now_ns();
    // ── END HOT LOOP ──────────────────────────────────────────────────────────

    const double elapsed_ms  = static_cast<double>(t1 - t0) / 1e6;
    const double ticks_per_s = static_cast<double>(tick_count) / (elapsed_ms / 1e3);

    spdlog::info("Hot loop: {:.1f} ms → {:.2f}M ticks/sec", elapsed_ms, ticks_per_s / 1e6);
    spdlog::info("Attempts: {}  Fills: {}  (fill rate {:.1f}%)",
                 attempt_count, trade_count,
                 attempt_count > 0 ? 100.0 * trade_count / attempt_count : 0.0);

    if (lat_engine.count > 0) {
        std::printf("\n=== process_tick() Latency ===\n");
        std::printf("  min=%.1fns  avg=%.3fus  max=%.1fus\n",
                    (double)lat_engine.min_ns,
                    lat_engine.avg_us(),
                    lat_engine.max_ns / 1000.0);
        lat_engine.print_histogram("process_tick");
        std::printf("==============================\n");
    }

    const arb::MetricsSummary metrics = arb::compute_metrics(
        trade_buf, trade_count,
        cfg.initial_balance,   // unified wallet: single starting balance
        attempt_count);

    print_trade_log(trade_buf, trade_count, cfg, metrics);
    write_latency_profile(trade_buf, trade_count, cfg, ticks_per_s, tick_count, lat_engine);

    // Show final account balance change.
    std::printf("\n=== Account Balance ===\n");
    std::printf("  Starting USDT: $%.2f\n", cfg.initial_balance);
    std::printf("  Final USDT:    $%.2f\n", ledger.quote_balance);
    std::printf("  Net change:    $%.2f\n", ledger.quote_balance - cfg.initial_balance);
    std::printf("======================\n");

    logger::flush();
    delete[] trade_buf;
    return EXIT_SUCCESS;
}
