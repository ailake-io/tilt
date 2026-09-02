#include "parser/ast_dump.hpp"

#include <ostream>
#include <string>
#include <string_view>

namespace tilt {
namespace {

using namespace ast;

std::string quote(std::string_view s) {
  std::string r = "\"";
  for (char c : s) {
    switch (c) {
      case '\n':
        r += "\\n";
        break;
      case '\t':
        r += "\\t";
        break;
      case '\r':
        r += "\\r";
        break;
      case '"':
        r += "\\\"";
        break;
      case '\\':
        r += "\\\\";
        break;
      default:
        r += c;
    }
  }
  r += '"';
  return r;
}

struct Printer {
  std::ostream& os;

  void indent(int n) {
    for (int i = 0; i < n; ++i) os << "  ";
  }

  void expr(const Expr& e) {
    switch (e.kind) {
      case ExprKind::IntLit:
        os << "(int " << quote(e.text) << ")";
        break;
      case ExprKind::DecimalLit:
        os << "(decimal " << quote(e.text) << ")";
        break;
      case ExprKind::TextLit:
        os << "(text " << quote(e.text) << ")";
        break;
      case ExprKind::BoolLit:
        os << "(bool " << (e.boolean ? "verdadeiro" : "falso") << ")";
        break;
      case ExprKind::NullLit:
        os << "(nulo)";
        break;
      case ExprKind::Name:
        os << "(name " << quote(e.text) << ")";
        break;
      case ExprKind::Member:
        os << "(member " << quote(e.text) << (e.optional ? " opcional " : " ");
        expr(*e.lhs);
        os << ")";
        break;
      case ExprKind::Index:
        os << "(index ";
        expr(*e.lhs);
        for (const auto& el : e.elems) {
          os << " ";
          expr(*el);
        }
        os << ")";
        break;
      case ExprKind::Slice:
        os << "(slice ";
        expr(*e.lhs);
        os << " ";
        expr(*e.rhs);
        os << " ";
        expr(*e.extra);
        os << ")";
        break;
      case ExprKind::Call:
        os << "(call ";
        expr(*e.lhs);
        for (const auto& a : e.args) {
          os << " ";
          if (!a.name.empty()) os << "(" << a.name << ": ";
          expr(*a.value);
          if (!a.name.empty()) os << ")";
        }
        os << ")";  // a trailing ':' block, if any, is printed by call_block()
        break;
      case ExprKind::Unary:
        os << "(unary " << quote(e.text) << " ";
        expr(*e.rhs);
        os << ")";
        break;
      case ExprKind::Binary:
        os << "(binary " << quote(e.text) << " ";
        expr(*e.lhs);
        os << " ";
        expr(*e.rhs);
        os << ")";
        break;
      case ExprKind::ListLit:
        os << "(list";
        for (const auto& el : e.elems) {
          os << " ";
          expr(*el);
        }
        os << ")";
        break;
      case ExprKind::MapLit:
        os << "(map";
        for (const auto& en : e.entries) {
          os << " (" << en.key << ": ";
          expr(*en.value);
          os << ")";
        }
        os << ")";
        break;
      case ExprKind::Assign:
        os << "(assign ";
        expr(*e.lhs);
        os << " ";
        expr(*e.rhs);
        os << ")";
        break;
      case ExprKind::Device:
        os << "(device " << quote(e.text) << " ";
        expr(*e.lhs);
        os << ")";
        break;
    }
    // A call may carry a nested block of named arguments.
    if (e.kind == ExprKind::Call && e.block) {
      // rendered compactly above; full block detail follows the item tree
    }
  }

  void block(const Block& b, int ind) {
    indent(ind);
    os << "(bloco\n";
    for (const auto& it : b.items) item(*it, ind + 1);
    indent(ind);
    os << ")\n";
  }

  void call_block(const Expr& e, int ind) {
    if (e.kind == ExprKind::Call && e.block) block(*e.block, ind);
    if (e.kind == ExprKind::Assign && e.rhs) call_block(*e.rhs, ind);
    if (e.kind == ExprKind::Device && e.lhs) call_block(*e.lhs, ind);
  }

  static bool has_call_block(const Expr& e) {
    if (e.kind == ExprKind::Call) return e.block != nullptr;
    if (e.kind == ExprKind::Assign && e.rhs) return has_call_block(*e.rhs);
    if (e.kind == ExprKind::Device && e.lhs) return has_call_block(*e.lhs);
    return false;
  }

  void stmt(const Stmt& s, int ind) {
    indent(ind);
    switch (s.kind) {
      case StmtKind::Expr:
        os << "(expr ";
        expr(*s.a);
        os << ")\n";
        call_block(*s.a, ind + 1);
        break;
      case StmtKind::Assign:
        os << "(assign ";
        expr(*s.a);
        os << " ";
        expr(*s.b);
        os << ")\n";
        call_block(*s.b, ind + 1);
        break;
      case StmtKind::Return:
        os << "(retornar";
        if (s.a) {
          os << " ";
          expr(*s.a);
        }
        os << ")\n";
        break;
      case StmtKind::If:
        os << "(se ";
        expr(*s.a);
        os << "\n";
        block(s.body, ind + 1);
        for (const auto& ei : s.elifs) {
          indent(ind + 1);
          os << "(senao-se ";
          expr(*ei.cond);
          os << "\n";
          block(ei.body, ind + 2);
          indent(ind + 1);
          os << ")\n";
        }
        if (s.else_body) {
          indent(ind + 1);
          os << "(senao\n";
          block(*s.else_body, ind + 2);
          indent(ind + 1);
          os << ")\n";
        }
        indent(ind);
        os << ")\n";
        break;
      case StmtKind::ForEach:
        os << "(para-cada " << quote(s.name) << " em ";
        expr(*s.a);
        os << "\n";
        block(s.body, ind + 1);
        indent(ind);
        os << ")\n";
        break;
      case StmtKind::While:
        os << "(enquanto ";
        expr(*s.a);
        os << "\n";
        block(s.body, ind + 1);
        indent(ind);
        os << ")\n";
        break;
      case StmtKind::Try:
        os << "(tentar\n";
        block(s.body, ind + 1);
        if (s.catch_body) {
          indent(ind + 1);
          os << "(capturar " << quote(s.name) << "\n";
          block(*s.catch_body, ind + 2);
          indent(ind + 1);
          os << ")\n";
        }
        indent(ind);
        os << ")\n";
        break;
    }
  }

  void header(const std::vector<ExprPtr>& hs) {
    if (hs.empty()) return;
    os << " (cabecalho";
    for (const auto& h : hs) {
      os << " ";
      expr(*h);
    }
    os << ")";
  }

  void item(const Item& it, int ind) {
    indent(ind);
    switch (it.kind) {
      case ItemKind::Decl: {
        os << "(decl " << quote(it.key);
        header(it.header);
        if (it.value) {
          os << " ";
          expr(*it.value);
        }
        bool nested = it.block || (it.value && has_call_block(*it.value));
        if (!nested) {
          os << ")\n";
          break;
        }
        os << "\n";
        if (it.value) call_block(*it.value, ind + 1);
        if (it.block) block(*it.block, ind + 1);
        indent(ind);
        os << ")\n";
        break;
      }
      case ItemKind::Field: {
        os << "(campo " << quote(it.key);
        header(it.header);
        if (it.value) {
          os << " ";
          expr(*it.value);
        }
        bool nested = it.block || (it.value && has_call_block(*it.value));
        if (!nested) {
          os << ")\n";
          break;
        }
        os << "\n";
        if (it.value) call_block(*it.value, ind + 1);
        if (it.block) block(*it.block, ind + 1);
        indent(ind);
        os << ")\n";
        break;
      }
      case ItemKind::ListEntry:
        os << "(item\n";
        if (it.block) block(*it.block, ind + 1);
        if (it.child) item(*it.child, ind + 1);
        indent(ind);
        os << ")\n";
        break;
      case ItemKind::Stmt:
        os << "(stmt\n";
        if (it.stmt) stmt(*it.stmt, ind + 1);
        indent(ind);
        os << ")\n";
        break;
    }
  }
};

}  // namespace

void dump_program(std::ostream& os, const ast::Program& program) {
  Printer p{os};
  os << "(programa\n";
  for (const auto& it : program.items) p.item(*it, 1);
  os << ")\n";
}

}  // namespace tilt
