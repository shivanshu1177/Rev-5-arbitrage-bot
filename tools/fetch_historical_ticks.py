#!/usr/bin/env python3
"""
Fetch CoinSwitch historical OHLCV candles and convert to MarketTick binary for backtesting.

Queries GET /trade/api/v2/candles?exchange=<venue>&symbol=<sym>&interval=60 for both
c2c1 and c2c2, then synthesises bid/ask ticks from the hourly close prices.

For each aligned hour T, the file contains:
  - MarketTick(venue=1, pair_id, bid/ask from c2c1 close, ts=T)
  - MarketTick(venue=2, pair_id, bid/ask from c2c2 close, ts=T+1ms)
The engine sees both venues fresh after the second tick and evaluates the spread.

Limitations:
  1. No intra-hour signals — closes only. Real opportunity is underestimated.
  2. Synthetic spread: bid = close*(1-hbps), ask = close*(1+hbps). Default 5 bps.
  3. Exchange-TS skew gate always passes (both venues at T have skew ~= 0 ms).
  4. CoinSwitch may not serve data all the way back to the requested start date;
     the script prints a warning and writes whatever it receives.

Usage:
  python3 tools/fetch_historical_ticks.py \\
    --config config/default.json \\
    --start  2024-01-01 \\
    --end    2026-04-30 \\
    --out    build/data/historical_hourly.bin

Requirements:
  pip install cryptography requests

Binary format: 64 bytes per tick (matches MarketTick in src/common/types.hpp):
  offset  0: uint64  timestamp_ns
  offset  8: uint8   venue (1=C2C1, 2=C2C2)
  offset  9: uint8   _pad (0)
  offset 10: uint16  pair_id
  offset 12: float   best_bid
  offset 16: float   best_ask
  offset 20: float   bid_qty
  offset 24: float   ask_qty
  offset 28: uint32  exchange_ts_ms  (candle close_time truncated to uint32)
  offset 32: 32 zero bytes
"""

import argparse
import json
import os
import struct
import sys
import time
from datetime import datetime, timezone
from urllib.parse import quote

try:
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
except ImportError:
    print("ERROR: 'cryptography' package not installed.")
    print("  Fix: pip install cryptography requests")
    sys.exit(1)

try:
    import requests as _requests
except ImportError:
    print("ERROR: 'requests' package not installed.")
    print("  Fix: pip install cryptography requests")
    sys.exit(1)

# ── Constants ─────────────────────────────────────────────────────────────────

TICK_FMT  = '<Q B B H f f f f I 32x'
TICK_SIZE = struct.calcsize(TICK_FMT)
assert TICK_SIZE == 64, f"MarketTick must be 64 bytes, got {TICK_SIZE}"

BASE_URL      = "https://coinswitch.co"
CANDLES_PATH  = "/trade/api/v2/candles"
KEYS_PATH     = "/trade/api/v2/validate/keys"

DEFAULT_CONFIG    = "config/default.json"
DEFAULT_OUT       = "build/data/historical_1min.bin"
DEFAULT_INTERVAL  = 1      # candle interval in minutes
DEFAULT_SPREAD    = 5.0    # half-spread in basis points
CANDLES_PER_REQ   = 500    # max candles to request per API call
SLEEP_BETWEEN_REQ = 0.5    # seconds between API calls (rate-limit headroom)

HOUR_MS = 3_600_000        # milliseconds in one hour

# ── Auth ──────────────────────────────────────────────────────────────────────

def sign_request(private_key_hex: str, method: str, path: str, epoch_ms: int) -> str:
    raw_key  = bytes.fromhex(private_key_hex)
    priv_key = Ed25519PrivateKey.from_private_bytes(raw_key)
    message  = (method + path + str(epoch_ms)).encode('ascii')
    return priv_key.sign(message).hex()

def auth_headers(api_key: str, private_key_hex: str,
                 method: str, signing_path: str) -> dict:
    epoch_ms  = int(time.time() * 1000)
    signature = sign_request(private_key_hex, method, signing_path, epoch_ms)
    return {
        "X-AUTH-APIKEY":    api_key,
        "X-AUTH-SIGNATURE": signature,
        "X-AUTH-EPOCH":     str(epoch_ms),
        "User-Agent":       "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
                            "AppleWebKit/537.36 (KHTML, like Gecko) "
                            "Chrome/124.0.0.0 Safari/537.36",
        "Accept":           "application/json",
    }

# ── Config ────────────────────────────────────────────────────────────────────

def load_config(path: str) -> dict:
    with open(path) as f:
        return json.load(f)

# ── API fetch ─────────────────────────────────────────────────────────────────

def fetch_candles(symbol: str, exchange: str, interval: int,
                  start_ms: int, end_ms: int,
                  api_key: str, private_key_hex: str) -> list | None:
    """
    Fetch up to CANDLES_PER_REQ candles for one symbol+venue+window.
    Returns list of candle dicts or None on error.

    Signing path includes query params (URL-decoded symbol), matching the
    CoinSwitch convention used in record_ticks.py for the depth endpoint.
    """
    params = (f"exchange={exchange}&symbol={symbol}"
              f"&interval={interval}&start_time={start_ms}&end_time={end_ms}")
    signing_path = f"{CANDLES_PATH}?{params}"

    encoded_sym = quote(symbol, safe='')
    url = (f"{BASE_URL}{CANDLES_PATH}"
           f"?exchange={exchange}&symbol={encoded_sym}"
           f"&interval={interval}&start_time={start_ms}&end_time={end_ms}")

    hdrs = auth_headers(api_key, private_key_hex, "GET", signing_path)
    try:
        r = _requests.get(url, headers=hdrs, timeout=(10, 30))
        if r.status_code != 200:
            print(f"  HTTP {r.status_code} for {symbol}@{exchange} "
                  f"[{start_ms}..{end_ms}]: {r.text[:120]}", flush=True)
            return None
        body = r.json()
        return body.get("data", [])
    except _requests.exceptions.RequestException as e:
        print(f"  Network error for {symbol}@{exchange}: {e}", flush=True)
        return None

# ── Tick packing ──────────────────────────────────────────────────────────────

def pack_tick(ts_ns: int, venue: int, pair_id: int,
              bid: float, ask: float,
              bid_qty: float, ask_qty: float,
              exchange_ts_ms: int) -> bytes:
    return struct.pack(TICK_FMT,
                       ts_ns, venue, 0, pair_id,
                       bid, ask, bid_qty, ask_qty,
                       exchange_ts_ms & 0xFFFFFFFF)

# ── Candle fetch with pagination ──────────────────────────────────────────────

def fetch_all_candles(symbol: str, exchange: str, venue_id: int,
                      pair_id: int, interval_min: int,
                      start_ms: int, end_ms: int,
                      api_key: str, private_key_hex: str) -> dict[int, tuple[float, float]]:
    """
    Fetch ALL candles for symbol+venue over [start_ms, end_ms], paginating in
    windows of CANDLES_PER_REQ candles.  Returns:
      { aligned_hour_ms: (close_price, volume), ... }
    Timestamps are rounded to the nearest hour boundary for cross-venue alignment.
    """
    window_ms    = CANDLES_PER_REQ * interval_min * 60 * 1000
    buckets: dict[int, tuple[float, float]] = {}
    cursor       = start_ms
    total_fetched = 0

    while cursor < end_ms:
        chunk_end = min(cursor + window_ms, end_ms)
        candles   = None
        for attempt in range(3):
            candles = fetch_candles(symbol, exchange, interval_min,
                                    cursor, chunk_end,
                                    api_key, private_key_hex)
            if candles is not None:
                break
            if attempt < 2:
                time.sleep(5 * (attempt + 1))   # 5s then 10s backoff before retry
        time.sleep(SLEEP_BETWEEN_REQ)

        if candles is None:
            cursor = chunk_end
            continue

        for c in candles:
            try:
                close     = float(c["c"])
                volume    = float(c.get("volume", 0) or 0)
                close_t   = int(c.get("close_time", c.get("end_time", 0)))
                if close <= 0.0 or close_t == 0:
                    continue
                # Align to nearest candle boundary
                candle_ms = interval_min * 60_000
                aligned   = (close_t // candle_ms) * candle_ms
                buckets[aligned] = (close, max(volume, 0.01))
                total_fetched += 1
            except (KeyError, ValueError, TypeError):
                continue

        cursor = chunk_end

    return buckets

# ── Main ──────────────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser(
        description="Fetch CoinSwitch historical OHLCV and write MarketTick binary")
    ap.add_argument("--config",     default=DEFAULT_CONFIG,
                    help="Path to config JSON (default: config/default.json)")
    ap.add_argument("--start",      required=True,
                    help="Start date YYYY-MM-DD (inclusive, UTC)")
    ap.add_argument("--end",        required=True,
                    help="End date YYYY-MM-DD (inclusive, UTC)")
    ap.add_argument("--interval",   type=int, default=DEFAULT_INTERVAL,
                    help="Candle interval in minutes (default: 1)")
    ap.add_argument("--spread-bps", type=float, default=DEFAULT_SPREAD,
                    dest="spread_bps",
                    help="Synthetic half-spread in bps around close (default: 5)")
    ap.add_argument("--out",        default=DEFAULT_OUT,
                    help="Output binary file path")
    args = ap.parse_args()

    # Parse dates
    try:
        start_dt = datetime.strptime(args.start, "%Y-%m-%d").replace(tzinfo=timezone.utc)
        end_dt   = datetime.strptime(args.end,   "%Y-%m-%d").replace(tzinfo=timezone.utc)
    except ValueError as e:
        sys.exit(f"Date parse error: {e} — use YYYY-MM-DD")

    start_ms = int(start_dt.timestamp() * 1000)
    end_ms   = int(end_dt.timestamp()   * 1000) + HOUR_MS * 24  # include end day

    if start_ms >= end_ms:
        sys.exit("ERROR: --start must be before --end")

    # Load config
    try:
        cfg = load_config(args.config)
    except FileNotFoundError:
        sys.exit(f"Config not found: {args.config}")

    api_key         = cfg.get("api_key", "")
    private_key_hex = cfg.get("private_key_hex", "")

    if not api_key or api_key.startswith("YOUR_"):
        sys.exit("ERROR: api_key not configured in config JSON.")
    if not private_key_hex or private_key_hex.startswith("YOUR_"):
        sys.exit("ERROR: private_key_hex not configured.")
    if len(private_key_hex) != 64:
        sys.exit(f"ERROR: private_key_hex must be 64 hex chars, got {len(private_key_hex)}")

    symbols = [(s["pair_id"], s["name"]) for s in cfg.get("symbols", [])]
    if not symbols:
        sys.exit("ERROR: No symbols in config.")

    half_bps = args.spread_bps / 10000.0
    venues   = [(1, "c2c1"), (2, "c2c2")]

    print(f"Fetching {args.interval}-min candles: {args.start} → {args.end}")
    print(f"Symbols : {[s for _, s in symbols]}")
    print(f"Spread  : ±{args.spread_bps} bps synthetic half-spread around close")
    print(f"Output  : {args.out}")
    print(flush=True)

    # Validate auth
    print("Verifying auth with CoinSwitch API... ", end="", flush=True)
    try:
        vr = _requests.get(
            BASE_URL + KEYS_PATH,
            headers=auth_headers(api_key, private_key_hex, "GET", KEYS_PATH),
            timeout=(5, 15),
        )
        if vr.status_code == 200 and "Valid" in vr.json().get("message", ""):
            print("OK")
        elif vr.status_code == 401:
            sys.exit(f"\nFailed: HTTP 401 — check api_key / private_key_hex\n{vr.text[:200]}")
        else:
            sys.exit(f"\nFailed: HTTP {vr.status_code}\n{vr.text[:200]}")
    except _requests.exceptions.RequestException as e:
        sys.exit(f"\nNetwork error: {e}")

    # Fetch candles for all symbol × venue combinations
    # Structure: data[pair_id][venue_id] = { hour_ms: (close, volume) }
    data: dict[int, dict[int, dict[int, tuple[float, float]]]] = {}

    for pair_id, symbol in symbols:
        data[pair_id] = {}
        for venue_id, exchange in venues:
            print(f"  Fetching {symbol} @ {exchange} ...", end=" ", flush=True)
            t0 = time.monotonic()
            buckets = fetch_all_candles(symbol, exchange, venue_id,
                                        pair_id, args.interval,
                                        start_ms, end_ms,
                                        api_key, private_key_hex)
            elapsed = time.monotonic() - t0
            print(f"{len(buckets):,} candles in {elapsed:.1f}s", flush=True)
            data[pair_id][venue_id] = buckets

    print(flush=True)

    # Build sorted tick list: for each hour where BOTH venues have data, emit two ticks
    all_ticks: list[tuple[int, int, int, float, float, float, float, int]] = []
    # tuple: (ts_ns, venue, pair_id, bid, ask, bid_qty, ask_qty, exchange_ts_ms)

    for pair_id, symbol in symbols:
        d1 = data[pair_id].get(1, {})
        d2 = data[pair_id].get(2, {})
        common_candles = sorted(set(d1.keys()) & set(d2.keys()))

        if not common_candles:
            print(f"  WARNING: no overlapping candles for {symbol} between c2c1 and c2c2")
            continue

        for candle_ms in common_candles:
            close1, vol1 = d1[candle_ms]
            close2, vol2 = d2[candle_ms]

            bid1 = close1 * (1.0 - half_bps)
            ask1 = close1 * (1.0 + half_bps)
            bid2 = close2 * (1.0 - half_bps)
            ask2 = close2 * (1.0 + half_bps)

            # c2c2 arrives first (at T), c2c1 arrives 1ms later (at T+1ms).
            # When the c2c1 tick is processed by the engine, c2c2 state is 1ms fresh
            # → staleness gate passes → signal evaluates both arb directions.
            ts_c2c2_ns = candle_ms * 1_000_000          # ms → ns (arrives first)
            ts_c2c1_ns = (candle_ms + 1) * 1_000_000    # 1 ms later (triggering tick)

            # exchange_ts_ms: truncate to uint32 (same as live path)
            exts = candle_ms & 0xFFFFFFFF

            all_ticks.append((ts_c2c2_ns, 2, pair_id, bid2, ask2, vol2, vol2, exts))
            all_ticks.append((ts_c2c1_ns, 1, pair_id, bid1, ask1, vol1, vol1, exts))

    if not all_ticks:
        sys.exit("ERROR: No ticks generated. Check that c2c1 and c2c2 have overlapping data.")

    # Sort by timestamp (primary), then venue (secondary) — already ordered above per symbol,
    # but multiple symbols interleave in time so a global sort is needed.
    all_ticks.sort(key=lambda t: (t[0], t[1]))

    # Write binary file
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "wb") as f:
        for (ts_ns, venue, pair_id, bid, ask, bq, aq, exts) in all_ticks:
            f.write(pack_tick(ts_ns, venue, pair_id, bid, ask, bq, aq, exts))

    # Summary
    n_ticks  = len(all_ticks)
    size_mb  = n_ticks * 64 / 1024 / 1024
    file_ok  = os.path.getsize(args.out) % 64 == 0

    print(f"Written : {n_ticks:,} ticks  ({size_mb:.2f} MB)  {'valid' if file_ok else 'INVALID'}")
    print(f"Output  : {args.out}")
    print()
    print("Per-symbol coverage:")
    for pair_id, symbol in symbols:
        d1 = data[pair_id].get(1, {})
        d2 = data[pair_id].get(2, {})
        common = set(d1.keys()) & set(d2.keys())
        if common:
            t_min = datetime.fromtimestamp(min(common)/1000, tz=timezone.utc).strftime("%Y-%m-%d")
            t_max = datetime.fromtimestamp(max(common)/1000, tz=timezone.utc).strftime("%Y-%m-%d")
            print(f"  {symbol:12s}  {len(common):7,} candles  {t_min} → {t_max}")
        else:
            print(f"  {symbol:12s}  no data")

    print()
    print("Next step: run the backtester:")
    print(f'  Set "data_file": "{args.out}" in your config, then:')
    print(f"  ./build/backtester --config config/default.json")

if __name__ == "__main__":
    main()
