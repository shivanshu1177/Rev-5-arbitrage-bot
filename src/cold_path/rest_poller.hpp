#pragma once
// Polls the CoinSwitch REST API for live order-book data (paper trading / recording mode).
//
// ALL symbols on BOTH venues are fetched in PARALLEL each poll cycle using libcurl's
// curl_multi interface. With N symbols, one cycle fires 2N requests simultaneously
// and completes in one network RTT (~80ms), instead of sequentially (N × 200ms).
//
// Before (sequential):  cycle = poll_ms × symbol_count  =  200ms × 3 = 600ms per symbol
// After  (parallel):    cycle = max(all_RTTs, poll_ms)  =  max(~80ms, 200ms) = 200ms total
//
// Authenticated endpoint (Ed25519 auth required):
//   GET https://coinswitch.co/trade/api/v2/depth?symbol=BTC%2FUSDT&exchange=c2c1
//
// The caller supplies a push_fn callback (called from the polling thread):
//   void push_fn(const arb::MarketTick&)
#include "common/types.hpp"
#include <functional>
#include <atomic>
#include <thread>

// Forward-declare CURL types to avoid pulling headers into all TUs.
using CURL  = void;
using CURLM = void;
struct curl_slist;

namespace arb {

struct PollConfig {
    const Config* cfg        = nullptr;
    int           poll_ms    = 200;    // milliseconds per full parallel batch
    float         spread_bps = 0.0f;  // synthetic spread added to c2c2 prices
    bool          verbose    = false;
};

// One request slot per symbol × venue.
// Owns the easy handle, header slist, and response buffer for one parallel request.
struct PollRequest {
    CURL*              easy         = nullptr;
    struct curl_slist* hdrs         = nullptr;
    char               response[65536] = {};
    size_t             response_len = 0;
    uint8_t            venue        = 0;       // 1=c2c1, 2=c2c2
    uint16_t           pair_id      = 0;
    float              spread_bps   = 0.0f;
    char               symbol_name[16] = {};  // "BTC/USDT" — for signing + display
    char               symbol_enc[32]  = {};  // "BTC%2FUSDT" — for URL
    char               exchange[8]     = {};  // "c2c1" or "c2c2"
};

class RestPoller {
public:
    using PushFn = std::function<void(const MarketTick&)>;

    RestPoller() = default;
    ~RestPoller() { stop(); }
    RestPoller(const RestPoller&)            = delete;
    RestPoller& operator=(const RestPoller&) = delete;

    // Start the polling loop in a background thread.
    bool start(const PollConfig& cfg, PushFn push_fn);

    // Signal the polling thread to stop and wait for it to join.
    void stop();

    [[nodiscard]] bool running() const noexcept {
        return running_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] uint64_t polls_ok()  const noexcept { return polls_ok_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t polls_err() const noexcept { return polls_err_.load(std::memory_order_relaxed); }

private:
    std::atomic<bool>     running_{false};
    std::thread           thread_;
    std::atomic<uint64_t> polls_ok_{0};
    std::atomic<uint64_t> polls_err_{0};

    void poll_loop(PollConfig cfg, PushFn push_fn);

    bool parse_orderbook_json(const char* body, size_t body_len,
                              uint8_t venue, uint16_t pair_id,
                              float spread_bps, MarketTick& out);
};

} // namespace arb
