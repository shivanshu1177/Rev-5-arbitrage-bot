cmake_minimum_required(VERSION 3.24)
project(ArbitrageBot LANGUAGES CXX)

set(CMAKE_CXX_STANDARD          20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Dependencies via FetchContent (avoids "brew install" requirement) ──────────
include(FetchContent)

# spdlog (async logging for cold path)
FetchContent_Declare(spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG        v1.15.3
    GIT_SHALLOW    TRUE)

# nlohmann/json (header-only, single-file download)
FetchContent_Declare(nlohmann_json
    URL      https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp
    DOWNLOAD_NO_EXTRACT TRUE
    DOWNLOAD_DIR "${CMAKE_BINARY_DIR}/include/nlohmann")

# IXWebSocket (TLS-enabled WebSocket client for live mode)
FetchContent_Declare(ixwebsocket
    GIT_REPOSITORY https://github.com/machinezone/IXWebSocket.git
    GIT_TAG        v11.4.4
    GIT_SHALLOW    TRUE)
set(USE_TLS ON CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(spdlog nlohmann_json ixwebsocket)

# Create a shim target so that #include <nlohmann/json.hpp> resolves.
add_library(nlohmann_json_iface INTERFACE)
target_include_directories(nlohmann_json_iface INTERFACE
    "${CMAKE_BINARY_DIR}/include")

# OpenSSL (Homebrew on macOS)
set(OPENSSL_ROOT_DIR "/opt/homebrew/opt/openssl@3" CACHE PATH "OpenSSL Homebrew prefix")
find_package(OpenSSL REQUIRED)

# CURL (Homebrew on macOS)
set(CURL_ROOT "/opt/homebrew/opt/curl" CACHE PATH "CURL Homebrew prefix")
find_package(CURL REQUIRED)

# ── Compiler flags ─────────────────────────────────────────────────────────────
# Common release flags for both hot and cold paths.
set(COMMON_FLAGS
    -O3
    -mcpu=apple-m2
    -flto=thin          # ThinLTO — works with Apple's ld; -flto (full) may fail on macOS
    -fomit-frame-pointer
    -DNDEBUG
    -Wall
    -Wextra
    -Wpedantic)

# ── Hot-path OBJECT library ────────────────────────────────────────────────────
# Compiled with -fno-exceptions: the hot-path code must never throw.
# Headers are included here to ensure they are compiled with these flags.
# Template instantiations happen in backtester.cpp / live.cpp (also -fno-exceptions).
add_library(hot_path_objects OBJECT
    src/hot_path/hot_path_impl.cpp)

target_include_directories(hot_path_objects PUBLIC
    ${CMAKE_SOURCE_DIR}/src)

target_compile_options(hot_path_objects PRIVATE
    ${COMMON_FLAGS}
    -fno-exceptions)

target_compile_features(hot_path_objects PUBLIC cxx_std_20)

# ── Cold-path static library ───────────────────────────────────────────────────
add_library(cold_path_lib STATIC
    src/cold_path/config_loader.cpp
    src/cold_path/file_source.cpp
    src/cold_path/order_sender.cpp
    src/cold_path/metrics.cpp
    src/cold_path/rest_poller.cpp)

target_include_directories(cold_path_lib PUBLIC
    ${CMAKE_SOURCE_DIR}/src)

target_compile_options(cold_path_lib PRIVATE
    ${COMMON_FLAGS})

target_link_libraries(cold_path_lib PUBLIC
    spdlog::spdlog
    nlohmann_json_iface
    OpenSSL::SSL
    OpenSSL::Crypto
    CURL::libcurl)

# ── backtest executable ────────────────────────────────────────────────────────
add_executable(backtest src/backtester.cpp)

target_include_directories(backtest PRIVATE ${CMAKE_SOURCE_DIR}/src)

target_compile_options(backtest PRIVATE
    ${COMMON_FLAGS})
# Note: backtester.cpp uses exceptions (spdlog, try/catch in main).
# The hot-path functions are in hot_path_objects which is -fno-exceptions.
# The two ABIs are compatible because the hot-path functions are all noexcept.

target_compile_definitions(backtest PRIVATE BACKTEST_MODE)

target_link_libraries(backtest PRIVATE
    $<TARGET_OBJECTS:hot_path_objects>
    cold_path_lib)

set_target_properties(backtest PROPERTIES
    INTERPROCEDURAL_OPTIMIZATION TRUE)

# ── live executable ────────────────────────────────────────────────────────────
# Live mode is NOT compiled with BACKTEST_MODE, so OrderSender::send_order() uses
# real CURL + OpenSSL signing. IXWebSocket integration is a TODO (see live.cpp).
add_executable(live src/live.cpp)

target_include_directories(live PRIVATE ${CMAKE_SOURCE_DIR}/src)

target_compile_options(live PRIVATE
    ${COMMON_FLAGS})

target_link_libraries(live PRIVATE
    $<TARGET_OBJECTS:hot_path_objects>
    cold_path_lib
    ixwebsocket)

set_target_properties(live PROPERTIES
    INTERPROCEDURAL_OPTIMIZATION TRUE)

# ── paper_trader executable ────────────────────────────────────────────────────
# Polls real CoinSwitch REST API, runs signal engine, records trades without sending orders.
add_executable(paper_trader src/paper_trader.cpp)

target_include_directories(paper_trader PRIVATE ${CMAKE_SOURCE_DIR}/src)

target_compile_options(paper_trader PRIVATE ${COMMON_FLAGS})

target_compile_definitions(paper_trader PRIVATE BACKTEST_MODE)

target_link_libraries(paper_trader PRIVATE
    $<TARGET_OBJECTS:hot_path_objects>
    cold_path_lib)

set_target_properties(paper_trader PROPERTIES
    INTERPROCEDURAL_OPTIMIZATION TRUE)

# Mock data is generated via Python: python3 tools/generate_mock_data.py
# See tools/generate_mock_data.py for usage.

# ── Helpful status messages ────────────────────────────────────────────────────
message(STATUS "OpenSSL: ${OPENSSL_VERSION} at ${OPENSSL_INCLUDE_DIR}")
message(STATUS "CURL:    ${CURL_VERSION_STRING}")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "")
message(STATUS "Targets:   backtest   paper_trader   live")
message(STATUS "Quick start:")
message(STATUS "  python3 ../tools/generate_mock_data.py --output ../data/mock_ticks.bin")
message(STATUS "  ./backtest ../config/default.json")
