# Cross-Venue Arbitrage Engine

C++20 statistical arbitrage engine detecting price discrepancies between two CoinSwitch Pro liquidity pools (C2C1/C2C2) via a lock-free SPSC ring buffer and a branchless 15 ns execution core. Built to demonstrate the software architecture patterns used in institutional HFT systems.

---

## Key Numbers

| Metric | Value | Conditions |
|---|---|---|
| Language | C++20, `-O3 -mcpu=apple-m2 -flto=thin` | Apple Clang, CMake |
| Backtest throughput (synthetic) | **7.67M ticks/sec** | 1M synthetic ticks, M2, single core |
| Backtest throughput (real data) | **5.23M ticks/sec** | 1.3M real CoinSwitch ticks, M2 |
| `process_tick` average | **451 ns** | 10-minute live WebSocket soak |
| `process_tick` max spike | 16 ms | OS interrupt / L3 eviction on macOS |
| WS→engine avg (2-min warm) | 13.9 µs | macOS, two unscheduled threads |
| WS→engine avg (10-min soak) | **801 µs** | macOS time-sharing degradation |
| WS→engine max | 260 ms | macOS scheduler preemption |
| WS→engine target (Linux) | < 5 µs | `isolcpus` + `SCHED_FIFO` + `mlockall` |

The soak averages are the realistic numbers on a consumer macOS machine.  
The 57× degradation between a warm run and a 10‑minute soak comes from the OS scheduler
(macOS has no real‑time priority for user processes).  
On Linux with CPU isolation the median stays sub‑microsecond, as the target row shows.

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
              └── LiveOrderHandler      → OrderSender    (Ed25519-signed REST, libcurl, order_id logged)
```

---

## Real-Data Backtest Finding

A 15‑minute live recording from CoinSwitch (5,268 ticks) across BTC/ETH/SOL on C2C1 and C2C2
produced **zero arbitrage signals** under any realistic fee assumption.

The two pools behave as if they share the same matching engine — their prices stay within
< 0.1 % of each other. The break‑even spread (fees + latency + leg‑failure) is **~17 bps**;
the observed spread was **0 bps**.

Synthetic scenarios show the engine and signal logic are correct:
- At 20 bps injected spread: **96.8 % win rate**, +$1,291 PnL on $500K capital (no TDS)
- At 25 bps injected spread: **96.8 % win rate**, +$2,723 PnL

The strategy is mathematically sound — the C2C1/C2C2 pair simply does not provide the
required divergence.

**With India’s 1 % TDS on sell value**: structurally unprofitable at any observable spread.
TDS at 10 % position size costs ~10 bps per trade — 5–10× the captured edge.

---

## What This Demonstrates

| Skill | Artifact |
|---|---|
| Lock‑free concurrent data structures | `src/hot_path/ring_buffer.hpp` — SPSC acquire/release, cache‑line padding |
| Hot/cold compilation boundary | CMake `hot_path_objects` OBJECT lib compiled `-fno-exceptions` |
| Branchless arithmetic | `signal_eval.hpp` — `std::bit_cast<uint32_t>` sign‑bit XOR; `virtual_ledger.hpp` — 4‑way IEEE 754 min |
| Per‑stage latency instrumentation | `live.cpp` — 7‑bucket log‑scale histograms, p50/p95/p99, two accumulators |
| Realistic simulation model | `sim_handler.hpp` — 70 % fill prob, 2 bps latency penalty, 5 % leg failure, 1 % TDS, 18 % GST |
| Memory‑mapped file I/O | `cold_path/file_source.cpp` — `mmap` + `MADV_SEQUENTIAL`, zero‑copy scan |
| Ed25519‑signed REST authentication | `cold_path/order_sender.cpp` — OpenSSL EVP, epoch‑ms nonce, CoinSwitch Pro API |
| WebSocket feed integration | `src/live.cpp` — IXWebSocket v11.4.4, Socket.IO v4, auto‑reconnect |
| Adaptive profit threshold | `signal_eval.hpp` — `max(MIN_PROFIT_USD, mid × MIN_PROFIT_BPS)`, no branch |
| C++20 concepts | `engine.hpp` — `OrderHandler` concept enforces `noexcept submit()` at compile time |

---

## Current Status

| Component | Status |
|---|---|
| Backtest engine | ✅ Complete |
| Paper trader (REST polling, no orders) | ✅ Complete |
| Parallel REST polling (`curl_multi`) | ✅ Complete |
| Ed25519 authentication | ✅ Verified against live API |
| IXWebSocket integration | ✅ Complete |
| WS subscription format verified on live endpoint | ✅ Complete |
| Order confirmation (`order_id` logged) | ✅ Complete |
| `LiveOrderHandler::submit()` — sends real orders | ⚠️ Implemented, untested (no fill‑confirmation feedback loop) |
| Parallel leg execution (buy + sell simultaneously) | ❌ P0 blocker (sequential `send_order` calls) |

---

## Build

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release \
         -DOPENSSL_ROOT_DIR=/opt/homebrew/opt/openssl@3 \
         -DCURL_ROOT=/opt/homebrew/opt/curl
cmake --build . -j8
./backtest ../config/default.jsonAll dependencies (spdlog, nlohmann/json, IXWebSocket) are fetched via CMake FetchContent
—no brew install required beyond OpenSSL and curl.

