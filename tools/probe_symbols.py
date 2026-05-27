#!/usr/bin/env python3
"""
Probe every symbol × venue in config and report which ones return valid order-book data.

Usage:
  python3 tools/probe_symbols.py
  python3 tools/probe_symbols.py --config config/default.json

Output:
  Per-row table showing HTTP status and parsed bid/ask for each symbol on c2c1 and c2c2.
  Summary lists at the end: working on both / partial / failing.
"""

import argparse
import json
import sys
import time
from urllib.parse import quote

try:
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
except ImportError:
    sys.exit("ERROR: pip install cryptography requests")

try:
    import requests as _requests
except ImportError:
    sys.exit("ERROR: pip install cryptography requests")

BASE_URL   = "https://coinswitch.co"
DEPTH_BASE = BASE_URL + "/trade/api/v2/depth"
DEPTH_PATH = "/trade/api/v2/depth"
DEFAULT_CONFIG = "config/default.json"

# ── Auth (identical to record_ticks.py) ──────────────────────────────────────

def sign_request(private_key_hex: str, method: str, path: str, epoch_ms: int) -> str:
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

# ── Probe one symbol + venue ──────────────────────────────────────────────────

def probe(symbol: str, exchange: str, api_key: str, private_key_hex: str) -> tuple[int, str]:
    """Returns (http_status, detail_str)."""
    encoded      = quote(symbol, safe='')
    url          = f"{DEPTH_BASE}?symbol={encoded}&exchange={exchange}"
    signing_path = f"{DEPTH_PATH}?symbol={symbol}&exchange={exchange}"
    hdrs         = auth_headers(api_key, private_key_hex, "GET", signing_path)
    try:
        r = _requests.get(url, headers=hdrs, timeout=(5, 15))
    except _requests.exceptions.RequestException as e:
        return 0, f"NET ERR: {e}"

    if r.status_code != 200:
        return r.status_code, f"HTTP {r.status_code}"

    try:
        body = r.json()
    except Exception:
        return r.status_code, "JSON parse fail"

    data = body.get("data", body)
    bids = data.get("bids", data.get("buy", []))
    asks = data.get("asks", data.get("sell", []))
    if not bids or not asks:
        # Try top-level keys
        bids = body.get("bids", body.get("buy", []))
        asks = body.get("asks", body.get("sell", []))

    if not bids or not asks:
        return 200, "200 no-data"

    try:
        bid = float(bids[0][0])
        ask = float(asks[0][0])
        if bid <= 0 or ask <= 0 or ask <= bid:
            return 200, f"200 invalid({bid:.2f}/{ask:.2f})"
        return 200, f"200 OK  bid={bid:.4f} ask={ask:.4f}"
    except (IndexError, ValueError, TypeError) as e:
        return 200, f"200 parse-err: {e}"

# ── Main ──────────────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser(description="Probe CoinSwitch depth endpoint for each symbol/venue")
    ap.add_argument("--config", default=DEFAULT_CONFIG)
    args = ap.parse_args()

    with open(args.config) as f:
        cfg = json.load(f)

    api_key         = cfg.get("api_key", "")
    private_key_hex = cfg.get("private_key_hex", "")
    symbols         = cfg.get("symbols", [])

    if not api_key or api_key.startswith("YOUR_"):
        sys.exit("ERROR: api_key not set in config")
    if not private_key_hex or len(private_key_hex) != 64:
        sys.exit("ERROR: private_key_hex invalid in config")
    if not symbols:
        sys.exit("ERROR: no symbols in config")

    col = 30
    print(f"\n{'Symbol':<14}  {'C2C1':<{col}}  {'C2C2':<{col}}")
    print("-" * (14 + 2 + col + 2 + col))

    both_ok   = []
    partial   = []
    all_fail  = []

    for sym in symbols:
        name = sym["name"]
        print(f"{name:<14}  ", end="", flush=True)

        status1, detail1 = probe(name, "c2c1", api_key, private_key_hex)
        print(f"{detail1:<{col}}  ", end="", flush=True)

        status2, detail2 = probe(name, "c2c2", api_key, private_key_hex)
        print(f"{detail2:<{col}}")

        ok1 = status1 == 200 and "OK" in detail1
        ok2 = status2 == 200 and "OK" in detail2

        if ok1 and ok2:
            both_ok.append(name)
        elif ok1 or ok2:
            partial.append(name)
        else:
            all_fail.append(name)

        time.sleep(0.1)  # gentle pacing

    print(f"\n{'='*60}")
    print(f"Working on BOTH venues ({len(both_ok)}): {', '.join(both_ok) or 'none'}")
    print(f"Partial (one venue)   ({len(partial)}):  {', '.join(partial) or 'none'}")
    print(f"Failing on both       ({len(all_fail)}): {', '.join(all_fail) or 'none'}")
    print(f"{'='*60}")
    print("\nKeep only 'Working on BOTH venues' symbols in config/default.json")
    print("for a clean recording with 0 errors.")

if __name__ == "__main__":
    main()
