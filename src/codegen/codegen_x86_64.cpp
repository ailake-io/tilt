#include "codegen/codegen_x86_64.hpp"

#include <cctype>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "vm/bytecode.hpp"
#include "vm/compiler.hpp"

namespace tilt::codegen {

using ast::Item;
using ast::ItemKind;
using vm::Chunk;
using vm::Op;

namespace {

// Eval-stack slots are 16 bytes so %rsp stays 16-aligned for `call`.
constexpr int kSlot = 16;

const char* arg_reg(int i) {
  static const char* regs[] = {"%rdi", "%rsi", "%rdx", "%rcx", "%r8", "%r9"};
  return regs[i];
}

std::string sanitize(const std::string& name) {
  std::string s;
  for (char c : name) s += (std::isalnum(static_cast<unsigned char>(c)) || c == '_') ? c : '_';
  return s;
}

struct Emitter {
  std::ostringstream os;
  std::string err;

  bool chunk_is_integer(const Chunk& c, const std::unordered_set<std::string>& pure) {
    for (const auto& v : c.consts) {
      if (v.kind != rt::ValueKind::Inteiro && v.kind != rt::ValueKind::Logico &&
          v.kind != rt::ValueKind::Nulo) {
        err = "constante nao inteira no corpo";
        return false;
      }
    }
    for (const auto& name : c.op_names) {
      static const std::unordered_set<std::string> allowed = {
          "+", "-", "*", "/", "%", "==", "!=", "<", "<=", ">", ">="};
      if (!allowed.count(name)) {
        err = "operador '" + name + "' nao suportado no codegen nativo";
        return false;
      }
    }
    for (const auto& in : c.code) {
      if (in.op == Op::Len) {
        err = "'tamanho' nao suportado no codegen nativo";
        return false;
      }
    }
    for (const auto& n : c.names) {
      if (!pure.count(n)) {
        err = "chamada nativa a '" + n + "' (fora do subconjunto inteiro)";
        return false;
      }
    }
    return true;
  }

  static long const_int(const rt::Value& v) {
    if (v.kind == rt::ValueKind::Inteiro) return static_cast<long>(v.i);
    if (v.kind == rt::ValueKind::Logico) return v.b ? 1 : 0;
    return 0;
  }

  void push_rax() { os << "  sub $16, %rsp\n  mov %rax, (%rsp)\n"; }
  void pop_rax() { os << "  mov (%rsp), %rax\n  add $16, %rsp\n"; }

  void emit_func(const std::string& name, int nparams, const Chunk& c) {
    const std::string sym = "tilt_fn_" + sanitize(name);
    const int frame = ((c.num_locals * kSlot) + 15) & ~15;

    os << "  .globl " << sym << "\n" << sym << ":\n";
    os << "  push %rbp\n  mov %rsp, %rbp\n";
    if (frame) os << "  sub $" << frame << ", %rsp\n";
    for (int p = 0; p < nparams && p < 6; ++p) {
      os << "  mov " << arg_reg(p) << ", " << (-(p + 1) * kSlot) << "(%rbp)\n";
    }

    auto local = [&](int slot) { return std::to_string(-(slot + 1) * kSlot) + "(%rbp)"; };
    auto lbl = [&](int ip) { return ".L" + sym + "_" + std::to_string(ip); };

    for (std::size_t ip = 0; ip < c.code.size(); ++ip) {
      os << lbl(static_cast<int>(ip)) << ":\n";
      const vm::Instr& in = c.code[ip];
      switch (in.op) {
        case Op::Const:
          os << "  mov $" << const_int(c.consts[static_cast<std::size_t>(in.a)]) << ", %rax\n";
          push_rax();
          break;
        case Op::LoadLocal:
          os << "  mov " << local(in.a) << ", %rax\n";
          push_rax();
          break;
        case Op::StoreLocal:
          pop_rax();
          os << "  mov %rax, " << local(in.a) << "\n";
          break;
        case Op::Pop:
          os << "  add $16, %rsp\n";
          break;
        case Op::Neg:
          pop_rax();
          os << "  neg %rax\n";
          push_rax();
          break;
        case Op::Not:
          pop_rax();
          os << "  test %rax, %rax\n  sete %al\n  movzbq %al, %rax\n";
          push_rax();
          break;
        case Op::Truthy:
          pop_rax();
          os << "  test %rax, %rax\n  setne %al\n  movzbq %al, %rax\n";
          push_rax();
          break;
        case Op::And:
        case Op::Or: {
          os << "  mov (%rsp), %rbx\n  add $16, %rsp\n";  // b
          pop_rax();                                       // a
          os << "  test %rax, %rax\n  setne %al\n  test %rbx, %rbx\n  setne %bl\n";
          os << (in.op == Op::And ? "  and %bl, %al\n" : "  or %bl, %al\n");
          os << "  movzbq %al, %rax\n";
          push_rax();
          break;
        }
        case Op::Binop: {
          const std::string& op = c.op_names[static_cast<std::size_t>(in.a)];
          os << "  mov (%rsp), %rbx\n  add $16, %rsp\n";  // rhs -> rbx
          pop_rax();                                       // lhs -> rax
          if (op == "+") os << "  add %rbx, %rax\n";
          else if (op == "-") os << "  sub %rbx, %rax\n";
          else if (op == "*") os << "  imul %rbx, %rax\n";
          else if (op == "/" || op == "%") {
            os << "  test %rbx, %rbx\n  jz 1f\n  cqto\n  idiv %rbx\n";
            if (op == "%") os << "  mov %rdx, %rax\n";
            os << "  jmp 2f\n1:\n  xor %rax, %rax\n2:\n";
          } else {
            const char* set = op == "==" ? "sete"
                              : op == "!=" ? "setne"
                              : op == "<" ? "setl"
                              : op == "<=" ? "setle"
                              : op == ">" ? "setg"
                                          : "setge";
            os << "  cmp %rbx, %rax\n  " << set << " %al\n  movzbq %al, %rax\n";
          }
          push_rax();
          break;
        }
        case Op::Jump:
          os << "  jmp " << lbl(in.a) << "\n";
          break;
        case Op::JumpIfFalse:
          pop_rax();
          os << "  test %rax, %rax\n  jz " << lbl(in.a) << "\n";
          break;
        case Op::CallFunc: {
          const int argc = in.b;
          // Stack top holds the last argument; pop into registers in reverse.
          for (int k = argc - 1; k >= 0 && k < 6; --k) {
            os << "  mov (%rsp), " << arg_reg(k) << "\n  add $16, %rsp\n";
          }
          os << "  call tilt_fn_" << sanitize(c.names[static_cast<std::size_t>(in.a)]) << "\n";
          push_rax();
          break;
        }
        case Op::Print: {
          const int argc = in.b;
          os << "  mov %rsp, %rdi\n  mov $" << argc << ", %rsi\n  call tilt_print_row\n";
          if (argc) os << "  add $" << (argc * kSlot) << ", %rsp\n";
          os << "  mov $0, %rax\n";
          push_rax();
          break;
        }
        case Op::Return:
          pop_rax();
          os << "  mov %rbp, %rsp\n  pop %rbp\n  ret\n";
          break;
        case Op::ReturnNil:
          os << "  xor %rax, %rax\n  mov %rbp, %rsp\n  pop %rbp\n  ret\n";
          break;
        case Op::Len:
          break;  // rejected earlier by chunk_is_integer
      }
    }
    os << lbl(static_cast<int>(c.code.size())) << ":\n";
    os << "  xor %rax, %rax\n  mov %rbp, %rsp\n  pop %rbp\n  ret\n\n";
  }
};

}  // namespace

Result emit_program(const ast::Program& program) {
  std::unordered_map<std::string, const Item*> funcs;
  std::unordered_set<std::string> names;
  for (const auto& it : program.items) {
    if (it && it->kind == ItemKind::Decl && it->key == "funcao" && !it->header.empty() &&
        it->header[0]->kind == ast::ExprKind::Name) {
      funcs[it->header[0]->text] = it.get();
      names.insert(it->header[0]->text);
    }
  }
  if (!funcs.count("principal")) {
    return {false, "", "codegen nativo (M12) exige uma 'funcao principal'"};
  }

  Emitter e;
  e.os << "  .text\n";
  for (const auto& [name, decl] : funcs) {
    Chunk chunk;
    try {
      chunk = vm::compile_function(*decl, names);
    } catch (const vm::NotCompilable& nc) {
      return {false, "", "funcao '" + name + "': " + nc.reason};
    }
    if (!e.chunk_is_integer(chunk, names)) {
      return {false, "", "funcao '" + name + "': " + e.err};
    }
    e.emit_func(name, static_cast<int>(decl->params.size()), chunk);
  }

  e.os << "  .globl main\nmain:\n  push %rbp\n  mov %rsp, %rbp\n";
  e.os << "  call tilt_fn_principal\n  xor %eax, %eax\n  mov %rbp, %rsp\n  pop %rbp\n  ret\n";
  e.os << "  .section .note.GNU-stack,\"\",@progbits\n";
  return {true, e.os.str(), ""};
}

const char* runtime_source() {
  return "#include <stdio.h>\n"
         "void tilt_print_row(char* p, long n) {\n"
         "  for (long i = n - 1; i >= 0; --i) {\n"
         "    if (i != n - 1) putchar(' ');\n"
         "    printf(\"%ld\", *(long*)(p + 16 * i));\n"
         "  }\n"
         "  putchar('\\n');\n"
         "}\n";
}

}  // namespace tilt::codegen
