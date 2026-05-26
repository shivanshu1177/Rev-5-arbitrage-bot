// Paper trading mode: polls real CoinSwitch REST API prices, runs the hot-path
// signal engine, records every arb opportunity — but sends NO real orders.
//
// Think of it as a "live backtest": you see exactly what the bot WOULD have done
// on real prices in real time. Perfect for validating the strategy before going live.
//
// Usage:
//   ./paper_trader config/default.json [--verbose] [--spread-bps 5]
//
//   --verbose      Print each price update and trade signal to stdout.
//   --spread-bps N Add N bps synthetic spread between C2C1 and C2C2 when both
//                  hit the same CoinSwitch endpoint. Use if the two products have
//                  identical prices and you want to test signal-firing behaviour.
//
// When Ctrl+C is pressed, prints a full metrics summary and exits cleanly.
#include "hot_path/engine.hpp"
#include "hot_path/market_state.hpp"
#include "hot_path/virtual_ledger.hpp"
#include "hot_path/order_builder.hpp"
#include "hot_path/ring_buffer.hpp"
#include "cold_path/config_loader.hpp"
#include "cold_path/rest_poller.hpp"
#include "cold_path/metrics.hpp"
#include "cold_path/logger.hpp"
#include "common/types.hpp"
#include "common/constants.hpp"
#include "common/time_utils.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <thread>
#include <csignal>
#include <new>
#include <chrono>
#include <cerrno>

// ── Shutdown flag ─────────────────────────────────────────────────────────────
static std::atomic<bool> g_stop{false};
static void on_signal(int) noexcept { g_stop.store(true, std::memory_order_relaxed); }

// ── Per-stage latency accumulator ─────────────────────────────────────────────
struct LatencyStats {
    uint64_t min_ns = UINT64_MAX;
    uint64_t max_ns = 0;
    uint64_t sum_ns = 0;
    uint64_t count  = 0;

    // Log-scale histogram: <1us, 1-10us, 10-100us, 100us-1ms, 1-10ms, 10-100ms, >100ms
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
};

// ── Paper trade handler ───────────────────────────────────────────────────────
namespace arb {

struct PaperTradeHandler {
    TradeRecord*       records;
    size_t*            count;
    bool               verbose;

    void submit(const OrderRequest& req) noexcept {
        if (*count >= constants::MAX_TRADES) return;
        TradeRecord& r = records[(*count)++];
        r.timestamp_ns = req.timestamp_ns;
        r.pair_id      = req.pair_id;
        r.buy_venue    = req.buy_venue;
        r.sell_venue   = req.sell_venue;
        r.buy_price    = req.buy_price;
        r.sell_price   = req.sell_price;
        r.qty          = req.qty;
        r.net_pnl      = req.net_profit * req.qty;

        if (verbose) {
            std::printf("\n[SIGNAL] pair=%u  BUY@C2C%u=%.4f  SELL@C2C%u=%.4f"
                        "  qty=%.6f  net=$%.4f\n",
                        req.pair_id, req.buy_venue, req.buy_price,
                        req.sell_venue, req.sell_price,
                        req.qty, r.net_pnl);
        }
    }
};

} // namespace arb

// ── helpers ───────────────────────────────────────────────────────────────────
static void init_all_templates(
    arb::OrderTemplate* buy_templates,
    arb::OrderTemplate* sell_templates,
    const arb::Config&  cfg) noexcept
{
    for (int v = 1; v <= 2; ++v) {
        for (uint16_t i = 0; i < cfg.symbol_count; ++i) {
            const uint16_t pid = cfg.symbols[i].pair_id;
            const uint16_t idx = arb::MarketState::map_to_index(
                                     static_cast<uint8_t>(v), pid);
            arb::init_template(buy_templates[idx],  pid,
                               static_cast<uint8_t>(v), cfg.symbols[i].name, "BUY");
            arb::init_template(sell_templates[idx], pid,
                               static_cast<uint8_t>(v), cfg.symbols[i].name, "SELL");
        }
    }
}

static void init_ledger(arb::VirtualLedger& ledger, const arb::Config& cfg) noexcept {
    ledger.reset();
    ledger.seed_quote(cfg.initial_balance);
    const float base_seed = cfg.initial_balance * 0.01f;
    for (uint16_t i = 0; i < cfg.symbol_count; ++i) {
        ledger.seed_base(cfg.symbols[i].pair_id, base_seed);
    }
}

// ── main ──────────────────────────────────────────────────────────────────────
int main(int argc, char** argv) {
    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);

    time_utils::init();
    logger::setup("paper_trader.log");

    // Parse args
    const char* config_path     = "config/default.json";
    bool        verbose         = false;
    bool        latency_profile = false;
    float       spread_bps      = 0.0f;
    int         poll_ms         = 200;
    const char* record_file     = nullptr;
    int         duration_minutes= 0;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--verbose") == 0) {
            verbose = true;
        } else if (std::strcmp(argv[i], "--latency-profile") == 0) {
            latency_profile = true;
        } else if (std::strcmp(argv[i], "--spread-bps") == 0 && i + 1 < argc) {
            spread_bps = std::strtof(argv[++i], nullptr);
        } else if (std::strcmp(argv[i], "--poll-ms") == 0 && i + 1 < argc) {
            poll_ms = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--record") == 0 && i + 1 < argc) {
            record_file = argv[++i];
        } else if (std::strcmp(argv[i], "--duration-minutes") == 0 && i + 1 < argc) {
            duration_minutes = std::atoi(argv[++i]);
        } else if (argv[i][0] != '-') {
            config_path = argv[i];
        }
    }

    // Open tick recording file if requested.
    std::FILE* record_fp = nullptr;
    if (record_file) {
        record_fp = std::fopen(record_file, "wb");
        if (!record_fp) {
            std::fprintf(stderr, "Failed to open record file %s: %s\n",
                         record_file, std::strerror(errno));
            return EXIT_FAILURE;
        }
        spdlog::info("Recording ticks to {}", record_file);
    }

    arb::Config cfg{};
    try {
        cfg = arb::load_config(config_path);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Config error: %s\n", e.what());
        return EXIT_FAILURE;
    }

    spdlog::info("Paper trader starting — {} symbols, balance=${:.0f} (unified wallet), "
                 "poll={}ms, spread-bps={:.1f}",
                 cfg.symbol_count, cfg.initial_balance, poll_ms, spread_bps);

    if (spread_bps > 0.0f) {
        spdlog::warn("Using synthetic spread of {:.1f} bps between C2C1 and C2C2 "
                     "(real API prices are the same endpoint)", spread_bps);
    }

    // Pre-allocate trade log
    auto* trade_buf = new(std::nothrow) arb::TradeRecord[constants::MAX_TRADES];
    if (!trade_buf) {
        std::fprintf(stderr, "Failed to allocate trade buffer\n");
        return EXIT_FAILURE;
    }
    size_t      trade_count = 0;
    LatencyStats lat_queue;   // REST receipt → engine pop
    LatencyStats lat_engine;  // process_tick() duration

    // SPSC ring buffer: poller thread → engine (main) thread
    static arb::SPSCRingBuffer<arb::MarketTick, constants::RING_BUFFER_SIZE> ring_buf;

    // Stack-allocate hot-path state
    arb::MarketState   state;
    arb::VirtualLedger ledger;
    arb::OrderTemplate buy_templates[constants::MAX_INSTRUMENTS];
    arb::OrderTemplate sell_templates[constants::MAX_INSTRUMENTS];
    state.reset();
    state.max_stale_ns = cfg.max_stale_ns;
    init_all_templates(buy_templates, sell_templates, cfg);
    init_ledger(ledger, cfg);

    arb::PaperTradeHandler handler{trade_buf, &trade_count, verbose};

    // ── Start REST poller (background thread) ─────────────────────────────────
    arb::PollConfig pcfg;
    pcfg.cfg        = &cfg;
    pcfg.poll_ms    = poll_ms;
    pcfg.spread_bps = spread_bps;
    pcfg.verbose    = verbose;

    arb::RestPoller poller;
    poller.start(pcfg, [&](const arb::MarketTick& t) {
        // Push tick from REST poller into ring buffer.
        // The engine thread will drain it.
        while (!ring_buf.push(t) && !g_stop.load()) {
            std::this_thread::yield();
        }
    });

    spdlog::info("Polling started. Press Ctrl+C to stop and see results.");

    // ── Engine loop (main thread) ──────────────────────────────────────────────
    // Spin-polls the ring buffer and processes each tick.
    const auto t_start = std::chrono::steady_clock::now();
    uint64_t tick_count  = 0;
    uint64_t spin_count  = 0;

    while (!g_stop.load(std::memory_order_relaxed)) {
        arb::MarketTick tick{};
        if (ring_buf.pop(tick)) {
            const uint64_t pop_ns  = time_utils::now_ns();
            const uint64_t wait_ns = pop_ns - tick.timestamp_ns;
            if (record_fp) std::fwrite(&tick, sizeof(tick), 1, record_fp);
            arb::process_tick(tick, state, ledger, buy_templates, sell_templates, handler);
            const uint64_t done_ns = time_utils::now_ns();
            if (latency_profile) {
                lat_queue.record(wait_ns);
                lat_engine.record(done_ns - pop_ns);
            }
            ++tick_count;
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }

        // Check stop deadline once per 65536 iterations — avoids steady_clock overhead in spin.
        if (duration_minutes > 0 && (++spin_count & 0xFFFFu) == 0) {
            const auto elapsed_min = std::chrono::duration_cast<std::chrono::minutes>(
                std::chrono::steady_clock::now() - t_start).count();
            if (elapsed_min >= duration_minutes)
                g_stop.store(true, std::memory_order_relaxed);
        }
    }

    // Drain remaining ticks
    {
        arb::MarketTick tick{};
        while (ring_buf.pop(tick)) {
            if (record_fp) std::fwrite(&tick, sizeof(tick), 1, record_fp);
            arb::process_tick(tick, state, ledger, buy_templates, sell_templates, handler);
            ++tick_count;
        }
    }

    if (record_fp) {
        std::fflush(record_fp);
        std::fclose(record_fp);
        if (record_file)
            spdlog::info("Ticks written to {} ({} ticks, {:.1f} KB)",
                         record_file, tick_count, tick_count * 64.0 / 1024.0);
    }

    const auto t_end   = std::chrono::steady_clock::now();
    const double run_s = std::chrono::duration<double>(t_end - t_start).count();

    // ── Stop poller and print summary ─────────────────────────────────────────
    poller.stop();

    std::printf("\n");
    spdlog::info("Paper trade session complete: {:.0f}s, {} ticks processed, "
                 "{} polls OK, {} errors",
                 run_s, tick_count, poller.polls_ok(), poller.polls_err());

    if (latency_profile && lat_queue.count > 0) {
        std::printf("\n=== Latency Profile ===\n");
        std::printf("  Ticks measured       : %llu\n",
                    static_cast<unsigned long long>(lat_queue.count));
        std::printf("  Queue wait (REST->pop): min=%.1fus  avg=%.1fus  max=%.1fus\n",
                    lat_queue.min_ns / 1e3, lat_queue.avg_us(), lat_queue.max_ns / 1e3);
        lat_queue.print_histogram("REST->pop");
        std::printf("  Engine (process_tick): min=%.0fns  avg=%.0fns  max=%.0fns\n",
                    static_cast<double>(lat_engine.min_ns),
                    lat_engine.avg_us() * 1000.0,
                    static_cast<double>(lat_engine.max_ns));
        lat_engine.print_histogram("process_tick");
        std::printf("  E2E tick->record     : avg=%.1fus\n",
                    lat_queue.avg_us() + lat_engine.avg_us());
        std::printf("=======================\n");
    }

    const arb::MetricsSummary metrics = arb::compute_metrics(
        trade_buf, trade_count, cfg.initial_balance);
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
