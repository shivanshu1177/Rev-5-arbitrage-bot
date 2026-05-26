#pragma once
#include "common/types.hpp"
#include <string>
#include <stdexcept>

namespace arb {

// Parse config/default.json and return a fully populated Config struct.
// Throws std::runtime_error on parse failure or missing required fields.
// Cold path only — uses exceptions, heap allocation, and std::string.
Config load_config(const std::string& path);

} // namespace arb
