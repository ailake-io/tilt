#include "codegen/codegen_arm64.hpp"

#include <cctype>
#include <cstdint>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "vm/bytecode.hpp"
#include "vm/compiler.hpp"

namespace tilt::codegen::arm64 {

using ast::Item;
using ast::ItemKind;
using vm::Chunk;
using vm::Op;

namespace {

// Slots da pilha de avaliacao em bytes (mesmo layout do x86-64: TV tem 40
// bytes, 48 mantem sp 16-alinhado a cada `bl`). O prologo so subtrai
// multiplos de 16 de sp, entao o alinhamento AAPCS esta garantido.
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

  // Carrega uma constante inteira de 64 bits em `reg` via movz/movk (o
  // pseudo `mov reg, #imm` so aceita imediatos logicos).
  void load_imm64(std::ostream& o, const char* reg, std::int64_t v) {
    const std::uint64_t uv = static_cast<std::uint64_t>(v);
    o << "  movz " << reg << ", #" << (uv & 0xffff) << "\n";
    for (int sh = 16; sh < 64; sh += 16) {
      const std::uint64_t chunk = (uv >> sh) & 0xffff;
      if (chunk) o << "  movk " << reg << ", #" << chunk << ", lsl #" << sh << "\n";
    }
  }

  // Copia os 6 qwords de um slot: [off_dst+base_dst] <- [off_src+base_src].
  void copy_slot(std::ostream& o, const std::string& dst_base, int dst_off,
                 const std::string& src_base, int src_off) {
    for (int f = 0; f < kFields; f += 2) {
      o << "  ldp x9, x10, [" << src_base << ", #" << (src_off + f * 8) << "]\n";
      o << "  stp x9, x10, [" << dst_base << ", #" << (dst_off + f * 8) << "]\n";
    }
  }

  void push_const_call(const char* fn) {  // sub slot; x0=slot; bl fn
    os << "  sub sp, sp, #" << kSlot << "\n  mov x0, sp\n  bl " << fn << "\n";
  }

  void push_nulo() { push_const_call("tv_nulo"); }

  // Emite um chunk como funcao `void sym(TV* out, TV* args)` (AAPCS).
  void emit_chunk(const std::string& sym, int nparams, const Chunk& c,
                  const std::unordered_map<int, std::string>& labels) {
    // Locais ocupam um bloco logo abaixo de x19/x20 salvos; a pilha de
    // avaliacao cresce a partir dai. local s fica em [sp, #(s*kSlot)].
    const int locals_frame = ((c.num_locals * kSlot) + 15) & ~15;

    os << "  .globl " << sym << "\n" << sym << ":\n";
    os << "  stp x29, x30, [sp, #-16]!\n  mov x29, sp\n";
    os << "  stp x19, x20, [sp, #-16]!\n";  // out, args (registradores)
    if (locals_frame) os << "  sub sp, sp, #" << locals_frame << "\n";
    if (nparams > 0) {
      // args = &arg0 (slot mais alto); arg_p fica em x20 - p*kSlot.
      for (int p = 0; p < nparams; ++p) {
        os << "  sub x9, x20, #" << (p * kSlot) << "\n";
        for (int f = 0; f < kFields; ++f) {
          os << "  ldr x10, [x9, #" << (f * 8) << "]\n  str x10, [sp, #" << (p * kSlot + f * 8)
             << "]\n";
        }
      }
    }

    auto lbl = [&](int ip) { return ".L" + sym + "_" + std::to_string(ip) + ":"; };

    for (std::size_t ip = 0; ip < c.code.size(); ++ip) {
      os << lbl(static_cast<int>(ip)) << "\n";
      const vm::Instr& in = c.code[ip];
      switch (in.op) {
        case Op::Const: {
          const rt::Value& v = c.consts[static_cast<std::size_t>(in.a)];
          switch (v.kind) {
            case rt::ValueKind::Inteiro:
              load_imm64(os, "x1", v.i);
              push_const_call("tv_inteiro");
              break;
            case rt::ValueKind::Logico:
              os << "  mov x1, #" << (v.b ? 1 : 0) << "\n";
              push_const_call("tv_logico");
              break;
            case rt::ValueKind::Nulo:
              push_const_call("tv_nulo");
              break;
            case rt::ValueKind::Decimal: {
              const std::string& l = labels.at(in.a);
              os << "  adrp x1, " << l << "\n  add x1, x1, #:lo12:" << l << "\n";
              os << "  ldr d0, [x1]\n";
              push_const_call("tv_decimal");
              break;
            }
            case rt::ValueKind::Texto: {
              const std::string& l = labels.at(in.a);
              os << "  adrp x1, " << l << "\n  add x1, x1, #:lo12:" << l << "\n";
              push_const_call("tv_texto");
              break;
            }
            default:
              break;  // rejeitado por chunk_supported
          }
          break;
        }
        case Op::LoadLocal:
          os << "  sub sp, sp, #" << kSlot << "\n";
          copy_slot(os, "sp", 0, "sp", kSlot + in.a * kSlot);
          break;
        case Op::StoreLocal:
          copy_slot(os, "sp", kSlot + in.a * kSlot, "sp", 0);
          os << "  add sp, sp, #" << kSlot << "\n";
          break;
        case Op::Pop:
          os << "  add sp, sp, #" << kSlot << "\n";
          break;
        case Op::Neg:
          os << "  mov x0, sp\n  mov x1, sp\n  bl tv_neg\n";
          break;
        case Op::Not:
          os << "  mov x0, sp\n  mov x1, sp\n  bl tv_not\n";
          break;
        case Op::Truthy:
          os << "  mov x0, sp\n  bl tv_truthy\n  mov x1, x0\n  mov x0, sp\n"
                "  bl tv_logico\n";
          break;
        case Op::Binop: {
          os << "  add x0, sp, #" << kSlot << "\n  add x1, sp, #" << kSlot << "\n  mov x2, #"
             << binop_id(c.op_names[static_cast<std::size_t>(in.a)]) << "\n"
             << "  mov x3, sp\n  bl tv_binop\n  add sp, sp, #" << kSlot << "\n";
          break;
        }
        case Op::Jump:
          os << "  b .L" << sym << "_" << in.a << "\n";
          break;
        case Op::JumpIfFalse:
          os << "  mov x0, sp\n  bl tv_truthy\n  add sp, sp, #" << kSlot
             << "\n  cbz x0, .L" << sym << "_" << in.a << "\n";
          break;
        case Op::CallFunc: {
          const int argc = in.b;
          // args na pilha: arg0 no endereco mais alto. out vai abaixo deles.
          os << "  sub sp, sp, #" << kSlot << "\n  mov x0, sp\n  add x1, sp, #" << (argc * kSlot)
             << "\n  bl tilt_fn_"
             << sanitize(c.names[static_cast<std::size_t>(in.a)]) << "\n";
          if (argc > 0) {
            // copia out para o slot de arg0 e desempilha os demais
            copy_slot(os, "sp", argc * kSlot, "sp", 0);
            os << "  add sp, sp, #" << (argc * kSlot) << "\n";
          }
          break;
        }
        case Op::Print: {
          const int argc = in.b;
          os << "  mov x1, sp\n  mov x0, #" << argc << "\n  bl tv_print\n";
          if (argc) os << "  add sp, sp, #" << (argc * kSlot) << "\n";
          push_nulo();
          break;
        }
        case Op::Len:
          os << "  mov x0, sp\n  mov x1, sp\n  bl tv_len\n";
          break;
        case Op::MakeList: {
          const int n = in.b;
          // elementos na pilha: elem0 no endereco mais alto. out vai abaixo;
          // depois da chamada o resultado sobe para o slot de elem0.
          os << "  sub sp, sp, #" << kSlot << "\n  mov x0, sp\n  mov x1, #" << n
             << "\n  add x2, sp, #" << kSlot << "\n  bl tv_makelist\n";
          if (n > 0) {
            copy_slot(os, "sp", n * kSlot, "sp", 0);
            os << "  add sp, sp, #" << (n * kSlot) << "\n";
          }
          break;
        }
        case Op::Index:
          os << "  add x0, sp, #" << kSlot << "\n  add x1, sp, #" << kSlot << "\n"
                "  mov x2, sp\n  bl tv_index\n  add sp, sp, #"
             << kSlot << "\n";
          break;
        case Op::Return:
          copy_slot(os, "x19", 0, "sp", 0);
          os << "  mov sp, x29\n  ldp x19, x20, [sp, #-16]!\n  ldp x29, x30, [sp], #16\n  ret\n";
          break;
        case Op::ReturnNil:
          for (int f = 0; f < kFields; ++f) os << "  mov x9, #0\n  str x9, [x19, #" << (f * 8) << "]\n";
          os << "  mov sp, x29\n  ldp x19, x20, [sp, #-16]!\n  ldp x29, x30, [sp], #16\n  ret\n";
          break;
      }
    }
    os << ".L" << sym << "_" << c.code.size() << ":\n";
    for (int f = 0; f < kFields; ++f) os << "  mov x9, #0\n  str x9, [x19, #" << (f * 8) << "]\n";
    os << "  mov sp, x29\n  ldp x19, x20, [sp, #-16]!\n  ldp x29, x30, [sp], #16\n  ret\n\n";
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

  // main: dois slots de TV (out + args) na propria pilha, como no x86-64.
  em.os << "  .globl main\nmain:\n  stp x29, x30, [sp, #-16]!\n  mov x29, sp\n"
           "  sub sp, sp, #96\n";
  for (const int u : entry) {
    if (!units[static_cast<std::size_t>(u)].header.empty()) {
      const std::string& h = units[static_cast<std::size_t>(u)].header;
      em.os << "  adrp x0, " << h << "\n  add x0, x0, #:lo12:" << h << "\n"
               "  bl tv_pipeline_header\n";
    }
    em.os << "  mov x0, sp\n  add x1, sp, #48\n  bl " << units[static_cast<std::size_t>(u)].sym
          << "\n";
  }
  em.os << "  mov w0, #0\n  add sp, sp, #96\n  ldp x29, x30, [sp], #16\n  ret\n";
  em.os << "  .section .note.GNU-stack,\"\",@progbits\n";

  Result r{true, "  .section .rodata\n" + em.rodata.str() + "  .text\n" + em.os.str(), ""};
  return r;
}

}  // namespace tilt::codegen::arm64
