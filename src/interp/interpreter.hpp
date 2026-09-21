#pragma once

#include <atomic>
#include <cstdint>
#include <ctime>
#include <functional>
#include <iosfwd>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common/source.hpp"
#include "diagnostics/diagnostic.hpp"
#include "parser/ast.hpp"
#include "runtime/gpu_runtime.hpp"
#include "runtime/llm.hpp"
#include "runtime/tensor.hpp"
#include "runtime/value.hpp"
#include "runtime/vectorstore.hpp"
#include "vm/bytecode.hpp"
#include "vm/bytecode_cache.hpp"
#include "vm/jit.hpp"

namespace tilt::rt {

// Funcao anonima (`funcao x: x * 2`): o corpo (no AST, que vive mais que o valor)
// e as variaveis visiveis na criacao, capturadas por valor.
struct Closure {
  const ast::Expr* lambda = nullptr;
  std::unordered_map<std::string, Value> capturadas;
  // Tabela de funcoes do modulo onde a lambda nasceu (nullptr fora de modulo).
  const std::unordered_map<std::string, const ast::Item*>* funcs = nullptr;
};

}  // namespace tilt::rt

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
  // ML classico (experimento, 1a passada): modelo ajustado que fica em
  // memoria para `experimento Nome.prever <mapa>`. Publico para os helpers
  // livres de pre-processamento em interpreter.cpp.
  struct ExpModel {
    std::string kind;  // regressao_linear | regressao_logistica | knn | kmeans |
                       // floresta_aleatoria | gradiente_impulsionado | svm
    bool classificacao = true;
    std::vector<std::string> numericas;  // atributos numericos (ordem)
    std::vector<std::string> quentes;    // atributos categoricos (um_de_n, ordem)
    // Padronizacao por atributo numerico (media/desvio do treino; desvio 0 =
    // constante, vira 0). So usada quando `padronizar:` a lista.
    std::vector<double> medias;
    std::vector<double> desvios;
    std::vector<char> usa_std;
    // Padronizacao interna (logistica/knn/svm precisam de escala): por posicao
    // final do vetor de atributos, aplicada depois dos passos do usuario.
    std::vector<double> imedias;
    std::vector<double> idesvios;
    // Imputacao (`- imputar: [cols]`): media (numerica) ou moda (categorica)
    // ajustada no treino e aplicada em treino/teste/prever.
    std::vector<std::string> imputar_cols;
    std::map<std::string, double> imputar_num;
    std::map<std::string, std::string> imputar_cat;
    // Categorias por coluna quente (ordem de aparição no treino).
    std::map<std::string, std::vector<std::string>> categorias;
    // Classes (ordem de aparição no treino) para classificacao.
    std::vector<rt::Value> classes;
    // Pesos: linear (w + bias no fim) e logistica binaria/svm (w + bias no fim,
    // no espaco padronizado interno); logistica multinomial usa `pesos_multi`
    // ((f+1) por classe); GBM usa `pesos` = {F0} + `arvores`. knn: base de
    // treino; kmeans: centroides.
    std::vector<double> pesos;
    std::vector<double> pesos_multi;
    std::vector<std::vector<double>> base_x;
    std::vector<int> base_y;
    std::vector<double> base_yr;
    std::vector<std::vector<double>> centroides;
    int vizinhos = 5;
    // Arvores (floresta_aleatoria e gradiente_impulsionado).
    struct NoArvore {
      bool folha = true;
      int atributo = -1;
      double limiar = 0.0;
      double valor = 0.0;  // folha: id de classe ou valor de regressao
      int esq = -1;
      int dir = -1;
    };
    struct Arvore {
      std::vector<NoArvore> nos;
    };
    std::vector<Arvore> arvores;
    int arvores_n = 50;
    int profundidade = 0;  // 0 = padrao por modelo
    double taxa_gbm = 0.1;
    double custo_svm = 1.0;
  };

  Interpreter(const ast::Program& program, DiagnosticEngine& diag, std::ostream& out);

  // `tilt executar --agendar`: acknowledge `agenda:` cron on pipelines.
  void set_schedule_mode(bool on) { schedule_mode_ = on; }

  // Diretorio do arquivo principal (entrada do CLI); modulos `importar x`
  // sao resolvidos primeiro como `<dir>/x.tilt` e depois na stdlib.
  void set_entry_dir(std::string dir) { entry_dir_ = std::move(dir); }

  // Returns 0 on success, 1 if a runtime error was reported.
  int run();

  // Resultado de um bloco `teste nome:` (ver run_testes).
  struct ResultadoTeste {
    std::string nome;
    bool ok = true;
    std::string mensagem;  // erro T901 (com linha) quando !ok
  };
  // `tilt repl`: executa instrucoes (passos) num ambiente que persiste entre as
  // chamadas. Com `eco`, o valor de uma expressao solta e impresso (se nao for
  // nulo). Devolve false com a mensagem em `erro` (T901 com a linha). O `Program`
  // dos passos e das declaracoes deve viver ate o fim da sessao.
  bool repl_executar(const ast::Block& passos, bool eco, std::string& erro);
  // Registra declaracoes de topo (funcao, tipo, llm, importar, seja...) de um
  // Program novo; lanca std::runtime_error com a mensagem se falhar.
  void repl_registrar(const ast::Program& programa);

  // `tilt rpc`: expoe funcoes e pipelines do programa a outros processos.
  // preparar_chamadas registra as declaracoes (uma vez); false + `erro` se falhar.
  bool preparar_chamadas(std::string& erro);
  struct FuncaoPublica {
    std::string nome;
    std::vector<std::string> params;
  };
  // Funcoes de topo (sem prefixo `_`) e nomes de pipelines, ordenados.
  std::vector<FuncaoPublica> funcoes_publicas() const;
  std::vector<std::string> pipelines_publicos() const;
  // Chama `nome` com args posicionais e depois nomeados (mapeados pelo nome do
  // parametro). false + `erro` (com a linha) em falha.
  bool chamar_por_nome(const std::string& nome, std::vector<rt::Value> args,
                       const std::vector<std::pair<std::string, rt::Value>>& nomeados,
                       rt::Value& resultado, std::string& erro);
  bool rodar_pipeline_por_nome(const std::string& nome, std::string& erro);

  // `tilt testar`: roda os blocos `teste` do programa (na ordem do arquivo) cujo
  // nome contem `filtro` (vazio = todos). Cada teste roda isolado: uma falha
  // (afirmar, erro de execucao) nao interrompe os demais. Registra funcoes e
  // declaracoes antes; nao roda pipelines nem treinos.
  // `apos_cada` (opcional) e chamado logo depois de cada teste, para o chamador
  // recolher a saida produzida por ele.
  std::vector<ResultadoTeste> run_testes(
      const std::string& filtro, const std::function<void(const ResultadoTeste&)>& apos_cada = {});

  // `tilt executar --agendar`: loop forever (or TILT_AGENDAR_MAX runs with the
  // TILT_AGORA fake clock) firing each pipeline at its `agenda:` cron.
  int run_scheduled();

  // `tilt executar --vm`: roda cada pipeline pelo bytecode VM quando o corpo
  // esta no subconjunto compilavel; cai de volta para o interpretador de
  // arvore por pipeline quando nao esta. Saida identica a run().
  int run_vm();
  // `tilt executar --jit`: tenta emitir codigo nativo em runtime para cada
  // chunk inteiro; bytecode fora desse subconjunto cai na VM automaticamente.
  int run_jit();
  // Hook de CallFunc da VM: `ler_csv` via runtime; o resto, funcoes de
  // usuario (com escopo de modulo). *handled=false = nome desconhecido.
  rt::Value vm_call_hook(const std::string& name, std::vector<rt::Value>& args, bool* handled);

  // Serves the first `servico` declaration. `max_requests <= 0` runs forever.
  // `threads` <= 0 picks a default (min(4, cores)); 1 runs route handling
  // serially; > 1 runs it on a worker pool (handler must be reentrant).
  int serve(int port_override, int max_requests, int threads);

 private:
  // Estado da quarentena: caminho do JSONL + contador (com mutex, pois
  // passos com timeout podem anexar de threads destacadas).
  struct QuarentenaState {
    std::string caminho;
    std::size_t n = 0;
    std::mutex mu;
  };

  struct Env {
    std::unordered_map<std::string, rt::Value> vars;
    // Tabela de funcoes do modulo dono deste escopo (scope de Module), para
    // chamadas entre funcoes do mesmo modulo sem prefixo. nullptr fora de
    // modulo.
    const std::unordered_map<std::string, const ast::Item*>* funcs = nullptr;
    Env* parent = nullptr;
    // Quarentena (dead-letter) herdada pela cadeia: `para cada` desvia a
    // linha que falha para o arquivo em vez de abortar. Compartilhado
    // (também entre tentativas e threads de timeout).
    std::shared_ptr<QuarentenaState> quarentena;

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
  // `parar` / `continuar`: lancados por exec_stmt e consumidos pelo laco mais
  // interno (o checker garante que so aparecem dentro de um).
  struct BreakSignal {};
  struct ContinueSignal {};
  struct RuntimeAbort {
    Span span;
    std::string message;
    DiagCode code;
    std::vector<std::string> notes;
  };

  [[noreturn]] void fail(Span span, std::string message,
                         DiagCode code = DiagCode::RuntimeError);

  void register_decls();
  void register_decls_de(const ast::Program& programa);
  Env repl_env_;  // ambiente persistente do REPL (pai = root_ no primeiro uso)
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
  // Deadline por passo de topo (`tempo_limite:` do pipeline): quando `prazo`
  // é dado, cada item do bloco roda numa thread com esse teto (T901 ao
  // estourar; a thread segue destacada). Chamadas internas passam nulo.
  // `dono` mantém o Env vivo para a thread destacada (vazamento deliberado
  // e limitado, via lista estática de zumbis).
  struct PrazoPasso {
    std::time_t segundos = 0;
    std::shared_ptr<Env> dono;
  };
  void exec_block(const ast::Block& block, Env& env, const PrazoPasso* prazo = nullptr);
  void exec_item(const ast::Item& item, Env& env);
  void exec_stmt(const ast::Stmt& stmt, Env& env);
  rt::Value* lookup_lvalue(const ast::Expr& target, Env& env);

  rt::Value eval(const ast::Expr& expr, Env& env);
  rt::Value eval_binary(const ast::Expr& expr, Env& env);
  rt::Value eval_call(const ast::Expr& expr, Env& env);
  rt::Value eval_builtin(const std::string& name, const ast::Expr& call, Env& env);
  rt::Value eval_method(const std::string& method, rt::Value receiver, const ast::Expr& call,
                        Env& env);
  rt::Value call_function(const ast::Item& fn, std::vector<rt::Value> args, Span span,
                          Env* module_scope = nullptr);

  std::vector<rt::Value> eval_args(const ast::Expr& call, Env& env);
  rt::Value call_closure(const rt::Closure& fn, std::vector<rt::Value> args, Span span);
  // Chunk de bytecode da funcao (compila/le do cache na 1a vez); nullptr se ela nao esta
  // no subconjunto da VM. O ponteiro vive tanto quanto o interpretador (vm_chunks_).
  std::shared_ptr<vm::Chunk> chunk_de_funcao(const ast::Item& fn);
  // Resolvedor para chamadas VM -> VM diretas (nome -> Chunk), ou nullptr.
  const vm::Chunk* resolver_chunk(const std::string& nome);
  rt::ValueMap eval_kwargs(const ast::Expr& call, Env& env);
  // `particionar_por:` como texto ou lista de textos (particao composta).
  std::vector<std::string> parse_particionar_por(const rt::ValueMap& kw, const char* builtin,
                                                 const Span& span);
  std::string interpolate(const std::string& text, Env& env);

  // Opcoes de leitura de CSV (`ler_csv "x.csv", separador: ";", nulos: ["NA"], ...`).
  struct CsvOpcoes {
    char separador = ',';
    bool detectar_separador = false;   // separador: "auto"
    bool cabecalho = true;             // sem_cabecalho: verdadeiro -> false
    std::vector<std::string> colunas;  // nomes das colunas (substituem/definem o cabecalho)
    std::size_t pular = 0;             // linhas ignoradas no inicio
    std::vector<std::string> nulos;    // textos lidos como nulo (ex.: "NA", "-")
  };
  rt::Value read_csv_file(const std::string& path, Span span, const CsvOpcoes* opcoes = nullptr);
  rt::Value read_fonte(const std::string& name, Span span);

  // Deep learning.
  struct Layer {
    enum Kind {
      Dense,
      Residual,
      Embedding,
      Recorrente,
      Activation,
      Softmax,
      Dropout,
      LayerNorm,
      Conv2d,
      NormaLote,
      Flatten,
      MaxPool
    } kind = Dense;
    rt::Tensor w;
    rt::Tensor b;
    std::string act;
    // Embedding: tabela [vocabulario, dimensao].
    std::int64_t vocabulario = 0;
    std::int64_t dimensao = 0;
    // Recorrente: W [entrada, portas*oculta], U [oculta, portas*oculta].
    rt::Tensor u;
    std::string recorrente_tipo;
    std::int64_t oculta = 0;
    // Adam moment estimates (allocated lazily during training).
    rt::Tensor m_w, v_w, m_b, v_b, m_u, v_u;
    // Running statistics for NormaLote (populated during training, used in inference).
    rt::Tensor media_running, var_running;
    // Batch statistics from the most recent training forward (per BN layer).
    rt::Tensor bn_media, bn_var;
    // Conv2d: kernel shape [C_out, C_in, KH, KW]
    std::int64_t passo = 1;
    std::int64_t padding = 0;    // Conv2d: borda zero simetrica
    std::int64_t dilatacao = 1;  // Conv2d: espacamento do nucleo
    // MaxPool: janela JxJ (passo uses `passo`, default = janela).
    std::int64_t janela = 0;
    // Flatten: largura apos achatar (para exportacao ONNX).
    std::int64_t plano = 0;
    // NormaLote: nome identificador
    std::string nome_norma_lote;
    // Dropout (`abandono: p`): probabilidade de zerar um valor no treino
    // (inverted dropout: os mantidos sao escalados por 1/(1-p)); identidade na
    // inferencia.
    float taxa_abandono = 0.5F;
  };
  rt::Tensor value_to_tensor(const rt::Value& v, Span span);
  void set_device(const ast::Item& decl);  // reads `dispositivo:` -> gpu on/off
  rt::Tensor mm(const rt::Tensor& a, const rt::Tensor& b);
  rt::Tensor act_relu(const rt::Tensor& x);
  std::vector<Layer> build_layers(const ast::Item& model_decl, std::int64_t in_dim,
                                 std::uint64_t seed_inicial = 0xC1A5);
  static bool camada_com_pesos(Layer::Kind kind);
  const std::vector<Layer>& build_model(const ast::Item& decl, std::int64_t in_dim, Span span);
  rt::Tensor forward_layers(const std::vector<Layer>& layers, rt::Tensor x);
  rt::Value model_forward(const ast::Item& decl, const rt::Value& input, Span span);
  rt::Value eval_modelo_call(const ast::Expr& call, Env& env);
  void run_treino(const ast::Item& decl);
  // Treino reutilizavel (treino avulso + busca em grade).
  struct TreinoCfg {
    std::string perda = "entropia_cruzada";
    std::string otim = "sgd";
    double lr = 0.1;
    int epocas = 50;
    int lote = -1;  // -1 = lote cheio
    int shard_id = 0;       // indice desta particao de dados
    int num_shards = 1;     // total de particoes
    std::string cluster_dir;
    int cluster_rank = 0;
    int cluster_world = 1;
    int cluster_timeout = 120;
    bool embaralhar = true;
    std::uint64_t seed_init = 0xC1A5;
    std::uint64_t seed_mistura = 7;
    double f_val = 0.0;
    int paciencia = 0;
    double melhorar_min = 0.0;
    std::string agenda_tipo = "constante";
    int degrau_a_cada = 10;
    double degrau_fator = 0.5;
    std::string checkpoint;
    std::string retomar;
    int a_cada = 0;
    const ast::Block* ao_epoca = nullptr;
    bool verbose = false;
    bool silencioso = false;
    // Dataloader streaming: quando `fluxo_csv` nao vazio, `x` vem em blocos
    // do arquivo em vez da RAM (`fluxo_n` linhas, `fluxo_f` atributos).
    std::string fluxo_csv;
    std::string fluxo_alvo;
    std::vector<std::string> fluxo_atributos;
    std::vector<std::int64_t> fluxo_desloc;  // CSV: byte offset de cada linha de dados
    bool fluxo_parquet = false;
    std::int64_t fluxo_n = 0;
    std::int64_t fluxo_f = 0;
    std::int64_t bloco = 1024;
  };
  struct TreinoRelato {
    float primeira = 0.0F;
    float ultima = 0.0F;
    int acertos = 0;
    int total = 0;
    int melhor_epoca = 0;
    int epocas_feitas = 0;
    bool parou_cedo = false;
    bool classificacao = true;
    std::vector<Layer> camadas;
  };
  // Dados avaliados de um bloco treino/busca: tensor RAM ou descritor de
  // fluxo CSV (neste caso `x` vazio, `yf` com os rotulos da varredura).
  struct DadosTreino {
    rt::Tensor x;
    std::vector<double> yf;
    std::int64_t n = 0;
  };
  // Le os campos de configuracao de um bloco treino/busca.
  void ler_cfg_treino(const ast::Block& cfg, std::int64_t n, const std::string& ctx, Span span,
                      TreinoCfg& out);
  // Avalia 'dados:' (-> RAM ou fluxo CSV) e preenche cfg.fluxo_* quando for fluxo.
  DadosTreino ler_dados_treino(const ast::Expr& expr_dados, const std::string& ctx, Span span,
                               TreinoCfg& cfg);
  // Nucleo do treino: ajusta, imprime (salvo silencioso) e devolve relato + camadas.
  TreinoRelato treinar_nucleo(const ast::Item& modelo_decl, rt::Tensor x, std::vector<double> yf,
                              const TreinoCfg& cfg, const std::string& ctx, Span span);
  void run_busca(const ast::Item& decl);

 private:
  void run_experimento(const ast::Item& decl);
  void run_avaliacao(const ast::Item& decl);
  rt::Value eval_experimento_call(const ast::Expr& call, Env& env);
  rt::Value experimento_prever(const std::string& nome, const rt::Value& entrada, Span span);

  // LLM + RAG.
  rt::LlmConfig llm_config(const std::string& name, Span span);
  // Primario + reservas (fallback em ordem).
  std::vector<rt::LlmConfig> cadeia_llm(const std::string& name, Span span);
  rt::Value eval_perguntar(const ast::Expr& call, Env& env, bool fluxo = false);
  rt::Value structured_from_tipo(const std::string& tipo_name, const std::string& raw, Span span);
  // Valor padrao de campo de `tipo`: avalia `campo: Tipo = padrao` (com o
  // escopo raiz) ou cai para o padrao do tipo declarado quando ausente/falha.
  rt::Value field_default(const ast::Item& field);
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
  std::unordered_map<std::string, ExpModel> experimentos_;
  std::mutex experimentos_mutex_;
  // Envs de passos estourados (timeout): a thread destacada segue com o Env
  // vivo até terminar ou até o fim do processo.
  static std::mutex zumbis_mu_;
  static std::vector<std::shared_ptr<Env>> zumbis_;
  std::unordered_map<const ast::Item*, std::shared_ptr<vm::Chunk>> vm_chunks_;  // null = not compilable
  // Cache .tiltc em disco (Fase 6): chunks por "pipeline N"/"funcao N",
  // chaveado pelo sha do fonte. Carregado em register_decls, descarregado
  // (se sujo) no fim de run()/run_vm(). TILT_VM_NOCACHE=1 desliga.
  vm::CachedProgram tiltc_prog_;
  std::string tiltc_path_;
  bool tiltc_loaded_ = false;
  bool tiltc_dirty_ = false;
  bool jit_mode_ = false;
  void tiltc_load();
  void tiltc_flush();
  void tiltc_note(const char* what);
  std::unordered_map<std::string, rt::MemoryIndex> index_stores_;
  std::unordered_map<std::string, std::string> agent_memory_;  // memoria: conversa
  // memoria: vetorial — um indice por agente (top-3 recuperado no prompt;
  // sem poda: acima de kMaxMemoriaTurnos, turnos novos nao entram).
  std::unordered_map<std::string, rt::MemoryIndex> agent_vector_memory_;
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
    bool cursor_active = false;
    bool cursor_loaded = false;
    rt::Value cursor_watermark;  // ultimo cursor consumido e persistido
    rt::Value cursor_observed;   // maior cursor lido no processo atual
    bool cursor_observed_valid = false;
    bool backfill_loaded = false;
  };
  std::unordered_map<std::string, WindowState> window_states_;
  // Offset persistente da janela: resolve o arquivo `<fonte>.tilt-offset`
  // quando a fonte e baseada em arquivo (csv/json/parquet) — vazio para os demais
  // conectores. Gravacao atomica (tmp + rename). Fase 12-4: com
  // TILT_CHECKPOINT_DIR o arquivo mora no diretorio compartilhado; janelas
  // de tempo/throttle tambem persistem `last_run` (com_relogio).
  std::string janela_offset_file(const std::string& fonte);
  void janela_offset_load(WindowState& st, const std::string& pipeline,
                          const std::string& offset_file);
  void janela_offset_save(WindowState& st, const std::string& pipeline,
                          const std::string& offset_file, bool com_relogio = false);
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

  // Metricas do servir (`metricas: verdadeiro`): contadores e latencia por
  // rota, protegidos por mutex (workers concorrentes).
  struct MetricasRota {
    long long total = 0;
    long long erros = 0;
    long long latencia_total_us = 0;
    long long latencia_max_us = 0;
  };
  struct MetricasServico {
    std::string inicio;
    long long requisicoes = 0;
    long long erros = 0;
    std::map<std::string, MetricasRota> por_rota;
  };
  MetricasServico metricas_;
  std::mutex metricas_mutex_;
  std::atomic<std::uint64_t> proximo_trace_id_{1};

  bool schedule_mode_ = false;
};

}  // namespace tilt
