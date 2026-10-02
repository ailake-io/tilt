#pragma once

#include <iosfwd>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "runtime/value.hpp"
#include "vm/bytecode.hpp"

namespace tilt::vm {

// Runtime native compiler for the scalar subset of bytecode. The backend emits
// executable memory directly; it never writes assembly or invokes a C compiler.
// Chunks outside this deliberately conservative subset fall back to Vm.
class Jit {
 public:
  using CallHook =
      std::function<rt::Value(const std::string&, std::vector<rt::Value>&, bool* handled)>;
  using CallSupport = std::function<bool(const std::string&)>;

  explicit Jit(std::ostream& out, CallHook call = {}, CallSupport support = {})
      : out_(out), call_(std::move(call)), support_(std::move(support)) {}

  static bool available();
  bool can_compile(const Chunk& chunk, std::string* reason = nullptr) const;
  rt::Value run(const Chunk& chunk, std::vector<rt::Value> args) const;

 private:
  [[maybe_unused]] std::ostream& out_;
  CallHook call_;
  CallSupport support_;
};

}  // namespace tilt::vm
