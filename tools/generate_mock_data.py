#!/usr/bin/env python3
"""
Generate synthetic MarketTick binary data for backtesting.

Binary layout per tick — must exactly match the C++ MarketTick struct (64 bytes):
  offset  0 : uint64_t timestamp_ns      (Q)
  offset  8 : uint8_t  venue             (B)
  offset  9 : uint8_t  _implicit_pad=0   (B, always 0)
  offset 10 : uint16_t pair_id           (H)
  offset 12 : float32  best_bid          (f)
  offset 16 : float32  best_ask          (f)
  offset 20 : float32  bid_qty           (f)
  offset 24 : float32  ask_qty           (f)
  offset 28 : uint8_t  _pad[36]          (36x)

Python struct format: '<BBH4f36x' preceded by 'Q' = '<QBBHffff36x'
Verify: 8+1+1+2+4+4+4+4+36 = 64 ✓
"""

import struct
import random
import argparse
import os
import math

# Must be exactly 64 bytes — matches C++ static_assert.
TICK_FMT  = '<QBBHffff36x'
TICK_SIZE = struct.calcsize(TICK_FMT)
assert TICK_SIZE == 64, f"Expected 64 bytes, got {TICK_SIZE}"

# Realistic reference mid-prices in USD for 20 pairs (pair_id 0..19).
# These are the starting prices for the random walk.
BASE_PRICES = [
    35_000.0,   # 0  BTC/USDT
     2_000.0,   # 1  ETH/USDT
        25.0,   # 2  SOL/USDT
       250.0,   # 3  BNB/USDT
         0.55,  # 4  XRP/USDT
         0.35,  # 5  ADA/USDT
         0.08,  # 6  DOGE/USDT
         0.90,  # 7  MATIC/USDT
         6.50,  # 8  DOT/USDT
        80.0,   # 9  LTC/USDT
        35.0,   # 10 AVAX/USDT
         7.50,  # 11 LINK/USDT
         6.00,  # 12 UNI/USDT
        12.0,   # 13 ATOM/USDT
        18.0,   # 14 ETC/USDT
         0.12,  # 15 XLM/USDT
         0.18,  # 16 ALGO/USDT
         5.50,  # 17 ICP/USDT
         4.80,  # 18 FIL/USDT
         0.45,  # 19 SAND/USDT
]

NUM_PAIRS = len(BASE_PRICES)


def generate(output_path: str, count: int, seed: int = 42,
             arb_inject_rate: float = 0.001):
    """
    Generate `count` MarketTick records with realistic price dynamics.

    arb_inject_rate: fraction of ticks where we deliberately create a small
    price discrepancy between the two venues for the same pair, to ensure the
    backtest fires at least some trades.
    """
    rng = random.Random(seed)
    os.makedirs(os.path.dirname(os.path.abspath(output_path)), exist_ok=True)

    # Price state per [pair][venue]: independent random walk per venue.
    # venue 0 = C2C1 (index 0), venue 1 = C2C2 (index 1).
    prices = [[BASE_PRICES[p], BASE_PRICES[p] * (1.0 + rng.gauss(0, 0.001))]
              for p in range(NUM_PAIRS)]

    ts_ns = 1_700_000_000_000_000_000  # ~November 2023 epoch in nanoseconds

    CHUNK = 10_000
    buf   = bytearray(TICK_SIZE * CHUNK)
    pos   = 0

    with open(output_path, 'wb') as f:
        for i in range(count):
            pair_id = rng.randrange(NUM_PAIRS)
            # venue: 1=C2C1, 2=C2C2
            venue   = rng.randint(1, 2)
            vi      = venue - 1

            # Random walk with mean reversion for this pair+venue.
            base   = BASE_PRICES[pair_id]
            drift  = rng.gauss(0, prices[pair_id][vi] * 0.0002)
            revert = (base - prices[pair_id][vi]) * 0.0005
            prices[pair_id][vi] = max(prices[pair_id][vi] + drift + revert, base * 0.1)

            mid = prices[pair_id][vi]

            # Occasionally inject a deliberate cross-venue price discrepancy
            # so the backtest actually fires some arb trades.
            if rng.random() < arb_inject_rate:
                other = 1 - vi
                # Push the other venue's price in the opposite direction by 0.3%.
                prices[pair_id][other] += mid * rng.choice([-0.003, 0.003])

            # Spread: 1–8 bps (typical for liquid crypto markets).
            spread_bps = rng.uniform(1.0, 8.0) / 10_000.0
            half_spread = mid * spread_bps / 2.0
            best_bid = max(mid - half_spread, 1e-8)
            best_ask = mid + half_spread

            bid_qty = rng.uniform(0.01, 5.0)
            ask_qty = rng.uniform(0.01, 5.0)

            # Advance timestamp by 0.1–2 ms.
            ts_ns += rng.randint(100_000, 2_000_000)

            struct.pack_into(TICK_FMT, buf, pos,
                             ts_ns, venue, 0, pair_id,
                             best_bid, best_ask, bid_qty, ask_qty)
            pos += TICK_SIZE

            if pos >= len(buf):
                f.write(buf)
                pos = 0

        if pos > 0:
            f.write(buf[:pos])

    size_mb = os.path.getsize(output_path) / 1024 / 1024
    print(f"Generated {count:,} ticks → {output_path} ({size_mb:.1f} MB)")
    print(f"Expected size: {count * 64 / 1024 / 1024:.1f} MB "
          f"({'OK' if abs(size_mb - count * 64 / 1024 / 1024) < 0.01 else 'MISMATCH'})")


def main():
    ap = argparse.ArgumentParser(description="Generate synthetic MarketTick binary data")
    ap.add_argument('--output',   default='data/mock_ticks.bin',
                    help='Output file path (default: data/mock_ticks.bin)')
    ap.add_argument('--count',    type=int, default=1_000_000,
                    help='Number of ticks to generate (default: 1,000,000)')
    ap.add_argument('--seed',     type=int, default=42,
                    help='Random seed for reproducibility (default: 42)')
    ap.add_argument('--arb-rate', type=float, default=0.001,
                    help='Fraction of ticks with injected arb opportunity (default: 0.001)')
    args = ap.parse_args()

    generate(args.output, args.count, args.seed, args.arb_rate)


if __name__ == '__main__':
    main()
