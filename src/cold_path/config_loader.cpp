#include "cold_path/config_loader.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <stdexcept>
#include <cstring>

namespace arb {

Config load_config(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        throw std::runtime_error("Cannot open config file: " + path);
    }

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(f);
    } catch (const nlohmann::json::exception& e) {
        throw std::runtime_error(std::string("Config JSON parse error: ") + e.what());
    }

    Config cfg{};

    // Symbols
    const auto& syms = j.at("symbols");
    cfg.symbol_count = static_cast<uint16_t>(syms.size());
    if (cfg.symbol_count > constants::PAIRS_PER_VENUE) {
        throw std::runtime_error("Too many symbols (max 100)");
    }
    for (size_t i = 0; i < cfg.symbol_count; ++i) {
        cfg.symbols[i].pair_id = syms[i].at("pair_id").get<uint16_t>();
        const std::string name = syms[i].at("name").get<std::string>();
        std::strncpy(cfg.symbols[i].name, name.c_str(), sizeof(cfg.symbols[i].name) - 1);
    }

    // Trading params
    cfg.fee_rate         = j.value("fee_rate",                   0.0004f);
    cfg.min_profit_usd   = j.value("min_profit_usd",             0.50f);
    cfg.max_position_pct = j.value("max_position_pct",           0.10f);
    {
        const uint32_t stale_ms = j.value("max_stale_ms", 5000u);
        cfg.max_stale_ns = static_cast<uint64_t>(stale_ms) * 1'000'000ULL;
    }
    cfg.max_exchange_ts_skew_ms = j.value("max_exchange_ts_skew_ms", 100u);
    cfg.order_timeout_ms        = j.value("order_timeout_ms",        500u);
    cfg.initial_balance  = j.value("initial_balance_per_venue",  50000.0f);

    // Data file path
    const std::string df = j.value("data_file", "data/mock_ticks.bin");
    std::strncpy(cfg.data_file, df.c_str(), sizeof(cfg.data_file) - 1);

    // WebSocket URL
    const std::string ws = j.value("ws_url", "wss://ws.coinswitch.co");
    std::strncpy(cfg.ws_url, ws.c_str(), sizeof(cfg.ws_url) - 1);

    // Shared credentials — one API key + one Ed25519 private key for both venues.
    const std::string ak = j.value("api_key",         "");
    const std::string pk = j.value("private_key_hex", "");
    std::strncpy(cfg.api_key,         ak.c_str(), sizeof(cfg.api_key)         - 1);
    std::strncpy(cfg.private_key_hex, pk.c_str(), sizeof(cfg.private_key_hex) - 1);

    // Per-venue: exchange_id ("c2c1"/"c2c2") and base_url.
    const char* venue_names[2] = {"C2C1", "C2C2"};
    if (j.contains("venues")) {
        for (int v = 0; v < 2; ++v) {
            const auto& vname = venue_names[v];
            if (!j["venues"].contains(vname)) continue;
            const auto& vj = j["venues"][vname];

            const std::string eid = vj.value("exchange_id", "");
            const std::string url = vj.value("base_url",    "https://coinswitch.co");
            std::strncpy(cfg.venues[v].exchange_id, eid.c_str(), sizeof(cfg.venues[v].exchange_id) - 1);
            std::strncpy(cfg.venues[v].base_url,    url.c_str(), sizeof(cfg.venues[v].base_url)    - 1);
        }
    }

    // Simulation realism parameters (all optional — defaults represent realistic assumptions).
    if (j.contains("sim")) {
        const auto& s          = j["sim"];
        cfg.sim.fill_probability    = s.value("fill_probability",    0.70f);
        cfg.sim.latency_penalty_bps = s.value("latency_penalty_bps", 2.0f);
        cfg.sim.leg_failure_prob    = s.value("leg_failure_prob",    0.05f);
        cfg.sim.leg_fail_loss_bps   = s.value("leg_fail_loss_bps",   50.0f);
        cfg.sim.tds_rate            = s.value("tds_rate",            0.01f);
        cfg.sim.gst_on_fee_rate     = s.value("gst_on_fee_rate",     0.18f);
    }

    return cfg;
}

} // namespace arb
