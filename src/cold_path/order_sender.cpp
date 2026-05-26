#include "cold_path/order_sender.hpp"

#ifndef BACKTEST_MODE
// Live mode: real libcurl + OpenSSL Ed25519 signing.
#include <curl/curl.h>
#include <openssl/evp.h>
#include <cstdio>
#include <cstring>
#include <chrono>

#include "cold_path/logger.hpp"

namespace {
// Captures the order response body into VenueHandle::last_response.
size_t order_write_cb(char* ptr, size_t size, size_t nmemb, void* ud) noexcept {
    auto* h = static_cast<arb::OrderSender::VenueHandle*>(ud);
    const size_t n = size * nmemb;
    if (h->response_len + n < sizeof(h->last_response) - 1) {
        std::memcpy(h->last_response + h->response_len, ptr, n);
        h->response_len += n;
        h->last_response[h->response_len] = '\0';
    }
    return n;
}
} // namespace
#endif

namespace arb {

bool OrderSender::init(const Config& cfg) noexcept {
#ifdef BACKTEST_MODE
    (void)cfg;
    return true;
#else
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        std::fprintf(stderr, "OrderSender: curl_global_init failed\n");
        return false;
    }
    for (int v = 0; v < 2; ++v) {
        handles_[v].curl = curl_easy_init();
        if (!handles_[v].curl) {
            std::fprintf(stderr, "OrderSender: curl_easy_init failed for venue %d\n", v + 1);
            return false;
        }
        // One API key + one Ed25519 key shared across both venues; exchange_id differentiates.
        std::strncpy(handles_[v].api_key,          cfg.api_key,              127);
        std::strncpy(handles_[v].private_key_hex,  cfg.private_key_hex,      127);
        std::strncpy(handles_[v].base_url,          cfg.venues[v].base_url,   255);
        std::strncpy(handles_[v].exchange_id,       cfg.venues[v].exchange_id,  15);

        CURL* c = static_cast<CURL*>(handles_[v].curl);
        curl_easy_setopt(c, CURLOPT_TCP_KEEPALIVE, 1L);
        curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L);
        curl_easy_setopt(c, CURLOPT_NOSIGNAL,       1L);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION,  order_write_cb);
        curl_easy_setopt(c, CURLOPT_WRITEDATA,      &handles_[v]);
    }
    return true;
#endif
}

void OrderSender::send_order(uint8_t venue, const OrderTemplate& tmpl) noexcept {
#ifdef BACKTEST_MODE
    (void)venue; (void)tmpl;
    return;
#else
    const uint8_t vi = venue - 1u;
    CURL* c = static_cast<CURL*>(handles_[vi].curl);
    if (!c) return;

    // Verified endpoint from official CoinSwitch docs.
    static const char* ORDER_PATH = "/trade/api/v2/order";
    char url[512];
    std::snprintf(url, sizeof(url), "%s%s", handles_[vi].base_url, ORDER_PATH);

    // CoinSwitch requires epoch in milliseconds (confirmed by Reference Client).
    const uint64_t now_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    // Sign: METHOD + decoded_path + epoch_ms  (body NOT in signing string).
    char sig_hex[129] = {};
    sign_request(vi, "POST", ORDER_PATH, now_ms, sig_hex);

    // Header names verified from official Reference Client.
    char h_key[160], h_sig[200], h_epoch[60];
    std::snprintf(h_key,   sizeof(h_key),   "X-AUTH-APIKEY: %s",    handles_[vi].api_key);
    std::snprintf(h_sig,   sizeof(h_sig),   "X-AUTH-SIGNATURE: %s", sig_hex);
    std::snprintf(h_epoch, sizeof(h_epoch), "X-AUTH-EPOCH: %llu",   (unsigned long long)now_ms);

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, h_key);
    headers = curl_slist_append(headers, h_sig);
    headers = curl_slist_append(headers, h_epoch);

    handles_[vi].response_len    = 0;
    handles_[vi].last_response[0]= '\0';

    curl_easy_setopt(c, CURLOPT_URL,           url);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER,    headers);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS,    tmpl.buffer);
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)tmpl.total_len);

    const CURLcode res = curl_easy_perform(c);
    curl_slist_free_all(headers);

    if (res != CURLE_OK) {
        spdlog::error("OrderSender: curl error venue={}: {}", venue, curl_easy_strerror(res));
        return;
    }

    long http_code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http_code);

    if (http_code == 200) {
        // Parse order_id from {"data":{"order_id":"<id>",...}}
        const char* p = std::strstr(handles_[vi].last_response, "\"order_id\"");
        if (p) {
            p = std::strchr(p, ':');
            if (p) {
                while (*p == ':' || *p == ' ' || *p == '"') ++p;
                char order_id[64] = {};
                size_t i = 0;
                while (*p && *p != '"' && i < 63) order_id[i++] = *p++;
                spdlog::info("OrderSender: placed venue={} order_id={}", venue, order_id);
            }
        }
    } else {
        spdlog::error("OrderSender: HTTP {} venue={} — {:.200s}",
                      http_code, venue, handles_[vi].last_response);
    }
#endif
}

void OrderSender::sign_request(uint8_t vi,
                               const char* method,
                               const char* path,
                               uint64_t    epoch_ms,
                               char out_hex[129]) noexcept {
#ifdef BACKTEST_MODE
    (void)vi; (void)method; (void)path; (void)epoch_ms; (void)out_hex;
#else
    // signing_string = METHOD + decoded_path + epoch_ms
    // e.g. "POST/trade/api/v2/order1716449072123"
    // The body is sent in the HTTP request but NOT signed (confirmed by Reference Client).
    // The path "/trade/api/v2/order" has no percent-encoded characters so no URL-decoding needed.
    char epoch_str[24];
    std::snprintf(epoch_str, sizeof(epoch_str), "%llu", (unsigned long long)epoch_ms);

    char msg[512]; size_t mlen = 0;
    auto cat = [&](const char* s, size_t n) {
        if (mlen + n < sizeof(msg)) { std::memcpy(msg + mlen, s, n); mlen += n; }
    };
    cat(method,    std::strlen(method));    // "POST"
    cat(path,      std::strlen(path));      // "/trade/api/v2/order"
    cat(epoch_str, std::strlen(epoch_str)); // "1716449072123"

    // Hex-decode the 64-char private_key_hex string → 32 raw bytes.
    uint8_t key_bytes[32];
    for (int i = 0; i < 32; ++i) {
        char b[3] = {handles_[vi].private_key_hex[i*2], handles_[vi].private_key_hex[i*2+1], '\0'};
        key_bytes[i] = static_cast<uint8_t>(std::strtoul(b, nullptr, 16));
    }

    // Ed25519 sign via OpenSSL EVP API.
    EVP_PKEY* pkey = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, key_bytes, 32);
    if (!pkey) return;

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) { EVP_PKEY_free(pkey); return; }

    EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, pkey);
    uint8_t sig[64]; size_t sig_len = 64;
    EVP_DigestSign(ctx, sig, &sig_len,
                   reinterpret_cast<const uint8_t*>(msg), mlen);

    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pkey);

    // Hex-encode 64 bytes → 128 hex chars + null.
    static const char* hex = "0123456789abcdef";
    for (size_t i = 0; i < sig_len; ++i) {
        out_hex[i*2]   = hex[(sig[i] >> 4) & 0xFu];
        out_hex[i*2+1] = hex[ sig[i]       & 0xFu];
    }
    out_hex[sig_len * 2] = '\0';
#endif
}

void OrderSender::cleanup() noexcept {
#ifndef BACKTEST_MODE
    for (auto& h : handles_) {
        if (h.curl) {
            curl_easy_cleanup(static_cast<CURL*>(h.curl));
            h.curl = nullptr;
        }
    }
    curl_global_cleanup();
#endif
}

} // namespace arb
