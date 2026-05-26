#include "cold_path/rest_poller.hpp"
#include "common/time_utils.hpp"
#include <curl/curl.h>
#include <openssl/evp.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <chrono>
#include <thread>
#include <vector>

// CoinSwitch depth endpoint — Ed25519 auth required on all requests.
// exchange=c2c1 or exchange=c2c2 selects the liquidity pool.
static const char* DEPTH_BASE = "https://coinswitch.co/trade/api/v2/depth?symbol=";
static const char* DEPTH_PATH = "/trade/api/v2/depth";
static constexpr size_t MAX_RESP = 65536;

namespace arb {

// ── Helpers ───────────────────────────────────────────────────────────────────

static uint64_t epoch_ms_now() noexcept {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

// URL-encode the symbol: "BTC/USDT" → "BTC%2FUSDT"
static void encode_symbol(const char* name, char* out, size_t cap) noexcept {
    size_t oi = 0;
    for (size_t i = 0; name[i] && oi + 4 < cap; ++i) {
        if (name[i] == '/') { out[oi++]='%'; out[oi++]='2'; out[oi++]='F'; }
        else                { out[oi++] = name[i]; }
    }
    out[oi] = '\0';
}

// ── libcurl write callback (used by parallel poll requests) ──────────────────
// Appends response chunks into PollRequest.response[].
static size_t poll_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) noexcept {
    auto* r = static_cast<PollRequest*>(userdata);
    const size_t n = size * nmemb;
    if (r->response_len + n < MAX_RESP - 1) {
        std::memcpy(r->response + r->response_len, ptr, n);
        r->response_len += n;
        r->response[r->response_len] = '\0';
    }
    return n;
}

// ── Minimal JSON parser ───────────────────────────────────────────────────────
// Finds the first price in a bid/ask array in the CoinSwitch JSON response.
// Avoids heap allocation — scans the response buffer directly.
static float find_first_price(const char* body, const char* key) noexcept {
    const char* p = std::strstr(body, key);
    if (!p) return 0.0f;
    p = std::strchr(p, '[');
    if (!p) return 0.0f;
    p = std::strchr(p + 1, '[');
    if (!p) return 0.0f;
    p = std::strchr(p, '"');
    if (!p) { /* bare number */ }
    else    { ++p; }
    return std::strtof(p, nullptr);
}

static float find_first_qty(const char* body, const char* key) noexcept {
    const char* p = std::strstr(body, key);
    if (!p) return 0.01f;
    p = std::strchr(p, '[');
    if (!p) return 0.01f;
    p = std::strchr(p + 1, '[');
    if (!p) return 0.01f;
    p = std::strchr(p + 1, ',');
    if (!p) return 0.01f;
    ++p;
    while (*p == ' ' || *p == '"') ++p;
    return std::strtof(p, nullptr);
}

// ── Ed25519 auth header builder ───────────────────────────────────────────────
// signing_string = "GET" + DEPTH_PATH + "?symbol=<decoded>&exchange=<ex>" + epoch_ms
// e.g. "GET/trade/api/v2/depth?symbol=BTC/USDT&exchange=c2c11716449072123"
static void build_depth_auth(
    const char* api_key, const char* private_key_hex, uint64_t epoch_ms,
    const char* symbol_decoded, const char* exchange,
    char out_h_key[160], char out_h_sig[200], char out_h_epoch[60]) noexcept
{
    char epoch_str[24];
    std::snprintf(epoch_str, sizeof(epoch_str), "%llu", (unsigned long long)epoch_ms);

    char msg[256]; size_t mlen = 0;
    auto cat = [&](const char* s, size_t n) {
        if (mlen + n < sizeof(msg) - 1) { std::memcpy(msg + mlen, s, n); mlen += n; }
    };
    cat("GET",          3);
    cat(DEPTH_PATH,     std::strlen(DEPTH_PATH));
    cat("?symbol=",     8);
    cat(symbol_decoded, std::strlen(symbol_decoded));
    cat("&exchange=",   10);
    cat(exchange,       std::strlen(exchange));
    cat(epoch_str,      std::strlen(epoch_str));

    uint8_t key_bytes[32] = {};
    for (int i = 0; i < 32; ++i) {
        char b[3] = {private_key_hex[i*2], private_key_hex[i*2+1], '\0'};
        key_bytes[i] = static_cast<uint8_t>(std::strtoul(b, nullptr, 16));
    }

    char sig_hex[129] = {};
    EVP_PKEY* pkey = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, key_bytes, 32);
    if (pkey) {
        EVP_MD_CTX* ctx = EVP_MD_CTX_new();
        if (ctx) {
            EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, pkey);
            uint8_t sig[64]; size_t sig_len = 64;
            EVP_DigestSign(ctx, sig, &sig_len,
                           reinterpret_cast<const uint8_t*>(msg), mlen);
            EVP_MD_CTX_free(ctx);
            static const char* hx = "0123456789abcdef";
            for (size_t i = 0; i < sig_len; ++i) {
                sig_hex[i*2]   = hx[(sig[i] >> 4) & 0xFu];
                sig_hex[i*2+1] = hx[ sig[i]       & 0xFu];
            }
            sig_hex[sig_len * 2] = '\0';
        }
        EVP_PKEY_free(pkey);
    }

    std::snprintf(out_h_key,   160, "X-AUTH-APIKEY: %s",    api_key);
    std::snprintf(out_h_sig,   200, "X-AUTH-SIGNATURE: %s", sig_hex);
    std::snprintf(out_h_epoch,  60, "X-AUTH-EPOCH: %s",     epoch_str);
}

// ── JSON → MarketTick parser ──────────────────────────────────────────────────
bool RestPoller::parse_orderbook_json(
    const char* body, size_t /*body_len*/,
    uint8_t venue, uint16_t pair_id,
    float spread_bps, MarketTick& out)
{
    float bid = find_first_price(body, "\"bids\"");
    float ask = find_first_price(body, "\"asks\"");
    float bid_qty = find_first_qty(body, "\"bids\"");
    float ask_qty = find_first_qty(body, "\"asks\"");

    if (bid <= 0.0f) {
        bid = find_first_price(body, "\"buy\"");
        ask = find_first_price(body, "\"sell\"");
        bid_qty = find_first_qty(body, "\"buy\"");
        ask_qty = find_first_qty(body, "\"sell\"");
    }

    if (bid <= 0.0f || ask <= 0.0f || ask < bid) return false;

    if (spread_bps > 0.0f && venue == 2) {
        const float mid    = (bid + ask) * 0.5f;
        const float offset = mid * spread_bps / 10000.0f;
        bid += offset;
        ask += offset;
    }

    out = MarketTick{};
    out.timestamp_ns  = time_utils::now_ns();
    out.venue         = venue;
    out._implicit_pad = 0;
    out.pair_id       = pair_id;
    out.best_bid      = bid;
    out.best_ask      = ask;
    out.bid_qty       = (bid_qty > 0.0f) ? bid_qty : 0.01f;
    out.ask_qty       = (ask_qty > 0.0f) ? ask_qty : 0.01f;
    return true;
}

// ── Parallel polling loop ─────────────────────────────────────────────────────
// All 2×symbol_count requests are fired simultaneously each cycle.
// Cycle time = max(longest_RTT, poll_ms) regardless of symbol count.
void RestPoller::poll_loop(PollConfig cfg, PushFn push_fn) {
    CURLM* multi = curl_multi_init();
    if (!multi) {
        std::fprintf(stderr, "RestPoller: curl_multi_init failed\n");
        running_.store(false);
        return;
    }

    const int N = cfg.cfg->symbol_count * 2;   // one slot per symbol × venue
    std::vector<PollRequest> reqs(static_cast<size_t>(N));

    const auto interval = std::chrono::milliseconds(cfg.poll_ms);

    while (running_.load(std::memory_order_relaxed)) {
        const auto t0 = std::chrono::steady_clock::now();

        // ── Setup: configure all easy handles and add to multi ────────────────
        for (int s = 0; s < (int)cfg.cfg->symbol_count; ++s) {
            const auto& sym = cfg.cfg->symbols[s];
            for (int v = 0; v < 2; ++v) {
                PollRequest& r = reqs[static_cast<size_t>(s * 2 + v)];

                // Free headers from previous cycle
                if (r.hdrs) { curl_slist_free_all(r.hdrs); r.hdrs = nullptr; }

                // Reset response buffer
                r.response_len = 0;
                r.response[0]  = '\0';

                // Request metadata
                r.venue      = static_cast<uint8_t>(v + 1);
                r.pair_id    = sym.pair_id;
                r.spread_bps = (v == 1) ? cfg.spread_bps : 0.0f;
                std::strncpy(r.symbol_name, sym.name,           sizeof(r.symbol_name) - 1);
                encode_symbol(sym.name, r.symbol_enc,           sizeof(r.symbol_enc));
                std::strncpy(r.exchange,    v == 0 ? "c2c1" : "c2c2", sizeof(r.exchange) - 1);

                if (!r.easy) r.easy = curl_easy_init();

                // Build URL and auth headers (fresh epoch_ms per request)
                const uint64_t now_ms = epoch_ms_now();
                char url[512], h_key[160], h_sig[200], h_epoch[60];
                std::snprintf(url, sizeof(url), "%s%s&exchange=%s",
                              DEPTH_BASE, r.symbol_enc, r.exchange);
                build_depth_auth(cfg.cfg->api_key, cfg.cfg->private_key_hex,
                                 now_ms, r.symbol_name, r.exchange,
                                 h_key, h_sig, h_epoch);

                r.hdrs = curl_slist_append(r.hdrs, h_key);
                r.hdrs = curl_slist_append(r.hdrs, h_sig);
                r.hdrs = curl_slist_append(r.hdrs, h_epoch);
                r.hdrs = curl_slist_append(r.hdrs, "Accept: application/json");

                curl_easy_setopt(r.easy, CURLOPT_URL,           url);
                curl_easy_setopt(r.easy, CURLOPT_HTTPHEADER,    r.hdrs);
                curl_easy_setopt(r.easy, CURLOPT_WRITEFUNCTION, poll_write_cb);
                curl_easy_setopt(r.easy, CURLOPT_WRITEDATA,     &r);
                curl_easy_setopt(r.easy, CURLOPT_TIMEOUT_MS,    3000L);
                curl_easy_setopt(r.easy, CURLOPT_NOSIGNAL,      1L);
                curl_easy_setopt(r.easy, CURLOPT_TCP_KEEPALIVE, 1L);
                curl_easy_setopt(r.easy, CURLOPT_PRIVATE,       &r);  // tag for harvest
                curl_easy_setopt(r.easy, CURLOPT_USERAGENT,
                    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 "
                    "(KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36");

                curl_multi_add_handle(multi, r.easy);
            }
        }

        // ── Fire: run all N requests in parallel ──────────────────────────────
        // curl_multi_perform is non-blocking; curl_multi_wait yields the thread
        // for up to 10ms between polls so we don't burn 100% CPU while waiting.
        int still_running = N;
        while (still_running > 0 && running_.load(std::memory_order_relaxed)) {
            curl_multi_perform(multi, &still_running);
            curl_multi_wait(multi, nullptr, 0, 10 /*ms timeout*/, nullptr);
        }

        // ── Harvest: collect completed responses ──────────────────────────────
        CURLMsg* msg;
        int msgs_left;
        while ((msg = curl_multi_info_read(multi, &msgs_left))) {
            if (msg->msg != CURLMSG_DONE) continue;

            // Retrieve the PollRequest tagged via CURLOPT_PRIVATE
            PollRequest* r = nullptr;
            curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &r);

            long http_code = 0;
            curl_easy_getinfo(msg->easy_handle, CURLINFO_RESPONSE_CODE, &http_code);

            if (msg->data.result == CURLE_OK && http_code == 200) {
                MarketTick tick{};
                if (parse_orderbook_json(r->response, r->response_len,
                                         r->venue, r->pair_id,
                                         r->spread_bps, tick)) {
                    push_fn(tick);
                    polls_ok_.fetch_add(1, std::memory_order_relaxed);
                    if (cfg.verbose) {
                        std::printf("[C2C%d] %s  bid=%.4f  ask=%.4f\n",
                                    r->venue, r->symbol_name,
                                    tick.best_bid, tick.best_ask);
                    }
                } else {
                    polls_err_.fetch_add(1, std::memory_order_relaxed);
                }
            } else {
                polls_err_.fetch_add(1, std::memory_order_relaxed);
            }

            // Must remove before the handle can be reused next cycle
            curl_multi_remove_handle(multi, msg->easy_handle);
        }

        // Sleep for the remainder of poll_ms.
        // This now covers the ENTIRE batch (not per-symbol), so the effective
        // refresh rate for every symbol is poll_ms (not poll_ms × N).
        const auto elapsed = std::chrono::steady_clock::now() - t0;
        if (elapsed < interval)
            std::this_thread::sleep_for(interval - elapsed);
    }

    // Cleanup: free all easy handles and header slists
    for (auto& r : reqs) {
        if (r.hdrs) curl_slist_free_all(r.hdrs);
        if (r.easy) curl_easy_cleanup(r.easy);
    }
    curl_multi_cleanup(multi);
}

// ── Public API ────────────────────────────────────────────────────────────────

bool RestPoller::start(const PollConfig& pcfg, PushFn push_fn) {
    if (running_.load()) return false;
    running_.store(true);
    thread_ = std::thread(&RestPoller::poll_loop, this, pcfg, std::move(push_fn));
    return true;
}

void RestPoller::stop() {
    running_.store(false, std::memory_order_relaxed);
    if (thread_.joinable()) thread_.join();
}

} // namespace arb
