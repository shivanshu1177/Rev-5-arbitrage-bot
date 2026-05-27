#pragma once
// Order template builder: pre-formats the JSON body for a REST order once per pair,
// then patches price and quantity in the hot path without any allocation or snprintf.
//
// Template buffer layout:
//   {"symbol":"BTCUSDT","side":"BUY","type":"LIMIT","time_in_force":"IOC","quantity":"QQQQQQQQQQQQQQ","price":"PPPPPPPPPPPPPP"}
//   where Q and P are fixed-width ASCII placeholders of known lengths.
//
// LIMIT+IOC: we either fill at the observed price or receive no fill at all.
// We never suffer adverse slippage beyond the signal's assumed price.
//
// patch_order() overwrites these placeholders using fast_ftoa(), which writes directly
// into the buffer at pre-recorded offsets. No null terminator in the patched region —
// the REST client uses total_len to know how many bytes to send.
#include "common/types.hpp"
#include <cstddef>
#include <cstring>
#include <cstdint>

namespace arb {

// One pre-formatted request body per venue+pair index.
struct OrderTemplate {
    char   buffer[512];   // full HTTP POST body JSON
    size_t qty_off;       // offset in buffer where qty digits start
    size_t qty_len;       // number of characters reserved for qty
    size_t price_off;     // offset in buffer where price digits start
    size_t price_len;     // number of characters reserved for price
    size_t total_len;     // total bytes in buffer (to pass to libcurl)
    uint16_t pair_id;
    uint8_t  venue;
    uint8_t  _pad;
};

// Write a positive float as fixed-point decimal into buf[0..max_len).
// Returns number of characters written (no null terminator).
// Handles prices up to 999,999.99 and quantities like 0.00000001.
// Integers only go up to ~4.2B (uint32_t), sufficient for any crypto price in USDT.
inline size_t fast_ftoa(float v, char* buf, size_t max_len, int decimals) noexcept {
    if (max_len == 0 || v < 0.0f) { return 0; }

    // Split into integer and fractional parts.
    auto ipart = static_cast<uint32_t>(v);
    float frac  = v - static_cast<float>(ipart);

    // Encode integer part into a reversed temporary buffer.
    char tmp[12];
    int  n = 0;
    if (ipart == 0u) {
        tmp[n++] = '0';
    } else {
        uint32_t ip = ipart;
        while (ip > 0u && n < 12) {
            tmp[n++] = static_cast<char>('0' + ip % 10u);
            ip /= 10u;
        }
    }

    size_t len = 0;
    for (int i = n - 1; i >= 0 && len < max_len; --i) {
        buf[len++] = tmp[i];
    }

    if (decimals > 0 && len < max_len) {
        buf[len++] = '.';
        for (int i = 0; i < decimals && len < max_len; ++i) {
            frac *= 10.0f;
            const int digit = static_cast<int>(frac);
            buf[len++] = static_cast<char>('0' + digit);
            frac -= static_cast<float>(digit);
        }
    }
    return len;
}

// Cold path: build the JSON skeleton once per pair+venue.
// symbol_name: e.g. "BTC/USDT" — forward-slash will be included in the body.
// For CoinSwitch Pro API the symbol format uses a forward slash: "BTC/USDT".
inline void init_template(
    OrderTemplate& t,
    uint16_t pair_id,
    uint8_t venue,
    const char* symbol_name,
    const char* side = "BUY") noexcept
{
    // Fixed-width placeholders: 14 chars each → sufficient for prices up to 999999.99999999
    // and quantities down to 0.00000001.
    static constexpr size_t PRICE_WIDTH = 14;
    static constexpr size_t QTY_WIDTH   = 14;
    static constexpr char   PLACEHOLDER = '0';
    char* p = t.buffer;

    auto append = [&](const char* s, size_t slen) {
        std::memcpy(p, s, slen);
        p += slen;
    };
    auto append_str = [&](const char* s) {
        size_t len = 0;
        while (s[len]) ++len;
        append(s, len);
    };

    append_str("{\"symbol\":\"");
    append_str(symbol_name);
    append_str("\",\"side\":\"");
    append_str(side);
    append_str("\",\"type\":\"LIMIT\",\"time_in_force\":\"IOC\",\"quantity\":\"");

    // Record qty placeholder offset
    t.qty_off = static_cast<size_t>(p - t.buffer);
    t.qty_len = QTY_WIDTH;
    std::memset(p, PLACEHOLDER, QTY_WIDTH);
    p += QTY_WIDTH;

    append_str("\",\"price\":\"");

    // Record price placeholder offset
    t.price_off = static_cast<size_t>(p - t.buffer);
    t.price_len = PRICE_WIDTH;
    std::memset(p, PLACEHOLDER, PRICE_WIDTH);
    p += PRICE_WIDTH;

    append_str("\"}");

    t.total_len = static_cast<size_t>(p - t.buffer);
    t.pair_id   = pair_id;
    t.venue     = venue;
    t._pad      = 0;
}

// Hot path: overwrite price and qty placeholders with current values.
// Pads the rest of each field with trailing zeros to maintain fixed width.
inline void patch_order(OrderTemplate& t, float price, float qty) noexcept {
    // Write price digits, then pad remaining with '0'.
    size_t plen = fast_ftoa(price, t.buffer + t.price_off, t.price_len, 8);
    std::memset(t.buffer + t.price_off + plen, '0', t.price_len - plen);

    // Write qty digits, then pad.
    size_t qlen = fast_ftoa(qty, t.buffer + t.qty_off, t.qty_len, 8);
    std::memset(t.buffer + t.qty_off + qlen, '0', t.qty_len - qlen);
}

} // namespace arb
