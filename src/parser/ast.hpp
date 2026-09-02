#pragma once

#include <memory>
#include <string>
#include <vector>

#include "common/source.hpp"

namespace tilt::ast {

struct Expr;
struct Item;
struct Stmt;
using ExprPtr = std::unique_ptr<Expr>;
using ItemPtr = std::unique_ptr<Item>;
using StmtPtr = std::unique_ptr<Stmt>;

struct Block {
  std::vector<ItemPtr> items;
  Span span;
};

// ---------------------------------------------------------------- expressions

enum class ExprKind {
  IntLit,
  DecimalLit,
  TextLit,
  BoolLit,
  NullLit,
  Name,
  Member,   // lhs '.' text          (optional == true when reached via '?.')
  Index,    // lhs '[' elems... ']'
  Slice,    // lhs '[' rhs '..' extra ']'
  Call,     // lhs applied to args (bare-call syntax); block holds a trailing ':' block
  Unary,    // text = operator, rhs = operand
  Binary,   // text = operator, lhs / rhs
  ListLit,  // elems
  MapLit,   // entries
  Assign,   // lhs = target, rhs = value
  Device,   // lhs = inner expression, text = device name ("no dispositivo <name>")
};

struct Arg {
  std::string name;  // empty => positional
  ExprPtr value;
};

struct MapEntry {
  std::string key;
  ExprPtr value;
};

struct Expr {
  ExprKind kind{};
  Span span;

  std::string text;       // literal text / name / operator / member name / device name
  bool boolean = false;   // BoolLit
  bool optional = false;  // Member reached via '?.'

  ExprPtr lhs;
  ExprPtr rhs;
  ExprPtr extra;

  std::vector<Arg> args;          // Call
  std::vector<ExprPtr> elems;     // ListLit, Index
  std::vector<MapEntry> entries;  // MapLit
  std::unique_ptr<Block> block;   // Call: trailing ':' block of named arguments
};

// ---------------------------------------------------------------- statements

enum class StmtKind { Expr, Assign, If, ForEach, While, Try, Return };

struct ElseIf {
  ExprPtr cond;
  Block body;
};

struct Stmt {
  StmtKind kind{};
  Span span;

  ExprPtr a;         // If/While cond; ForEach iterable; Return value; Assign target; Expr expr
  ExprPtr b;         // Assign value
  std::string name;  // ForEach loop variable; Try catch variable

  Block body;
  std::vector<ElseIf> elifs;
  std::unique_ptr<Block> else_body;
  std::unique_ptr<Block> catch_body;
};

// ---------------------------------------------------------------- items

// A program is a block at indentation 0. Item covers every line shape:
//   Decl       `<keyword> <header...>:` <block>        (top level only)
//   Field      `<key> <header...>: <value?>` <block?>
//   ListEntry  `- <child-item>`
//   Stmt       a control-flow / assignment / bare-expression statement
enum class ItemKind { Decl, Field, ListEntry, Stmt };

struct Item {
  ItemKind kind{};
  Span span;

  std::string key;               // Decl keyword / Field key
  std::vector<ExprPtr> header;   // tokens between key and ':'  (e.g. `rota post "/x"`)
  std::vector<Arg> params;       // Decl `funcao`: parameters (name + optional type expr)
  ExprPtr value;                 // inline value after ':' (Field); RHS (`seja`); `funcao` return type
  std::unique_ptr<Block> block;  // nested block (Decl / Field)

  StmtPtr stmt;   // Stmt
  ItemPtr child;  // ListEntry
};

struct Program {
  std::vector<ItemPtr> items;
};

}  // namespace tilt::ast
