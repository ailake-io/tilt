#pragma once

#include <ctime>
#include <iosfwd>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common/source.hpp"
#include "diagnostics/diagnostic.hpp"
#include "parser/ast.hpp"
#include "runtime/llm.hpp"
#include "runtime/gpu_runtime.hpp"
#include "runtime/tensor.hpp"
#include "runtime/value.hpp"
#include "runtime/vectorstore.hpp"
#include "vm/bytecode.hpp"

namespace tilt {

// Estado da resposta sendo montada por `responder:`/`responder_em_fluxo:`
// dentro de uma rota de `servico`. Apontado por um thread_local no
// interpretador (uma rota por thread do pool), nunca compartilhado.
struct RouteResponse {
  int status = 200;
  rt::Value dados;
  bool set = false;
};

// Tree-walking interpreter. Executes every top-level `pipeline` in order (and a
// `funcao principal` if no pipeline is present). Real compute + a small set of
// built-ins and Tabela operations; connectors, LLM, agents and GPU raise a
// "not implemented" runtime error pointing at the milestone that will add them.
class Interpreter {
 public:
  Interpreter(const ast::Program& program, DiagnosticEngine& diag, std::ostream& out);

  // `tilt executar --agendar`: acknowledge `agenda:` cron on pipelines.
  void set_schedule_mode(bool on) { schedule_mode_ = on; }

  // Diretorio do arquivo principal (entrada do CLI); modulos `importar x`
  // sao resolvidos primeiro como `<dir>/x.tilt` e depois na stdlib.
  void set_entry_dir(std::string dir) { entry_dir_ = std::move(dir); }

  // Returns 0 on success, 1 if a runtime error was reported.
  int run();

  // `tilt executar --agendar`: loop forever (or TILT_AGENDAR_MAX runs with the
  // TILT_AGORA fake clock) firing each pipeline at its `agenda:` cron.
  int run_scheduled();

  // `tilt executar --vm`: roda cada pipeline pelo bytecode VM quando o corpo
  // esta no subconjunto compilavel; cai de volta para o interpretador de
  // arvore por pipeline quando nao esta. Saida identica a run().
  int run_vm();

  // Serves the first `servico` declaration. `max_requests <= 0` runs forever.
  // `threads` <= 0 picks a default (min(4, cores)); 1 runs route handling
  // serially; > 1 runs it on a worker pool (handler must be reentrant).
  int serve(int port_override, int max_requests, int threads);

 private:
  struct Env {
    std::unordered_map<std::string, rt::Value> vars;
    // Tabela de funcoes do modulo dono deste escopo (scope de Module), para
    // chamadas entre funcoes do mesmo modulo sem prefixo. nullptr fora de
    // modulo.
    const std::unordered_map<std::string, const ast::Item*>* funcs = nullptr;
    Env* parent = nullptr;

    rt::Value* lookup(const std::string& name);
    void set(const std::string& name, rt::Value value);
  };

  // Modulo carregado via `importar nome` / `de nome importar f`. Mantem o
  // Program do arquivo vivo (os itens apontam para ele) e o escopo com as
  // variaveis de topo do modulo (parent = root_), visivel as suas funcoes.
  struct Module {
    std::string path;
    std::shared_ptr<ast::Program> program;
    std::unordered_map<std::string, const ast::Item*> funcs;  // 'funcao' exportadas
    Env scope;
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

  void register_decls();
  // Carrega o modulo `name` procurando `<from_dir>/name.tilt` e depois os
  // diretorios da stdlib (TILT_STDLIB_PATH, stdlib/ ao lado do binario,
  // <exe>/../share/tilt/stdlib). Falha com a lista de caminhos tentados.
  std::shared_ptr<Module> load_module(const std::string& name, const std::string& from_dir,
                                      Span span);
  std::vector<std::string> stdlib_dirs() const;
  // `now < 0` le o relogio atual (fake via TILT_AGORA ou real); o loop de
  // agenda passa o tempo fake avancado explicitamente.
  void run_pipeline(const ast::Item& pipeline, std::time_t now = -1);
  // Campo `janela:`: le novos elementos da `entrada:` (fonte), aplica a
  // semantica de janela e, se ela fechar, preenche `batch` (vai para a
  // variavel `linhas`). Retorna false quando os passos nao devem rodar.
  bool run_janela(const ast::Item& janela, const ast::Item& pipeline, std::time_t now,
                  std::vector<rt::Value>& batch);
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
  rt::Value call_function(const ast::Item& fn, std::vector<rt::Value> args, Span span,
                          Env* module_scope = nullptr);

  std::vector<rt::Value> eval_args(const ast::Expr& call, Env& env);
  rt::ValueMap eval_kwargs(const ast::Expr& call, Env& env);
  // `particionar_por:` como texto ou lista de textos (particao composta).
  std::vector<std::string> parse_particionar_por(const rt::ValueMap& kw, const char* builtin,
                                                 const Span& span);
  std::string interpolate(const std::string& text, Env& env);

  rt::Value read_csv_file(const std::string& path, Span span);
  rt::Value read_fonte(const std::string& name, Span span);

  // Deep learning.
  struct Layer {
    enum Kind { Dense, Activation, Softmax, Dropout, LayerNorm } kind = Dense;
    rt::Tensor w;
    rt::Tensor b;
    std::string act;
    // Adam moment estimates (allocated lazily during training).
    rt::Tensor m_w, v_w, m_b, v_b;
  };
  rt::Tensor value_to_tensor(const rt::Value& v, Span span);
  void set_device(const ast::Item& decl);  // reads `dispositivo:` -> gpu on/off
  rt::Tensor mm(const rt::Tensor& a, const rt::Tensor& b);
  rt::Tensor act_relu(const rt::Tensor& x);
  std::vector<Layer> build_layers(const ast::Item& model_decl, std::int64_t in_dim);
  const std::vector<Layer>& build_model(const ast::Item& decl, std::int64_t in_dim, Span span);
  rt::Tensor forward_layers(const std::vector<Layer>& layers, rt::Tensor x);
  rt::Value model_forward(const ast::Item& decl, const rt::Value& input, Span span);
  rt::Value eval_modelo_call(const ast::Expr& call, Env& env);
  void run_treino(const ast::Item& decl);

  // LLM + RAG.
  rt::LlmConfig llm_config(const std::string& name, Span span);
  rt::Value eval_perguntar(const ast::Expr& call, Env& env);
  rt::Value structured_from_tipo(const std::string& tipo_name, const std::string& raw, Span span);
  rt::Value eval_indice_method(const std::string& indice_name, const std::string& method,
                               const ast::Expr& call, Env& env);

  // Agents.
  rt::Value run_tool(const ast::Item& tool_decl, const rt::ValueMap& args, Span span);
  rt::Value eval_agente_responder(const std::string& agent_name, const ast::Expr& call, Env& env);
  rt::Value eval_equipe_call(const std::string& team_name, const ast::Expr& call, Env& env);

  const ast::Program& program_;
  DiagnosticEngine& diag_;
  std::ostream& out_;

  Env root_;
  std::unordered_map<std::string, const ast::Item*> functions_;
  // Escopo do modulo de origem de cada funcao trazida por `de m importar f`
  // (nullptr = funcao declarada no programa principal).
  std::unordered_map<const ast::Item*, std::shared_ptr<Module>> func_module_;
  std::unordered_map<std::string, std::shared_ptr<Module>> modules_;  // por nome
  std::unordered_map<std::string, std::shared_ptr<Module>> modules_by_path_;
  std::unordered_set<std::string> loading_modules_;  // guarda de ciclo (caminhos)
  std::string entry_dir_;                          // dir do arquivo principal
  std::unordered_map<std::string, const ast::Item*> entities_;
  std::vector<const ast::Item*> pipelines_;
  std::unordered_map<std::string, std::vector<Layer>> model_cache_;
  std::unordered_map<const ast::Item*, std::shared_ptr<vm::Chunk>> vm_chunks_;  // null = not compilable
  std::unordered_map<std::string, rt::MemoryIndex> index_stores_;
  std::unordered_map<std::string, std::string> agent_memory_;  // memoria: conversa
  // Streaming (`janela:`) por pipeline: offset de elementos ja consumidos da
  // fonte, buffer de pendentes e relogio da ultima execucao dos passos. Para
  // janela de contagem sobre fonte de arquivo (csv/json) o offset persiste em
  // `<caminho>.tilt-offset` (mapa por pipeline), salvo quando avanca.
  struct WindowState {
    std::size_t offset = 0;
    std::vector<rt::Value> buffer;
    bool ran_once = false;
    std::time_t last_run = 0;
    std::size_t persisted_offset = 0;  // ultimo offset gravado no arquivo
    bool offset_loaded = false;        // arquivo de offset ja foi consultado
  };
  std::unordered_map<std::string, WindowState> window_states_;
  // Offset persistente da janela de contagem: resolve o arquivo
  // `<fonte>.tilt-offset` quando a fonte e baseada em arquivo (csv/json)
  // — vazio para os demais conectores. Gravacao atomica (tmp + rename).
  std::string janela_offset_file(const std::string& fonte);
  void janela_offset_load(WindowState& st, const std::string& pipeline,
                          const std::string& offset_file);
  void janela_offset_save(WindowState& st, const std::string& pipeline,
                          const std::string& offset_file);
  bool gpu_announced_ = false;  // printed the backend banner once

  // Serializam caches/armazenamento mutavel compartilhado entre as threads
  // do pool de rotas (ver serve()): compilacao lazy de chunks/modelos,
  // indice em memoria e memoria de conversa dos agentes.
  std::mutex vm_chunks_mutex_;
  std::mutex model_cache_mutex_;
  std::mutex index_stores_mutex_;
  std::mutex agent_memory_mutex_;
  std::mutex log_mutex_;  // linhas de log do handler de requisicoes

  // Execucao de 'se': informa ao 'exec_block' se algum ramo foi tomado, para
  // ele parear um 'senao:' solto (item de campo em 'passos:') com o 'se'
  // anterior. Fora desse par, 'senao' executa incondicionalmente (legado).
  // Estado por thread: rotas paralelas nao podem interferir uma na outra.

  bool schedule_mode_ = false;
};

}  // namespace tilt
