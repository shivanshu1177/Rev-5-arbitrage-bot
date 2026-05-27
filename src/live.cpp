// Live trading entry point.
// Connects to the CoinSwitch Pro WebSocket for real-time market data,
// then runs the same hot-path core (process_tick) as the backtester.
//
// Architecture:
//   WebSocket thread  (cold path) → push MarketTick into SPSCRingBuffer
//   Engine thread     (hot path)  → spin-poll RingBuffer, call process_tick
//   Order sender      (live mode) → sign + send REST orders via libcurl
//
// NOTE: CoinSwitch Pro WebSocket subscription format and JSON field names
//       are based on the documented API. Verify against live endpoint if
//       field names change (look for "exchange", "symbol", "data.bids/asks").
#include "hot_path/engine.hpp"
#include "hot_path/market_state.hpp"
#include "hot_path/virtual_ledger.hpp"
#include "hot_path/order_builder.hpp"
#include "hot_path/ring_buffer.hpp"
#include "cold_path/config_loader.hpp"
#include "cold_path/order_sender.hpp"
#include "cold_path/order_tracker.hpp"
#include "cold_path/sim_handler.hpp"
#include "cold_path/logger.hpp"
#include "common/types.hpp"
#include "common/constants.hpp"
#include "common/time_utils.hpp"

#include "cold_path/metrics.hpp"

#include <ixwebsocket/IXWebSocket.h>
#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <thread>
#include <csignal>
#include <chrono>
#include <charconv>
#include <new>
#include <string>
#include <unordered_map>
#include <utility>

// ── Globals for graceful shutdown ─────────────────────────────────────────────

static std::atomic<bool> g_running{true};

static void signal_handler(int) noexcept {
    g_running.store(false, std::memory_order_relaxed);
}

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

// ── Live order handler ────────────────────────────────────────────────────────

namespace arb {

struct LiveOrderHandler {
    OrderSender*   sender;
    OrderTemplate* buy_templates;
    OrderTemplate* sell_templates;
    OrderTracker*  tracker;

    void submit(const OrderRequest& req) noexcept {
        // Reconstruct indices from the request (mirrors engine.hpp patch_order indexing).
        const uint16_t bi = MarketState::map_to_index(req.buy_venue,  req.pair_id);
        const uint16_t si = MarketState::map_to_index(req.sell_venue, req.pair_id);
        // Send both legs. send_order() is blocking but sub-millisecond for a signed REST call.
        sender->send_order(req.buy_venue,  buy_templates[bi]);
        sender->send_order(req.sell_venue, sell_templates[si]);
        // Register both legs in the state machine for timeout/failure detection.
        tracker->track_arb(req,
            sender->last_order_id(req.buy_venue  - 1u),
            sender->last_order_id(req.sell_venue - 1u));
    }
};

} // namespace arb

// ── main ──────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    time_utils::init();
    logger::setup("live.log");

    const char* config_path     = "config/default.json";
    bool        paper_mode      = false;
    bool        latency_profile = false;
    int         duration_minutes= 0;

    for (int i = 1; i < argc; ++i) {
        if      (std::strcmp(argv[i], "--paper")            == 0) paper_mode      = true;
        else if (std::strcmp(argv[i], "--latency-profile")  == 0) latency_profile = true;
        else if (std::strcmp(argv[i], "--duration-minutes") == 0 && i+1 < argc)
            duration_minutes = std::atoi(argv[++i]);
        else if (argv[i][0] != '-') config_path = argv[i];
    }

    arb::Config cfg{};
    try {
        cfg = arb::load_config(config_path);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Config error: %s\n", e.what());
        return EXIT_FAILURE;
    }

    // Verify that API key is configured (not placeholder value).
    if (cfg.api_key[0] == 'Y') {
        spdlog::warn("API key appears to be a placeholder value — edit config/default.json");
    }

    spdlog::info("Live mode starting. Venues: C2C1={}, C2C2={}",
                 cfg.venues[0].base_url, cfg.venues[1].base_url);

    // Initialise order sender (real CURL handles to CoinSwitch REST API).
    arb::OrderSender sender;
    if (!sender.init(cfg)) {
        std::fprintf(stderr, "OrderSender init failed\n");
        return EXIT_FAILURE;
    }

    auto* trade_buf = new(std::nothrow) arb::TradeRecord[constants::MAX_TRADES];
    if (!trade_buf) { std::fprintf(stderr, "alloc failed\n"); return EXIT_FAILURE; }
    size_t       trade_count   = 0;
    size_t       attempt_count = 0;
    LatencyStats lat_queue, lat_engine;
    arb::OrderTracker tracker;

    // SPSC ring buffer between WebSocket thread and engine thread.
    static arb::SPSCRingBuffer<arb::MarketTick, constants::RING_BUFFER_SIZE> ring_buf;

    // Hot-path data structures (stack-allocated in the engine thread).
    arb::MarketState   state;
    arb::VirtualLedger ledger;
    arb::OrderTemplate buy_templates[constants::MAX_INSTRUMENTS];
    arb::OrderTemplate sell_templates[constants::MAX_INSTRUMENTS];
    state.reset();
    state.max_stale_ns = cfg.max_stale_ns;
    // WS push feed: tighten staleness threshold — 500ms of silence from one venue
    // is enough to suppress signals and avoid acting on stale cross-venue prices.
    state.max_stale_ns            = 500'000'000ULL;
    state.max_exchange_ts_skew_ms = cfg.max_exchange_ts_skew_ms;
    ledger.reset();
    // Seed balances from config (in production: fetch from REST API /account/balance).
    ledger.seed_quote(cfg.initial_balance);
    const float coin_seed = cfg.initial_balance * 0.01f;
    for (uint16_t i = 0; i < cfg.symbol_count; ++i) {
        const uint16_t pid = cfg.symbols[i].pair_id;
        const uint16_t i1  = arb::MarketState::map_to_index(1, pid);
        const uint16_t i2  = arb::MarketState::map_to_index(2, pid);
        arb::init_template(buy_templates[i1],  pid, 1, cfg.symbols[i].name, "BUY");
        arb::init_template(buy_templates[i2],  pid, 2, cfg.symbols[i].name, "BUY");
        arb::init_template(sell_templates[i1], pid, 1, cfg.symbols[i].name, "SELL");
        arb::init_template(sell_templates[i2], pid, 2, cfg.symbols[i].name, "SELL");
        ledger.seed_base(pid, coin_seed);
    }

    arb::LiveOrderHandler handler{&sender, buy_templates, sell_templates, &tracker};

    // ── Socket.IO v4 helpers (cold path — WS callback only) ──────────────────

    // "BTC/USDT" → "BTC,USDT" for Socket.IO subscription pairs
    auto to_ws_pair = [](const char* s) -> std::string {
        std::string out(s);
        auto pos = out.find('/');
        if (pos != std::string::npos) out[pos] = ',';
        return out;
    };

    // Extract /namespace and remaining payload from SIO message body (after EIO+SIO type).
    // "/c2c1,[...]" → {"/c2c1", "[...]"}    "[...]" → {"", "[...]"}
    auto parse_ns = [](const std::string& body) -> std::pair<std::string, std::string> {
        if (!body.empty() && body[0] == '/') {
            auto comma = body.find(',');
            if (comma != std::string::npos)
                return {body.substr(0, comma), body.substr(comma + 1)};
        }
        return {"", body};
    };

    // Send a Socket.IO EVENT frame: "42{ns},[event, data]"
    auto sio_emit = [](ix::WebSocket& ws, const char* ns,
                       const char* event, const nlohmann::json& data) {
        std::string pkt = "42";
        pkt += ns;
        pkt += ',';
        pkt += nlohmann::json::array({event, data}).dump();
        ws.send(pkt);
    };

    // Subscribe to FETCH_ORDER_BOOK_CS_PRO for all symbols on one namespace.
    auto subscribe_ns = [&](ix::WebSocket& ws, const char* ns) {
        for (uint16_t i = 0; i < cfg.symbol_count; ++i) {
            sio_emit(ws, ns, "FETCH_ORDER_BOOK_CS_PRO",
                     {{"event", "subscribe"}, {"pair", to_ws_pair(cfg.symbols[i].name)}});
        }
        spdlog::info("Subscribed {} symbols on {}", cfg.symbol_count, ns);
    };

    // Comma-format → pair_id lookup for WS payload parsing.
    std::unordered_map<std::string, uint16_t> ws_pair_id;
    for (uint16_t i = 0; i < cfg.symbol_count; ++i)
        ws_pair_id[to_ws_pair(cfg.symbols[i].name)] = cfg.symbols[i].pair_id;

    // ── WebSocket thread (cold path) ──────────────────────────────────────────
    std::thread ws_thread([&]() {
        spdlog::info("WebSocket thread: connecting to {}", cfg.ws_url);

        ix::WebSocket ws;
        ws.setUrl(cfg.ws_url);
        ws.enableAutomaticReconnection();

        bool c2c1_ready = false, c2c2_ready = false;

        ws.setOnMessageCallback([&](const ix::WebSocketMessagePtr& msg) {
            if (msg->type == ix::WebSocketMessageType::Open) {
                // TCP connection established. Do NOT send namespace CONNECTs yet —
                // wait for the EIO '0' OPEN packet (Socket.IO handshake must complete first).
                spdlog::info("WS TCP connected — awaiting EIO OPEN");
                c2c1_ready = false;
                c2c2_ready = false;

            } else if (msg->type == ix::WebSocketMessageType::Message) {
                const std::string& raw = msg->str;
                if (raw.empty()) return;

                const char eio = raw[0];

                // EIO OPEN: Socket.IO handshake complete. Now connect to namespaces.
                if (eio == '0') {
                    spdlog::info("EIO OPEN received — connecting to namespaces");
                    ws.send("40/c2c1,");
                    ws.send("40/c2c2,");
                    return;
                }

                // EIO PING: keep-alive from server → reply with PONG.
                if (eio == '2') { ws.send("3"); return; }

                // EIO MESSAGE carries a Socket.IO packet.
                if (eio != '4' || raw.size() < 2) return;
                const char sio  = raw[1];
                const std::string body = raw.substr(2);

                // SIO CONNECT ack: namespace is live, send subscriptions.
                if (sio == '0') {
                    auto [ns, rest] = parse_ns(body);
                    if (ns == "/c2c1" && !c2c1_ready) {
                        c2c1_ready = true;
                        subscribe_ns(ws, "/c2c1");
                    } else if (ns == "/c2c2" && !c2c2_ready) {
                        c2c2_ready = true;
                        subscribe_ns(ws, "/c2c2");
                    }
                    return;
                }

                // SIO EVENT: parse order-book snapshot.
                if (sio != '2') return;

                auto [ns, payload] = parse_ns(body);
                const uint8_t venue = (ns == "/c2c1") ? 1u : (ns == "/c2c2") ? 2u : 0u;
                if (venue == 0) return;

                // Payload: ["FETCH_ORDER_BOOK_CS_PRO", {"s":"BTC,USDT","bids":[...],"asks":[...]}]
                auto arr = nlohmann::json::parse(payload, nullptr, false);
                if (arr.is_discarded() || !arr.is_array() || arr.size() < 2) return;
                if (arr[0] != "FETCH_ORDER_BOOK_CS_PRO") return;

                auto& data = arr[1];
                if (!data.contains("bids") || !data.contains("asks")) return;
                if (data["bids"].empty() || data["asks"].empty()) return;

                // CoinSwitch has used "s", "pair", and "symbol" across API versions.
                // Try all known field names; drop the tick if none match.
                std::string sym;
                for (const char* key : {"s", "pair", "symbol"}) {
                    if (data.contains(key)) { sym = data[key].get<std::string>(); break; }
                }
                if (sym.empty()) return;
                auto it = ws_pair_id.find(sym);
                if (it == ws_pair_id.end()) return;

                arb::MarketTick tick{};
                tick.venue         = venue;
                tick._implicit_pad = 0;
                tick.pair_id       = it->second;

                // from_chars: non-throwing, locale-independent. JSON field access can still
                // throw on unexpected structure, so the outer try/catch stays.
                try {
                    const std::string bp = data["bids"][0][0].get<std::string>();
                    const std::string ap = data["asks"][0][0].get<std::string>();
                    const std::string bq = data["bids"][0][1].get<std::string>();
                    const std::string aq = data["asks"][0][1].get<std::string>();
                    auto fc = [](const std::string& s, float& v) -> bool {
                        auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
                        return ec == std::errc{};
                    };
                    if (!fc(bp, tick.best_bid) || !fc(ap, tick.best_ask) ||
                        !fc(bq, tick.bid_qty)  || !fc(aq, tick.ask_qty)) return;
                } catch (...) { return; }

                // Extract exchange-side timestamp for cross-venue skew gate.
                // API docs confirm field name is "timestamp" (Unix ms integer).
                // Probe in doc-first order; handle all nlohmann storage types (unsigned,
                // signed, float) since large ints may be stored as any of these.
                // uint32_t truncation is safe: skew gate compares deltas (≤100ms),
                // never absolute values, so the truncation cancels out.
                tick.exchange_ts_ms = 0;
                for (const char* key : {"timestamp", "t", "ts", "T"}) {
                    if (!data.contains(key)) continue;
                    const auto& tv = data[key];
                    uint64_t val = 0;
                    if      (tv.is_number_unsigned()) val = tv.get<uint64_t>();
                    else if (tv.is_number_integer())  val = static_cast<uint64_t>(tv.get<int64_t>());
                    else if (tv.is_number_float())    val = static_cast<uint64_t>(tv.get<double>());
                    else continue;
                    if (val == 0) continue;
                    tick.exchange_ts_ms = static_cast<uint32_t>(val);
                    break;
                }

                // Local monotonic clock for staleness tracking (consistent across both venues).
                tick.timestamp_ns = time_utils::now_ns();

                if (tick.best_bid <= 0.0f || tick.best_ask <= 0.0f) return;
                if (tick.best_ask <= tick.best_bid) [[unlikely]] return;  // crossed/locked book

                while (!ring_buf.push(tick) && g_running.load(std::memory_order_relaxed))
                    std::this_thread::yield();

            } else if (msg->type == ix::WebSocketMessageType::Error) {
                spdlog::error("WS error: {}", msg->errorInfo.reason);
            } else if (msg->type == ix::WebSocketMessageType::Close) {
                spdlog::warn("WS closed (code={} reason={})",
                             msg->closeInfo.code, msg->closeInfo.reason);
            }
        });

        ws.start();
        while (g_running.load(std::memory_order_relaxed))
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        ws.stop();
        spdlog::info("WebSocket thread exited");
    });

    // ── Order Updates WebSocket thread (cold path) ───────────────────────────
    // Subscribes to the /orderupdates namespace and calls tracker.update_by_exchange_oid()
    // whenever an order's fill status changes. This replaces polling GET /order and
    // keeps VirtualLedger accurate in live mode.
    // URL: same host as cfg.ws_url but with /spot/order-updates/ path.
    std::string ou_url = cfg.ws_url;
    {
        const std::string old_sfx = "/spot/";
        const std::string new_sfx = "/spot/order-updates/";
        const auto pos = ou_url.find(old_sfx);
        if (pos != std::string::npos)
            ou_url.replace(pos, old_sfx.size(), new_sfx);
    }

    std::thread ou_thread([&]() {
        spdlog::info("Order-updates WS: connecting to {}", ou_url);
        ix::WebSocket ou;
        ou.setUrl(ou_url);
        ou.enableAutomaticReconnection();

        ou.setOnMessageCallback([&](const ix::WebSocketMessagePtr& msg) {
            if (msg->type != ix::WebSocketMessageType::Message) {
                if (msg->type == ix::WebSocketMessageType::Error)
                    spdlog::error("Order-updates WS error: {}", msg->errorInfo.reason);
                return;
            }
            const std::string& raw = msg->str;
            if (raw.empty()) return;

            const char eio = raw[0];
            if (eio == '0') { ou.send("40/orderupdates,"); return; }  // EIO OPEN → connect ns
            if (eio == '2') { ou.send("3");               return; }   // EIO PING → PONG
            if (eio != '4' || raw.size() < 2)             return;

            const char sio  = raw[1];
            const std::string body = raw.substr(2);

            if (sio == '0') {
                // SIO CONNECT ack: subscribe with API key
                auto [ns, rest] = parse_ns(body);
                if (ns == "/orderupdates") {
                    sio_emit(ou, "/orderupdates", "FETCH_ORDER_UPDATES",
                             {{"event", "subscribe"}, {"apikey", cfg.api_key}});
                    spdlog::info("Order-updates WS: subscribed");
                }
                return;
            }
            if (sio != '2') return;

            auto [ns, payload] = parse_ns(body);
            if (ns != "/orderupdates") return;

            auto arr = nlohmann::json::parse(payload, nullptr, false);
            if (arr.is_discarded() || !arr.is_array() || arr.size() < 2) return;
            if (arr[0] != "FETCH_ORDER_UPDATES") return;

            const auto& d = arr[1];
            // Fields per API docs:  i=order_id  X=status  z=filled_base_qty
            if (!d.contains("i") || !d.contains("X")) return;

            const std::string oid    = d["i"].get<std::string>();
            const std::string status = d["X"].get<std::string>();
            float filled = 0.0f;
            if (d.contains("z")) {
                const std::string zs = d["z"].get<std::string>();
                std::from_chars(zs.data(), zs.data() + zs.size(), filled);
            }

            arb::OrderState st = arb::OrderState::PENDING;
            if      (status == "EXECUTED")           st = arb::OrderState::FILLED;
            else if (status == "PARTIALLY_EXECUTED") st = arb::OrderState::PARTIAL;
            else if (status == "CANCELLED")          st = arb::OrderState::CANCELLED;

            tracker.update_by_exchange_oid(oid.c_str(), st, filled);
            spdlog::info("Order update: oid={} status={} filled={:.6f}", oid, status, filled);
        });

        ou.start();
        while (g_running.load(std::memory_order_relaxed))
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        ou.stop();
        spdlog::info("Order-updates WS thread exited");
    });

    // ── Engine thread (hot path) ──────────────────────────────────────────────
    spdlog::info("Engine running in {} mode. Press Ctrl+C to stop.",
                 paper_mode ? "PAPER" : "LIVE");

    uint64_t   tick_count = 0;
    const auto t_start    = std::chrono::steady_clock::now();

    auto engine_loop = [&](auto& h) {
        arb::MarketTick t{};
        uint64_t spin_count = 0;
        while (g_running.load(std::memory_order_relaxed)) {
            if (ring_buf.pop(t)) {
                const uint64_t pop_ns  = time_utils::now_ns();
                arb::process_tick(t, state, ledger, buy_templates, sell_templates, h);
                const uint64_t done_ns = time_utils::now_ns();
                lat_queue.record(pop_ns - t.timestamp_ns);
                lat_engine.record(done_ns - pop_ns);
                ++tick_count;
            }
            // Check stop deadline + order timeouts once per 65536 iterations —
            // avoids steady_clock overhead in tight spin.
            if ((++spin_count & 0xFFFFu) == 0) {
                if (duration_minutes > 0) {
                    const auto elapsed = std::chrono::duration_cast<std::chrono::minutes>(
                        std::chrono::steady_clock::now() - t_start).count();
                    if (elapsed >= duration_minutes)
                        g_running.store(false, std::memory_order_relaxed);
                }
                // Time-gated order timeout check (~100ms between checks).
                static uint64_t last_timeout_check_ns = 0;
                const uint64_t now_check = time_utils::now_ns();
                if (now_check - last_timeout_check_ns > 100'000'000ULL) {
                    auto failures = tracker.check_timeouts(cfg.order_timeout_ms);
                    for (auto& rec : failures)
                        tracker.handle_leg_failure(rec, ledger);
                    last_timeout_check_ns = now_check;
                }
            }
        }
        // drain remaining ticks after shutdown signal
        while (ring_buf.pop(t)) {
            arb::process_tick(t, state, ledger, buy_templates, sell_templates, h);
            ++tick_count;
        }
    };

    if (paper_mode) {
        arb::BacktestOrderHandler phandler{
            trade_buf, &trade_count, &attempt_count, &ledger, cfg.sim
        };
        engine_loop(phandler);
        spdlog::info("Paper attempts: {}  fills: {}  (fill rate {:.1f}%)",
                     attempt_count, trade_count,
                     attempt_count > 0 ? 100.0 * trade_count / attempt_count : 0.0);
    } else {
        engine_loop(handler);
    }

    spdlog::info("Shutdown signal received. Joining WS threads...");
    if (ws_thread.joinable()) ws_thread.join();
    if (ou_thread.joinable()) ou_thread.join();

    // ── Exit report ───────────────────────────────────────────────────────────
    const double run_s = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t_start).count();
    spdlog::info("Session complete: {:.0f}s, {} ticks", run_s, tick_count);

    if (lat_queue.count > 0) {
        if (latency_profile) {
            std::printf("\n=== WS Latency Profile ===\n");
            std::printf("  Ticks measured        : %llu\n",
                        static_cast<unsigned long long>(lat_queue.count));
            std::printf("  WS parse -> engine pop: min=%.1fus  avg=%.1fus  max=%.1fus\n",
                        lat_queue.min_ns / 1e3, lat_queue.avg_us(), lat_queue.max_ns / 1e3);
            lat_queue.print_histogram("WS->pop");
            std::printf("  Engine (process_tick) : min=%.0fns  avg=%.0fns  max=%.0fns\n",
                        static_cast<double>(lat_engine.min_ns),
                        lat_engine.avg_us() * 1000.0,
                        static_cast<double>(lat_engine.max_ns));
            lat_engine.print_histogram("process_tick");
            std::printf("  E2E WS -> record      : avg=%.1fus\n",
                        lat_queue.avg_us() + lat_engine.avg_us());
            std::printf("==========================\n");
        }
        FILE* pf = std::fopen("latency_profile_ws.json", "w");
        if (pf) {
            std::fprintf(pf, "{\n  \"mode\": \"%s\",\n  \"tick_count\": %llu,\n",
                         paper_mode ? "paper_ws" : "live_ws",
                         static_cast<unsigned long long>(lat_queue.count));
            lat_queue.write_json(pf, "queue_latency");
            std::fprintf(pf, ",\n");
            lat_engine.write_json(pf, "engine_latency");
            std::fprintf(pf, "\n}\n");
            std::fclose(pf);
            std::printf("Latency profile  : latency_profile_ws.json\n");
            std::system("python3 tools/generate_charts.py "
                        "--input latency_profile_ws.json "
                        "--out latency_histogram_ws.png 2>/dev/null && "
                        "echo 'Histogram        : latency_histogram_ws.png'");
        }
    }

    if (paper_mode) {
        const arb::MetricsSummary metrics = arb::compute_metrics(
            trade_buf, trade_count, cfg.initial_balance);
        arb::print_summary(metrics);
        std::printf("\n=== Account Balance ===\n");
        std::printf("  Starting USDT: $%.2f\n", cfg.initial_balance);
        std::printf("  Final USDT:    $%.2f\n", ledger.quote_balance);
        std::printf("  Net change:    $%.2f\n", ledger.quote_balance - cfg.initial_balance);
        std::printf("======================\n");
    }

    delete[] trade_buf;
    sender.cleanup();
    logger::flush();
    spdlog::info("Live mode exited cleanly.");
    return EXIT_SUCCESS;
}
