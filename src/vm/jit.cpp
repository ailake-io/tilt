#include "vm/jit.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

#if defined(__x86_64__) && (defined(__linux__) || defined(__APPLE__))
#define TILT_JIT_X86_64 1
#include <sys/mman.h>
#include <unistd.h>
#else
#define TILT_JIT_X86_64 0
#endif

#if defined(__aarch64__) && (defined(__linux__) || defined(__APPLE__))
#define TILT_JIT_ARM64 1
#include <sys/mman.h>
#include <unistd.h>
#else
#define TILT_JIT_ARM64 0
#endif

namespace tilt::vm {

namespace {

#if TILT_JIT_X86_64

constexpr std::size_t kMaxLocals = 64;
constexpr std::size_t kMaxStack = 256;

struct JitState {
  std::int64_t locals[kMaxLocals]{};
  std::uint8_t local_kind[kMaxLocals]{};
  std::int64_t stack[kMaxStack]{};
  std::uint8_t stack_kind[kMaxStack]{};
  std::uint32_t sp = 0;
  std::uint8_t result_kind = static_cast<std::uint8_t>(rt::ValueKind::Nulo);
  bool decimal_mode = false;
  bool failed = false;
  const char* error = nullptr;
  std::ostream* out = nullptr;
  const Jit::CallHook* call = nullptr;
};

constexpr std::size_t kStackOffset = offsetof(JitState, stack);
constexpr std::size_t kStackKindOffset = offsetof(JitState, stack_kind);
constexpr std::size_t kSpOffset = offsetof(JitState, sp);
constexpr std::size_t kResultKindOffset = offsetof(JitState, result_kind);

extern "C" void jit_print(JitState* state, int argc) {
  if (argc < 1 || static_cast<std::size_t>(argc) > state->sp || argc > 32) {
    state->failed = true;
    state->error = "JIT: pilha invalida em imprimir";
    return;
  }
  const std::size_t first = state->sp - static_cast<std::size_t>(argc);
  for (int k = 0; k < argc; ++k) {
    if (k) *state->out << " ";
    const std::size_t index = first + static_cast<std::size_t>(k);
    const auto kind = static_cast<rt::ValueKind>(state->stack_kind[index]);
    rt::Value value;
    if (kind == rt::ValueKind::Logico) value = rt::Value::logico(state->stack[index] != 0);
    else if (kind == rt::ValueKind::Nulo) value = rt::Value::nulo();
    else if (kind == rt::ValueKind::Decimal) {
      double d = 0.0;
      std::uint64_t bits = static_cast<std::uint64_t>(state->stack[index]);
      std::memcpy(&d, &bits, sizeof d);
      value = rt::Value::decimal(d);
    } else value = rt::Value::inteiro(state->stack[index]);
    *state->out << rt::to_display(value);
  }
  *state->out << "\n";
  state->sp = static_cast<std::uint32_t>(first);
  state->stack[state->sp] = 0;
  state->stack_kind[state->sp++] = static_cast<std::uint8_t>(rt::ValueKind::Nulo);
}

const char* jit_binop_name(int op) {
  switch (static_cast<BinOp>(op)) {
    case BinOp::Soma: return "+";
    case BinOp::Sub: return "-";
    case BinOp::Mul: return "*";
    case BinOp::Div: return "/";
    case BinOp::Mod: return "%";
    case BinOp::Eq: return "==";
    case BinOp::Ne: return "!=";
    case BinOp::Lt: return "<";
    case BinOp::Le: return "<=";
    case BinOp::Gt: return ">";
    case BinOp::Ge: return ">=";
    case BinOp::Generico: break;
  }
  return "?";
}

rt::Value jit_value(const JitState* state, std::size_t index) {
  const auto kind = static_cast<rt::ValueKind>(state->stack_kind[index]);
  if (kind == rt::ValueKind::Decimal) {
    double d = 0.0;
    const std::uint64_t bits = static_cast<std::uint64_t>(state->stack[index]);
    std::memcpy(&d, &bits, sizeof d);
    return rt::Value::decimal(d);
  }
  if (kind == rt::ValueKind::Logico) return rt::Value::logico(state->stack[index] != 0);
  if (kind == rt::ValueKind::Nulo) return rt::Value::nulo();
  return kind == rt::ValueKind::Inteiro ? rt::Value::inteiro(state->stack[index]) : rt::Value::nulo();
}

void jit_store(JitState* state, rt::Value value) {
  if (state->sp >= kMaxStack) {
    state->failed = true;
    state->error = "JIT: pilha cheia";
    return;
  }
  std::int64_t payload = 0;
  if (value.kind == rt::ValueKind::Decimal) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value.d, sizeof bits);
    payload = static_cast<std::int64_t>(bits);
  } else if (value.kind == rt::ValueKind::Inteiro || value.kind == rt::ValueKind::Logico) {
    payload = value.kind == rt::ValueKind::Logico ? (value.b ? 1 : 0) : value.i;
  } else if (value.kind != rt::ValueKind::Nulo) {
    state->failed = true;
    state->error = "JIT: retorno nao escalar";
    return;
  }
  state->stack[state->sp] = payload;
  state->stack_kind[state->sp++] = static_cast<std::uint8_t>(value.kind);
}

extern "C" void jit_binop(JitState* state, int op) {
  if (state->sp < 2) {
    state->failed = true;
    state->error = "JIT: pilha insuficiente em operador";
    return;
  }
  const std::size_t rhs_index = --state->sp;
  const std::size_t lhs_index = --state->sp;
  const rt::Value lhs = jit_value(state, lhs_index);
  const rt::Value rhs = jit_value(state, rhs_index);
  bool ok = false;
  const rt::Value result = rt::apply_binop(jit_binop_name(op), lhs, rhs, &ok);
  if (!ok) {
    state->failed = true;
    state->error = "JIT: operador desconhecido";
    return;
  }
  jit_store(state, result);
}

extern "C" void jit_unary_truthy(JitState* state, int negate) {
  if (state->sp < 1) {
    state->failed = true;
    state->error = "JIT: pilha insuficiente em verdade";
    return;
  }
  const rt::Value value = jit_value(state, --state->sp);
  jit_store(state, rt::Value::logico(negate ? !value.truthy() : value.truthy()));
}

extern "C" void jit_neg(JitState* state) {
  if (state->sp < 1) {
    state->failed = true;
    state->error = "JIT: pilha insuficiente em negacao";
    return;
  }
  const rt::Value value = jit_value(state, --state->sp);
  if (value.kind == rt::ValueKind::Inteiro) jit_store(state, rt::Value::inteiro(-value.i));
  else if (value.kind == rt::ValueKind::Decimal) jit_store(state, rt::Value::decimal(-value.d));
  else {
    state->failed = true;
    state->error = "JIT: negacao requer numero";
  }
}

extern "C" int jit_pop_truthy(JitState* state) {
  if (state->sp < 1) {
    state->failed = true;
    state->error = "JIT: pilha insuficiente em salto";
    return 0;
  }
  return jit_value(state, --state->sp).truthy() ? 1 : 0;
}

extern "C" void jit_call(JitState* state, const char* name, int argc) {
  if (!state->call || argc < 0 || static_cast<std::size_t>(argc) > state->sp) {
    state->failed = true;
    state->error = "JIT: chamada sem callback ou argumentos insuficientes";
    return;
  }
  std::vector<rt::Value> args(static_cast<std::size_t>(argc));
  for (int k = argc - 1; k >= 0; --k) args[static_cast<std::size_t>(k)] = jit_value(state, --state->sp);
  bool handled = false;
  rt::Value result = (*state->call)(name, args, &handled);
  if (!handled) {
    state->failed = true;
    state->error = "JIT: funcao desconhecida";
    return;
  }
  jit_store(state, std::move(result));
}

class ExecutableMemory {
 public:
  explicit ExecutableMemory(const std::vector<std::uint8_t>& bytes) {
    const long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) throw std::runtime_error("JIT: tamanho de pagina invalido");
    size_ = (bytes.size() + static_cast<std::size_t>(page) - 1) / static_cast<std::size_t>(page) *
            static_cast<std::size_t>(page);
    ptr_ = mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr_ == MAP_FAILED) {
      ptr_ = nullptr;
      throw std::runtime_error("JIT: mmap falhou");
    }
    std::memcpy(ptr_, bytes.data(), bytes.size());
    if (mprotect(ptr_, size_, PROT_READ | PROT_EXEC) != 0) {
      munmap(ptr_, size_);
      ptr_ = nullptr;
      throw std::runtime_error("JIT: mprotect falhou");
    }
  }

  ~ExecutableMemory() {
    if (ptr_) munmap(ptr_, size_);
  }

  void* data() const { return ptr_; }

 private:
  void* ptr_ = nullptr;
  std::size_t size_ = 0;
};

class Emitter {
 public:
  void u8(std::uint8_t v) { code_.push_back(v); }

  void u32(std::uint32_t v) {
    for (int k = 0; k < 4; ++k) u8(static_cast<std::uint8_t>(v >> (k * 8)));
  }

  void i32(std::int32_t v) { u32(static_cast<std::uint32_t>(v)); }

  void i64(std::int64_t v) {
    for (int k = 0; k < 8; ++k)
      u8(static_cast<std::uint8_t>(static_cast<std::uint64_t>(v) >> (k * 8)));
  }

  std::size_t pos() const { return code_.size(); }

  void mov_rax_imm64(std::int64_t v) {
    u8(0x48);
    u8(0xb8);
    i64(v);
  }

  void mov_rsi_imm64(std::int64_t v) {
    u8(0x48);
    u8(0xbe);
    i64(v);
  }

  void mov_esi_imm32(std::int32_t v) {
    u8(0xbe);
    u32(static_cast<std::uint32_t>(v));
  }

  void mov_edx_imm32(std::int32_t v) {
    u8(0xba);
    u32(static_cast<std::uint32_t>(v));
  }

  // r12 is the JitState pointer.  The code follows the SysV x86-64 ABI.
  void load_sp_ecx() {
    u8(0x41);
    u8(0x8b);
    u8(0x8c);
    u8(0x24);
    u32(static_cast<std::uint32_t>(kSpOffset));
  }

  void store_sp_ecx() {
    u8(0x41);
    u8(0x89);
    u8(0x8c);
    u8(0x24);
    u32(static_cast<std::uint32_t>(kSpOffset));
  }

  void load_stack_rax_ecx() {
    u8(0x49);
    u8(0x8b);
    u8(0x84);
    u8(0xcc);
    u32(static_cast<std::uint32_t>(kStackOffset));
  }

  void store_stack_rax_ecx() {
    u8(0x49);
    u8(0x89);
    u8(0x84);
    u8(0xcc);
    u32(static_cast<std::uint32_t>(kStackOffset));
  }

  void load_stack_kind_al_ecx() {
    u8(0x41);
    u8(0x8a);
    u8(0x84);
    u8(0x0c);
    u32(static_cast<std::uint32_t>(kStackKindOffset));
  }

  void store_stack_kind_al_ecx() {
    u8(0x41);
    u8(0x88);
    u8(0x84);
    u8(0x0c);
    u32(static_cast<std::uint32_t>(kStackKindOffset));
  }

  void load_local_kind_al(std::int32_t slot) {
    u8(0x41);
    u8(0x8a);
    u8(0x84);
    u8(0x24);
    u32(static_cast<std::uint32_t>(offsetof(JitState, local_kind) +
                                   slot * static_cast<std::int32_t>(sizeof(std::uint8_t))));
  }

  void store_local_kind_al(std::int32_t slot) {
    u8(0x41);
    u8(0x88);
    u8(0x84);
    u8(0x24);
    u32(static_cast<std::uint32_t>(offsetof(JitState, local_kind) +
                                   slot * static_cast<std::int32_t>(sizeof(std::uint8_t))));
  }

  void store_result_kind_al() {
    u8(0x41);
    u8(0x88);
    u8(0x84);
    u8(0x24);
    u32(static_cast<std::uint32_t>(kResultKindOffset));
  }

  void push_kind_al() {
    load_sp_ecx();
    u8(0xff);
    u8(0xc9);
    store_stack_kind_al_ecx();
  }

  void push_kind_imm(std::uint8_t kind) {
    load_sp_ecx();
    u8(0xff);
    u8(0xc9);
    u8(0x41);
    u8(0xc6);
    u8(0x84);
    u8(0x0c);
    u32(static_cast<std::uint32_t>(kStackKindOffset));
    u8(kind);
  }

  void load_local_rax(std::int32_t slot) {
    u8(0x49);
    u8(0x8b);
    u8(0x84);
    u8(0x24);
    u32(static_cast<std::uint32_t>(slot * static_cast<std::int32_t>(sizeof(std::int64_t))));
  }

  void store_local_rax(std::int32_t slot) {
    u8(0x49);
    u8(0x89);
    u8(0x84);
    u8(0x24);
    u32(static_cast<std::uint32_t>(slot * static_cast<std::int32_t>(sizeof(std::int64_t))));
  }

  void push_rax() {
    load_sp_ecx();
    store_stack_rax_ecx();
    u8(0xff);
    u8(0xc1);  // inc ecx
    store_sp_ecx();
  }

  void pop_rax() {
    load_sp_ecx();
    u8(0xff);
    u8(0xc9);  // dec ecx
    store_sp_ecx();
    load_stack_rax_ecx();
  }

  void prologue() {
    u8(0x41);
    u8(0x54);  // push r12
    u8(0x49);
    u8(0x89);
    u8(0xfc);  // mov r12, rdi
  }

  void epilogue() {
    u8(0x41);
    u8(0x5c);  // pop r12
    u8(0xc3);  // ret
  }

  std::size_t jmp() {
    u8(0xe9);
    const std::size_t patch = pos();
    i32(0);
    return patch;
  }

  std::size_t jz_rax() {
    u8(0x0f);
    u8(0x84);
    const std::size_t patch = pos();
    i32(0);
    return patch;
  }

  void patch_rel32(std::size_t at, std::size_t target) {
    const std::int64_t delta =
        static_cast<std::int64_t>(target) - static_cast<std::int64_t>(at + sizeof(std::int32_t));
    if (delta < INT32_MIN || delta > INT32_MAX)
      throw std::runtime_error("JIT: salto distante demais");
    const auto v = static_cast<std::uint32_t>(static_cast<std::int32_t>(delta));
    for (int k = 0; k < 4; ++k)
      code_[at + static_cast<std::size_t>(k)] = static_cast<std::uint8_t>(v >> (k * 8));
  }

  void add_rax_rdx() {
    u8(0x48);
    u8(0x01);
    u8(0xd0);
  }
  void sub_rax_rdx() {
    u8(0x48);
    u8(0x29);
    u8(0xd0);
  }
  void imul_rax_rdx() {
    u8(0x48);
    u8(0x0f);
    u8(0xaf);
    u8(0xc2);
  }

  void cmp_to_bool(std::uint8_t condition) {
    u8(0x48);
    u8(0x39);
    u8(0xd0);  // cmp rax, rdx
    u8(0x0f);
    u8(condition);
    u8(0xc0);  // setcc al
    u8(0x48);
    u8(0x0f);
    u8(0xb6);
    u8(0xc0);  // movzx rax, al
  }

  void call_print(std::int32_t argc) {
    u8(0x4c);
    u8(0x89);
    u8(0xe7);  // mov rdi, r12
    u8(0xbe);  // mov esi, imm32
    u32(static_cast<std::uint32_t>(argc));
    mov_rax_imm64(reinterpret_cast<std::int64_t>(&jit_print));
    u8(0xff);
    u8(0xd0);  // call rax
  }

  void call_binop(std::int32_t op) {
    u8(0x4c);
    u8(0x89);
    u8(0xe7);  // mov rdi, r12
    mov_esi_imm32(op);
    mov_rax_imm64(reinterpret_cast<std::int64_t>(&jit_binop));
    u8(0xff);
    u8(0xd0);
  }

  void call_unary_truthy(bool negate) {
    u8(0x4c);
    u8(0x89);
    u8(0xe7);  // mov rdi, r12
    mov_esi_imm32(negate ? 1 : 0);
    mov_rax_imm64(reinterpret_cast<std::int64_t>(&jit_unary_truthy));
    u8(0xff);
    u8(0xd0);
  }

  void call_neg() {
    u8(0x4c);
    u8(0x89);
    u8(0xe7);  // mov rdi, r12
    mov_rax_imm64(reinterpret_cast<std::int64_t>(&jit_neg));
    u8(0xff);
    u8(0xd0);
  }

  void test_eax() {
    u8(0x85);
    u8(0xc0);
  }

  void call_pop_truthy() {
    u8(0x4c);
    u8(0x89);
    u8(0xe7);  // mov rdi, r12
    mov_rax_imm64(reinterpret_cast<std::int64_t>(&jit_pop_truthy));
    u8(0xff);
    u8(0xd0);
  }

  void call_func(const char* name, std::int32_t argc) {
    u8(0x4c);
    u8(0x89);
    u8(0xe7);  // mov rdi, r12
    mov_rsi_imm64(reinterpret_cast<std::int64_t>(name));
    mov_edx_imm32(argc);
    mov_rax_imm64(reinterpret_cast<std::int64_t>(&jit_call));
    u8(0xff);
    u8(0xd0);
  }

  const std::vector<std::uint8_t>& code() const { return code_; }

 private:
  std::vector<std::uint8_t> code_;
};

using Entry = std::int64_t (*)(JitState*);

bool jit_binop_supported(const std::string& op) {
  static const std::set<std::string> ops = {
      "+", "-", "*", "/", "%", "==", "!=", "<", "<=", ">", ">="};
  return ops.count(op) != 0;
}

#endif  // TILT_JIT_X86_64

#if TILT_JIT_ARM64

// O backend AArch64 usa o mesmo contrato de valores do JIT x86, mas mantém
// os helpers fora do código gerado. Isso permite executar bytecode escalar em
// memória executável sem depender de assembler ou de um compilador externo.
constexpr std::size_t kArmMaxLocals = 64;
constexpr std::size_t kArmMaxStack = 256;

struct ArmJitState {
  std::int64_t locals[kArmMaxLocals]{};
  std::uint8_t local_kind[kArmMaxLocals]{};
  std::int64_t stack[kArmMaxStack]{};
  std::uint8_t stack_kind[kArmMaxStack]{};
  std::uint32_t sp = 0;
  std::uint8_t result_kind = static_cast<std::uint8_t>(rt::ValueKind::Nulo);
  bool failed = false;
  const char* error = nullptr;
  std::ostream* out = nullptr;
  const Jit::CallHook* call = nullptr;
};

rt::Value arm_value(const ArmJitState* state, std::size_t index) {
  const auto kind = static_cast<rt::ValueKind>(state->stack_kind[index]);
  if (kind == rt::ValueKind::Decimal) {
    double d = 0.0;
    const std::uint64_t bits = static_cast<std::uint64_t>(state->stack[index]);
    std::memcpy(&d, &bits, sizeof d);
    return rt::Value::decimal(d);
  }
  if (kind == rt::ValueKind::Logico) return rt::Value::logico(state->stack[index] != 0);
  if (kind == rt::ValueKind::Nulo) return rt::Value::nulo();
  return rt::Value::inteiro(state->stack[index]);
}

void arm_store(ArmJitState* state, rt::Value value) {
  if (state->sp >= kArmMaxStack) {
    state->failed = true;
    state->error = "JIT ARM64: pilha cheia";
    return;
  }
  std::int64_t payload = 0;
  if (value.kind == rt::ValueKind::Decimal) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value.d, sizeof bits);
    payload = static_cast<std::int64_t>(bits);
  } else if (value.kind == rt::ValueKind::Inteiro || value.kind == rt::ValueKind::Logico) {
    payload = value.kind == rt::ValueKind::Logico ? (value.b ? 1 : 0) : value.i;
  } else if (value.kind != rt::ValueKind::Nulo) {
    state->failed = true;
    state->error = "JIT ARM64: retorno nao escalar";
    return;
  }
  state->stack[state->sp] = payload;
  state->stack_kind[state->sp++] = static_cast<std::uint8_t>(value.kind);
}

extern "C" void arm_push(ArmJitState* state, std::int64_t payload, std::uint8_t kind) {
  if (state->sp >= kArmMaxStack) {
    state->failed = true;
    state->error = "JIT ARM64: pilha cheia";
    return;
  }
  state->stack[state->sp] = payload;
  state->stack_kind[state->sp++] = kind;
}

extern "C" void arm_load_local(ArmJitState* state, std::int32_t slot) {
  if (slot < 0 || static_cast<std::size_t>(slot) >= kArmMaxLocals) {
    state->failed = true;
    state->error = "JIT ARM64: local fora da faixa";
    return;
  }
  arm_push(state, state->locals[slot], state->local_kind[slot]);
}

extern "C" void arm_store_local(ArmJitState* state, std::int32_t slot) {
  if (slot < 0 || static_cast<std::size_t>(slot) >= kArmMaxLocals || state->sp == 0) {
    state->failed = true;
    state->error = "JIT ARM64: store local invalido";
    return;
  }
  const std::size_t top = --state->sp;
  state->locals[slot] = state->stack[top];
  state->local_kind[slot] = state->stack_kind[top];
}

extern "C" void arm_pop(ArmJitState* state) {
  if (state->sp == 0) {
    state->failed = true;
    state->error = "JIT ARM64: pilha insuficiente";
    return;
  }
  --state->sp;
}

const char* arm_binop_name(std::int32_t op) {
  switch (static_cast<BinOp>(op)) {
    case BinOp::Soma: return "+";
    case BinOp::Sub: return "-";
    case BinOp::Mul: return "*";
    case BinOp::Div: return "/";
    case BinOp::Mod: return "%";
    case BinOp::Eq: return "==";
    case BinOp::Ne: return "!=";
    case BinOp::Lt: return "<";
    case BinOp::Le: return "<=";
    case BinOp::Gt: return ">";
    case BinOp::Ge: return ">=";
    case BinOp::Generico: break;
  }
  return "?";
}

extern "C" void arm_binop(ArmJitState* state, std::int32_t op) {
  if (state->sp < 2) {
    state->failed = true;
    state->error = "JIT ARM64: pilha insuficiente em operador";
    return;
  }
  const rt::Value rhs = arm_value(state, --state->sp);
  const rt::Value lhs = arm_value(state, --state->sp);
  bool ok = false;
  const rt::Value result = rt::apply_binop(arm_binop_name(op), lhs, rhs, &ok);
  if (!ok) {
    state->failed = true;
    state->error = "JIT ARM64: operador desconhecido";
    return;
  }
  arm_store(state, result);
}

extern "C" void arm_neg(ArmJitState* state) {
  if (state->sp < 1) {
    state->failed = true;
    state->error = "JIT ARM64: pilha insuficiente em negacao";
    return;
  }
  const rt::Value value = arm_value(state, --state->sp);
  if (value.kind == rt::ValueKind::Inteiro) arm_store(state, rt::Value::inteiro(-value.i));
  else if (value.kind == rt::ValueKind::Decimal) arm_store(state, rt::Value::decimal(-value.d));
  else {
    state->failed = true;
    state->error = "JIT ARM64: negacao requer numero";
  }
}

extern "C" void arm_truthy(ArmJitState* state, std::int32_t negate) {
  if (state->sp < 1) {
    state->failed = true;
    state->error = "JIT ARM64: pilha insuficiente em verdade";
    return;
  }
  const bool truth = arm_value(state, --state->sp).truthy();
  arm_store(state, rt::Value::logico(negate ? !truth : truth));
}

extern "C" std::int32_t arm_pop_truthy(ArmJitState* state) {
  if (state->sp < 1) {
    state->failed = true;
    state->error = "JIT ARM64: pilha insuficiente em salto";
    return 0;
  }
  return arm_value(state, --state->sp).truthy() ? 1 : 0;
}

extern "C" void arm_call(ArmJitState* state, const char* name, std::int32_t argc) {
  if (!state->call || argc < 0 || static_cast<std::size_t>(argc) > state->sp) {
    state->failed = true;
    state->error = "JIT ARM64: chamada sem callback ou argumentos insuficientes";
    return;
  }
  std::vector<rt::Value> args(static_cast<std::size_t>(argc));
  for (std::int32_t k = argc - 1; k >= 0; --k)
    args[static_cast<std::size_t>(k)] = arm_value(state, --state->sp);
  bool handled = false;
  rt::Value result = (*state->call)(name, args, &handled);
  if (!handled) {
    state->failed = true;
    state->error = "JIT ARM64: funcao desconhecida";
    return;
  }
  arm_store(state, std::move(result));
}

extern "C" void arm_print(ArmJitState* state, std::int32_t argc) {
  if (argc < 1 || static_cast<std::size_t>(argc) > state->sp || argc > 32) {
    state->failed = true;
    state->error = "JIT ARM64: pilha invalida em imprimir";
    return;
  }
  const std::size_t first = state->sp - static_cast<std::size_t>(argc);
  for (std::int32_t k = 0; k < argc; ++k) {
    if (k) *state->out << " ";
    *state->out << rt::to_display(arm_value(state, first + static_cast<std::size_t>(k)));
  }
  *state->out << "\n";
  state->sp = static_cast<std::uint32_t>(first);
  arm_store(state, rt::Value::nulo());
}

extern "C" std::int64_t arm_finish(ArmJitState* state) {
  if (state->sp == 0) {
    state->result_kind = static_cast<std::uint8_t>(rt::ValueKind::Nulo);
    return 0;
  }
  const std::size_t top = state->sp - 1;
  state->result_kind = state->stack_kind[top];
  return state->stack[top];
}

extern "C" std::int64_t arm_finish_nil(ArmJitState* state) {
  state->result_kind = static_cast<std::uint8_t>(rt::ValueKind::Nulo);
  return 0;
}

class ArmExecutableMemory {
 public:
  explicit ArmExecutableMemory(const std::vector<std::uint8_t>& bytes) {
    const long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) throw std::runtime_error("JIT ARM64: tamanho de pagina invalido");
    size_ = (bytes.size() + static_cast<std::size_t>(page) - 1) /
            static_cast<std::size_t>(page) * static_cast<std::size_t>(page);
    ptr_ = mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr_ == MAP_FAILED) {
      ptr_ = nullptr;
      throw std::runtime_error("JIT ARM64: mmap falhou");
    }
    std::memcpy(ptr_, bytes.data(), bytes.size());
    __builtin___clear_cache(static_cast<char*>(ptr_), static_cast<char*>(ptr_) + bytes.size());
    if (mprotect(ptr_, size_, PROT_READ | PROT_EXEC) != 0) {
      munmap(ptr_, size_);
      ptr_ = nullptr;
      throw std::runtime_error("JIT ARM64: mprotect falhou");
    }
  }
  ~ArmExecutableMemory() { if (ptr_) munmap(ptr_, size_); }
  void* data() const { return ptr_; }
 private:
  void* ptr_ = nullptr;
  std::size_t size_ = 0;
};

class ArmEmitter {
 public:
  void u32(std::uint32_t value) {
    code_.push_back(static_cast<std::uint8_t>(value));
    code_.push_back(static_cast<std::uint8_t>(value >> 8));
    code_.push_back(static_cast<std::uint8_t>(value >> 16));
    code_.push_back(static_cast<std::uint8_t>(value >> 24));
  }
  std::size_t pos() const { return code_.size(); }
  void mov_imm(int reg, std::uint64_t value) {
    u32(0xd2800000u | ((value & 0xffffu) << 5) | static_cast<std::uint32_t>(reg));
    for (int shift = 16; shift < 64; shift += 16) {
      const std::uint32_t part = static_cast<std::uint32_t>((value >> shift) & 0xffffu);
      if (part) u32(0xf2800000u | (static_cast<std::uint32_t>(shift / 16) << 21) |
                         (part << 5) | static_cast<std::uint32_t>(reg));
    }
  }
  void call(void* fn, std::initializer_list<std::uint64_t> args = {}) {
    int reg = 0;
    for (std::uint64_t arg : args) mov_imm(reg++, arg);
    mov_imm(16, reinterpret_cast<std::uint64_t>(fn));
    u32(0xd63f0200u);  // blr x16
  }
  void call_state(void* fn, std::initializer_list<std::uint64_t> args = {}) {
    u32(0xaa1303e0u);  // mov x0, x19
    int reg = 1;
    for (std::uint64_t arg : args) mov_imm(reg++, arg);
    mov_imm(16, reinterpret_cast<std::uint64_t>(fn));
    u32(0xd63f0200u);  // blr x16
  }
  void prologue() { u32(0xa9bf7bf3u); u32(0xaa0003f3u); }  // stp x19,x30; mov x19,x0
  void epilogue() { u32(0xa8c17bf3u); u32(0xd65f03c0u); }  // ldp x19,x30; ret
  void branch(std::size_t target_placeholder) {
    patches_.push_back({target_placeholder, pos(), false});
    u32(0x14000000u);
  }
  void branch_false(std::size_t target_placeholder) {
    patches_.push_back({target_placeholder, pos(), true});
    u32(0x34000000u);  // cbz w0, label
  }
  void patch(std::vector<std::size_t>& labels) {
    for (const Patch& p : patches_) {
      if (p.target >= labels.size()) throw std::runtime_error("JIT ARM64: salto fora do chunk");
      const std::int64_t delta = static_cast<std::int64_t>(labels[p.target]) -
                                 static_cast<std::int64_t>(p.at);
      if ((delta & 3) != 0) throw std::runtime_error("JIT ARM64: salto desalinhado");
      const std::int64_t words = delta / 4;
      std::uint32_t ins = read(p.at);
      if (p.conditional) {
        if (words < -(1 << 18) || words >= (1 << 18))
          throw std::runtime_error("JIT ARM64: salto condicional distante demais");
        ins |= (static_cast<std::uint32_t>(words) & 0x7ffffu) << 5;
      } else {
        if (words < -(1 << 25) || words >= (1 << 25))
          throw std::runtime_error("JIT ARM64: salto distante demais");
        ins |= static_cast<std::uint32_t>(words) & 0x3ffffffu;
      }
      write(p.at, ins);
    }
  }
  const std::vector<std::uint8_t>& code() const { return code_; }
 private:
  struct Patch { std::size_t target; std::size_t at; bool conditional; };
  std::uint32_t read(std::size_t at) const {
    return static_cast<std::uint32_t>(code_[at]) |
           (static_cast<std::uint32_t>(code_[at + 1]) << 8) |
           (static_cast<std::uint32_t>(code_[at + 2]) << 16) |
           (static_cast<std::uint32_t>(code_[at + 3]) << 24);
  }
  void write(std::size_t at, std::uint32_t value) {
    for (int k = 0; k < 4; ++k) code_[at + static_cast<std::size_t>(k)] =
        static_cast<std::uint8_t>(value >> (k * 8));
  }
  std::vector<std::uint8_t> code_;
  std::vector<Patch> patches_;
};

#endif  // TILT_JIT_ARM64

}  // namespace

bool Jit::available() {
#if TILT_JIT_X86_64 || TILT_JIT_ARM64
  return true;
#else
  return false;
#endif
}

bool Jit::can_compile(const Chunk& chunk, std::string* reason) const {
#if !TILT_JIT_X86_64 && !TILT_JIT_ARM64
  if (reason) *reason = "backend nativo indisponivel nesta arquitetura";
  (void)chunk;
  return false;
#elif TILT_JIT_X86_64
  auto reject = [reason](std::string why) {
    if (reason) *reason = std::move(why);
    return false;
  };
  if (chunk.num_locals < 0 || static_cast<std::size_t>(chunk.num_locals) > kMaxLocals)
    return reject("quantidade de locais excede o limite nativo");
  if (chunk.code.empty() || chunk.code.size() > 100'000)
    return reject("tamanho do chunk fora do limite nativo");
  for (const auto& v : chunk.consts) {
    if (v.kind != rt::ValueKind::Inteiro && v.kind != rt::ValueKind::Decimal &&
        v.kind != rt::ValueKind::Logico && v.kind != rt::ValueKind::Nulo)
      return reject("JIT requer constantes escalares");
  }
  for (std::size_t ip = 0; ip < chunk.code.size(); ++ip) {
    const Instr& in = chunk.code[ip];
    switch (in.op) {
      case Op::Const:
        if (in.a < 0 || static_cast<std::size_t>(in.a) >= chunk.consts.size())
          return reject("constante fora da faixa");
        break;
      case Op::LoadLocal:
      case Op::StoreLocal:
        if (in.a < 0 || static_cast<std::size_t>(in.a) >= kMaxLocals)
          return reject("local fora da faixa");
        break;
      case Op::Binop:
        if (in.a < 0 || static_cast<std::size_t>(in.a) >= chunk.op_names.size() ||
            !jit_binop_supported(chunk.op_names[static_cast<std::size_t>(in.a)]))
          return reject("operador fora do subconjunto inteiro");
        break;
      case Op::SuperLocalConstBinop:
      case Op::SuperLocalLocalBinop:
      case Op::SuperConstLocalBinop:
        if (in.c < 0 || static_cast<std::size_t>(in.c) >= chunk.op_names.size() ||
            binop_de(chunk.op_names[static_cast<std::size_t>(in.c)]) == BinOp::Generico)
          return reject("superinstrucao com operador invalido");
        if (in.op == Op::SuperLocalConstBinop &&
            (in.a < 0 || static_cast<std::size_t>(in.a) >= kMaxLocals || in.b < 0 ||
             static_cast<std::size_t>(in.b) >= chunk.consts.size()))
          return reject("superinstrucao local/const fora da faixa");
        if (in.op == Op::SuperLocalLocalBinop &&
            (in.a < 0 || in.b < 0 || static_cast<std::size_t>(in.a) >= kMaxLocals ||
             static_cast<std::size_t>(in.b) >= kMaxLocals))
          return reject("superinstrucao local/local fora da faixa");
        if (in.op == Op::SuperConstLocalBinop &&
            (in.a < 0 || static_cast<std::size_t>(in.a) >= chunk.consts.size() || in.b < 0 ||
             static_cast<std::size_t>(in.b) >= kMaxLocals))
          return reject("superinstrucao const/local fora da faixa");
        break;
      case Op::Jump:
      case Op::JumpIfFalse:
        if (in.a < 0 || static_cast<std::size_t>(in.a) >= chunk.code.size())
          return reject("salto fora do chunk");
        break;
      case Op::Print:
        if (in.b < 1 || in.b > 32) return reject("quantidade de argumentos de imprimir invalida");
        break;
      case Op::CallFunc:
        if (in.a < 0 || static_cast<std::size_t>(in.a) >= chunk.names.size() || in.b < 0 ||
            in.b > 32)
          return reject("chamada fora da faixa");
        if (!call_) return reject("chamada exige callback do runtime");
        if (support_ && !support_(chunk.names[static_cast<std::size_t>(in.a)]))
          return reject("chamada fora do subconjunto escalar");
        break;
      case Op::Pop:
      case Op::Neg:
      case Op::Not:
      case Op::Truthy:
      case Op::Return:
      case Op::ReturnNil:
        break;
      default:
        return reject("instrucao fora do subconjunto inteiro nativo");
    }
  }
  return true;
#else
  auto reject = [reason](std::string why) {
    if (reason) *reason = std::move(why);
    return false;
  };
  if (chunk.num_locals < 0 || static_cast<std::size_t>(chunk.num_locals) > kArmMaxLocals)
    return reject("quantidade de locais excede o limite nativo ARM64");
  if (chunk.code.empty() || chunk.code.size() > 100'000)
    return reject("tamanho do chunk fora do limite nativo ARM64");
  for (const auto& v : chunk.consts) {
    if (v.kind != rt::ValueKind::Inteiro && v.kind != rt::ValueKind::Decimal &&
        v.kind != rt::ValueKind::Logico && v.kind != rt::ValueKind::Nulo)
      return reject("JIT ARM64 requer constantes escalares");
  }
  for (std::size_t ip = 0; ip < chunk.code.size(); ++ip) {
    const Instr& in = chunk.code[ip];
    switch (in.op) {
      case Op::Const:
        if (in.a < 0 || static_cast<std::size_t>(in.a) >= chunk.consts.size())
          return reject("constante fora da faixa");
        break;
      case Op::LoadLocal:
      case Op::StoreLocal:
        if (in.a < 0 || static_cast<std::size_t>(in.a) >= kArmMaxLocals)
          return reject("local fora da faixa");
        break;
      case Op::Binop:
        if (in.a < 0 || static_cast<std::size_t>(in.a) >= chunk.op_names.size() ||
            binop_de(chunk.op_names[static_cast<std::size_t>(in.a)]) == BinOp::Generico)
          return reject("operador fora do subconjunto escalar ARM64");
        break;
      case Op::SuperLocalConstBinop:
      case Op::SuperLocalLocalBinop:
      case Op::SuperConstLocalBinop:
        if (in.c < 0 || static_cast<std::size_t>(in.c) >= chunk.op_names.size() ||
            binop_de(chunk.op_names[static_cast<std::size_t>(in.c)]) == BinOp::Generico)
          return reject("superinstrucao com operador invalido");
        if (in.op == Op::SuperLocalConstBinop &&
            (in.a < 0 || static_cast<std::size_t>(in.a) >= kArmMaxLocals || in.b < 0 ||
             static_cast<std::size_t>(in.b) >= chunk.consts.size()))
          return reject("superinstrucao local/const fora da faixa");
        if (in.op == Op::SuperLocalLocalBinop &&
            (in.a < 0 || in.b < 0 || static_cast<std::size_t>(in.a) >= kArmMaxLocals ||
             static_cast<std::size_t>(in.b) >= kArmMaxLocals))
          return reject("superinstrucao local/local fora da faixa");
        if (in.op == Op::SuperConstLocalBinop &&
            (in.a < 0 || static_cast<std::size_t>(in.a) >= chunk.consts.size() || in.b < 0 ||
             static_cast<std::size_t>(in.b) >= kArmMaxLocals))
          return reject("superinstrucao const/local fora da faixa");
        break;
      case Op::Jump:
      case Op::JumpIfFalse:
        if (in.a < 0 || static_cast<std::size_t>(in.a) >= chunk.code.size())
          return reject("salto fora do chunk");
        break;
      case Op::CallFunc:
        if (in.a < 0 || static_cast<std::size_t>(in.a) >= chunk.names.size() || in.b < 0 ||
            in.b > 32)
          return reject("chamada fora da faixa");
        if (!call_) return reject("chamada exige callback do runtime");
        if (support_ && !support_(chunk.names[static_cast<std::size_t>(in.a)]))
          return reject("chamada fora do subconjunto escalar ARM64");
        break;
      case Op::Print:
        if (in.b < 1 || in.b > 32) return reject("quantidade de argumentos invalida");
        break;
      case Op::Pop:
      case Op::Neg:
      case Op::Not:
      case Op::Truthy:
      case Op::Return:
      case Op::ReturnNil:
        break;
      default:
        return reject("instrucao fora do subconjunto escalar ARM64");
    }
  }
  return true;
#endif
}

rt::Value Jit::run(const Chunk& chunk, std::vector<rt::Value> args) const {
#if !TILT_JIT_X86_64 && !TILT_JIT_ARM64
  (void)chunk;
  (void)args;
  throw std::runtime_error("JIT: backend nativo indisponivel nesta arquitetura");
#elif TILT_JIT_X86_64
  std::string why;
  if (!can_compile(chunk, &why)) throw std::runtime_error("JIT: " + why);

  Emitter e;
  bool decimal_mode = false;
  for (const auto& value : chunk.consts)
    decimal_mode = decimal_mode || value.kind == rt::ValueKind::Decimal;
  for (const auto& value : args)
    decimal_mode = decimal_mode || value.kind == rt::ValueKind::Decimal;
  e.prologue();
  std::vector<std::size_t> labels(chunk.code.size());
  struct Patch {
    std::size_t at;
    std::size_t target;
  };
  std::vector<Patch> patches;

  for (std::size_t ip = 0; ip < chunk.code.size(); ++ip) {
    labels[ip] = e.pos();
    const Instr& in = chunk.code[ip];
    switch (in.op) {
      case Op::Const:
        {
          const rt::Value& value = chunk.consts[static_cast<std::size_t>(in.a)];
          std::int64_t bits = 0;
          if (value.kind == rt::ValueKind::Decimal) {
            std::uint64_t raw = 0;
            std::memcpy(&raw, &value.d, sizeof raw);
            bits = static_cast<std::int64_t>(raw);
          } else if (value.kind == rt::ValueKind::Logico) {
            bits = value.b ? 1 : 0;
          } else if (value.kind == rt::ValueKind::Inteiro) {
            bits = value.i;
          }
          e.mov_rax_imm64(bits);
          e.push_rax();
          e.push_kind_imm(static_cast<std::uint8_t>(value.kind));
        }
        break;
      case Op::LoadLocal:
        e.load_local_rax(in.a);
        e.push_rax();
        e.load_local_kind_al(in.a);
        e.push_kind_al();
        break;
      case Op::StoreLocal:
        e.pop_rax();
        e.store_local_rax(in.a);
        e.load_stack_kind_al_ecx();
        e.store_local_kind_al(in.a);
        break;
      case Op::Pop:
        e.pop_rax();
        break;
      case Op::Neg:
        if (decimal_mode) {
          e.call_neg();
        } else {
          e.pop_rax();
          e.u8(0x48);
          e.u8(0xf7);
          e.u8(0xd8);  // neg rax
          e.push_rax();
          e.push_kind_imm(static_cast<std::uint8_t>(rt::ValueKind::Inteiro));
        }
        break;
      case Op::Not:
      case Op::Truthy:
        if (decimal_mode) {
          e.call_unary_truthy(in.op == Op::Not);
        } else {
          e.pop_rax();
          e.u8(0x48);
          e.u8(0x85);
          e.u8(0xc0);  // test rax, rax
          e.u8(0x0f);
          e.u8(in.op == Op::Not ? 0x94 : 0x95);  // sete / setne
          e.u8(0xc0);
          e.u8(0x48);
          e.u8(0x0f);
          e.u8(0xb6);
          e.u8(0xc0);
          e.push_rax();
          e.push_kind_imm(static_cast<std::uint8_t>(rt::ValueKind::Logico));
        }
        break;
      case Op::Binop: {
        const std::string& op = chunk.op_names[static_cast<std::size_t>(in.a)];
        if (decimal_mode || op == "/" || op == "%") {
          e.call_binop(in.b);
          break;
        }
        e.pop_rax();
        e.u8(0x48);
        e.u8(0x89);
        e.u8(0xc2);  // mov rdx, rax (b)
        e.pop_rax();
        if (op == "+")
          e.add_rax_rdx();
        else if (op == "-")
          e.sub_rax_rdx();
        else if (op == "*")
          e.imul_rax_rdx();
        else if (op == "==")
          e.cmp_to_bool(0x94);
        else if (op == "!=")
          e.cmp_to_bool(0x95);
        else if (op == "<")
          e.cmp_to_bool(0x9c);
        else if (op == "<=")
          e.cmp_to_bool(0x9e);
        else if (op == ">")
          e.cmp_to_bool(0x9f);
        else if (op == ">=")
          e.cmp_to_bool(0x9d);
        e.push_rax();
        const bool comparison =
            op == "==" || op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">=";
        e.push_kind_imm(
            static_cast<std::uint8_t>(comparison ? rt::ValueKind::Logico : rt::ValueKind::Inteiro));
        break;
      }
      case Op::SuperLocalConstBinop:
      case Op::SuperLocalLocalBinop:
      case Op::SuperConstLocalBinop: {
        if (in.op == Op::SuperLocalConstBinop || in.op == Op::SuperLocalLocalBinop) {
          e.load_local_rax(in.a);
          e.push_rax();
          e.load_local_kind_al(in.a);
          e.push_kind_al();
        }
        if (in.op == Op::SuperLocalConstBinop || in.op == Op::SuperConstLocalBinop) {
          const rt::Value& value = chunk.consts[static_cast<std::size_t>(in.op == Op::SuperLocalConstBinop ? in.b : in.a)];
          std::int64_t bits = 0;
          if (value.kind == rt::ValueKind::Decimal) {
            std::uint64_t raw = 0;
            std::memcpy(&raw, &value.d, sizeof raw);
            bits = static_cast<std::int64_t>(raw);
          } else if (value.kind == rt::ValueKind::Logico) bits = value.b ? 1 : 0;
          else if (value.kind == rt::ValueKind::Inteiro) bits = value.i;
          e.mov_rax_imm64(bits);
          e.push_rax();
          e.push_kind_imm(static_cast<std::uint8_t>(value.kind));
        } else {
          e.load_local_rax(in.b);
          e.push_rax();
          e.load_local_kind_al(in.b);
          e.push_kind_al();
        }
        e.call_binop(static_cast<std::int32_t>(binop_de(chunk.op_names[static_cast<std::size_t>(in.c)])));
        break;
      }
      case Op::Jump:
        patches.push_back({e.jmp(), static_cast<std::size_t>(in.a)});
        break;
      case Op::JumpIfFalse:
        if (decimal_mode) {
          e.call_pop_truthy();
          e.test_eax();
        } else {
          e.pop_rax();
          e.u8(0x48);
          e.u8(0x85);
          e.u8(0xc0);  // test rax, rax
        }
        patches.push_back({e.jz_rax(), static_cast<std::size_t>(in.a)});
        break;
      case Op::CallFunc:
        e.call_func(chunk.names[static_cast<std::size_t>(in.a)].c_str(), in.b);
        break;
      case Op::Print:
        e.call_print(in.b);
        break;
      case Op::Return:
        e.pop_rax();
        e.load_stack_kind_al_ecx();
        e.store_result_kind_al();
        e.load_stack_rax_ecx();
        e.epilogue();
        break;
      case Op::ReturnNil:
        e.u8(0xb0);
        e.u8(static_cast<std::uint8_t>(rt::ValueKind::Nulo));
        e.store_result_kind_al();
        e.u8(0x48);
        e.u8(0x31);
        e.u8(0xc0);  // xor rax, rax
        e.epilogue();
        break;
      default:
        throw std::runtime_error("JIT: instrucao inesperada");
    }
  }
  e.u8(0x48);
  e.u8(0x31);
  e.u8(0xc0);
  e.epilogue();
  for (const Patch& p : patches) e.patch_rel32(p.at, labels[p.target]);

  ExecutableMemory memory(e.code());
  JitState state;
  state.out = &out_;
  state.decimal_mode = decimal_mode;
  state.call = call_ ? &call_ : nullptr;
  for (std::size_t k = 0; k < args.size() && k < kMaxLocals; ++k) {
    if (args[k].kind != rt::ValueKind::Inteiro && args[k].kind != rt::ValueKind::Decimal &&
        args[k].kind != rt::ValueKind::Logico)
      throw std::runtime_error("JIT: argumento deve ser escalar");
    if (args[k].kind == rt::ValueKind::Decimal) {
      std::uint64_t bits = 0;
      std::memcpy(&bits, &args[k].d, sizeof bits);
      state.locals[k] = static_cast<std::int64_t>(bits);
    } else {
      state.locals[k] = args[k].kind == rt::ValueKind::Logico ? (args[k].b ? 1 : 0) : args[k].i;
    }
    state.local_kind[k] = static_cast<std::uint8_t>(args[k].kind);
  }
  const auto result = reinterpret_cast<Entry>(memory.data())(&state);
  if (state.failed) throw std::runtime_error(state.error ? state.error : "JIT: falha nativa");
  const auto kind = static_cast<rt::ValueKind>(state.result_kind);
  if (kind == rt::ValueKind::Logico) return rt::Value::logico(result != 0);
  if (kind == rt::ValueKind::Inteiro) return rt::Value::inteiro(result);
  if (kind == rt::ValueKind::Decimal) {
    double d = 0.0;
    const std::uint64_t bits = static_cast<std::uint64_t>(result);
    std::memcpy(&d, &bits, sizeof d);
    return rt::Value::decimal(d);
  }
  return rt::Value::nulo();
#else
  std::string why;
  if (!can_compile(chunk, &why)) throw std::runtime_error("JIT ARM64: " + why);

  ArmEmitter e;
  e.prologue();
  std::vector<std::size_t> labels(chunk.code.size());
  for (std::size_t ip = 0; ip < chunk.code.size(); ++ip) {
    labels[ip] = e.pos();
    const Instr& in = chunk.code[ip];
    switch (in.op) {
      case Op::Const: {
        const rt::Value& value = chunk.consts[static_cast<std::size_t>(in.a)];
        std::uint64_t payload = 0;
        if (value.kind == rt::ValueKind::Inteiro) payload = static_cast<std::uint64_t>(value.i);
        else if (value.kind == rt::ValueKind::Logico) payload = value.b ? 1 : 0;
        else if (value.kind == rt::ValueKind::Decimal)
          std::memcpy(&payload, &value.d, sizeof payload);
        e.call_state(reinterpret_cast<void*>(&arm_push),
                     {payload, static_cast<std::uint8_t>(value.kind)});
        break;
      }
      case Op::LoadLocal:
        e.call_state(reinterpret_cast<void*>(&arm_load_local), {static_cast<std::uint64_t>(in.a)});
        break;
      case Op::StoreLocal:
        e.call_state(reinterpret_cast<void*>(&arm_store_local), {static_cast<std::uint64_t>(in.a)});
        break;
      case Op::Pop:
        e.call_state(reinterpret_cast<void*>(&arm_pop));
        break;
      case Op::Neg:
        e.call_state(reinterpret_cast<void*>(&arm_neg));
        break;
      case Op::Not:
        e.call_state(reinterpret_cast<void*>(&arm_truthy), {1});
        break;
      case Op::Truthy:
        e.call_state(reinterpret_cast<void*>(&arm_truthy), {0});
        break;
      case Op::Binop:
        e.call_state(reinterpret_cast<void*>(&arm_binop),
                     {static_cast<std::uint64_t>(binop_de(chunk.op_names[static_cast<std::size_t>(in.a)]))});
        break;
      case Op::SuperLocalConstBinop:
        e.call_state(reinterpret_cast<void*>(&arm_load_local), {static_cast<std::uint64_t>(in.a)});
        {
          const rt::Value& value = chunk.consts[static_cast<std::size_t>(in.b)];
          std::uint64_t payload = 0;
          if (value.kind == rt::ValueKind::Inteiro) payload = static_cast<std::uint64_t>(value.i);
          else if (value.kind == rt::ValueKind::Logico) payload = value.b ? 1 : 0;
          else if (value.kind == rt::ValueKind::Decimal)
            std::memcpy(&payload, &value.d, sizeof payload);
          e.call_state(reinterpret_cast<void*>(&arm_push),
                       {payload, static_cast<std::uint8_t>(value.kind)});
        }
        e.call_state(reinterpret_cast<void*>(&arm_binop),
                     {static_cast<std::uint64_t>(binop_de(chunk.op_names[static_cast<std::size_t>(in.c)]))});
        break;
      case Op::SuperLocalLocalBinop:
        e.call_state(reinterpret_cast<void*>(&arm_load_local), {static_cast<std::uint64_t>(in.a)});
        e.call_state(reinterpret_cast<void*>(&arm_load_local), {static_cast<std::uint64_t>(in.b)});
        e.call_state(reinterpret_cast<void*>(&arm_binop),
                     {static_cast<std::uint64_t>(binop_de(chunk.op_names[static_cast<std::size_t>(in.c)]))});
        break;
      case Op::SuperConstLocalBinop: {
        const rt::Value& value = chunk.consts[static_cast<std::size_t>(in.a)];
        std::uint64_t payload = 0;
        if (value.kind == rt::ValueKind::Inteiro) payload = static_cast<std::uint64_t>(value.i);
        else if (value.kind == rt::ValueKind::Logico) payload = value.b ? 1 : 0;
        else if (value.kind == rt::ValueKind::Decimal)
          std::memcpy(&payload, &value.d, sizeof payload);
        e.call_state(reinterpret_cast<void*>(&arm_push),
                     {payload, static_cast<std::uint8_t>(value.kind)});
        e.call_state(reinterpret_cast<void*>(&arm_load_local), {static_cast<std::uint64_t>(in.b)});
        e.call_state(reinterpret_cast<void*>(&arm_binop),
                     {static_cast<std::uint64_t>(binop_de(chunk.op_names[static_cast<std::size_t>(in.c)]))});
        break;
      }
      case Op::Jump:
        e.branch(static_cast<std::size_t>(in.a));
        break;
      case Op::JumpIfFalse:
        e.call_state(reinterpret_cast<void*>(&arm_pop_truthy));
        e.branch_false(static_cast<std::size_t>(in.a));
        break;
      case Op::CallFunc:
        e.call_state(reinterpret_cast<void*>(&arm_call),
                     {reinterpret_cast<std::uint64_t>(chunk.names[static_cast<std::size_t>(in.a)].c_str()),
                      static_cast<std::uint64_t>(in.b)});
        break;
      case Op::Print:
        e.call_state(reinterpret_cast<void*>(&arm_print), {static_cast<std::uint64_t>(in.b)});
        break;
      case Op::Return:
        e.call_state(reinterpret_cast<void*>(&arm_finish));
        e.epilogue();
        break;
      case Op::ReturnNil:
        e.call_state(reinterpret_cast<void*>(&arm_finish_nil));
        e.epilogue();
        break;
      default:
        throw std::runtime_error("JIT ARM64: instrucao inesperada");
    }
  }
  e.call_state(reinterpret_cast<void*>(&arm_finish_nil));
  e.epilogue();
  e.patch(labels);

  ArmExecutableMemory memory(e.code());
  ArmJitState state;
  state.out = &out_;
  state.call = call_ ? &call_ : nullptr;
  for (std::size_t k = 0; k < args.size() && k < kArmMaxLocals; ++k) {
    if (args[k].kind != rt::ValueKind::Inteiro && args[k].kind != rt::ValueKind::Decimal &&
        args[k].kind != rt::ValueKind::Logico)
      throw std::runtime_error("JIT ARM64: argumento deve ser escalar");
    if (args[k].kind == rt::ValueKind::Decimal)
      std::memcpy(&state.locals[k], &args[k].d, sizeof args[k].d);
    else
      state.locals[k] = args[k].kind == rt::ValueKind::Logico ? (args[k].b ? 1 : 0) : args[k].i;
    state.local_kind[k] = static_cast<std::uint8_t>(args[k].kind);
  }
  using ArmEntry = std::int64_t (*)(ArmJitState*);
  const std::int64_t result = reinterpret_cast<ArmEntry>(memory.data())(&state);
  if (state.failed) throw std::runtime_error(state.error ? state.error : "JIT ARM64: falha nativa");
  const auto kind = static_cast<rt::ValueKind>(state.result_kind);
  if (kind == rt::ValueKind::Logico) return rt::Value::logico(result != 0);
  if (kind == rt::ValueKind::Inteiro) return rt::Value::inteiro(result);
  if (kind == rt::ValueKind::Decimal) {
    double d = 0.0;
    const std::uint64_t bits = static_cast<std::uint64_t>(result);
    std::memcpy(&d, &bits, sizeof d);
    return rt::Value::decimal(d);
  }
  return rt::Value::nulo();
#endif
}

}  // namespace tilt::vm
