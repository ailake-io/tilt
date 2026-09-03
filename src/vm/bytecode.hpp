#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "runtime/value.hpp"

namespace tilt::vm {

enum class Op : std::uint8_t {
  Const,       // a: const index
  LoadLocal,   // a: slot
  StoreLocal,  // a: slot (consumes stack top)
  Pop,
  Neg,
  Not,
  Truthy,      // replace top with logico(truthy)
  And,         // pop 2 -> logico
  Or,
  Binop,       // a: op_names index
  Jump,        // a: target ip
  JumpIfFalse, // a: target ip (consumes stack top)
  CallFunc,    // a: names index, b: argc
  Print,       // b: argc
  Len,         // 1 arg -> inteiro
  Return,      // pop -> function result
  ReturnNil,
};

struct Instr {
  Op op{};
  std::int32_t a = 0;
  std::int32_t b = 0;
};

struct Chunk {
  std::vector<Instr> code;
  std::vector<rt::Value> consts;
  std::vector<std::string> op_names;  // for Binop
  std::vector<std::string> names;     // for CallFunc
  int num_locals = 0;
};

}  // namespace tilt::vm
