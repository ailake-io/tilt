#include "vm/jit.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
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
  bool failed = false;
  const char* error = nullptr;
  std::ostream* out = nullptr;
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
    const rt::Value value = kind == rt::ValueKind::Logico
                                ? rt::Value::logico(state->stack[index] != 0)
                                : rt::Value::inteiro(state->stack[index]);
    *state->out << rt::to_display(value);
  }
  *state->out << "\n";
  state->sp = static_cast<std::uint32_t>(first);
  state->stack[state->sp] = 0;
  state->stack_kind[state->sp++] = static_cast<std::uint8_t>(rt::ValueKind::Nulo);
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

  const std::vector<std::uint8_t>& code() const { return code_; }

 private:
  std::vector<std::uint8_t> code_;
};

using Entry = std::int64_t (*)(JitState*);

bool integer_binop(const std::string& op) {
  static const std::set<std::string> ops = {"+", "-", "*", "==", "!=", "<", "<=", ">", ">="};
  return ops.count(op) != 0;
}

#endif  // TILT_JIT_X86_64

}  // namespace

bool Jit::available() {
#if TILT_JIT_X86_64
  return true;
#else
  return false;
#endif
}

bool Jit::can_compile(const Chunk& chunk, std::string* reason) const {
#if !TILT_JIT_X86_64
  if (reason) *reason = "backend nativo indisponivel nesta arquitetura";
  (void)chunk;
  return false;
#else
  auto reject = [reason](std::string why) {
    if (reason) *reason = std::move(why);
    return false;
  };
  if (chunk.num_locals < 0 || static_cast<std::size_t>(chunk.num_locals) > kMaxLocals)
    return reject("quantidade de locais excede o limite nativo");
  if (chunk.code.empty() || chunk.code.size() > 100'000)
    return reject("tamanho do chunk fora do limite nativo");
  for (const auto& v : chunk.consts) {
    if (v.kind != rt::ValueKind::Inteiro) return reject("JIT inteiro requer constantes inteiras");
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
            !integer_binop(chunk.op_names[static_cast<std::size_t>(in.a)]))
          return reject("operador fora do subconjunto inteiro");
        break;
      case Op::Jump:
      case Op::JumpIfFalse:
        if (in.a < 0 || static_cast<std::size_t>(in.a) >= chunk.code.size())
          return reject("salto fora do chunk");
        break;
      case Op::Print:
        if (in.b < 1 || in.b > 32) return reject("quantidade de argumentos de imprimir invalida");
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
#endif
}

rt::Value Jit::run(const Chunk& chunk, std::vector<rt::Value> args) const {
#if !TILT_JIT_X86_64
  (void)chunk;
  (void)args;
  throw std::runtime_error("JIT: backend nativo indisponivel nesta arquitetura");
#else
  std::string why;
  if (!can_compile(chunk, &why)) throw std::runtime_error("JIT: " + why);

  Emitter e;
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
        e.mov_rax_imm64(chunk.consts[static_cast<std::size_t>(in.a)].i);
        e.push_rax();
        e.push_kind_imm(static_cast<std::uint8_t>(rt::ValueKind::Inteiro));
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
        e.pop_rax();
        e.u8(0x48);
        e.u8(0xf7);
        e.u8(0xd8);  // neg rax
        e.push_rax();
        e.push_kind_imm(static_cast<std::uint8_t>(rt::ValueKind::Inteiro));
        break;
      case Op::Not:
      case Op::Truthy:
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
        break;
      case Op::Binop: {
        e.pop_rax();
        e.u8(0x48);
        e.u8(0x89);
        e.u8(0xc2);  // mov rdx, rax (b)
        e.pop_rax();
        const std::string& op = chunk.op_names[static_cast<std::size_t>(in.a)];
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
      case Op::Jump:
        patches.push_back({e.jmp(), static_cast<std::size_t>(in.a)});
        break;
      case Op::JumpIfFalse:
        e.pop_rax();
        e.u8(0x48);
        e.u8(0x85);
        e.u8(0xc0);  // test rax, rax
        patches.push_back({e.jz_rax(), static_cast<std::size_t>(in.a)});
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
  for (std::size_t k = 0; k < args.size() && k < kMaxLocals; ++k) {
    if (args[k].kind != rt::ValueKind::Inteiro)
      throw std::runtime_error("JIT: argumento nao inteiro");
    state.locals[k] = args[k].i;
    state.local_kind[k] = static_cast<std::uint8_t>(rt::ValueKind::Inteiro);
  }
  const auto result = reinterpret_cast<Entry>(memory.data())(&state);
  if (state.failed) throw std::runtime_error(state.error ? state.error : "JIT: falha nativa");
  const auto kind = static_cast<rt::ValueKind>(state.result_kind);
  if (kind == rt::ValueKind::Logico) return rt::Value::logico(result != 0);
  if (kind == rt::ValueKind::Inteiro) return rt::Value::inteiro(result);
  return rt::Value::nulo();
#endif
}

}  // namespace tilt::vm
