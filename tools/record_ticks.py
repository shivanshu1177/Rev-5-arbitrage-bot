#!/usr/bin/env python3
"""
Record live CoinSwitch order-book snapshots to a MarketTick binary file.

The output file can be replayed directly by the backtester:
  # In config/default.json set: "data_file": "data/real_ticks.bin"
  ./backtest config/default.json

Usage:
  python3 tools/record_ticks.py                          # run until Ctrl+C
  python3 tools/record_ticks.py --hours 8                # overnight run
  python3 tools/record_ticks.py --hours 168              # 1-week run
  python3 tools/record_ticks.py --interval 1.0           # 1 s between polls (gentler)
  python3 tools/record_ticks.py --out data/real_ticks.bin --config config/default.json

Requirements:
  pip install cryptography requests   (Ed25519 signing + HTTP client)

Binary format: 64 bytes per tick (matches MarketTick in src/common/types.hpp):
  offset  0: uint64  timestamp_ns
  offset  8: uint8   venue (1=C2C1, 2=C2C2)
  offset  9: uint8   _pad (0)
  offset 10: uint16  pair_id
  offset 12: float   best_bid
  offset 16: float   best_ask
  offset 20: float   bid_qty
  offset 24: float   ask_qty
  offset 28: 36 zero bytes
"""

import argparse
import json
import os
import signal
import struct
import sys
import time
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

TICK_FMT  = '<Q B B H f f f f I 32x'   # I = uint32 exchange_ts_ms at offset 28
TICK_SIZE = struct.calcsize(TICK_FMT)
assert TICK_SIZE == 64, f"MarketTick must be 64 bytes, got {TICK_SIZE}"

BASE_URL   = "https://coinswitch.co"
DEPTH_BASE = BASE_URL + "/trade/api/v2/depth"
DEPTH_PATH = "/trade/api/v2/depth"   # signing uses path only, no query string
KEYS_PATH  = "/trade/api/v2/validate/keys"

DEFAULT_CONFIG   = "config/default.json"
DEFAULT_OUT      = "data/real_ticks.bin"
DEFAULT_INTERVAL = 0.5   # seconds between polls per symbol (round-robin)

# ── Auth ──────────────────────────────────────────────────────────────────────

def sign_request(private_key_hex: str, method: str, path: str, epoch_ms: int) -> str:
    """
    Ed25519-sign a CoinSwitch API request.
    signing_string = METHOD + path + epoch_ms_str  (no query string, no body)
    Returns 128-char lowercase hex signature.
    """
    raw_key  = bytes.fromhex(private_key_hex)
    priv_key = Ed25519PrivateKey.from_private_bytes(raw_key)
    message  = (method + path + str(epoch_ms)).encode('ascii')
    return priv_key.sign(message).hex()

def auth_headers(api_key: str, private_key_hex: str, method: str, path: str) -> dict:
    epoch_ms  = int(time.time() * 1000)
    signature = sign_request(private_key_hex, method, path, epoch_ms)
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

def build_symbol_table(cfg: dict) -> list[tuple[int, str]]:
    """Return [(pair_id, symbol_name), ...] from config."""
    return [(s["pair_id"], s["name"]) for s in cfg["symbols"]]

# ── JSON parsing (mirrors rest_poller.cpp logic) ──────────────────────────────

def parse_orderbook(body: dict) -> tuple[float, float, float, float] | None:
    """
    Extract (bid, ask, bid_qty, ask_qty) from the CoinSwitch depth response.
    Tries data.bids/asks first, then bids/asks at top level, then buy/sell.
    Returns None if parsing fails or prices are invalid.
    """
    data = body.get("data", body)

    def extract(d: dict, bid_key: str, ask_key: str):
        bids = d.get(bid_key, [])
        asks = d.get(ask_key, [])
        if not bids or not asks:
            return None
        try:
            bid     = float(bids[0][0]);  bid_qty = float(bids[0][1])
            ask     = float(asks[0][0]);  ask_qty = float(asks[0][1])
            return bid, ask, bid_qty, ask_qty
        except (IndexError, ValueError, TypeError):
            return None

    result = (extract(data, "bids", "asks")
              or extract(body, "bids", "asks")
              or extract(data, "buy", "sell")
              or extract(body, "buy", "sell"))

    if result is None:
        return None
    bid, ask, bid_qty, ask_qty = result
    if bid <= 0.0 or ask <= 0.0 or ask <= bid:
        return None
    try:
        exts = int(data.get("timestamp", 0) or 0)
    except (TypeError, ValueError):
        exts = 0
    return bid, ask, bid_qty, ask_qty, exts

# ── HTTP fetch ────────────────────────────────────────────────────────────────

def fetch_orderbook(symbol: str, exchange: str,
                    api_key: str, private_key_hex: str) -> dict | None:
    """Fetch depth for one symbol+venue. Returns parsed JSON dict or None."""
    encoded      = quote(symbol, safe='')   # "BTC/USDT" → "BTC%2FUSDT"
    url          = f"{DEPTH_BASE}?symbol={encoded}&exchange={exchange}"
    signing_path = f"{DEPTH_PATH}?symbol={symbol}&exchange={exchange}"  # URL-decoded
    hdrs         = auth_headers(api_key, private_key_hex, "GET", signing_path)
    try:
        r = _requests.get(url, headers=hdrs, timeout=(5, 15))
        if r.status_code != 200:
            return {"_http_error": r.status_code}
        return r.json()
    except _requests.exceptions.RequestException:
        return None

# ── Tick packing ──────────────────────────────────────────────────────────────

def pack_tick(ts_ns: int, venue: int, pair_id: int,
              bid: float, ask: float, bid_qty: float, ask_qty: float,
              exchange_ts_ms: int = 0) -> bytes:
    return struct.pack(TICK_FMT, ts_ns, venue, 0, pair_id,
                       bid, ask, bid_qty, ask_qty,
                       exchange_ts_ms & 0xFFFFFFFF)

# ── Main recorder loop ────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser(description="Record live CoinSwitch order-book data to binary")
    ap.add_argument("--config",   default=DEFAULT_CONFIG, help="Path to config/default.json")
    ap.add_argument("--out",      default=DEFAULT_OUT,    help="Output binary file path")
    ap.add_argument("--hours",    type=float, default=0,  help="Stop after N hours (0 = run forever)")
    ap.add_argument("--interval", type=float, default=DEFAULT_INTERVAL,
                    help="Seconds between poll rounds per symbol (default: 0.5)")
    args = ap.parse_args()

    # Load config
    try:
        cfg = load_config(args.config)
    except FileNotFoundError:
        sys.exit(f"Config not found: {args.config}")

    api_key         = cfg.get("api_key", "")
    private_key_hex = cfg.get("private_key_hex", "")

    if not api_key or api_key.startswith("YOUR_"):
        sys.exit("ERROR: api_key not configured in config JSON. "
                 "Set it to your real CoinSwitch API key.")
    if not private_key_hex or private_key_hex.startswith("YOUR_"):
        sys.exit("ERROR: private_key_hex not configured. "
                 "Set it to your 64-character hex Ed25519 private key.")
    if len(private_key_hex) != 64:
        sys.exit(f"ERROR: private_key_hex must be 64 hex chars, got {len(private_key_hex)}")

    symbols = build_symbol_table(cfg)
    if not symbols:
        sys.exit("ERROR: No symbols in config.")

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)

    deadline = time.monotonic() + args.hours * 3600 if args.hours > 0 else float("inf")

    # Signal handling: Ctrl+C prints summary and exits cleanly.
    stop = {"flag": False}
    def on_sigint(*_):
        print("\nStopping recorder (Ctrl+C)...", flush=True)
        stop["flag"] = True
    signal.signal(signal.SIGINT, on_sigint)

    ticks_written = 0
    polls_ok      = 0
    polls_err     = 0
    t_start       = time.monotonic()

    print(f"Recording to {args.out}")
    print(f"Symbols: {len(symbols)}  Venues: 2  Interval: {args.interval}s")
    if args.hours > 0:
        print(f"Duration: {args.hours:.1f} hours")
    else:
        print("Duration: until Ctrl+C")
    print(flush=True)

    # Smoke-test: validate API key via the dedicated /validate/keys endpoint.
    # Returns {"message": "Valid Access"} on success — no symbol parameter needed.
    print("Verifying auth with CoinSwitch API...", end=" ", flush=True)
    try:
        vr = _requests.get(
            BASE_URL + KEYS_PATH,
            headers=auth_headers(api_key, private_key_hex, "GET", KEYS_PATH),
            timeout=(5, 15),
        )
        if vr.status_code == 200 and vr.json().get("message") == "Valid Access":
            print("OK — Valid Access")
        elif vr.status_code == 401:
            sys.exit(f"\nFailed: HTTP 401 — key not registered on CoinSwitch portal, "
                     f"or signature wrong.\nResponse: {vr.text[:200]}")
        elif vr.status_code == 403:
            sys.exit(f"\nFailed: HTTP 403 — Cloudflare block (User-Agent issue) or IP ban.\n"
                     f"Response: {vr.text[:200]}")
        else:
            sys.exit(f"\nFailed: HTTP {vr.status_code}\n{vr.text[:200]}")
    except _requests.exceptions.RequestException as e:
        sys.exit(f"\nFailed: network error — {e}")

    with open(args.out, "ab") as f:
        sym_idx = 0
        while not stop["flag"] and time.monotonic() < deadline:
            pair_id, symbol = symbols[sym_idx % len(symbols)]
            sym_idx += 1

            for venue_id, exchange in [(1, "c2c1"), (2, "c2c2")]:
                if stop["flag"]:
                    break
                resp = fetch_orderbook(symbol, exchange, api_key, private_key_hex)
                if resp is None or "_http_error" in resp:
                    polls_err += 1
                    continue
                result = parse_orderbook(resp)
                if result is None:
                    polls_err += 1
                    continue
                bid, ask, bid_qty, ask_qty, exts = result
                tick = pack_tick(time.time_ns(), venue_id, pair_id,
                                 bid, ask, bid_qty, ask_qty, exts)
                f.write(tick)
                polls_ok      += 1
                ticks_written += 1

            # Progress line every 100 ticks
            if ticks_written > 0 and ticks_written % 100 == 0:
                elapsed   = time.monotonic() - t_start
                rate      = ticks_written / elapsed if elapsed > 0 else 0
                size_mb   = ticks_written * 64 / 1024 / 1024
                remaining = (deadline - time.monotonic()) / 3600 if args.hours > 0 else float("inf")
                eta_str   = f"ETA {remaining:.1f}h" if args.hours > 0 else "∞"
                ts        = time.strftime("%H:%M:%S")
                print(f"\r[{ts}] ticks={ticks_written:,}  ok={polls_ok:,}  "
                      f"err={polls_err}  rate={rate:.1f}/s  "
                      f"file={size_mb:.2f}MB  {eta_str}",
                      end="", flush=True)

            time.sleep(args.interval)

    elapsed  = time.monotonic() - t_start
    size_mb  = ticks_written * 64 / 1024 / 1024
    print(f"\n\nDone. {ticks_written:,} ticks  {size_mb:.2f} MB  {elapsed/3600:.2f}h")
    print(f"Output: {args.out}")
    print(f"\nNext step: set \"data_file\": \"{args.out}\" in your config, then:")
    print(f"  ./build/backtest config/default.json")

    if ticks_written > 0:
        file_size   = os.path.getsize(args.out)
        total_ticks = file_size // 64
        valid_str   = "valid" if file_size % 64 == 0 else "INVALID — not multiple of 64 bytes"
        print(f"\nFile has {total_ticks:,} ticks total ({valid_str})")

if __name__ == "__main__":
    main()
