#!/usr/bin/env python3
"""
Capture real CoinSwitch Pro WebSocket market data to a binary MarketTick file.

The captured file can then be replayed offline by the backtester:
  ./backtest config/default.json   (with data_file pointing to the captured file)

Usage:
  python3 tools/fetch_real_ticks.py --duration 600 --output data/real_ticks.bin

Requirements:
  pip install websocket-client

NOTE: You must verify the CoinSwitch Pro WebSocket URL and subscription message
format from their official API documentation before running against a real account.
The formats below are best-effort approximations and may need adjustment.
"""

import struct
import time
import argparse
import os
import sys
import json

try:
    import websocket
except ImportError:
    print("ERROR: websocket-client not installed. Run: pip install websocket-client")
    sys.exit(1)

# Same format as generate_mock_data.py — must match C++ MarketTick.
TICK_FMT  = '<QBBHffff36x'
TICK_SIZE = struct.calcsize(TICK_FMT)
assert TICK_SIZE == 64

# Map symbol name → pair_id (must match config/default.json).
SYMBOL_TO_PAIR_ID = {
    "BTC/USDT":   0, "ETH/USDT":   1, "SOL/USDT":   2, "BNB/USDT":   3,
    "XRP/USDT":   4, "ADA/USDT":   5, "DOGE/USDT":  6, "MATIC/USDT": 7,
    "DOT/USDT":   8, "LTC/USDT":   9, "AVAX/USDT": 10, "LINK/USDT": 11,
    "UNI/USDT":  12, "ATOM/USDT": 13, "ETC/USDT":  14, "XLM/USDT":  15,
    "ALGO/USDT": 16, "ICP/USDT":  17, "FIL/USDT":  18, "SAND/USDT": 19,
}

# ── TODO: Verify these from CoinSwitch Pro API documentation ─────────────────
WS_URL_DEFAULT = "wss://ws-api.coinswitch.co"

def make_subscribe_msg(symbol: str) -> str:
    """Build the WebSocket subscription message for a given symbol.
    TODO: Verify the exact format from CoinSwitch Pro API docs."""
    return json.dumps({
        "method": "subscribe",
        "params": {
            "channel": "order-book",
            "symbol":  symbol
        }
    })

def parse_tick(msg: str, venue: int) -> tuple | None:
    """
    Parse a WebSocket orderbook update message into (bid, ask, bid_qty, ask_qty, symbol).
    TODO: Adjust field paths once the actual CoinSwitch Pro response format is known.
    Returns None if the message cannot be parsed.
    """
    try:
        d = json.loads(msg)
        # Hypothetical response structure — adjust based on real API:
        data   = d.get("data", d)
        bids   = data.get("bids", [])
        asks   = data.get("asks", [])
        symbol = data.get("symbol", d.get("symbol", ""))
        if not bids or not asks or symbol not in SYMBOL_TO_PAIR_ID:
            return None
        best_bid  = float(bids[0][0]);  bid_qty  = float(bids[0][1])
        best_ask  = float(asks[0][0]);  ask_qty  = float(asks[0][1])
        return (best_bid, best_ask, bid_qty, ask_qty, symbol)
    except Exception:
        return None
# ─────────────────────────────────────────────────────────────────────────────


class TickRecorder:
    def __init__(self, output_path: str, duration_s: float, venue: int):
        self.output_path = output_path
        self.end_time    = time.monotonic() + duration_s
        self.venue       = venue
        self.count       = 0
        self.f           = None

    def __enter__(self):
        os.makedirs(os.path.dirname(os.path.abspath(self.output_path)), exist_ok=True)
        self.f = open(self.output_path, 'wb')
        return self

    def __exit__(self, *_):
        if self.f:
            self.f.close()

    def on_message(self, ws, msg):
        if time.monotonic() >= self.end_time:
            ws.close()
            return
        result = parse_tick(msg, self.venue)
        if result is None:
            return
        best_bid, best_ask, bid_qty, ask_qty, symbol = result
        pair_id   = SYMBOL_TO_PAIR_ID[symbol]
        ts_ns     = int(time.time_ns())
        tick_bytes = struct.pack(TICK_FMT,
                                 ts_ns, self.venue, 0, pair_id,
                                 best_bid, best_ask, bid_qty, ask_qty)
        self.f.write(tick_bytes)
        self.count += 1
        if self.count % 1000 == 0:
            elapsed = time.monotonic() - (self.end_time - args.duration)
            print(f"  Captured {self.count:,} ticks ({elapsed:.0f}s elapsed)...",
                  end='\r', flush=True)

    def on_error(self, ws, error):
        print(f"\nWebSocket error: {error}")

    def on_close(self, ws, *_):
        print(f"\nWebSocket closed. Total ticks captured: {self.count:,}")

    def on_open(self, ws):
        print(f"Connected. Subscribing to {len(SYMBOL_TO_PAIR_ID)} symbols...")
        for symbol in SYMBOL_TO_PAIR_ID:
            ws.send(make_subscribe_msg(symbol))


def main():
    global args
    ap = argparse.ArgumentParser(description="Capture real CoinSwitch tick data to binary file")
    ap.add_argument('--output',   default='data/real_ticks.bin')
    ap.add_argument('--duration', type=float, default=60.0,
                    help='Capture duration in seconds (default: 60)')
    ap.add_argument('--ws-url',   default=WS_URL_DEFAULT,
                    help=f'WebSocket URL (default: {WS_URL_DEFAULT})')
    ap.add_argument('--venue',    type=int, default=1, choices=[1, 2],
                    help='Venue ID to tag ticks with (1=C2C1, 2=C2C2)')
    args = ap.parse_args()

    print(f"Connecting to {args.ws_url}")
    print(f"Recording for {args.duration:.0f} seconds → {args.output}")

    with TickRecorder(args.output, args.duration, args.venue) as recorder:
        ws = websocket.WebSocketApp(
            args.ws_url,
            on_open    = recorder.on_open,
            on_message = recorder.on_message,
            on_error   = recorder.on_error,
            on_close   = recorder.on_close)
        ws.run_forever(sslopt={"check_hostname": True})

    if os.path.exists(args.output):
        size_mb = os.path.getsize(args.output) / 1024 / 1024
        print(f"Output: {args.output} ({size_mb:.2f} MB)")
    else:
        print("No output file written (0 ticks captured).")


if __name__ == '__main__':
    main()
