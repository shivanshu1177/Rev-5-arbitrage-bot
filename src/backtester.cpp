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

    arb::MarketState   state;
    arb::VirtualLedger ledger;
    arb::OrderTemplate buy_templates[constants::MAX_INSTRUMENTS];
    arb::OrderTemplate sell_templates[constants::MAX_INSTRUMENTS];

    state.reset();
    state.max_stale_ns = cfg.max_stale_ns;
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
            arb::process_tick(t, state, ledger, buy_templates, sell_templates, handler);
        }
    } else {
        for (size_t i = 0; i < tick_count; ++i) {
            arb::process_tick(ticks[i], state, ledger, buy_templates, sell_templates, handler);
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

    const arb::MetricsSummary metrics = arb::compute_metrics(
        trade_buf, trade_count,
        cfg.initial_balance,   // unified wallet: single starting balance
        attempt_count);

    arb::print_summary(metrics);

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
