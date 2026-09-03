#pragma once

#include <string>

#include "runtime/value.hpp"

namespace tilt::rt {

// Minimal JSON reader/writer for the local `json` connector.
// json_parse throws std::runtime_error on malformed input.
Value json_parse(const std::string& text);

// Deterministic pretty output: 2-space indent, keys in insertion order.
std::string json_dump(const Value& value);

}  // namespace tilt::rt
