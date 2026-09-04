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

// Slots da pilha de avaliacao em bytes. O TV tem 40 bytes; 48 mantem %rsp
// alinhado a 16 para `call` (o rbp ja esta alinhado apos o prologo).
constexpr int kSlot = 48;
constexpr int kFields = 6;  // qwords copiados por slot (40 bytes uteis)

std::string sanitize(const std::string& name) {
  std::string s;
  for (char c : name) s += (std::isalnum(static_cast<unsigned char>(c)) || c == '_') ? c : '_';
  return s;
}

// ids dos operadores binarios conhecidos pelo runtime C (tv_binop)
int binop_id(const std::string& op) {
  static const char* const kOps[] = {"+", "-", "*", "/", "%", "==", "!=",
                                     "<", "<=", ">",  ">=", "contem"};
  for (int i = 0; i < 12; ++i) {
    if (op == kOps[i]) return i;
  }
  return -1;
}

struct Emitter {
  std::ostringstream rodata;
  std::ostringstream os;
  int next_chunk = 0;

  // Verifica se o chunk usa apenas o que o runtime C implementa.
  bool chunk_supported(const Chunk& c, std::string* err) {
    for (const auto& v : c.consts) {
      switch (v.kind) {
        case rt::ValueKind::Nulo:
        case rt::ValueKind::Logico:
        case rt::ValueKind::Inteiro:
        case rt::ValueKind::Decimal:
        case rt::ValueKind::Texto:
          break;
        default:
          *err = "constante do tipo '" + std::string(v.type_name()) + "' fora do subconjunto nativo";
          return false;
      }
    }
    for (const auto& name : c.op_names) {
      if (binop_id(name) < 0) {
        *err = "operador '" + name + "' nao suportado no codegen nativo";
        return false;
      }
    }
    return true;
  }

  // Emite o literal de texto/decimal no rodata e devolve o rotulo.
  std::string const_label(int chunk_id, int const_idx, const rt::Value& v) {
    const std::string lbl = ".LCc" + std::to_string(chunk_id) + "_" + std::to_string(const_idx);
    if (v.kind == rt::ValueKind::Texto) {
      rodata << lbl << ":\n  .byte ";
      for (std::size_t k = 0; k < v.s.size(); ++k) {
        if (k) rodata << ',';
        rodata << static_cast<int>(static_cast<unsigned char>(v.s[k]));
      }
      if (v.s.empty()) rodata << "0";
      rodata << (v.s.empty() ? "\n" : ",0\n");
    } else {
      std::ostringstream bits;
      bits.precision(17);
      bits << v.d;
      rodata << lbl << ":\n  .double " << bits.str() << "\n";
    }
    return lbl;
  }

  // Copia os 6 qwords de um slot: [off+base] <- [off+base].
  void copy_slot(std::ostream& o, const std::string& dst_base, int dst_off,
                 const std::string& src_base, int src_off) {
    for (int f = 0; f < kFields; ++f) {
      o << "  mov " << (src_off + f * 8) << "(" << src_base << "), %rax\n";
      o << "  mov %rax, " << (dst_off + f * 8) << "(" << dst_base << ")\n";
    }
  }

  void push_const_call(const char* fn) {  // sub slot; rdi=slot; call fn
    os << "  sub $" << kSlot << ", %rsp\n  mov %rsp, %rdi\n  call " << fn << "\n";
  }

  void push_nulo() { push_const_call("tv_nulo"); }

  // Emite um chunk como funcao `void sym(TV* out, TV* args)`.
  void emit_chunk(const std::string& sym, int nparams, const Chunk& c,
                  const std::unordered_map<int, std::string>& labels) {
    const int frame = ((c.num_locals * kSlot) + 15) & ~15;

    os << "  .globl " << sym << "\n" << sym << ":\n";
    os << "  push %rbp\n  mov %rsp, %rbp\n  push %rbx\n  push %r12\n";
    os << "  mov %rdi, %rbx\n  mov %rsi, %r12\n";  // out, args (registradores)
    if (frame) os << "  sub $" << frame << ", %rsp\n";
    if (nparams > 0) {
      // args = &arg0 (slot mais alto); arg_p fica em -p*kSlot(%r12).
      for (int p = 0; p < nparams; ++p) {
        for (int f = 0; f < kFields; ++f) {
          os << "  mov " << (-(p * kSlot) + f * 8) << "(%r12), %rax\n  mov %rax, "
             << (-16 - ((p + 1) * kSlot) + f * 8) << "(%rbp)\n";
        }
      }
    }

    // Locais ficam abaixo de rbx/r12 salvos: local s em rbp-16-(s+1)*kSlot.
    auto local = [&](int slot) { return -16 - ((slot + 1) * kSlot); };
    auto lbl = [&](int ip) { return ".L" + sym + "_" + std::to_string(ip) + ":"; };

    for (std::size_t ip = 0; ip < c.code.size(); ++ip) {
      os << lbl(static_cast<int>(ip)) << "\n";
      const vm::Instr& in = c.code[ip];
      switch (in.op) {
        case Op::Const: {
          const rt::Value& v = c.consts[static_cast<std::size_t>(in.a)];
          switch (v.kind) {
            case rt::ValueKind::Inteiro:
              os << "  mov $" << v.i << ", %rsi\n";
              push_const_call("tv_inteiro");
              break;
            case rt::ValueKind::Logico:
              os << "  mov $" << (v.b ? 1 : 0) << ", %rsi\n";
              push_const_call("tv_logico");
              break;
            case rt::ValueKind::Nulo:
              push_const_call("tv_nulo");
              break;
            case rt::ValueKind::Decimal:
              os << "  movsd " << labels.at(in.a) << "(%rip), %xmm0\n";
              push_const_call("tv_decimal");
              break;
            case rt::ValueKind::Texto:
              os << "  lea " << labels.at(in.a) << "(%rip), %rsi\n";
              push_const_call("tv_texto");
              break;
            default:
              break;  // rejeitado por chunk_supported
          }
          break;
        }
        case Op::LoadLocal:
          os << "  sub $" << kSlot << ", %rsp\n";
          copy_slot(os, "%rsp", 0, "%rbp", local(in.a));
          break;
        case Op::StoreLocal:
          copy_slot(os, "%rbp", local(in.a), "%rsp", 0);
          os << "  add $" << kSlot << ", %rsp\n";
          break;
        case Op::Pop:
          os << "  add $" << kSlot << ", %rsp\n";
          break;
        case Op::Neg:
          os << "  mov %rsp, %rsi\n  mov %rsp, %rdi\n  call tv_neg\n";
          break;
        case Op::Not:
          os << "  mov %rsp, %rsi\n  mov %rsp, %rdi\n  call tv_not\n";
          break;
        case Op::Truthy:
          os << "  mov %rsp, %rdi\n  call tv_truthy\n  mov %rax, %rsi\n  mov %rsp, %rdi\n"
                "  call tv_logico\n";
          break;
        case Op::And:
        case Op::Or:
          os << "  mov %rsp, %rdi\n  call tv_truthy\n  mov %rax, %r12\n  add $" << kSlot
             << ", %rsp\n";
          os << "  mov %rsp, %rdi\n  call tv_truthy\n";
          os << "  test %rax, %rax\n  setne %al\n  movzbq %al, %rax\n";
          os << "  test %r12, %r12\n  setne %r12b\n  movzbq %r12b, %r12\n";
          os << (in.op == Op::And ? "  and %r12, %rax\n" : "  or %r12, %rax\n");
          os << "  mov %rax, %rsi\n  mov %rsp, %rdi\n  call tv_logico\n";
          break;
        case Op::Binop: {
          os << "  lea " << kSlot << "(%rsp), %rdi\n  lea " << kSlot << "(%rsp), %rsi\n  mov $"
             << binop_id(c.op_names[static_cast<std::size_t>(in.a)]) << ", %edx\n"
             << "  mov %rsp, %rcx\n  call tv_binop\n  add $" << kSlot << ", %rsp\n";
          break;
        }
        case Op::Jump:
          os << "  jmp .L" << sym << "_" << in.a << "\n";
          break;
        case Op::JumpIfFalse:
          os << "  mov %rsp, %rdi\n  call tv_truthy\n  add $" << kSlot
             << ", %rsp\n  test %rax, %rax\n  jz .L" << sym << "_" << in.a << "\n";
          break;
        case Op::CallFunc: {
          const int argc = in.b;
          // args na pilha: arg0 no endereco mais alto. out vai abaixo deles.
          os << "  sub $" << kSlot << ", %rsp\n  mov %rsp, %rdi\n  lea " << (argc * kSlot)
             << "(%rsp), %rsi\n  call tilt_fn_"
             << sanitize(c.names[static_cast<std::size_t>(in.a)]) << "\n";
          if (argc > 0) {
            // copia out para o slot de arg0 e desempilha os demais
            copy_slot(os, "%rsp", argc * kSlot, "%rsp", 0);
            os << "  add $" << (argc * kSlot) << ", %rsp\n";
          }
          break;
        }
        case Op::Print: {
          const int argc = in.b;
          os << "  mov %rsp, %rsi\n  mov $" << argc << ", %edi\n  call tv_print\n";
          if (argc) os << "  add $" << (argc * kSlot) << ", %rsp\n";
          push_nulo();
          break;
        }
        case Op::Len:
          os << "  mov %rsp, %rsi\n  mov %rsp, %rdi\n  call tv_len\n";
          break;
        case Op::MakeList: {
          const int n = in.b;
          // elementos na pilha: elem0 no endereco mais alto. out vai abaixo;
          // depois da chamada o resultado sobe para o slot de elem0.
          os << "  sub $" << kSlot << ", %rsp\n  mov %rsp, %rdi\n  mov $" << n
             << ", %esi\n  lea " << kSlot << "(%rsp), %rdx\n  call tv_makelist\n";
          if (n > 0) {
            copy_slot(os, "%rsp", n * kSlot, "%rsp", 0);
            os << "  add $" << (n * kSlot) << ", %rsp\n";
          }
          break;
        }
        case Op::Index:
          os << "  lea " << kSlot << "(%rsp), %rdi\n  lea " << kSlot << "(%rsp), %rsi\n"
                "  mov %rsp, %rdx\n  call tv_index\n  add $"
             << kSlot << ", %rsp\n";
          break;
        case Op::Return:
          copy_slot(os, "%rbx", 0, "%rsp", 0);
          os << "  lea -16(%rbp), %rsp\n  pop %r12\n  pop %rbx\n  pop %rbp\n  ret\n";
          break;
        case Op::ReturnNil:
          for (int f = 0; f < kFields; ++f) os << "  movq $0, " << (f * 8) << "(%rbx)\n";
          os << "  lea -16(%rbp), %rsp\n  pop %r12\n  pop %rbx\n  pop %rbp\n  ret\n";
          break;
      }
    }
    os << ".L" << sym << "_" << c.code.size() << ":\n";
    for (int f = 0; f < kFields; ++f) os << "  movq $0, " << (f * 8) << "(%rbx)\n";
    os << "  lea -16(%rbp), %rsp\n  pop %r12\n  pop %rbx\n  pop %rbp\n  ret\n\n";
  }
};

struct Unit {
  std::string sym;
  std::string header;  // rotulo do "== pipeline N ==" (vazio para funcao)
  int nparams = 0;
  vm::Chunk chunk;
};

}  // namespace

Result emit_program(const ast::Program& program) {
  std::unordered_map<std::string, const Item*> funcs;
  std::unordered_set<std::string> names;
  std::vector<const Item*> pipelines;
  for (const auto& it : program.items) {
    if (!it || it->kind != ItemKind::Decl || it->header.empty() ||
        it->header[0]->kind != ast::ExprKind::Name) {
      continue;
    }
    if (it->key == "funcao") {
      funcs[it->header[0]->text] = it.get();
      names.insert(it->header[0]->text);
    } else if (it->key == "pipeline") {
      pipelines.push_back(it.get());
    }
  }

  std::vector<Unit> units;       // tudo que vira simbolo (funcoes + pipelines)
  std::vector<int> entry;        // indices em `units` que main chama, em ordem
  auto add_unit = [&](const std::string& sym, const std::string& header, int nparams,
                      const Item& decl, bool is_pipeline) -> std::string {
    Unit u;
    u.sym = sym;
    u.header = header;
    u.nparams = nparams;
    try {
      u.chunk = is_pipeline ? vm::compile_pipeline(decl, names) : vm::compile_function(decl, names);
    } catch (const vm::NotCompilable& nc) {
      return std::string(nc.reason);
    }
    units.push_back(std::move(u));
    return "";
  };

  // Funcoes sao sempre emitidas: pipelines podem chama-las via CallFunc.
  for (const auto& [name, decl] : funcs) {
    const std::string err = add_unit("tilt_fn_" + sanitize(name), "",
                                     static_cast<int>(decl->params.size()), *decl, false);
    if (!err.empty()) return {false, "", "funcao '" + name + "': " + err};
  }

  if (!pipelines.empty()) {
    for (const Item* p : pipelines) {
      const std::string nm = p->header[0]->text;
      const std::string err =
          add_unit("tilt_pl_" + sanitize(nm), "== pipeline " + nm + " ==", 0, *p, true);
      if (!err.empty()) return {false, "", "pipeline '" + nm + "': " + err};
      entry.push_back(static_cast<int>(units.size()) - 1);
    }
  } else {
    if (!funcs.count("principal")) {
      return {false, "", "codegen nativo exige 'pipeline's ou uma 'funcao principal'"};
    }
    for (std::size_t u = 0; u < units.size(); ++u) {
      if (units[u].sym == "tilt_fn_principal") entry.push_back(static_cast<int>(u));
    }
  }

  Emitter em;
  std::vector<std::unordered_map<int, std::string>> all_labels;
  for (std::size_t u = 0; u < units.size(); ++u) {
    std::unordered_map<int, std::string> labels;
    for (int k = 0; k < static_cast<int>(units[u].chunk.consts.size()); ++k) {
      const rt::Value& v = units[u].chunk.consts[static_cast<std::size_t>(k)];
      if (v.kind == rt::ValueKind::Texto || v.kind == rt::ValueKind::Decimal) {
        labels[k] = em.const_label(static_cast<int>(u), k, v);
      }
    }
    if (!units[u].header.empty()) {
      const std::string h = ".LCh" + std::to_string(u);
      em.rodata << h << ":\n  .byte ";
      for (std::size_t k = 0; k < units[u].header.size(); ++k) {
        if (k) em.rodata << ',';
        em.rodata << static_cast<int>(static_cast<unsigned char>(units[u].header[k]));
      }
      em.rodata << ",0\n";  // tv_pipeline_header adiciona o '\n'
      units[u].header = h;
    }
    all_labels.push_back(std::move(labels));
  }

  for (std::size_t u = 0; u < units.size(); ++u) {
    std::string err;
    if (!em.chunk_supported(units[u].chunk, &err)) {
      return {false, "", "unidade '" + units[u].sym + "': " + err};
    }
    em.emit_chunk(units[u].sym, units[u].nparams, units[u].chunk, all_labels[u]);
  }

  em.os << "  .globl main\nmain:\n  push %rbp\n  mov %rsp, %rbp\n  sub $96, %rsp\n";
  for (const int u : entry) {
    if (!units[static_cast<std::size_t>(u)].header.empty()) {
      em.os << "  lea " << units[static_cast<std::size_t>(u)].header << "(%rip), %rdi\n"
               "  call tv_pipeline_header\n";
    }
    em.os << "  mov %rsp, %rdi\n  lea 48(%rsp), %rsi\n  call " << units[static_cast<std::size_t>(u)].sym
          << "\n";
  }
  em.os << "  xor %eax, %eax\n  mov %rbp, %rsp\n  pop %rbp\n  ret\n";
  em.os << "  .section .note.GNU-stack,\"\",@progbits\n";

  Result r{true, "  .section .rodata\n" + em.rodata.str() + "  .text\n" + em.os.str(), ""};
  return r;
}

const char* runtime_source() {
  return R"RUNTIME(#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Espelha runtime/value.cpp: kinds, to_display, truthy, apply_binop.
enum { T_NULO = 0, T_LOGICO = 1, T_INTEIRO = 2, T_DECIMAL = 3, T_TEXTO = 4, T_LISTA = 5 };

typedef struct TV {
  long kind;
  long i;
  double d;
  char* s;
  void* list;
  long pad;  // slot de pilha tem 48 bytes (kSlot); manter sizeof(TV) == 48
} TV;

typedef struct TList {
  TV* data;
  long n;
} TList;

static char* tdup(const char* s) {
  const size_t n = strlen(s);
  char* p = (char*)malloc(n + 1);
  memcpy(p, s, n + 1);
  return p;
}

void tv_nulo(TV* o) { o->kind = T_NULO; o->i = 0; o->d = 0.0; o->s = 0; o->list = 0; }
void tv_logico(TV* o, long b) { o->kind = T_LOGICO; o->i = b != 0; o->d = 0.0; o->s = 0; o->list = 0; }
void tv_inteiro(TV* o, long v) { o->kind = T_INTEIRO; o->i = v; o->d = 0.0; o->s = 0; o->list = 0; }
void tv_decimal(TV* o, double v) { o->kind = T_DECIMAL; o->i = 0; o->d = v; o->s = 0; o->list = 0; }
void tv_texto(TV* o, const char* s) { o->kind = T_TEXTO; o->i = 0; o->d = 0.0; o->s = tdup(s); o->list = 0; }

static int ttruthy(const TV* v) {
  switch (v->kind) {
    case T_NULO: return 0;
    case T_LOGICO:
    case T_INTEIRO: return v->i != 0;
    case T_DECIMAL: return v->d != 0.0;
    case T_TEXTO: return v->s && v->s[0] != 0;
    case T_LISTA: return v->list && ((TList*)v->list)->n > 0;
  }
  return 0;
}

static double tnum(const TV* v) {
  if (v->kind == T_INTEIRO || v->kind == T_LOGICO) return (double)v->i;
  if (v->kind == T_DECIMAL) return v->d;
  return 0.0;
}

static char* num_to_str(double v) {
  char b[64];
  if (isfinite(v) && v == (double)(long)v) snprintf(b, sizeof b, "%ld", (long)v);
  else snprintf(b, sizeof b, "%g", v);
  return tdup(b);
}

static char* cat2(const char* a, const char* b) {
  const size_t na = strlen(a), nb = strlen(b);
  char* p = (char*)malloc(na + nb + 1);
  memcpy(p, a, na);
  memcpy(p + na, b, nb + 1);
  return p;
}

static char* tdisplay(const TV* v) {
  char b[64];
  switch (v->kind) {
    case T_NULO: return tdup("nulo");
    case T_LOGICO: return tdup(v->i ? "verdadeiro" : "falso");
    case T_INTEIRO: snprintf(b, sizeof b, "%ld", v->i); return tdup(b);
    case T_DECIMAL: return num_to_str(v->d);
    case T_TEXTO: return tdup(v->s ? v->s : "");
    case T_LISTA: {
      const TList* l = (const TList*)v->list;
      char* r = tdup("[");
      if (l) {
        for (long k = 0; k < l->n; ++k) {
          if (k) r = cat2(r, ", ");
          r = cat2(r, tdisplay(&l->data[k]));
        }
      }
      return cat2(r, "]");
    }
  }
  return tdup("?");
}

static int teq(const TV* a, const TV* b) {
  const int an = a->kind == T_INTEIRO || a->kind == T_DECIMAL;
  const int bn = b->kind == T_INTEIRO || b->kind == T_DECIMAL;
  if (an && bn) return tnum(a) == tnum(b);
  if (a->kind != b->kind) return 0;
  switch (a->kind) {
    case T_NULO: return 1;
    case T_LOGICO: return a->i == b->i;
    case T_TEXTO: return strcmp(a->s ? a->s : "", b->s ? b->s : "") == 0;
    case T_LISTA: {
      const TList* x = (const TList*)a->list;
      const TList* y = (const TList*)b->list;
      if (!x || !y || x->n != y->n) return 0;
      for (long k = 0; k < x->n; ++k) {
        if (!teq(&x->data[k], &y->data[k])) return 0;
      }
      return 1;
    }
  }
  return 0;
}

// op: 0:+ 1:- 2:* 3:/ 4:% 5:== 6:!= 7:< 8:<= 9:> 10:>= 11:contem
void tv_binop(TV* o, const TV* a, long op, const TV* b) {
  if (op == 5) { tv_logico(o, teq(a, b)); return; }
  if (op == 6) { tv_logico(o, !teq(a, b)); return; }
  if (op == 11) {
    if (a->kind == T_TEXTO && b->kind == T_TEXTO) {
      tv_logico(o, strstr(a->s ? a->s : "", b->s ? b->s : "") != 0);
    } else if (a->kind == T_LISTA && a->list) {
      const TList* l = (const TList*)a->list;
      long r = 0;
      for (long k = 0; k < l->n; ++k) {
        if (teq(&l->data[k], b)) { r = 1; break; }
      }
      tv_logico(o, r);
    } else {
      tv_logico(o, 0);
    }
    return;
  }
  if (op == 0 && (a->kind == T_TEXTO || b->kind == T_TEXTO)) {
    char* sa = tdisplay(a);
    char* sb = tdisplay(b);
    tv_texto(o, cat2(sa, sb));
    free(sa);
    free(sb);
    return;
  }
  if (op >= 7 && op <= 10) {
    double x, y;
    if (a->kind == T_TEXTO && b->kind == T_TEXTO) {
      x = (double)strcmp(a->s ? a->s : "", b->s ? b->s : "");
      y = 0.0;
    } else {
      x = tnum(a);
      y = tnum(b);
    }
    tv_logico(o, op == 7 ? x < y : op == 8 ? x <= y : op == 9 ? x > y : x >= y);
    return;
  }
  {
    const double x = tnum(a);
    const double y = tnum(b);
    double r = 0.0;
    if (op == 0) r = x + y;
    else if (op == 1) r = x - y;
    else if (op == 2) r = x * y;
    else if (op == 3) r = y == 0.0 ? 0.0 : x / y;
    else r = y == 0.0 ? 0.0 : fmod(x, y);
    const int both_int = a->kind == T_INTEIRO && b->kind == T_INTEIRO;
    if (both_int && op != 3) tv_inteiro(o, (long)r);
    else tv_decimal(o, r);
  }
}

void tv_neg(TV* o, const TV* a) {
  if (a->kind == T_INTEIRO) tv_inteiro(o, -a->i);
  else tv_decimal(o, -tnum(a));
}

void tv_not(TV* o, const TV* a) { tv_logico(o, !ttruthy(a)); }

long tv_truthy(const TV* a) { return ttruthy(a); }

void tv_len(TV* o, const TV* a) {
  if (a->kind == T_LISTA && a->list) tv_inteiro(o, ((TList*)a->list)->n);
  else if (a->kind == T_TEXTO) tv_inteiro(o, (long)strlen(a->s ? a->s : ""));
  else tv_inteiro(o, 0);
}

void tv_makelist(TV* o, long n, const TV* base) {
  // base = endereco mais baixo da pilha (ultimo elemento); elem0 fica no
  // endereco mais alto.
  TList* l = (TList*)malloc(sizeof(TList));
  l->data = (TV*)malloc(sizeof(TV) * (size_t)(n > 0 ? n : 1));
  l->n = n;
  for (long k = 0; k < n; ++k) l->data[k] = base[n - 1 - k];
  o->kind = T_LISTA;
  o->i = 0;
  o->d = 0.0;
  o->s = 0;
  o->list = l;
}

void tv_index(TV* o, const TV* a, const TV* idx) {
  if (a->kind != T_LISTA || !a->list) {
    fprintf(stderr, "tilt nativo: indice espera uma lista\n");
    exit(1);
  }
  const TList* l = (const TList*)a->list;
  const long i = (long)tnum(idx);
  if (i < 0 || i >= l->n) {
    fprintf(stderr, "tilt nativo: indice fora da faixa\n");
    exit(1);
  }
  *o = l->data[i];
}

void tv_print(long n, const TV* base) {
  // base = endereco mais baixo (ultimo arg); arg0 no endereco mais alto.
  for (long k = 0; k < n; ++k) {
    if (k) putchar(' ');
    char* s = tdisplay(&base[n - 1 - k]);
    fputs(s, stdout);
    free(s);
  }
  putchar('\n');
}

void tv_pipeline_header(const char* name) { printf("%s\n", name); }
)RUNTIME";
}

}  // namespace tilt::codegen
