#!/usr/bin/env python3
"""
Latency histogram generator for the CoinSwitch Arbitrage Bot.

Auto-called by each binary at run end:
  ./build/backtest      → latency_profile_backtest.json → latency_histogram_backtest.png
  ./build/live --paper  → latency_profile_ws.json       → latency_histogram_ws.png
  ./build/paper_trader  → latency_profile_rest.json     → latency_histogram_rest.png

Manual usage:
  python3 tools/generate_charts.py                         # regenerate all found profiles
  python3 tools/generate_charts.py --input <file.json> --out <file.png>
  python3 tools/generate_charts.py --legacy                # regenerate docs/ static charts
"""

import argparse
import json
import os
import sys

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
import numpy as np

BUCKET_LABELS_US = ['<1µs', '1-10µs', '10-100µs', '100µs-1ms', '1-10ms', '10-100ms', '>100ms']

# ── Helpers ───────────────────────────────────────────────────────────────────

def percentile_bucket(raw, pct):
    total = sum(raw)
    if total == 0:
        return 0
    cum = 0
    for i, v in enumerate(raw):
        cum += v
        if cum / total >= pct:
            return i
    return len(raw) - 1

def _add_pct_labels(ax, bars, values):
    for bar, v in zip(bars, values):
        if v > 0.5:
            ax.text(bar.get_x() + bar.get_width() / 2, bar.get_height() + 0.5,
                    f'{v:.1f}%', ha='center', va='bottom', fontsize=9)

# ── Live / Paper-WS histogram (queue + engine panels) ────────────────────────

def chart_ws_or_rest(data: dict, out_path: str) -> None:
    mode       = data.get("mode", "live_ws")
    tick_count = data.get("tick_count", 0)
    q          = data["queue_latency"]
    e          = data["engine_latency"]

    q_raw  = q["buckets"]
    e_raw  = e["buckets"]
    q_tot  = sum(q_raw) or 1
    e_tot  = sum(e_raw) or 1
    q_pct  = [100.0 * v / q_tot for v in q_raw]
    e_pct  = [100.0 * v / e_tot for v in e_raw]
    x      = np.arange(len(BUCKET_LABELS_US))

    is_ws  = "ws" in mode
    label  = "WebSocket" if is_ws else "REST"
    color_q = '#2196F3' if is_ws else '#4CAF50'
    color_e = '#0D47A1' if is_ws else '#1B5E20'

    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(14, 5))
    fig.suptitle(
        f'{label} Latency Profile  —  {tick_count:,} ticks\n'
        f'Queue avg={q["avg_us"]:.1f}µs  Engine avg={e["avg_us"]:.3f}µs',
        fontsize=13, fontweight='bold')

    # Panel 1: queue latency
    bars1 = ax1.bar(x, q_pct, color=color_q, edgecolor='white', linewidth=0.5)
    _add_pct_labels(ax1, bars1, q_pct)
    p50 = percentile_bucket(q_raw, 0.50)
    p95 = percentile_bucket(q_raw, 0.95)
    p99 = percentile_bucket(q_raw, 0.99)
    ax1.axvline(x=p50+0.5, color='#4CAF50', lw=2, ls='--', label=f'p50 ({BUCKET_LABELS_US[p50]})')
    ax1.axvline(x=p95+0.5, color='#FF9800', lw=2, ls='--', label=f'p95 ({BUCKET_LABELS_US[p95]})')
    ax1.axvline(x=p99+0.5, color='#F44336', lw=2, ls='--', label=f'p99 ({BUCKET_LABELS_US[p99]})')
    ax1.set_xticks(x); ax1.set_xticklabels(BUCKET_LABELS_US, fontsize=9, rotation=15)
    ax1.set_ylabel('% of Ticks'); ax1.set_title(f'{label} receive → engine pop')
    ax1.legend(fontsize=9); ax1.grid(axis='y', alpha=0.3); ax1.set_facecolor('#F9F9F9')
    ax1.set_ylim(0, max(q_pct) * 1.25 if max(q_pct) > 0 else 1)

    # Panel 2: engine latency
    bars2 = ax2.bar(x, e_pct, color=color_e, edgecolor='white', linewidth=0.5)
    _add_pct_labels(ax2, bars2, e_pct)
    pe50 = percentile_bucket(e_raw, 0.50)
    pe95 = percentile_bucket(e_raw, 0.95)
    ax2.axvline(x=pe50+0.5, color='#4CAF50', lw=2, ls='--', label=f'p50 ({BUCKET_LABELS_US[pe50]})')
    ax2.axvline(x=pe95+0.5, color='#FF9800', lw=2, ls='--', label=f'p95 ({BUCKET_LABELS_US[pe95]})')
    ax2.set_xticks(x); ax2.set_xticklabels(BUCKET_LABELS_US, fontsize=9, rotation=15)
    ax2.set_ylabel('% of Ticks'); ax2.set_title('process_tick() duration')
    ax2.legend(fontsize=9); ax2.grid(axis='y', alpha=0.3); ax2.set_facecolor('#F9F9F9')
    ax2.set_ylim(0, max(e_pct) * 1.25 if max(e_pct) > 0 else 1)

    fig.tight_layout()
    fig.savefig(out_path, dpi=150, bbox_inches='tight')
    plt.close(fig)

# ── Backtest latency cost histogram ──────────────────────────────────────────

def chart_backtest(data: dict, out_path: str) -> None:
    trade_count   = data.get("trade_count", 0)
    tick_count    = data.get("tick_count", 0)
    penalty_bps   = data.get("latency_penalty_bps", 0)
    total_cost    = data.get("total_latency_cost_usd", 0)
    avg_cost      = data.get("avg_latency_cost_usd", 0)
    ticks_per_sec = data.get("hot_loop_ticks_per_sec", 0)
    eng           = data.get("engine_latency")

    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(14, 5))
    fig.suptitle(
        f'Backtest Latency Profile  —  {tick_count:,} ticks  |  {trade_count} trades  |  {penalty_bps:.0f} bps penalty\n'
        f'Total latency cost: ${total_cost:.2f}  |  Avg per trade: ${avg_cost:.2f}',
        fontsize=13, fontweight='bold')

    if eng:
        # Panel 1: real process_tick() ns histogram
        e_raw = eng["buckets"]
        e_tot = sum(e_raw) or 1
        e_pct = [100.0 * v / e_tot for v in e_raw]
        x     = np.arange(len(BUCKET_LABELS_US))
        bars  = ax1.bar(x, e_pct, color='#2196F3', edgecolor='white', linewidth=0.5)
        _add_pct_labels(ax1, bars, e_pct)
        p50 = percentile_bucket(e_raw, 0.50)
        p95 = percentile_bucket(e_raw, 0.95)
        p99 = percentile_bucket(e_raw, 0.99)
        ax1.axvline(x=p50+0.5, color='#4CAF50', lw=2, ls='--', label=f'p50 ({BUCKET_LABELS_US[p50]})')
        ax1.axvline(x=p95+0.5, color='#FF9800', lw=2, ls='--', label=f'p95 ({BUCKET_LABELS_US[p95]})')
        ax1.axvline(x=p99+0.5, color='#F44336', lw=2, ls='--', label=f'p99 ({BUCKET_LABELS_US[p99]})')
        ax1.set_xticks(x); ax1.set_xticklabels(BUCKET_LABELS_US, fontsize=9, rotation=15)
        ax1.set_ylabel('% of Ticks')
        ax1.set_title(f'process_tick() Latency — avg={eng["avg_us"]:.3f}µs')
        ax1.legend(fontsize=9); ax1.grid(axis='y', alpha=0.3); ax1.set_facecolor('#F9F9F9')
        ax1.set_ylim(0, max(e_pct) * 1.25 if max(e_pct) > 0 else 1)
    else:
        # Fallback: old per-trade cost distribution for pre-instrumented profiles
        cb     = data.get("cost_buckets", {})
        labels = cb.get("labels", [])
        counts = cb.get("counts", [])
        x      = np.arange(len(labels))
        bars   = ax1.bar(x, counts, color='#FF9800', edgecolor='white', linewidth=0.5)
        for bar, c in zip(bars, counts):
            if c > 0:
                ax1.text(bar.get_x() + bar.get_width()/2, bar.get_height() + 0.1,
                         str(c), ha='center', va='bottom', fontsize=10, fontweight='bold')
        ax1.set_xticks(x); ax1.set_xticklabels(labels, fontsize=10)
        ax1.set_ylabel('Number of Trades'); ax1.set_title('Simulated Latency Cost per Trade')
        ax1.grid(axis='y', alpha=0.3); ax1.set_facecolor('#F9F9F9')

    # Panel 2: throughput badge + cost summary
    ax2.axis('off')
    tps_m = ticks_per_sec / 1e6
    color = '#4CAF50' if tps_m >= 5 else '#FF9800' if tps_m >= 1 else '#F44336'
    ax2.add_patch(plt.Circle((0.5, 0.58), 0.30, color=color, alpha=0.15, transform=ax2.transAxes))
    ax2.text(0.5, 0.72, f'{tps_m:.2f}M', ha='center', va='center',
             fontsize=42, fontweight='bold', color=color, transform=ax2.transAxes)
    ax2.text(0.5, 0.55, 'ticks / sec', ha='center', va='center',
             fontsize=16, color='#555', transform=ax2.transAxes)
    ax2.text(0.5, 0.38, f'Latency penalty: {penalty_bps:.0f} bps', ha='center', va='center',
             fontsize=12, color='#333', transform=ax2.transAxes)
    ax2.text(0.5, 0.27, f'Total cost: ${total_cost:.2f}', ha='center', va='center',
             fontsize=12, color='#333', transform=ax2.transAxes)
    ax2.text(0.5, 0.16, f'Avg/trade: ${avg_cost:.2f}', ha='center', va='center',
             fontsize=12, color='#333', transform=ax2.transAxes)
    ax2.set_title('Hot-Loop Throughput', fontsize=13)

    fig.tight_layout()
    fig.savefig(out_path, dpi=150, bbox_inches='tight')
    plt.close(fig)

# ── Dispatch ──────────────────────────────────────────────────────────────────

def generate_from_json(input_path: str, out_path: str) -> None:
    with open(input_path) as f:
        data = json.load(f)
    mode = data.get("mode", "")
    if mode == "backtest":
        chart_backtest(data, out_path)
    elif mode in ("live_ws", "paper_ws", "live", "paper"):
        chart_ws_or_rest(data, out_path)
    elif mode == "paper_rest":
        data["mode"] = "paper_rest"
        chart_ws_or_rest(data, out_path)
    else:
        # best-effort: try to detect from keys
        if "cost_buckets" in data:
            chart_backtest(data, out_path)
        elif "queue_latency" in data:
            chart_ws_or_rest(data, out_path)
        else:
            print(f"Unknown mode '{mode}' in {input_path}", file=sys.stderr)
            sys.exit(1)

# ── Legacy hardcoded charts (docs/) ──────────────────────────────────────────

DOCS_DIR = os.path.join(os.path.dirname(__file__), '..', 'docs')

BUCKET_LABELS = ['<1µs', '1-10µs', '10-100µs', '100µs-1ms', '1-10ms', '10-100ms', '>100ms']
N_BUCKETS = len(BUCKET_LABELS)
WS_QUEUE_RAW   = [425, 134, 68, 7, 9, 2, 0]
WS_ENGINE_RAW  = [519, 91, 31, 4, 0, 0, 0]
REST_QUEUE_RAW = [7, 156, 1137, 524, 12, 0, 0]
REST_ENGINE_RAW= [1826, 4, 6, 0, 0, 0, 0]

def _pct(raw):
    t = sum(raw) or 1
    return [100.0 * v / t for v in raw]

def chart_ws_queue_latency():
    os.makedirs(DOCS_DIR, exist_ok=True)
    pct = _pct(WS_QUEUE_RAW)
    fig, ax = plt.subplots(figsize=(10, 5))
    x = np.arange(N_BUCKETS)
    bars = ax.bar(x, pct, color='#2196F3', edgecolor='white', linewidth=0.5)
    p50 = percentile_bucket(WS_QUEUE_RAW, 0.50)
    p95 = percentile_bucket(WS_QUEUE_RAW, 0.95)
    p99 = percentile_bucket(WS_QUEUE_RAW, 0.99)
    ax.axvline(x=p50+0.5, color='#4CAF50', lw=2, ls='--', label=f'p50 ({BUCKET_LABELS[p50]})')
    ax.axvline(x=p95+0.5, color='#FF9800', lw=2, ls='--', label=f'p95 ({BUCKET_LABELS[p95]})')
    ax.axvline(x=p99+0.5, color='#F44336', lw=2, ls='--', label=f'p99 ({BUCKET_LABELS[p99]})')
    _add_pct_labels(ax, bars, pct)
    ax.set_xticks(x); ax.set_xticklabels(BUCKET_LABELS, fontsize=10)
    ax.set_xlabel('Latency Bucket'); ax.set_ylabel('% of Ticks')
    ax.set_title('WebSocket Queue-Wait Latency\n(645 samples, avg=104.9µs)', fontsize=13, fontweight='bold')
    ax.legend(); ax.grid(axis='y', alpha=0.3); ax.set_facecolor('#F9F9F9')
    ax.set_ylim(0, max(pct) * 1.2)
    fig.tight_layout()
    out = os.path.join(DOCS_DIR, 'ws_queue_latency.png')
    fig.savefig(out, dpi=150, bbox_inches='tight'); plt.close(fig); print(f'Saved {out}')

def chart_engine_latency():
    os.makedirs(DOCS_DIR, exist_ok=True)
    ws_pct   = _pct(WS_ENGINE_RAW)
    rest_pct = _pct(REST_ENGINE_RAW)
    fig, ax  = plt.subplots(figsize=(10, 5))
    x = np.arange(N_BUCKETS); w = 0.38
    bars1 = ax.bar(x-w/2, ws_pct,   width=w, color='#2196F3', edgecolor='white', label='Live WS')
    bars2 = ax.bar(x+w/2, rest_pct, width=w, color='#4CAF50', edgecolor='white', label='Paper REST')
    for bar, v in list(zip(bars1, ws_pct)) + list(zip(bars2, rest_pct)):
        if v > 1.0:
            ax.text(bar.get_x()+bar.get_width()/2, bar.get_height()+0.5,
                    f'{v:.0f}%', ha='center', va='bottom', fontsize=8)
    ax.set_xticks(x); ax.set_xticklabels(BUCKET_LABELS, fontsize=10)
    ax.set_xlabel('Latency Bucket'); ax.set_ylabel('% of Ticks')
    ax.set_title('Engine process_tick() Duration\n(WS avg=2.8µs | REST avg=0.2µs)', fontsize=13, fontweight='bold')
    ax.legend(); ax.set_ylim(0, 105); ax.grid(axis='y', alpha=0.3); ax.set_facecolor('#F9F9F9')
    fig.tight_layout()
    out = os.path.join(DOCS_DIR, 'engine_latency.png')
    fig.savefig(out, dpi=150, bbox_inches='tight'); plt.close(fig); print(f'Saved {out}')

def chart_latency_comparison():
    os.makedirs(DOCS_DIR, exist_ok=True)
    labels = ['Queue\nREST→engine', 'Queue\nWS→engine', 'Engine\n(REST)', 'Engine\n(WS)']
    values = [87.8, 104.9, 0.199, 2.793]
    colors = ['#4CAF50', '#2196F3', '#8BC34A', '#64B5F6']
    fig, ax = plt.subplots(figsize=(9, 5))
    x = np.arange(len(labels))
    bars = ax.bar(x, values, color=colors, edgecolor='white', width=0.5)
    for bar, v in zip(bars, values):
        ax.text(bar.get_x()+bar.get_width()/2, bar.get_height()+1.5,
                f'{v:.1f}µs', ha='center', va='bottom', fontsize=11, fontweight='bold')
    ax.set_xticks(x); ax.set_xticklabels(labels, fontsize=11)
    ax.set_ylabel('Average Latency (µs)')
    ax.set_title('REST vs WebSocket Latency Comparison', fontsize=13, fontweight='bold')
    ax.legend(handles=[mpatches.Patch(color='#4CAF50', label='REST'),
                        mpatches.Patch(color='#2196F3', label='WS')], fontsize=10)
    ax.set_ylim(0, max(values)*1.25); ax.grid(axis='y', alpha=0.3); ax.set_facecolor('#F9F9F9')
    fig.tight_layout()
    out = os.path.join(DOCS_DIR, 'latency_comparison.png')
    fig.savefig(out, dpi=150, bbox_inches='tight'); plt.close(fig); print(f'Saved {out}')

# ── Entry point ───────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser(description='Generate latency histograms')
    ap.add_argument('--input',   help='JSON profile file written by a binary run')
    ap.add_argument('--out',     help='Output PNG path (required with --input)')
    ap.add_argument('--legacy',  action='store_true',
                    help='Regenerate hardcoded docs/ charts from static data')
    args = ap.parse_args()

    if args.input:
        if not os.path.exists(args.input):
            print(f'File not found: {args.input}', file=sys.stderr); sys.exit(1)
        out = args.out or args.input.replace('.json', '.png')
        generate_from_json(args.input, out)
        print(f'Histogram written to: {out}')
        return

    if args.legacy:
        print('Generating legacy docs/ charts...')
        chart_ws_queue_latency()
        chart_engine_latency()
        chart_latency_comparison()
        print('Done.')
        return

    # Auto-mode: regenerate all profile JSONs found in the working directory
    profiles = [
        ('latency_profile_backtest.json', 'latency_histogram_backtest.png'),
        ('latency_profile_ws.json',       'latency_histogram_ws.png'),
        ('latency_profile_rest.json',     'latency_histogram_rest.png'),
    ]
    found = [(j, p) for j, p in profiles if os.path.exists(j)]
    if not found:
        print('No latency_profile_*.json files found. Run a binary first.')
        sys.exit(0)
    for json_file, png_file in found:
        generate_from_json(json_file, png_file)
        print(f'Histogram written to: {png_file}')

if __name__ == '__main__':
    main()
