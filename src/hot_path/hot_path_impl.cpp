// This compilation unit exists so that all hot-path headers are compiled as part of
// the hot_path_objects OBJECT library, which is built with -fno-exceptions.
// The template process_tick<> is not explicitly instantiated here — it is instantiated
// implicitly in backtester.cpp and live.cpp, which are also compiled with -fno-exceptions.
// Including all headers here ensures the non-template parts (MarketState::update,
// VirtualLedger::try_arb, etc.) are compiled with the correct flags.
#include "hot_path/ring_buffer.hpp"
#include "hot_path/market_state.hpp"
#include "hot_path/signal_eval.hpp"
#include "hot_path/virtual_ledger.hpp"
#include "hot_path/order_builder.hpp"
#include "hot_path/engine.hpp"
