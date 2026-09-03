#pragma once

#include <iosfwd>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/source.hpp"
#include "diagnostics/diagnostic.hpp"
#include "parser/ast.hpp"
#include "runtime/value.hpp"

namespace tilt {

// Tree-walking interpreter. Executes every top-level `pipeline` in order (and a
// `funcao principal` if no pipeline is present). Real compute + a small set of
// built-ins and Tabela operations; connectors, LLM, agents and GPU raise a
// "not implemented" runtime error pointing at the milestone that will add them.
class Interpreter {
 public:
  Interpreter(const ast::Program& program, DiagnosticEngine& diag, std::ostream& out);

  // `tilt executar --agendar`: acknowledge `agenda:` cron on pipelines.
  void set_schedule_mode(bool on) { schedule_mode_ = on; }

  // Returns 0 on success, 1 if a runtime error was reported.
  int run();

 private:
  struct Env {
    std::unordered_map<std::string, rt::Value> vars;
    Env* parent = nullptr;

    rt::Value* lookup(const std::string& name);
    void set(const std::string& name, rt::Value value);
  };

  struct ReturnSignal {
    rt::Value value;
  };
  struct RuntimeAbort {
    Span span;
    std::string message;
    DiagCode code;
    std::vector<std::string> notes;
  };

  [[noreturn]] void fail(Span span, std::string message,
                         DiagCode code = DiagCode::RuntimeError);

  void run_pipeline(const ast::Item& pipeline);
  void run_verificar(const ast::Item& field, Env& env);
  void exec_block(const ast::Block& block, Env& env);
  void exec_item(const ast::Item& item, Env& env);
  void exec_stmt(const ast::Stmt& stmt, Env& env);

  rt::Value eval(const ast::Expr& expr, Env& env);
  rt::Value eval_binary(const ast::Expr& expr, Env& env);
  rt::Value eval_call(const ast::Expr& expr, Env& env);
  rt::Value eval_builtin(const std::string& name, const ast::Expr& call, Env& env);
  rt::Value eval_method(const std::string& method, rt::Value receiver, const ast::Expr& call,
                        Env& env);
  rt::Value call_function(const ast::Item& fn, std::vector<rt::Value> args, Span span);

  std::vector<rt::Value> eval_args(const ast::Expr& call, Env& env);
  rt::ValueMap eval_kwargs(const ast::Expr& call, Env& env);
  std::string interpolate(const std::string& text, Env& env);

  const ast::Program& program_;
  DiagnosticEngine& diag_;
  std::ostream& out_;

  Env root_;
  std::unordered_map<std::string, const ast::Item*> functions_;
  std::unordered_map<std::string, const ast::Item*> entities_;
  std::vector<const ast::Item*> pipelines_;
  bool schedule_mode_ = false;
};

}  // namespace tilt
