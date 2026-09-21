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
  Binop,       // a: op_names index
  Jump,        // a: target ip
  JumpIfFalse,  // a: target ip (consumes stack top)
  CallFunc,    // a: names index, b: argc
  Print,       // b: argc
  Len,         // 1 arg -> inteiro
  MakeList,    // b: argc (pops argc values -> lista)
  Index,       // pop idx, pop lista -> elemento
  GetField,    // a: names index (field), b: 1 if reached via '?.' (missing -> nulo)
  Return,      // pop -> function result
  ReturnNil,
};

// Operador de um Binop pre-decodificado (Instr::b): a VM despacha por este numero e
// so cai no apply_binop por texto (0 = desconhecido / caminho generico).
enum class BinOp : std::int32_t {
  Generico = 0,
  Soma,
  Sub,
  Mul,
  Div,
  Mod,
  Eq,
  Ne,
  Lt,
  Le,
  Gt,
  Ge,
};

inline BinOp binop_de(const std::string& op) {
  if (op == "+") return BinOp::Soma;
  if (op == "-") return BinOp::Sub;
  if (op == "*") return BinOp::Mul;
  if (op == "/") return BinOp::Div;
  if (op == "%") return BinOp::Mod;
  if (op == "==") return BinOp::Eq;
  if (op == "!=") return BinOp::Ne;
  if (op == "<") return BinOp::Lt;
  if (op == "<=") return BinOp::Le;
  if (op == ">") return BinOp::Gt;
  if (op == ">=") return BinOp::Ge;
  return BinOp::Generico;
}

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
