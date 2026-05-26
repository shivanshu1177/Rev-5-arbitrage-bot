# Cross-Venue Arbitrage Engine

C++20 statistical arbitrage engine detecting price discrepancies between two CoinSwitch Pro accounts
via a lock-free SPSC ring buffer and a branchless 15 ns execution core. Built to demonstrate the
software architecture patterns used in institutional HFT systems.

---

## Key Numbers

| Metric | Value | Conditions |
|---|---|---|
| Language | C++20, `-O3 -mcpu=apple-m2 -flto=thin` | Apple Clang, CMake |
| Backtest throughput | **7.67M ticks/sec** | 1M synthetic ticks, M2, single core |
| `process_tick` average | **451 ns** | 10-minute live WebSocket soak |
| `process_tick` max spike | 16 ms | OS interrupt / L3 eviction on macOS |
| WS→engine avg (2-min warm) | 13.9 µs | macOS, two unscheduled threads |
| WS→engine avg (10-min soak) | **801 µs** | macOS time-sharing degradation |
| WS→engine max | 260 ms | macOS scheduler preemption |
| WS→engine target (Linux) | < 5 µs | `isolcpus` + `SCHED_FIFO` + `mlockall` |

The soak numbers are the ones that matter. See [`inout.md`](inout.md) §3 for the full telemetry
breakdown and what the 57× degradation tells you about OS scheduler behaviour.

---

## Architecture

```
WebSocket thread (cold path: IXWebSocket, Socket.IO v4, std::from_chars, crossed-book guard)
        │  lock-free push — acquire/release, no mutex
        ▼
SPSCRingBuffer<MarketTick, 65536>   (alignas(64) producer/consumer positions, no false sharing)
        │  spin pop
        ▼
process_tick<H>(tick, MarketState, VirtualLedger, OrderTemplate[], handler)
  ├── MarketState::update()       — 4.8 KB, L1-resident on M2
  ├── evaluate_signal()           — 3-stage branchless (FCMP+CSEL, bit_cast XOR, uint32 thresh)
  └── VirtualLedger::try_arb()   — 4-way branchless min (IEEE 754 uint32 comparison)
              │
              ├── BacktestOrderHandler  → TradeRecord[]  (fill prob, latency penalty, TDS/GST)
              └── LiveOrderHandler      → OrderSender    (Ed25519-signed REST, libcurl)
```

---

## What This Demonstrates

Each skill is mapped to a concrete artifact — nothing claimed that isn't in the source.

| Skill | Artifact |
|---|---|
| Lock-free concurrent data structures | `src/hot_path/ring_buffer.hpp` — SPSC acquire/release, cache-line padding |
| Hot/cold compilation boundary | CMake `hot_path_objects` OBJECT lib compiled `-fno-exceptions`; cold path uses exceptions normally |
| Branchless arithmetic | `signal_eval.hpp` — `std::bit_cast<uint32_t>` sign-bit XOR; `virtual_ledger.hpp` — 4-way IEEE 754 min |
| Per-stage latency instrumentation | `live.cpp` — 7-bucket log-scale histograms, p50/p95/p99, two independent accumulators |
| Realistic simulation model | `sim_handler.hpp` — 70% fill prob, 2 bps latency penalty, 5% leg failure, 1% TDS, 18% GST |
| Memory-mapped file I/O | `cold_path/file_source.cpp` — `mmap` + `MADV_SEQUENTIAL`, zero-copy sequential scan |
| Ed25519-signed REST authentication | `cold_path/order_sender.cpp` — OpenSSL EVP, epoch-ms nonce, CoinSwitch Pro API |
| Adaptive profit threshold | `signal_eval.hpp` — `max(MIN_PROFIT_USD, mid × MIN_PROFIT_BPS)`, no runtime branch |
| C++20 concepts | `engine.hpp` — `OrderHandler` concept enforces `noexcept submit()` at compile time |

---

## Build

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release \
         -DOPENSSL_ROOT_DIR=/opt/homebrew/opt/openssl@3 \
         -DCURL_ROOT=/opt/homebrew/opt/curl
cmake --build . -j8
./backtest ../config/default.json
```

All dependencies (`spdlog`, `nlohmann/json`, `IXWebSocket`) are fetched via CMake `FetchContent` —
no `brew install` required beyond OpenSSL and curl.

---
