#pragma once
#include <spdlog/spdlog.h>
#include <spdlog/async.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <memory>
#include <string>

namespace logger {

// Call once at startup (cold path, before hot loop).
// Creates an async logger that writes to both stdout and a log file.
// The async thread pool is non-blocking — logging calls return immediately.
inline void setup(const std::string& log_file = "arbitrage.log") {
    spdlog::init_thread_pool(8192, 1);
    auto stdout_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    auto file_sink   = std::make_shared<spdlog::sinks::basic_file_sink_mt>(log_file, true);
    auto arb_logger  = std::make_shared<spdlog::async_logger>(
        "arb",
        spdlog::sinks_init_list{stdout_sink, file_sink},
        spdlog::thread_pool(),
        spdlog::async_overflow_policy::overrun_oldest);
    arb_logger->set_level(spdlog::level::info);
    arb_logger->set_pattern("[%H:%M:%S.%e] [%l] %v");
    spdlog::register_logger(arb_logger);
    spdlog::set_default_logger(arb_logger);
}

[[nodiscard]] inline std::shared_ptr<spdlog::logger> get() {
    return spdlog::get("arb");
}

inline void flush() {
    if (auto lg = spdlog::get("arb")) lg->flush();
}

} // namespace logger
