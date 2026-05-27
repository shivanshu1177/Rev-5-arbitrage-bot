#!/usr/bin/env python3
"""
Generate mock_ticks.bin for backtester replay.

Binary format (64 bytes per tick, little-endian):
  offset  0: uint64  timestamp_ns
  offset  8: uint8   venue (1=C2C1, 2=C2C2)
  offset  9: uint8   _pad
  offset 10: uint16  pair_id
  offset 12: float   best_bid
  offset 16: float   best_ask
  offset 20: float   bid_qty
  offset 24: float   ask_qty
  offset 28: 36 zero bytes

Price model
-----------
Each pair maintains a single "true mid" that GBM-walks independently.
C2C1 and C2C2 quotes are both derived from that mid with small half-spread noise.
In normal ticks there is no exploitable spread.

Arb events:
  During an arb window (ARB_ROUNDS rounds) the emitted C2C2 bid is bumped
  above C2C1 ask by (threshold * ARB_MARGIN).  The underlying GBM mid is NOT
  changed, so prices return to normal as soon as the window ends.
  ARB_PROB is the per-round probability of starting a new window on any
  arb-capable pair.

Signal fires when (evaluate_signal in engine.hpp):
  bid_other - ask_here > FEE_RATE * (ask_here + bid_other) + MIN_PROFIT_USD
"""

import struct, random, math, sys, os, time

OUT_PATH   = sys.argv[1] if len(sys.argv) > 1 else "data/mock_ticks.bin"
N_ROUNDS   = int(sys.argv[2]) if len(sys.argv) > 2 else 125_000
SEED       = 42

FEE_RATE   = 0.0004
MIN_PROFIT = 0.50       # USD per coin (signal threshold in engine.hpp)
HALF_SPREAD = 0.00025   # 2.5 bps half-spread on each venue
VOL_STEP   = 0.0001     # 1 bp GBM vol per round step

ARB_PROB   = 0.0008     # per-round probability of starting an arb window (~0.1%)
ARB_ROUNDS = 5          # rounds the window stays open
ARB_MARGIN = 1.6        # injected spread = threshold * ARB_MARGIN

PAIRS = [
    (0,  45_000.0, 0.5),     # BTC/USDT
    (1,   3_000.0, 5.0),     # ETH/USDT
    (2,     150.0, 40.0),    # SOL/USDT
    (3,     400.0, 12.0),    # BNB/USDT
    (4,       0.55, 5000.0), # XRP/USDT
    (5,       0.45, 6000.0), # ADA/USDT
    (6,       0.12, 20000.0),# DOGE/USDT
    (7,       0.85, 3000.0), # MATIC/USDT
    (8,       7.5,  400.0),  # DOT/USDT
    (9,     100.0,  20.0),   # LTC/USDT
    (10,     38.0,  80.0),   # AVAX/USDT
    (11,     14.5, 200.0),   # LINK/USDT
    (12,      7.8, 350.0),   # UNI/USDT
    (13,     10.5, 280.0),   # ATOM/USDT
    (14,     28.0, 100.0),   # ETC/USDT
    (15,      0.11, 25000.0),# XLM/USDT
    (16,      0.18, 15000.0),# ALGO/USDT
    (17,     12.0, 250.0),   # ICP/USDT
    (18,      6.0, 500.0),   # FIL/USDT
    (19,      0.40, 7000.0), # SAND/USDT
]

def pack_tick(ts_ns, venue, pair_id, bid, ask, bid_qty, ask_qty):
    return struct.pack('<Q B B H f f f f 36x',
                       ts_ns, venue, 0, pair_id,
                       bid, ask, bid_qty, ask_qty)

def min_bid_for_signal(ask_here):
    """Minimum bid_other to fire a signal given ask_here."""
    return (ask_here * (1 + FEE_RATE) + MIN_PROFIT) / (1 - FEE_RATE)

def can_arb(price):
    """True if arb window can produce a signal without extreme (>50 bps) price deviation."""
    needed = min_bid_for_signal(price) - price  # required spread
    return needed < price * 0.005               # cap at 50 bps

def main():
    random.seed(SEED)
    os.makedirs(os.path.dirname(OUT_PATH) or ".", exist_ok=True)

    n_pairs = len(PAIRS)

    # GBM mid per pair (both venues share the same mid + tiny noise)
    mids = [p[1] for p in PAIRS]

    # Arb state per pair: [rounds_remaining, direction (+1/-1)]
    arb = [[0, 1] for _ in range(n_pairs)]

    arb_capable = [i for i, p in enumerate(PAIRS) if can_arb(p[1])]

    ts_ns   = 1_700_000_000_000_000_000
    tick_ns = 500_000  # 500 µs per tick within each round

    total_arb_windows = 0

    t0 = time.perf_counter()
    with open(OUT_PATH, "wb") as f:
        buf = bytearray()

        for _ in range(N_ROUNDS):
            # Maybe open a new arb window on a capable pair this round.
            if arb_capable and random.random() < ARB_PROB:
                p = random.choice(arb_capable)
                if arb[p][0] == 0:              # don't restart an active window
                    arb[p] = [ARB_ROUNDS, random.choice([1, -1])]
                    total_arb_windows += 1

            for p_idx in range(n_pairs):
                _, base_price, depth = PAIRS[p_idx]

                # GBM step: one mid shared by both venues (tightly correlated)
                mids[p_idx] *= math.exp(random.gauss(0, VOL_STEP))
                # Clamp to ±90% of base price
                lo, hi = base_price * 0.1, base_price * 10.0
                mid = max(lo, min(hi, mids[p_idx]))
                mids[p_idx] = mid

                # Normal bid/ask for each venue
                hs   = HALF_SPREAD * mid
                bid1 = mid - hs + random.gauss(0, hs * 0.1)
                ask1 = mid + hs + random.gauss(0, hs * 0.1)
                bid2 = mid - hs + random.gauss(0, hs * 0.1)
                ask2 = mid + hs + random.gauss(0, hs * 0.1)

                # Override prices during arb window (GBM mid unchanged).
                rem, direction = arb[p_idx]
                if rem > 0:
                    needed = min_bid_for_signal(ask1) - ask1
                    bump   = needed * ARB_MARGIN
                    if direction == 1:
                        # C2C2.bid > C2C1.ask  → signal: buy C2C1, sell C2C2
                        bid2 = ask1 + bump
                        ask2 = bid2 + 2 * hs   # keep ask > bid
                    else:
                        # C2C1.bid > C2C2.ask  → signal: buy C2C2, sell C2C1
                        bid1 = ask2 + bump
                        ask1 = bid1 + 2 * hs
                    arb[p_idx][0] -= 1

                dq1 = depth * random.uniform(0.5, 2.0)
                dq2 = depth * random.uniform(0.5, 2.0)

                # Emit C2C1 first (seeds state for C2C2 evaluation on same round)
                buf += pack_tick(ts_ns, 1, PAIRS[p_idx][0], bid1, ask1, dq1, dq1)
                ts_ns += tick_ns
                buf += pack_tick(ts_ns, 2, PAIRS[p_idx][0], bid2, ask2, dq2, dq2)
                ts_ns += tick_ns

            if len(buf) >= 8 * 1024 * 1024:
                f.write(buf)
                buf = bytearray()

        if buf:
            f.write(buf)

    n_ticks = N_ROUNDS * n_pairs * 2
    elapsed = time.perf_counter() - t0
    size_mb = n_ticks * 64 / 1024 / 1024
    print(f"Wrote {n_ticks:,} ticks → {OUT_PATH}  ({size_mb:.1f} MB, {elapsed:.1f}s)")
    print(f"Arb-capable pairs: {arb_capable}  |  arb windows injected: {total_arb_windows}")
    for i in arb_capable:
        pid, price, _ = PAIRS[i]
        spread = min_bid_for_signal(price) - price
        print(f"  pair_id={pid}  price=${price:>10,.2f}  "
              f"min spread=${spread:.2f} ({spread/price*10000:.1f} bps)")

if __name__ == "__main__":
    main()
