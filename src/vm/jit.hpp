#pragma once

#include <iosfwd>
#include <string>
#include <vector>

#include "runtime/value.hpp"
#include "vm/bytecode.hpp"

namespace tilt::vm {

// Runtime native compiler for the integer subset of bytecode.  The backend
// emits executable memory directly; it never writes assembly or invokes a C
// compiler.  Chunks outside this deliberately conservative subset must fall
// back to Vm.
class Jit {
 public:
  explicit Jit(std::ostream& out) : out_(out) {}

  static bool available();
  bool can_compile(const Chunk& chunk, std::string* reason = nullptr) const;
  rt::Value run(const Chunk& chunk, std::vector<rt::Value> args) const;

 private:
  std::ostream& out_;
};

}  // namespace tilt::vm
