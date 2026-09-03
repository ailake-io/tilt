#pragma once

#include <functional>
#include <iosfwd>
#include <vector>

#include "runtime/value.hpp"
#include "vm/bytecode.hpp"

namespace tilt::vm {

// Executes a Chunk. Function calls are delegated to `call_hook`, which sets
// *handled = false when it does not know the name.
class Vm {
 public:
  using CallHook =
      std::function<rt::Value(const std::string&, std::vector<rt::Value>&, bool* handled)>;

  Vm(std::ostream& out, CallHook call_hook) : out_(out), call_(std::move(call_hook)) {}

  rt::Value run(const Chunk& chunk, std::vector<rt::Value> args);

 private:
  std::ostream& out_;
  CallHook call_;
};

}  // namespace tilt::vm
