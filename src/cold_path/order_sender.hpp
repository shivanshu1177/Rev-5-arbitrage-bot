#pragma once
// Order sender — live mode only.
// Maintains one persistent libcurl handle per venue (C2C1, C2C2), each with keep-alive.
// Signs every request with Ed25519 using the shared account private key.
//
// CoinSwitch signing (verified from official Reference Client):
//   signing_string = METHOD + decoded_path + epoch_ms
//   e.g. "POST/trade/api/v2/order1716449072123"
//   Body is sent in the HTTP request but NOT included in the signing string.
//   Epoch is Unix milliseconds (not seconds).
//   Signature is hex-encoded (64 bytes → 128 hex chars).
//
//   Headers on every authenticated request:
//     X-AUTH-APIKEY:    <api_key>
//     X-AUTH-SIGNATURE: <hex(Ed25519(signing_string))>
//     X-AUTH-EPOCH:     <epoch_ms>
//     Content-Type:     application/json
//
//   All endpoints including /trade/api/v2/depth require the same auth headers.
//
// In BACKTEST_MODE, send_order() is compiled to a no-op (guarded by #ifdef).
#include "common/types.hpp"
#include "hot_path/order_builder.hpp"
#include <cstddef>

// Forward-declare CURL to avoid pulling <curl/curl.h> into all translation units.
using CURL = void;

namespace arb {

class OrderSender {
public:
    struct VenueHandle {
        CURL* curl                = nullptr;
        char  api_key[128]        = {};
        char  private_key_hex[128]= {};
        char  base_url[256]       = {};
        char  exchange_id[16]     = {};
        char  last_response[4096] = {};
        size_t response_len       = 0;
    };


    OrderSender() = default;
    ~OrderSender() { cleanup(); }
    OrderSender(const OrderSender&)            = delete;
    OrderSender& operator=(const OrderSender&) = delete;

    bool init(const Config& cfg) noexcept;
    void send_order(uint8_t venue, const OrderTemplate& tmpl) noexcept;
    void cleanup() noexcept;

private:
    VenueHandle handles_[2];   // [0]=C2C1, [1]=C2C2
    // Ed25519 sign: signing_string = method + decoded_path + epoch_ms (NO body).
    // Body is sent separately in the HTTP request and is NOT signed.
    // Writes 128 hex chars + null into out_hex[129].
    void sign_request(uint8_t venue_idx,
                      const char* method,    // "POST"
                      const char* path,      // "/trade/api/v2/order"
                      uint64_t    epoch_ms,  // Unix milliseconds
                      char out_hex[129]) noexcept;
};

} // namespace arb
