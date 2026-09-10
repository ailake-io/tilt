#include "interp/interpreter.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <exception>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <memory>
#include <ostream>
#include <sstream>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>

#include "lexer/lexer.hpp"
#include "parser/parser.hpp"
#include "runtime/gpu_runtime.hpp"
#include "runtime/http_server.hpp"
#include "runtime/json.hpp"
#include "runtime/llm.hpp"
#include "runtime/compat.hpp"
#include "runtime/parquet.hpp"
#include "runtime/delta.hpp"
#include "runtime/iceberg.hpp"
#include "runtime/qdrant.hpp"
#include "runtime/redis.hpp"
#include "runtime/kafka.hpp"
#include "runtime/mongo.hpp"
#include "runtime/s3.hpp"
#include "runtime/sqlite.hpp"
#include "runtime/postgres.hpp"
#include "runtime/pgvector.hpp"
#include "runtime/vectorstore.hpp"
#include "semantic/checker.hpp"
#include "vm/compiler.hpp"
#include "vm/vm.hpp"

namespace tilt {

// Estado por thread do pool de rotas (serve()): cada worker executa uma
// rota de ponta a ponta na sua thread, entao a resposta corrente, o flag de
// 'se' e o dispositivo ativo nunca sao compartilhados entre requisicoes.
thread_local RouteResponse* route_resp_ = nullptr;  // non-null only while handling a request
thread_local bool last_if_taken_ = false;
thread_local bool use_gpu_ = false;  // active for the current model/treino call

using ast::Expr;
using ast::ExprKind;
using ast::Item;
using ast::ItemKind;
using ast::Stmt;
using ast::StmtKind;
using rt::Value;
using rt::ValueKind;

namespace {

constexpr std::int64_t kLoopGuard = 5'000'000;

bool word_in(std::string_view w, std::initializer_list<std::string_view> set) {
  for (std::string_view s : set) {
    if (w == s) return true;
  }
  return false;
}

std::string decl_name(const Item& it) {
  if (!it.header.empty() && it.header[0] && it.header[0]->kind == ExprKind::Name) {
    return it.header[0]->text;
  }
  return {};
}

const Item* find_field(const ast::Block& block, std::string_view key) {
  for (const auto& it : block.items) {
    if (it && it->kind == ItemKind::Field && it->key == key) return it.get();
  }
  return nullptr;
}

Value parse_scalar(const std::string& cell) {
  if (!cell.empty()) {
    char* end = nullptr;
    long long asi = std::strtoll(cell.c_str(), &end, 10);
    if (end && *end == '\0') return Value::inteiro(asi);
    double asd = std::strtod(cell.c_str(), &end);
    if (end && *end == '\0') return Value::decimal(asd);
  }
  return Value::texto(cell);
}

std::vector<std::string> split_csv_line(const std::string& line) {
  std::vector<std::string> out;
  std::string field;
  bool quoted = false;
  for (std::size_t k = 0; k < line.size(); ++k) {
    char c = line[k];
    if (quoted) {
      if (c == '"' && k + 1 < line.size() && line[k + 1] == '"') {
        field += '"';
        ++k;
      } else if (c == '"') {
        quoted = false;
      } else {
        field += c;
      }
    } else if (c == '"') {
      quoted = true;
    } else if (c == ',') {
      out.push_back(field);
      field.clear();
    } else if (c != '\r') {
      field += c;
    }
  }
  out.push_back(field);
  return out;
}

}  // namespace

// ------------------------------------------------------------------ Env

Value* Interpreter::Env::lookup(const std::string& name) {
  for (Env* e = this; e; e = e->parent) {
    auto it = e->vars.find(name);
    if (it != e->vars.end()) return &it->second;
  }
  return nullptr;
}

void Interpreter::Env::set(const std::string& name, Value value) {
  for (Env* e = this; e; e = e->parent) {
    auto it = e->vars.find(name);
    if (it != e->vars.end()) {
      it->second = std::move(value);
      return;
    }
  }
  vars[name] = std::move(value);
}

// ------------------------------------------------------------------ setup

Interpreter::Interpreter(const ast::Program& program, DiagnosticEngine& diag, std::ostream& out)
    : program_(program), diag_(diag), out_(out) {}

void Interpreter::fail(Span span, std::string message, DiagCode code) {
  throw RuntimeAbort{span, std::move(message), code, {}};
}

void Interpreter::register_decls() {
  for (const auto& item : program_.items) {
    if (!item || item->kind != ItemKind::Decl) continue;
    const std::string& kw = item->key;
    const std::string name = decl_name(*item);
    if (kw == "seja" || kw == "constante") {
      if (!name.empty()) root_.vars[name] = item->value ? eval(*item->value, root_) : Value::nulo();
    } else if (kw == "funcao") {
      if (!name.empty()) functions_[name] = item.get();
    } else if (kw == "pipeline") {
      pipelines_.push_back(item.get());
    } else if (kw == "treino") {
      // handled by the dedicated training loop; must not shadow `modelo <name>`
    } else if (!name.empty()) {
      entities_[name] = item.get();
    }
  }
}

namespace {

// Extracts N from `ao_falhar: repetir N[, espera: "..."]`. Returns 0 if absent.
int retry_count(const Item& pipeline) {
  const Item* f = find_field(*pipeline.block, "ao_falhar");
  if (!f || !f->value) return 0;
  const Expr* v = f->value.get();
  if (v->kind == ExprKind::Call && v->lhs && v->lhs->kind == ExprKind::Name &&
      v->lhs->text == "repetir" && !v->args.empty() &&
      v->args[0].value->kind == ExprKind::IntLit) {
    return static_cast<int>(std::strtol(v->args[0].value->text.c_str(), nullptr, 10));
  }
  return 0;
}

// ------------------------------------------------------------------ janela
// `janela: N` (IntLit) e janela de contagem; `janela: "30s"` / `"5min"` /
// `"1h"` e janela de tempo (throttle quando sem `entrada:`).
struct JanelaSpec {
  enum Kind { Contagem, Tempo } kind = Contagem;
  long count = 0;         // Contagem: elementos por execucao
  std::time_t dur = 0;    // Tempo: duracao em segundos
  bool valid = false;
  std::string erro;       // motivo da invalidade (mensagem para fail())
};

// "30s" | "5min" | "1h" -> segundos. Falso para qualquer outro formato.
bool parse_duracao(const std::string& s, std::time_t& out) {
  std::size_t i = 0;
  while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
  if (i == 0 || i == s.size()) return false;
  const long n = std::strtol(s.substr(0, i).c_str(), nullptr, 10);
  if (n <= 0) return false;
  const std::string suf = s.substr(i);
  long mult;
  if (suf == "s") mult = 1;
  else if (suf == "min") mult = 60;
  else if (suf == "h") mult = 3600;
  else return false;
  out = static_cast<std::time_t>(n * mult);
  return true;
}

JanelaSpec parse_janela(const Item& field) {
  JanelaSpec spec;
  const Expr* v = field.value.get();
  auto rejeita = [&](std::string msg) {
    spec.valid = false;
    spec.erro = std::move(msg);
    return spec;
  };
  if (!v) return rejeita("espera um inteiro (contagem) ou uma duracao (\"30s\", \"5min\", \"1h\")");
  if (v->kind == ExprKind::IntLit) {
    spec.kind = JanelaSpec::Contagem;
    spec.count = std::strtol(v->text.c_str(), nullptr, 10);
    if (spec.count <= 0) return rejeita("contagem precisa ser um inteiro positivo");
    spec.valid = true;
    return spec;
  }
  if (v->kind == ExprKind::TextLit && parse_duracao(v->text, spec.dur)) {
    spec.kind = JanelaSpec::Tempo;
    spec.valid = true;
    return spec;
  }
  return rejeita("valor invalido; use um inteiro (contagem) ou duracao \"30s\", \"5min\", \"1h\"");
}

// ------------------------------------------------------------------ cron
// Subconjunto de cron de 5 campos: minuto hora dia-do-mes mes dia-da-semana.
// Cada campo aceita: * | n | a-b | a-b/n | */n | listas com virgula.
struct CronRange {
  int a, b, step;
  bool matches(int v) const { return v >= a && v <= b && (v - a) % step == 0; }
};

struct CronField {
  bool any = false;
  bool is_dow = false;
  std::vector<CronRange> ranges;

  bool matches(int v) const {
    if (any) return true;
    for (const CronRange& r : ranges) {
      if (r.matches(v)) return true;
    }
    // domingo: 0 e 7 sao equivalentes
    if (is_dow && v == 0) {
      for (const CronRange& r : ranges) {
        if (r.matches(7)) return true;
      }
    }
    return false;
  }
};

bool parse_cron_field(const std::string& s, int lo, int hi, bool is_dow, CronField& out) {
  out = CronField{};
  out.is_dow = is_dow;
  if (s == "*") {
    out.any = true;
    return true;
  }
  std::size_t pos = 0;
  while (pos <= s.size()) {
    const std::size_t comma = s.find(',', pos);
    const std::string part =
        s.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
    if (part.empty()) return false;
    int step = 1;
    std::string range = part;
    if (const std::size_t slash = part.find('/'); slash != std::string::npos) {
      step = std::atoi(part.substr(slash + 1).c_str());
      range = part.substr(0, slash);
    }
    int a, b;
    if (range == "*") {
      a = lo;
      b = hi;
    } else if (const std::size_t dash = range.find('-'); dash != std::string::npos) {
      a = std::atoi(range.substr(0, dash).c_str());
      b = std::atoi(range.substr(dash + 1).c_str());
    } else {
      a = b = std::atoi(range.c_str());
    }
    if (step < 1 || a < lo || a > b || b > hi) return false;
    out.ranges.push_back({a, b, step});
    if (comma == std::string::npos) break;
    pos = comma + 1;
  }
  return !out.ranges.empty();
}

struct CronSpec {
  CronField min, hour, dom, mon, dow;
  bool valid = false;
};

CronSpec parse_cron(const std::string& expr) {
  CronSpec c;
  std::vector<std::string> parts;
  std::size_t pos = 0;
  while (pos <= expr.size()) {
    const std::size_t sp = expr.find_first_of(" \t", pos);
    parts.push_back(expr.substr(pos, sp == std::string::npos ? std::string::npos : sp - pos));
    if (sp == std::string::npos) break;
    pos = expr.find_first_not_of(" \t", sp);
    if (pos == std::string::npos) break;
  }
  c.valid = parts.size() == 5 && parse_cron_field(parts[0], 0, 59, false, c.min) &&
            parse_cron_field(parts[1], 0, 23, false, c.hour) &&
            parse_cron_field(parts[2], 1, 31, false, c.dom) &&
            parse_cron_field(parts[3], 1, 12, false, c.mon) &&
            parse_cron_field(parts[4], 0, 7, true, c.dow);
  return c;
}

std::time_t next_cron_fire(const CronSpec& c, std::time_t after) {
  // arredonda para o inicio do proximo minuto
  std::time_t t = after + 60;
  std::tm local = rt::tilt_localtime(t);
  local.tm_sec = 0;
  t = std::mktime(&local);
  for (int i = 0; i < 366 * 24 * 60; ++i, t += 60) {
    std::tm cur = rt::tilt_localtime(t);
    const int dow = cur.tm_wday;
    if (c.min.matches(cur.tm_min) && c.hour.matches(cur.tm_hour) &&
        c.dom.matches(cur.tm_mday) && c.mon.matches(cur.tm_mon + 1) && c.dow.matches(dow)) {
      return t;
    }
  }
  return -1;
}

std::string format_local(std::time_t t) {
  const std::tm local = rt::tilt_localtime(t);
  char buf[64];
  std::snprintf(buf, sizeof buf, "%04d-%02d-%02d %02d:%02d", local.tm_year + 1900,
                local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min);
  return buf;
}

// Relogio fake para testes: TILT_AGORA="YYYY-MM-DDTHH:MM[:SS]" (hora local;
// tambem aceita espaco no lugar de 'T'). Com relogio fake, o loop nao dorme
// e avanca o tempo sozinho; TILT_AGENDAR_MAX limita o numero de execucoes
// (padrao 1 no modo fake).
bool fake_now(std::time_t& out) {
  const char* env = std::getenv("TILT_AGORA");
  if (!env || !*env) return false;
  std::tm local{};
  int sec = 0;
  if (std::sscanf(env, "%d-%d-%dT%d:%d:%d", &local.tm_year, &local.tm_mon, &local.tm_mday,
                  &local.tm_hour, &local.tm_min, &sec) < 5 &&
      std::sscanf(env, "%d-%d-%d %d:%d:%d", &local.tm_year, &local.tm_mon, &local.tm_mday,
                  &local.tm_hour, &local.tm_min, &sec) < 5) {
    return false;
  }
  local.tm_year -= 1900;
  local.tm_mon -= 1;
  local.tm_sec = sec;
  out = std::mktime(&local);
  return true;
}

}  // namespace
int Interpreter::run() {
  try {
    register_decls();

    bool did_something = false;
    for (const auto& item : program_.items) {
      if (item && item->kind == ItemKind::Decl && item->key == "treino") {
        run_treino(*item);
        did_something = true;
      }
    }

    if (!pipelines_.empty()) {
      for (const Item* p : pipelines_) run_pipeline(*p);
    } else if (auto it = functions_.find("principal"); it != functions_.end()) {
      call_function(*it->second, {}, it->second->span);
    } else if (!did_something) {
      out_ << "nada para executar: nenhum 'pipeline', 'treino' nem 'funcao principal'\n";
    }
    return 0;
  } catch (const RuntimeAbort& a) {
    Diagnostic d;
    d.severity = Severity::Error;
    d.code = a.code;
    d.span = a.span;
    d.message = a.message;
    d.notes = a.notes;
    diag_.report(std::move(d));
    return 1;
  }
}

int Interpreter::run_vm() {
  try {
    register_decls();

    bool did_something = false;
    for (const auto& item : program_.items) {
      if (item && item->kind == ItemKind::Decl && item->key == "treino") {
        run_treino(*item);
        did_something = true;
      }
    }

    if (!pipelines_.empty()) {
      std::unordered_set<std::string> names;
      for (const auto& kv : functions_) names.insert(kv.first);
      for (const Item* p : pipelines_) {
        // Pipeline no subconjunto -> bytecode VM; fora dele -> arvore.
        std::shared_ptr<vm::Chunk> chunk;
        try {
          chunk = std::make_shared<vm::Chunk>(vm::compile_pipeline(*p, names));
        } catch (const vm::NotCompilable&) {
          chunk = nullptr;
        }
        if (!chunk) {
          run_pipeline(*p);
          continue;
        }
        out_ << "== pipeline " << decl_name(*p) << " ==\n";
        vm::Vm machine(out_, [this](const std::string& name, std::vector<Value>& a, bool* handled) {
          auto f = functions_.find(name);
          if (f == functions_.end()) {
            *handled = false;
            return Value::nulo();
          }
          *handled = true;
          return call_function(*f->second, std::move(a), Span{});
        });
        try {
          machine.run(*chunk, {});
        } catch (const std::exception& e) {
          fail(p->span, std::string("VM: ") + e.what());
        }
      }
    } else if (auto it = functions_.find("principal"); it != functions_.end()) {
      call_function(*it->second, {}, it->second->span);
    } else if (!did_something) {
      out_ << "nada para executar: nenhum 'pipeline', 'treino' nem 'funcao principal'\n";
    }
    return 0;
  } catch (const RuntimeAbort& a) {
    Diagnostic d;
    d.severity = Severity::Error;
    d.code = a.code;
    d.span = a.span;
    d.message = a.message;
    d.notes = a.notes;
    diag_.report(std::move(d));
    return 1;
  }
}

int Interpreter::run_scheduled() {
  try {
    register_decls();

    struct Scheduled {
      const Item* pipeline;
      CronSpec cron;
    };
    std::vector<Scheduled> scheduled;
    std::vector<const Item*> immediate;
    for (const Item* p : pipelines_) {
      const Item* ag = p->block ? find_field(*p->block, "agenda") : nullptr;
      if (ag && ag->value && ag->value->kind == ExprKind::TextLit) {
        CronSpec c = parse_cron(ag->value->text);
        if (!c.valid) {
          fail(ag->value->span, "agenda '" + ag->value->text + "' nao e um cron valido de 5 campos");
        }
        scheduled.push_back({p, c});
      } else {
        immediate.push_back(p);
      }
    }

    for (const Item* p : immediate) run_pipeline(*p);
    if (scheduled.empty()) {
      out_ << "nenhuma agenda definida; nada a agendar\n";
      return 0;
    }

    // treinos e funcao principal nao entram no loop; rodam uma vez antes
    for (const auto& item : program_.items) {
      if (item && item->kind == ItemKind::Decl && item->key == "treino") run_treino(*item);
    }

    std::time_t now = std::time(nullptr);
    const bool fake = fake_now(now);
    long max_runs = 0;
    if (const char* m = std::getenv("TILT_AGENDAR_MAX")) {
      max_runs = std::atol(m);
    } else if (fake) {
      max_runs = 1;
    }
    if (fake && max_runs <= 0) {
      fail({}, "TILT_AGORA definido sem TILT_AGENDAR_MAX; o relogio fake precisa de limite");
    }

    long fired_total = 0;
    while (true) {
      // proximo disparo entre todos os pipelines agendados
      std::time_t next = -1;
      for (const Scheduled& s : scheduled) {
        const std::time_t t = next_cron_fire(s.cron, now);
        if (t < 0) {
          fail({}, "agenda sem proxima ocorrencia nos proximos 366 dias");
        }
        if (next < 0 || t < next) next = t;
      }
      out_ << "proxima execucao: " << format_local(next) << "\n";
      if (!fake) {
        const std::time_t cur = std::time(nullptr);
        if (next > cur) {
          std::this_thread::sleep_for(std::chrono::seconds(next - cur));
        }
      }
      now = next;
      for (const Scheduled& s : scheduled) {
        if (next_cron_fire(s.cron, now - 60) == now) run_pipeline(*s.pipeline, now);
      }
      ++fired_total;
      if (max_runs > 0 && fired_total >= max_runs) break;
    }
    return 0;
  } catch (const RuntimeAbort& a) {
    Diagnostic d;
    d.severity = Severity::Error;
    d.code = a.code;
    d.span = a.span;
    d.message = a.message;
    d.notes = a.notes;
    diag_.report(std::move(d));
    return 1;
  }
}


void Interpreter::run_pipeline(const Item& pipeline, std::time_t now) {
  out_ << "== pipeline " << decl_name(pipeline) << " ==\n";
  if (!pipeline.block) return;

  if (const Item* ag = find_field(*pipeline.block, "agenda");
      ag && ag->value && ag->value->kind == ExprKind::TextLit) {
    const std::string& cron = ag->value->text;
    if (!parse_cron(cron).valid) {
      fail(ag->value->span, "agenda '" + cron + "' nao e um cron valido de 5 campos");
    }
  }

  const Item* passos = find_field(*pipeline.block, "passos");
  if (!passos || !passos->block) {
    out_ << "  (sem passos)\n";
    return;
  }

  // Streaming com `janela:`: acumula elementos da `entrada:` e so executa os
  // passos quando a janela fecha, com `linhas` = lote consumido.
  std::vector<Value> janela_lotes;
  const Item* janela = find_field(*pipeline.block, "janela");
  if (janela) {
    if (now < 0) {
      std::time_t t;
      now = fake_now(t) ? t : std::time(nullptr);
    }
    if (!run_janela(*janela, pipeline, now, janela_lotes)) {
      out_ << "  (janela nao fechou; passos nao executados)\n";
      return;
    }
  }

  const int retries = retry_count(pipeline);
  for (int attempt = 0; attempt <= retries; ++attempt) {
    try {
      Env env;
      env.parent = &root_;
      if (janela) env.vars["linhas"] = Value::lista(janela_lotes);
      exec_block(*passos->block, env);
      return;
    } catch (const RuntimeAbort& a) {
      if (attempt >= retries) throw;
      out_ << "[retry] passo falhou (" << a.message << "); tentativa " << (attempt + 2) << "/"
           << (retries + 1) << "\n";
    }
  }
}

bool Interpreter::run_janela(const Item& janela, const Item& pipeline, std::time_t now,
                             std::vector<Value>& batch) {
  const JanelaSpec spec = parse_janela(janela);
  if (!spec.valid) fail(janela.span, "janela: " + spec.erro);

  // `sobreposicao: M` (default 0 = tumbling): o lote mantem os M ultimos
  // elementos do lote anterior no inicio de `linhas` — janela deslizante.
  long sobreposicao = 0;
  if (const Item* sp = find_field(*pipeline.block, "sobreposicao")) {
    if (!sp->value || sp->value->kind != ExprKind::IntLit) {
      fail(sp->span, "sobreposicao: espera um inteiro (elementos mantidos entre lotes)");
    }
    sobreposicao = std::strtol(sp->value->text.c_str(), nullptr, 10);
    if (sobreposicao < 0) fail(sp->span, "sobreposicao: precisa ser >= 0");
    if (spec.kind != JanelaSpec::Contagem) {
      fail(sp->span, "sobreposicao: exige janela de contagem (janela: N inteiro)");
    }
    if (sobreposicao >= spec.count) {
      fail(sp->span, "sobreposicao: precisa ser menor que a janela");
    }
  }

  const Item* entrada = find_field(*pipeline.block, "entrada");
  std::string fonte;
  if (entrada) {
    if (!entrada->value || entrada->value->kind != ExprKind::Name) {
      fail(entrada->span, "entrada: espera o nome de uma 'fonte' declarada");
    }
    fonte = entrada->value->text;
    if (!entities_.count(fonte)) {
      fail(entrada->span, "entrada: fonte '" + fonte + "' nao declarada");
    }
  }
  if (spec.kind == JanelaSpec::Contagem && fonte.empty()) {
    fail(janela.span, "janela: contagem exige 'entrada:' (fonte)");
  }

  const std::string pipe_name = decl_name(pipeline);
  WindowState& st = window_states_[pipe_name];
  // Offset persistente: so janela de contagem com fonte de arquivo, e nunca
  // com TILT_JANELA_ESTADO=memoria (pipelines efemeros/testes).
  std::string offset_file;
  if (spec.kind == JanelaSpec::Contagem && !fonte.empty()) {
    const char* estado = std::getenv("TILT_JANELA_ESTADO");
    if (!estado || std::string(estado) != "memoria") {
      offset_file = janela_offset_file(fonte);
      if (!offset_file.empty()) janela_offset_load(st, pipe_name, offset_file);
    }
  }
  if (!fonte.empty()) {
    // Le a fonte inteira a cada tick; so os elementos alem do offset acumulam.
    Value data = read_fonte(fonte, entrada->span);
    if (data.list) {
      if (st.offset > data.list->size()) st.offset = data.list->size();  // fonte encolheu
      for (std::size_t i = st.offset; i < data.list->size(); ++i) {
        st.buffer.push_back((*data.list)[i]);
      }
      st.offset = data.list->size();
    }
    // Grava so quando o offset avanca (nada consumido = sem arquivo novo).
    if (!offset_file.empty() && st.offset > st.persisted_offset) {
      janela_offset_save(st, pipe_name, offset_file);
    }
  }

  bool roda = false;
  if (spec.kind == JanelaSpec::Contagem) {
    // Contagem nao depende do relogio: fecha com novos elementos suficientes.
    // Com sobreposicao M, o lote inteiro tem N elementos mas so (N - M) saem
    // do buffer — os M ultimos repetem no proximo lote.
    const long consome = spec.count - sobreposicao;
    if (st.buffer.size() >= static_cast<std::size_t>(spec.count)) {
      batch.assign(st.buffer.begin(), st.buffer.begin() + spec.count);
      st.buffer.erase(st.buffer.begin(), st.buffer.begin() + consome);
      roda = true;
    }
  } else {
    const bool decorreu = !st.ran_once || (now - st.last_run) >= spec.dur;
    if (fonte.empty()) {
      roda = decorreu;  // throttle: no maximo 1 execucao por duracao
    } else if (decorreu && !st.buffer.empty()) {
      batch = st.buffer;  // janela de tempo: entrega tudo que acumulou e zera
      st.buffer.clear();
      roda = true;
    }
  }
  if (roda) {
    st.ran_once = true;
    st.last_run = now;
  }
  return roda;
}

void Interpreter::run_verificar(const Item& field, Env& env) {
  const std::string var =
      (!field.header.empty() && field.header[0] && field.header[0]->kind == ExprKind::Name)
          ? field.header[0]->text
          : "";
  Value* target = env.lookup(var);
  if (!target || (target->kind != ValueKind::Tabela && target->kind != ValueKind::Lista) ||
      !target->list) {
    fail(field.span, "verificar: '" + var + "' nao e uma tabela");
  }
  const rt::ValueList& rows = *target->list;

  std::string on_violate = "abortar";
  std::vector<std::string> violations;
  auto note = [&](std::string m) {
    if (violations.size() < 5) violations.push_back(std::move(m));
  };

  if (field.block) {
    for (const auto& raw : field.block->items) {
      if (!raw) continue;
      const Item* rule = (raw->kind == ItemKind::ListEntry && raw->child) ? raw->child.get()
                                                                          : raw.get();
      if (rule->kind != ItemKind::Field) continue;
      const std::string& k = rule->key;

      if (k == "ao_violar") {
        if (rule->value && rule->value->kind == ExprKind::Name) on_violate = rule->value->text;
        continue;
      }
      if (k == "nao_nulo" && rule->value) {
        std::vector<std::string> cols;
        if (rule->value->kind == ExprKind::ListLit) {
          for (const auto& el : rule->value->elems) {
            if (el && el->kind == ExprKind::Name) cols.push_back(el->text);
          }
        } else if (rule->value->kind == ExprKind::Name) {
          cols.push_back(rule->value->text);
        }
        for (std::size_t r = 0; r < rows.size(); ++r) {
          for (const std::string& c : cols) {
            const Value* cell = rows[r].map ? rows[r].map->find(c) : nullptr;
            if (!cell || cell->kind == ValueKind::Nulo) {
              note("coluna '" + c + "' nula na linha " + std::to_string(r + 1));
            }
          }
        }
      } else if (k == "unico" && rule->value && rule->value->kind == ExprKind::Name) {
        const std::string col = rule->value->text;
        std::vector<std::string> seen;
        for (std::size_t r = 0; r < rows.size(); ++r) {
          const Value* cell = rows[r].map ? rows[r].map->find(col) : nullptr;
          std::string key = cell ? to_display(*cell) : "";
          if (std::find(seen.begin(), seen.end(), key) != seen.end()) {
            note("valor repetido em '" + col + "' na linha " + std::to_string(r + 1));
          } else {
            seen.push_back(key);
          }
        }
      } else if (k == "intervalo" && rule->value) {
        for (std::size_t r = 0; r < rows.size(); ++r) {
          Env inner;
          inner.parent = &env;
          inner.vars["linha"] = rows[r];
          if (!eval(*rule->value, inner).truthy()) {
            note("condicao de intervalo falhou na linha " + std::to_string(r + 1));
          }
        }
      }
    }
  }

  if (violations.empty()) {
    out_ << "verificar " << var << ": ok (" << rows.size() << " linhas)\n";
    return;
  }
  const std::string summary = "verificar " + var + ": " + std::to_string(violations.size()) +
                              (violations.size() >= 5 ? "+ violacao(oes)" : " violacao(oes)");
  if (on_violate == "avisar") {
    out_ << "[aviso] " << summary << "\n";
    for (const std::string& v : violations) out_ << "  - " << v << "\n";
    return;
  }
  throw RuntimeAbort{field.span, summary, DiagCode::DataQualityViolation, violations};
}

// ------------------------------------------------------------------ fontes
// Valor de texto de um campo de `fonte` (aceita texto, nome e env("...")).
std::string fonte_field_text(const ast::Block& block, std::string_view key) {
  const Item* f = find_field(block, key);
  if (!f || !f->value) return "";
  const Expr* v = f->value.get();
  if (v->kind == ExprKind::TextLit || v->kind == ExprKind::Name) return v->text;
  if (v->kind == ExprKind::Call && v->lhs && v->lhs->kind == ExprKind::Name &&
      v->lhs->text == "env" && !v->args.empty() &&
      v->args[0].value->kind == ExprKind::TextLit) {
    const char* e = std::getenv(v->args[0].value->text.c_str());
    return e ? std::string(e) : "";
  }
  return "";
}

// Arquivo de offset da janela de contagem para uma fonte: `<caminho>.tilt-offset`,
// compartilhado entre pipelines pela mesma fonte (o JSON dentro e um mapa por
// pipeline). Somente fontes baseadas em arquivo (csv/json); Kafka com `grupo:`
// ja tem checkpoint no broker e os demais conectores nao usam offset de arquivo.
std::string Interpreter::janela_offset_file(const std::string& fonte) {
  const Item* decl = entities_.at(fonte);
  if (!decl->block) return "";
  const std::string tipo = fonte_field_text(*decl->block, "tipo");
  if (tipo != "csv" && tipo != "json") return "";
  std::string path = fonte_field_text(*decl->block, "caminho");
  if (path.empty()) path = fonte_field_text(*decl->block, "arquivo");
  if (path.empty()) path = fonte_field_text(*decl->block, "url");
  if (path.rfind("file://", 0) == 0) path = path.substr(7);
  if (path.empty()) return "";
  return path + ".tilt-offset";
}

void Interpreter::janela_offset_load(WindowState& st, const std::string& pipeline,
                                     const std::string& offset_file) {
  if (st.offset_loaded) return;
  st.offset_loaded = true;
  std::ifstream in(offset_file);
  if (!in) return;  // sem arquivo: comeca do zero e nada cria ainda
  std::ostringstream ss;
  ss << in.rdbuf();
  Value parsed;
  try {
    parsed = rt::json_parse(ss.str());
  } catch (const std::exception&) {
    return;  // arquivo corrompido/incompleto: recomeca do zero
  }
  if (parsed.kind != ValueKind::Mapa || !parsed.map) return;
  if (const Value* v = parsed.map->find(pipeline); v && v->is_number()) {
    st.offset = static_cast<std::size_t>(v->as_number());
    st.persisted_offset = st.offset;
  }
}

void Interpreter::janela_offset_save(WindowState& st, const std::string& pipeline,
                                     const std::string& offset_file) {
  Value map = Value::mapa();
  std::ifstream in(offset_file);
  if (in) {
    std::ostringstream ss;
    ss << in.rdbuf();
    try {
      Value parsed = rt::json_parse(ss.str());
      if (parsed.kind == ValueKind::Mapa && parsed.map) {
        for (const auto& [k, v] : parsed.map->items) {
          if (k != pipeline && v.is_number()) map.map->set(k, v);  // offsets dos demais pipelines
        }
      }
    } catch (const std::exception&) {
      // sobrescreve arquivo ilegivel
    }
  }
  map.map->set(pipeline, Value::inteiro(static_cast<std::int64_t>(st.offset)));
  const std::string tmp = offset_file + ".tmp";
  {
    std::ofstream out(tmp, std::ios::trunc);
    if (!out) return;  // sem permissao: segue so com offset em memoria
    out << rt::json_dump(map) << "\n";
  }
  if (std::rename(tmp.c_str(), offset_file.c_str()) != 0) {
    std::remove(tmp.c_str());
    return;
  }
  st.persisted_offset = st.offset;
}

Value Interpreter::read_csv_file(const std::string& path, Span span) {
  std::ifstream in(path);
  if (!in) fail(span, "nao foi possivel abrir '" + path + "'");
  std::string line;
  std::vector<std::string> headers;
  rt::ValueList rows;
  bool first = true;
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    auto cells = split_csv_line(line);
    if (first) {
      headers = cells;
      first = false;
      continue;
    }
    Value row = Value::mapa();
    for (std::size_t k = 0; k < headers.size(); ++k) {
      row.map->set(headers[k], k < cells.size() ? parse_scalar(cells[k]) : Value::nulo());
    }
    rows.push_back(std::move(row));
  }
  return Value::tabela(std::move(rows));
}

Value Interpreter::read_fonte(const std::string& name, Span span) {
  const Item* decl = entities_.at(name);
  if (!decl->block) fail(span, "fonte '" + name + "' sem configuracao");

  auto field_text = [&](std::string_view key) { return fonte_field_text(*decl->block, key); };

  const std::string tipo = field_text("tipo");
  std::string path = field_text("caminho");
  if (path.empty()) path = field_text("arquivo");
  if (path.empty()) path = field_text("url");
  if (path.rfind("file://", 0) == 0) path = path.substr(7);

  if (tipo == "csv") {
    if (path.empty()) fail(span, "fonte '" + name + "': falta 'caminho:'");
    return read_csv_file(path, span);
  }
  if (tipo == "json") {
    if (path.empty()) fail(span, "fonte '" + name + "': falta 'caminho:'");
    std::ifstream in(path);
    if (!in) fail(span, "nao foi possivel abrir '" + path + "'");
    std::ostringstream ss;
    ss << in.rdbuf();
    Value parsed;
    try {
      parsed = rt::json_parse(ss.str());
    } catch (const std::exception& e) {
      fail(span, std::string(e.what()));
    }
    if (parsed.kind == ValueKind::Lista) parsed.kind = ValueKind::Tabela;
    return parsed;
  }
  if (tipo == "parquet") {
    if (path.empty()) fail(span, "fonte '" + name + "': falta 'caminho:'");
    try {
      Value t = rt::parquet_read(path);
      t.kind = ValueKind::Tabela;
      return t;
    } catch (const std::exception& e) {
      fail(span, std::string(e.what()));
    }
  }
  if (tipo == "delta") {
    if (path.empty()) fail(span, "fonte '" + name + "': falta 'caminho:'");
    try {
      Value t = rt::delta_read(path);
      t.kind = ValueKind::Tabela;
      return t;
    } catch (const std::exception& e) {
      fail(span, std::string(e.what()));
    }
  }
  if (tipo == "sqlite" || tipo == "postgres") {
    const Item* c = find_field(*decl->block, "consulta");
    if (!c || !c->value || c->value->kind != ExprKind::TextLit) {
      fail(span, "fonte '" + name + "': falta 'consulta: \"select ...\"'");
    }
    const std::string& sql = c->value->text;
    try {
      Value t = tipo == "sqlite" ? rt::sqlite_query(path, sql) : rt::postgres_query(path, sql);
      t.kind = ValueKind::Tabela;
      return t;
    } catch (const std::exception& e) {
      fail(span, std::string(e.what()));
    }
  }
  if (tipo == "kafka") {
    const std::string topico = field_text("topico");
    if (topico.empty()) {
      fail(span, "fonte '" + name + "': falta 'topico: \"...\"'");
    }
    const std::string broker = field_text("broker");
    const std::string grupo = field_text("grupo");
    std::int64_t max = 100;
    if (const Item* m = find_field(*decl->block, "max");
        m && m->value && m->value->kind == ExprKind::IntLit) {
      try {
        max = std::stoll(m->value->text);
      } catch (...) {
        fail(span, "fonte '" + name + "': 'max' deve ser inteiro");
      }
    }
    try {
      if (!grupo.empty()) {
        // Com grupo: o commit de offsets vira o checkpoint natural da fonte
        // (cada `ler` consome so o que ainda nao foi commitado).
        Value out = Value::lista();
        for (const auto& [part, valor] :
             rt::kafka_consume_group(broker, grupo, topico, static_cast<int>(max))) {
          (void)part;
          out.list->push_back(Value::texto(valor));
        }
        return out;
      }
      return rt::kafka_ler(topico, field_text("desde") == "fim", max, broker);
    } catch (const std::exception& e) {
      fail(span, std::string(e.what()));
    }
  }
  fail(span, "fonte '" + name + "': conector '" + (tipo.empty() ? "?" : tipo) +
                 "' nao implementado (M5.2)",
       DiagCode::ConnectorNotImplemented);
}

// ------------------------------------------------------------------ deep learning

namespace {

// Extracts a tensor from its JSON form { "forma": [...], "dados": [...] }.
bool tensor_from_json(const Value& v, rt::Tensor& out) {
  if (v.kind != ValueKind::Mapa || !v.map) return false;
  const Value* forma = v.map->find("forma");
  const Value* dados = v.map->find("dados");
  if (!forma || !dados || forma->kind != ValueKind::Lista || dados->kind != ValueKind::Lista ||
      !forma->list || !dados->list) {
    return false;
  }
  out.shape.clear();
  for (const Value& d : *forma->list) out.shape.push_back(static_cast<std::int64_t>(d.as_number()));
  out.data.clear();
  for (const Value& d : *dados->list) out.data.push_back(static_cast<float>(d.as_number()));
  return out.size() == static_cast<std::int64_t>(out.data.size());
}

void flatten_nested(const Value& v, std::vector<std::int64_t>& shape, std::vector<float>& data,
                    std::size_t depth) {
  if (v.kind == ValueKind::Lista && v.list) {
    if (shape.size() == depth) shape.push_back(static_cast<std::int64_t>(v.list->size()));
    for (const Value& e : *v.list) flatten_nested(e, shape, data, depth + 1);
  } else {
    data.push_back(static_cast<float>(v.as_number()));
  }
}

// Walks `camadas:` items (including folded `- densa: N` / `ativacao: relu` blocks).
void each_layer_spec(const ast::Block& block,
                     const std::function<void(const std::string&, const ast::Expr*)>& emit) {
  for (const auto& raw : block.items) {
    if (!raw) continue;
    if (raw->kind == ItemKind::ListEntry) {
      if (raw->block) {
        each_layer_spec(*raw->block, emit);
        continue;
      }
      const Item* c = raw->child.get();
      if (!c) continue;
      if (c->kind == ItemKind::Field) {
        emit(c->key, c->value.get());
      } else if (c->kind == ItemKind::Stmt && c->stmt && c->stmt->a &&
                 c->stmt->a->kind == ExprKind::Name) {
        emit(c->stmt->a->text, nullptr);
      }
    } else if (raw->kind == ItemKind::Field) {
      emit(raw->key, raw->value.get());
    }
  }
}

}  // namespace

rt::Tensor Interpreter::value_to_tensor(const Value& v, Span span) {
  if (v.kind == ValueKind::Tensor && v.tensor) return *v.tensor;
  if (v.kind == ValueKind::Lista) {
    std::vector<std::int64_t> shape;
    std::vector<float> data;
    flatten_nested(v, shape, data, 0);
    rt::Tensor t;
    t.shape = shape;
    t.data = std::move(data);
    if (t.size() != static_cast<std::int64_t>(t.data.size())) {
      fail(span, "lista aninhada irregular; nao forma um tensor");
    }
    return t;
  }
  if (v.is_number()) return rt::Tensor::filled({1}, static_cast<float>(v.as_number()));
  fail(span, std::string("nao e possivel converter '") + v.type_name() + "' em tensor");
}

std::vector<Interpreter::Layer> Interpreter::build_layers(const Item& decl, std::int64_t in_dim) {
  const std::string name = decl_name(decl);
  std::vector<Layer> layers;
  std::int64_t dim = in_dim;
  std::uint64_t seed = 0xC1A5;

  if (decl.block) {
    const Item* camadas = find_field(*decl.block, "camadas");
    if (camadas && camadas->block) {
      each_layer_spec(*camadas->block, [&](const std::string& key, const ast::Expr* value) {
        if (key == "densa" && value && value->kind == ExprKind::IntLit) {
          std::int64_t n = std::strtoll(value->text.c_str(), nullptr, 10);
          Layer l;
          l.kind = Layer::Dense;
          l.w = rt::Tensor::xavier({dim, n}, dim, n, seed++);
          l.b = rt::Tensor::zeros({n});
          layers.push_back(std::move(l));
          dim = n;
        } else if (key == "linear" && value && value->kind == ExprKind::ListLit &&
                   value->elems.size() == 2) {
          std::int64_t a = std::strtoll(value->elems[0]->text.c_str(), nullptr, 10);
          std::int64_t b = std::strtoll(value->elems[1]->text.c_str(), nullptr, 10);
          Layer l;
          l.kind = Layer::Dense;
          l.w = rt::Tensor::xavier({a, b}, a, b, seed++);
          l.b = rt::Tensor::zeros({b});
          layers.push_back(std::move(l));
          dim = b;
        } else if (key == "ativacao" && value && value->kind == ExprKind::Name) {
          Layer l;
          l.kind = Layer::Activation;
          l.act = value->text;
          layers.push_back(std::move(l));
        } else if (key == "softmax") {
          Layer l;
          l.kind = Layer::Softmax;
          layers.push_back(std::move(l));
        } else if (key == "abandono" || key == "dropout") {
          Layer l;
          l.kind = Layer::Dropout;
          layers.push_back(std::move(l));
        } else if (key == "norma_camada") {
          Layer l;
          l.kind = Layer::LayerNorm;
          layers.push_back(std::move(l));
        } else if (key == "norma_lote" || key == "conv2d") {
          fail(decl.span, "modelo '" + name + "': camada '" + key +
                              "' ainda nao suportada (M6.3); remova-a do modelo por ora");
        } else if (word_in(key, {"relu", "gelu", "silu", "sigmoide", "tanh"})) {
          Layer l;
          l.kind = Layer::Activation;
          l.act = key;
          layers.push_back(std::move(l));
        }
      });
    }
  }
  return layers;
}

void Interpreter::set_device(const Item& decl) {
  use_gpu_ = false;
  if (!decl.block) return;
  const Item* f = find_field(*decl.block, "dispositivo");
  std::string dev = "cpu";
  if (f && f->value && (f->value->kind == ExprKind::TextLit || f->value->kind == ExprKind::Name)) {
    dev = f->value->text;
  }
  if (rt::GpuRuntime::instance().ensure(dev)) {
    use_gpu_ = true;
    std::lock_guard<std::mutex> lk(log_mutex_);
    if (!gpu_announced_) {
      out_ << "[gpu] " << rt::GpuRuntime::instance().info() << "\n";
      gpu_announced_ = true;
    }
  }
}

rt::Tensor Interpreter::mm(const rt::Tensor& a, const rt::Tensor& b) {
  if (use_gpu_ && a.rank() == 2 && b.rank() == 2 && a.shape[1] == b.shape[0]) {
    rt::Tensor c;
    c.shape = {a.shape[0], b.shape[1]};
    c.data.resize(static_cast<std::size_t>(a.shape[0] * b.shape[1]));
    if (rt::GpuRuntime::instance().gemm(a.data.data(), b.data.data(), c.data.data(),
                                        static_cast<int>(a.shape[0]), static_cast<int>(a.shape[1]),
                                        static_cast<int>(b.shape[1]))) {
      return c;
    }
  }
  return rt::matmul(a, b);
}

rt::Tensor Interpreter::act_relu(const rt::Tensor& x) {
  if (use_gpu_) {
    rt::Tensor out = x;
    if (rt::GpuRuntime::instance().relu(out.data.data(), out.data.size())) return out;
  }
  return rt::apply_unary(x, "relu");
}

const std::vector<Interpreter::Layer>& Interpreter::build_model(const Item& decl,
                                                               std::int64_t in_dim, Span span) {
  const std::string name = decl_name(decl);
  {
    std::lock_guard<std::mutex> lk(model_cache_mutex_);
    if (auto it = model_cache_.find(name); it != model_cache_.end()) return it->second;
  }
  std::vector<Layer> layers = build_layers(decl, in_dim);

  // Carga de 'pesos: "arquivo"' (formato tilt-pesos, gerado por
  // modelo <Nome>.salvar_pesos). Arquivo ausente mantem o init Xavier.
  if (decl.block) {
    if (const Item* pw = find_field(*decl.block, "pesos");
        pw && pw->value && pw->value->kind == ExprKind::TextLit) {
      const std::string& path = pw->value->text;
      std::ifstream f(path);
      if (!f) {
        out_ << "[nota] modelo " << name << ": arquivo de pesos '" << path
             << "' nao encontrado; usando init Xavier\n";
      } else {
        std::ostringstream ss;
        ss << f.rdbuf();
        Value doc = Value::nulo();
        try {
          doc = rt::json_parse(ss.str());
        } catch (...) {
          doc = Value::nulo();
        }
        const Value* camadas =
            doc.kind == ValueKind::Mapa && doc.map ? doc.map->find("camadas") : nullptr;
        if (!camadas || camadas->kind != ValueKind::Lista || !camadas->list) {
          fail(span, "modelo '" + name + "': arquivo de pesos '" + path +
                         "' invalido (esperado JSON tilt-pesos com 'camadas')");
        }
        std::size_t li = 0;
        for (const Value& c : *camadas->list) {
          while (li < layers.size() && layers[li].kind != Layer::Dense) ++li;
          if (li >= layers.size()) {
            fail(span, "modelo '" + name + "': o arquivo '" + path +
                           "' tem mais camadas de pesos do que o modelo tem camadas densa");
          }
          rt::Tensor w, b;
          const Value* wv = c.kind == ValueKind::Mapa && c.map ? c.map->find("w") : nullptr;
          const Value* bv = c.kind == ValueKind::Mapa && c.map ? c.map->find("b") : nullptr;
          if (!wv || !bv || !tensor_from_json(*wv, w) || !tensor_from_json(*bv, b)) {
            fail(span, "modelo '" + name + "': arquivo de pesos '" + path +
                           "' invalido (cada camada precisa de 'w' e 'b' com forma e dados)");
          }
          if (w.shape != layers[li].w.shape || b.shape != layers[li].b.shape) {
            fail(span, "modelo '" + name + "': forma de pesos incompativel na camada densa " +
                           std::to_string(li) + " (modelo espera w " + layers[li].w.shape_str() +
                           " b " + layers[li].b.shape_str() + ", arquivo tem w " + w.shape_str() +
                           " b " + b.shape_str() + ")");
          }
          layers[li].w = std::move(w);
          layers[li].b = std::move(b);
          ++li;
        }
      }
    }
  }
  std::lock_guard<std::mutex> lk(model_cache_mutex_);
  return model_cache_.emplace(name, std::move(layers)).first->second;
}

rt::Tensor Interpreter::forward_layers(const std::vector<Layer>& layers, rt::Tensor x) {
  for (const Layer& l : layers) {
    switch (l.kind) {
      case Layer::Dense:
        x = rt::add(mm(x, l.w), l.b);
        break;
      case Layer::Activation:
        x = l.act == "relu" ? act_relu(x) : rt::apply_unary(x, l.act);
        break;
      case Layer::Softmax:
        x = rt::softmax_last(x);
        break;
      case Layer::LayerNorm:
        x = rt::layer_norm_last(x);
        break;
      case Layer::Dropout:
        break;
    }
  }
  return x;
}

rt::Value Interpreter::model_forward(const Item& decl, const Value& input, Span span) {
  set_device(decl);
  rt::Tensor x = value_to_tensor(input, span);
  std::int64_t in_dim = x.shape.empty() ? static_cast<std::int64_t>(x.data.size()) : x.shape.back();
  const std::vector<Layer>& layers = build_model(decl, in_dim, span);
  try {
    return Value::tensor_de(forward_layers(layers, std::move(x)));
  } catch (const std::exception& e) {
    fail(span, std::string("modelo ") + decl_name(decl) + ": " + e.what());
  }
}

rt::Value Interpreter::eval_modelo_call(const Expr& call, Env& env) {
  if (call.args.size() != 1 || call.args[0].value->kind != ExprKind::Call) {
    fail(call.span, "uso: modelo <Nome>.executar <entrada>");
  }
  const Expr& inner = *call.args[0].value;
  if (!inner.lhs || inner.lhs->kind != ExprKind::Member || !inner.lhs->lhs ||
      inner.lhs->lhs->kind != ExprKind::Name) {
    fail(inner.span, "uso: modelo <Nome>.executar <entrada>");
  }
  const std::string mname = inner.lhs->lhs->text;
  const std::string method = inner.lhs->text;

  auto it = entities_.find(mname);
  if (it == entities_.end() || it->second->key != "modelo") {
    fail(inner.span, "'" + mname + "' nao e um modelo declarado");
  }
  if (method == "executar" || method == "para_frente") {
    if (inner.args.empty()) fail(inner.span, method + " precisa de uma entrada");
    Value input = eval(*inner.args[0].value, env);
    return model_forward(*it->second, input, inner.span);
  }
  if (method == "salvar_pesos") {
    if (inner.args.empty() || inner.args[0].value->kind != ExprKind::TextLit) {
      fail(inner.span, "uso: modelo " + mname + ".salvar_pesos \"caminho\"");
    }
    const std::string& path = inner.args[0].value->text;
    // A dimensao de entrada vem da anotacao 'entrada: tensor[..., N]'.
    std::int64_t in_dim = -1;
    if (it->second->block) {
      if (const Item* ent = find_field(*it->second->block, "entrada");
          ent && ent->value && ent->value->kind == ExprKind::Index && !ent->value->elems.empty()) {
        const Expr* last = ent->value->elems.back().get();
        if (last && last->kind == ExprKind::IntLit) in_dim = std::stoll(last->text);
      }
    }
    if (in_dim < 0) {
      fail(inner.span, "modelo '" + mname +
                           "': 'salvar_pesos' precisa de 'entrada: tensor[..., N]' anotado "
                           "para inferir a dimensao de entrada");
    }
    const std::vector<Layer>& layers = build_model(*it->second, in_dim, inner.span);
    Value cl = Value::lista();
    for (const Layer& l : layers) {
      if (l.kind != Layer::Dense) continue;
      Value c = Value::mapa();
      c.map->set("tipo", Value::texto("densa"));
      c.map->set("w", Value::tensor_de(l.w));
      c.map->set("b", Value::tensor_de(l.b));
      cl.list->push_back(std::move(c));
    }
    Value doc = Value::mapa();
    doc.map->set("formato", Value::texto("tilt-pesos"));
    doc.map->set("versao", Value::inteiro(1));
    doc.map->set("camadas", cl);
    std::ofstream out(path, std::ios::trunc);
    if (!out) fail(inner.span, "modelo '" + mname + "': nao foi possivel gravar '" + path + "'");
    out << rt::json_dump(doc) << "\n";
    out_ << "modelo " << mname << ": pesos salvos em " << path << " (" << cl.list->size()
         << " camadas)\n";
    return Value::logico(true);
  }
  fail(inner.span, "metodo de modelo '" + method + "' desconhecido (use executar / para_frente / salvar_pesos)");
}

namespace {

int field_int(const ast::Block& block, std::string_view key, int fallback) {
  const Item* f = find_field(block, key);
  if (f && f->value && f->value->kind == ExprKind::IntLit) {
    return static_cast<int>(std::strtol(f->value->text.c_str(), nullptr, 10));
  }
  return fallback;
}
double field_num(const ast::Block& block, std::string_view key, double fallback) {
  const Item* f = find_field(block, key);
  if (f && f->value && (f->value->kind == ExprKind::DecimalLit || f->value->kind == ExprKind::IntLit)) {
    return std::strtod(f->value->text.c_str(), nullptr);
  }
  return fallback;
}
std::string field_word(const ast::Block& block, std::string_view key, std::string fallback) {
  const Item* f = find_field(block, key);
  if (f && f->value && f->value->kind == ExprKind::Name) return f->value->text;
  return fallback;
}

// Sum a [B, C] gradient over the batch dimension -> [C].
rt::Tensor col_sum(const rt::Tensor& g) {
  const auto c = static_cast<std::size_t>(g.shape.size() == 2 ? g.shape[1] : g.data.size());
  rt::Tensor out = rt::Tensor::zeros({static_cast<std::int64_t>(c)});
  for (std::size_t k = 0; k < g.data.size(); ++k) out.data[k % c] += g.data[k];
  return out;
}

float activation_deriv(const std::string& fn, float pre, float post) {
  if (fn == "relu") return pre > 0.0F ? 1.0F : 0.0F;
  if (fn == "sigmoide") return post * (1.0F - post);
  if (fn == "tanh") return 1.0F - post * post;
  if (fn == "silu") {
    float s = 1.0F / (1.0F + std::exp(-pre));
    return s * (1.0F + pre * (1.0F - s));
  }
  if (fn == "gelu") {
    // Mesma aproximacao tanh da forward (rt::apply_unary):
    // gelu(x) = 0.5 x (1 + tanh(u)), u = c (x + 0.044715 x^3), c = 0.7978845608
    // d/dx = 0.5 (1 + tanh(u)) + 0.5 x sech^2(u) c (1 + 0.134145 x^2)
    const float c = 0.7978845608F;
    const float u = c * (pre + 0.044715F * pre * pre * pre);
    const float t = std::tanh(u);
    return 0.5F * (1.0F + t) +
           0.5F * pre * (1.0F - t * t) * c * (1.0F + 0.134145F * pre * pre);
  }
  return 1.0F;
}

// Backward de norma_camada (sem affine), por linha da ultima dimensao:
// dL/dx = inv * (g - mean(g) - y * mean(g*y)), com y a saida normalizada do
// forward e inv = 1/sqrt(var + eps) recomputado da entrada (eps = 1e-5).
void layer_norm_backward(rt::Tensor& grad, const rt::Tensor& in, const rt::Tensor& y) {
  const auto w = static_cast<std::size_t>(in.shape.back());
  for (std::size_t base = 0; base < grad.data.size(); base += w) {
    float mean = 0.0F;
    for (std::size_t k = 0; k < w; ++k) mean += in.data[base + k];
    mean /= static_cast<float>(w);
    float var = 0.0F;
    for (std::size_t k = 0; k < w; ++k) {
      const float d = in.data[base + k] - mean;
      var += d * d;
    }
    var /= static_cast<float>(w);
    const float inv = 1.0F / std::sqrt(var + 1e-5F);
    float mg = 0.0F, mgy = 0.0F;
    for (std::size_t k = 0; k < w; ++k) {
      mg += grad.data[base + k];
      mgy += grad.data[base + k] * y.data[base + k];
    }
    mg /= static_cast<float>(w);
    mgy /= static_cast<float>(w);
    for (std::size_t k = 0; k < w; ++k) {
      grad.data[base + k] = inv * (grad.data[base + k] - mg - y.data[base + k] * mgy);
    }
  }
}

}  // namespace

void Interpreter::run_treino(const Item& decl) {
  const std::string name = decl_name(decl);
  auto mit = entities_.find(name);
  if (mit == entities_.end() || mit->second->key != "modelo") {
    fail(decl.span, "treino '" + name + "': nao existe 'modelo " + name + "'");
  }
  if (!decl.block) return;
  const ast::Block& cfg = *decl.block;

  const Item* dados = find_field(cfg, "dados");
  if (!dados || !dados->value) fail(decl.span, "treino '" + name + "': falta 'dados:'");
  Value data = eval(*dados->value, root_);
  if (data.kind != ValueKind::Mapa || !data.map || !data.map->find("x") || !data.map->find("y")) {
    fail(dados->value->span, "treino: 'dados' deve produzir { x: <tensor>, y: <lista> }");
  }
  rt::Tensor x = value_to_tensor(*data.map->find("x"), decl.span);
  if (x.rank() != 2) fail(decl.span, "treino: 'x' deve ser 2D [amostras, atributos]");
  const std::int64_t n = x.shape[0];
  const std::int64_t f = x.shape[1];

  const std::string perda = field_word(cfg, "perda", "entropia_cruzada");
  const bool ce = perda == "entropia_cruzada";
  const bool mse = perda == "quadratica";

  std::vector<double> yf;
  const Value* yv = data.map->find("y");
  if (yv->kind == ValueKind::Lista && yv->list) {
    for (const Value& e : *yv->list) yf.push_back(e.as_number());
  }
  if (static_cast<std::int64_t>(yf.size()) != n) fail(decl.span, "treino: |x| != |y|");
  std::vector<int> y;
  int classes = 1;
  for (double v : yf) {
    y.push_back(static_cast<int>(v));
    classes = std::max(classes, static_cast<int>(v) + 1);
  }

  const std::string otim = field_word(cfg, "otimizador", "sgd");
  double lr = field_num(cfg, "taxa", field_num(cfg, "taxa_aprendizado", 0.1));
  const int epocas = field_int(cfg, "epocas", 50);
  const Item* vf = find_field(cfg, "verboso");
  const bool verbose = vf && vf->value && vf->value->kind == ExprKind::BoolLit && vf->value->boolean;

  set_device(*mit->second);
  std::vector<Layer> layers = build_layers(*mit->second, f);
  if (!ce && !mse) {
    fail(decl.span, "treino: perda '" + perda +
                        "' nao suportada (use 'entropia_cruzada' | 'quadratica')");
  }
  if (layers.empty()) fail(decl.span, "treino '" + name + "': modelo sem camadas");
  if (ce && layers.back().kind != Layer::Softmax) {
    fail(decl.span, "treino: perda 'entropia_cruzada' exige 'softmax' na ultima camada");
  }
  if (mse) {
    if (layers.back().kind == Layer::Softmax) {
      fail(decl.span,
           "treino: perda 'quadratica' exige ultima camada 'densa'/'linear' sem 'softmax'");
    }
    std::int64_t width = 1;
    for (auto rit = layers.rbegin(); rit != layers.rend(); ++rit) {
      if (rit->kind == Layer::Dense) {
        width = rit->w.shape[1];
        break;
      }
    }
    if (width != 1) {
      fail(decl.span, "treino: perda 'quadratica' e regressao escalar; a saida do modelo deve ter "
                      "largura 1 (tem " +
                          std::to_string(width) + ")");
    }
  }
  for (Layer& l : layers) {
    if (l.kind != Layer::Dense) continue;
    l.m_w = rt::Tensor::zeros(l.w.shape);
    l.v_w = rt::Tensor::zeros(l.w.shape);
    l.m_b = rt::Tensor::zeros(l.b.shape);
    l.v_b = rt::Tensor::zeros(l.b.shape);
  }

  const float inv_n = 1.0F / static_cast<float>(n);
  float first_loss = 0.0F;
  float last_loss = 0.0F;
  int adam_t = 0;

  try {
    for (int epoch = 1; epoch <= epocas; ++epoch) {
      // Forward with cached inputs per layer.
      std::vector<rt::Tensor> ins;
      ins.reserve(layers.size() + 1);
      rt::Tensor cur = x;
      for (const Layer& l : layers) {
        ins.push_back(cur);
        switch (l.kind) {
          case Layer::Dense: cur = rt::add(mm(cur, l.w), l.b); break;
          case Layer::Activation:
            cur = l.act == "relu" ? act_relu(cur) : rt::apply_unary(cur, l.act);
            break;
          case Layer::Softmax: cur = rt::softmax_last(cur); break;
          case Layer::LayerNorm: cur = rt::layer_norm_last(cur); break;
          case Layer::Dropout: break;
        }
      }
      const rt::Tensor& probs = cur;  // [N, C] (probs no CE, valores no MSE)

      // Perda e gradiente da saida.
      float loss = 0.0F;
      rt::Tensor grad = probs;
      if (ce) {
        // entropia_cruzada + softmax: dL/d(logits) = probs - onehot(y), medio.
        for (std::int64_t i = 0; i < n; ++i) {
          const int label = y[static_cast<std::size_t>(i)];
          const auto idx = static_cast<std::size_t>(i * classes + label);
          loss -= std::log(std::max(probs.data[idx], 1e-9F));
          for (std::int64_t c = 0; c < classes; ++c) {
            auto g = static_cast<std::size_t>(i * classes + c);
            grad.data[g] = (grad.data[g] - (c == label ? 1.0F : 0.0F)) * inv_n;
          }
        }
        loss *= inv_n;
      } else {
        // quadratica (regressao escalar): saida [N, 1], dL/dy = 2*(pred - alvo)/n.
        for (std::int64_t i = 0; i < n; ++i) {
          const float d = probs.data[static_cast<std::size_t>(i)] -
                          static_cast<float>(yf[static_cast<std::size_t>(i)]);
          loss += d * d;
          grad.data[static_cast<std::size_t>(i)] = 2.0F * d * inv_n;
        }
        loss *= inv_n;
      }
      if (epoch == 1) first_loss = loss;
      last_loss = loss;
      if (verbose && (epoch == 1 || epoch % std::max(1, epocas / 10) == 0)) {
        out_ << "  epoca " << epoch << " perda " << loss << "\n";
      }

      // Backward: skip the softmax layer (fused above); update Dense/Activation.
      ++adam_t;
      for (std::int64_t li = static_cast<std::int64_t>(layers.size()) - 1; li >= 0; --li) {
        Layer& l = layers[static_cast<std::size_t>(li)];
        const rt::Tensor& in = ins[static_cast<std::size_t>(li)];
        if (l.kind == Layer::Softmax || l.kind == Layer::Dropout) continue;
        if (l.kind == Layer::LayerNorm) {
          // Saida normalizada do forward: entrada da proxima camada, ou a
          // propria saida final (probs) quando norma_camada e a ultima camada.
          const rt::Tensor& y = static_cast<std::size_t>(li) + 1 < ins.size()
                                    ? ins[static_cast<std::size_t>(li) + 1]
                                    : probs;
          layer_norm_backward(grad, in, y);
          continue;
        }
        if (l.kind == Layer::Activation) {
          const rt::Tensor& out_act = ins[static_cast<std::size_t>(li) + 1 < ins.size()
                                              ? static_cast<std::size_t>(li) + 1
                                              : static_cast<std::size_t>(li)];
          for (std::size_t k = 0; k < grad.data.size(); ++k) {
            grad.data[k] *= activation_deriv(l.act, in.data[k],
                                             k < out_act.data.size() ? out_act.data[k] : 0.0F);
          }
          continue;
        }
        // Dense
        rt::Tensor dw = rt::matmul(rt::transpose2d(in), grad);  // [F_in, C]
        rt::Tensor db = col_sum(grad);
        rt::Tensor grad_in = rt::matmul(grad, rt::transpose2d(l.w));

        if (otim == "adam") {
          const float b1 = 0.9F, b2 = 0.999F, eps = 1e-8F;
          const float c1 = 1.0F - std::pow(b1, static_cast<float>(adam_t));
          const float c2 = 1.0F - std::pow(b2, static_cast<float>(adam_t));
          auto step = [&](rt::Tensor& w, rt::Tensor& m, rt::Tensor& v, const rt::Tensor& g) {
            for (std::size_t k = 0; k < w.data.size(); ++k) {
              m.data[k] = b1 * m.data[k] + (1.0F - b1) * g.data[k];
              v.data[k] = b2 * v.data[k] + (1.0F - b2) * g.data[k] * g.data[k];
              float mh = m.data[k] / c1;
              float vh = v.data[k] / c2;
              w.data[k] -= static_cast<float>(lr) * mh / (std::sqrt(vh) + eps);
            }
          };
          step(l.w, l.m_w, l.v_w, dw);
          step(l.b, l.m_b, l.v_b, db);
        } else {
          for (std::size_t k = 0; k < l.w.data.size(); ++k) {
            l.w.data[k] -= static_cast<float>(lr) * dw.data[k];
          }
          for (std::size_t k = 0; k < l.b.data.size(); ++k) {
            l.b.data[k] -= static_cast<float>(lr) * db.data[k];
          }
        }
        grad = std::move(grad_in);
      }
    }

    if (ce) {
      // Final training-set accuracy.
      rt::Tensor probs = forward_layers(layers, x);
      int correct = 0;
      for (std::int64_t i = 0; i < n; ++i) {
        std::int64_t best = 0;
        for (std::int64_t c = 1; c < classes; ++c) {
          if (probs.data[static_cast<std::size_t>(i * classes + c)] >
              probs.data[static_cast<std::size_t>(i * classes + best)]) {
            best = c;
          }
        }
        if (best == y[static_cast<std::size_t>(i)]) ++correct;
      }
      out_ << "treino " << name << ": perda caiu " << (last_loss < first_loss ? "sim" : "nao")
           << " | acuracia " << correct << "/" << n << "\n";
    } else {
      char mse_buf[64];
      std::snprintf(mse_buf, sizeof(mse_buf), "%.4f", last_loss);
      out_ << "treino " << name << ": perda caiu " << (last_loss < first_loss ? "sim" : "nao")
           << " | mse " << mse_buf << "\n";
    }
  } catch (const std::exception& e) {
    fail(decl.span, std::string("treino ") + name + ": " + e.what());
  }

  {
    std::lock_guard<std::mutex> lk(model_cache_mutex_);
    model_cache_[name] = std::move(layers);
  }
}

// ------------------------------------------------------------------ LLM + RAG

namespace {

std::string field_str(const ast::Block& block, std::string_view key) {
  const Item* f = find_field(block, key);
  return (f && f->value && f->value->kind == ExprKind::TextLit) ? f->value->text : std::string();
}

std::string field_env_or_text(const ast::Block& block, std::string_view key) {
  const Item* f = find_field(block, key);
  if (!f || !f->value) return {};
  const Expr* v = f->value.get();
  if (v->kind == ExprKind::TextLit || v->kind == ExprKind::Name) return v->text;
  if (v->kind == ExprKind::Call && v->lhs && v->lhs->kind == ExprKind::Name &&
      v->lhs->text == "env" && !v->args.empty() && v->args[0].value->kind == ExprKind::TextLit) {
    const char* e = std::getenv(v->args[0].value->text.c_str());
    return e ? std::string(e) : std::string();
  }
  return {};
}

std::string row_text(const Value& item) {
  if (item.kind == ValueKind::Mapa && item.map) {
    for (const char* key : {"texto", "conteudo", "text", "trecho", "content"}) {
      if (const Value* v = item.map->find(key); v && v->kind == ValueKind::Texto) return v->s;
    }
  }
  return item.kind == ValueKind::Texto ? item.s : to_display(item);
}

Value default_for_type(const Expr* type_expr) {
  if (!type_expr) return Value::nulo();
  if (type_expr->kind == ExprKind::Name) {
    const std::string& w = type_expr->text;
    if (w == "texto") return Value::texto("exemplo");
    if (w == "inteiro") return Value::inteiro(0);
    if (w == "decimal") return Value::decimal(0.0);
    if (w == "logico") return Value::logico(false);
    return Value::nulo();
  }
  if (type_expr->kind == ExprKind::TextLit) return Value::texto(type_expr->text);
  if (type_expr->kind == ExprKind::Binary && type_expr->text == "|") {
    const Expr* e = type_expr;
    while (e && e->kind == ExprKind::Binary) e = e->lhs.get();
    return Value::texto(e && e->kind == ExprKind::TextLit ? e->text : "");
  }
  if (type_expr->kind == ExprKind::Index && type_expr->lhs &&
      type_expr->lhs->kind == ExprKind::Name && type_expr->lhs->text == "lista") {
    return Value::lista();
  }
  return Value::nulo();
}

}  // namespace

rt::LlmConfig Interpreter::llm_config(const std::string& name, Span span) {
  auto it = entities_.find(name);
  if (it == entities_.end() || it->second->key != "llm" || !it->second->block) {
    fail(span, "'" + name + "' nao e um 'llm' declarado");
  }
  const ast::Block& b = *it->second->block;
  rt::LlmConfig cfg;
  cfg.provider = field_word(b, "provedor", "anthropic");
  cfg.model = field_str(b, "modelo");
  cfg.api_key = field_env_or_text(b, "chave");
  cfg.base_url = field_env_or_text(b, "base_url");
  cfg.temperature = field_num(b, "temperatura", 0.2);
  cfg.max_tokens = field_int(b, "max_tokens", 1024);
  return cfg;
}

rt::Value Interpreter::eval_perguntar(const Expr& call, Env& env) {
  std::string llm_name;
  if (!call.args.empty() && call.args[0].name.empty()) {
    Value v = eval(*call.args[0].value, env);
    llm_name = v.kind == ValueKind::Texto ? v.s : "";
  }

  rt::ValueMap kw;
  for (const auto& a : call.args) {
    if (!a.name.empty()) kw.set(a.name, eval(*a.value, env));
  }
  if (call.block) {
    for (const auto& it : call.block->items) {
      if (it && it->kind == ItemKind::Field && it->value) kw.set(it->key, eval(*it->value, env));
    }
  }

  auto text_of = [](const Value* v) { return v && v->kind == ValueKind::Texto ? v->s : std::string(); };
  std::string system = text_of(kw.find("sistema"));
  std::string user = text_of(kw.find("usuario"));
  if (user.empty()) user = text_of(kw.find("prompt"));

  rt::LlmConfig cfg = llm_config(llm_name, call.span);
  std::string raw;
  try {
    raw = rt::llm_chat(cfg, system, user);
  } catch (const std::exception& e) {
    fail(call.span, std::string("LLM: ") + e.what());
  }

  if (const Value* fmt = kw.find("formato"); fmt && fmt->kind == ValueKind::Texto) {
    return structured_from_tipo(fmt->s, raw, call.span);
  }
  Value out = Value::mapa();
  out.map->set("texto", Value::texto(raw));
  out.map->set("modelo", Value::texto(cfg.model));
  return out;
}

rt::Value Interpreter::structured_from_tipo(const std::string& tipo_name, const std::string& raw,
                                           Span span) {
  auto it = entities_.find(tipo_name);
  if (it == entities_.end() || it->second->key != "tipo" || !it->second->block) {
    fail(span, "formato: '" + tipo_name + "' nao e um 'tipo' declarado");
  }
  Value parsed;
  bool have_parsed = false;
  if (!rt::llm_is_mock()) {
    try {
      parsed = rt::json_parse(raw);
      have_parsed = parsed.kind == ValueKind::Mapa;
    } catch (...) {
      fail(span, "a resposta do LLM nao e um JSON valido para o tipo '" + tipo_name + "'");
    }
  }

  Value out = Value::mapa();
  for (const auto& f : it->second->block->items) {
    if (!f || f->kind != ItemKind::Field) continue;
    Value v;
    if (have_parsed && parsed.map) {
      const Value* got = parsed.map->find(f->key);
      v = got ? *got : default_for_type(f->value.get());
    } else {
      v = default_for_type(f->value.get());
    }
    out.map->set(f->key, std::move(v));
  }
  return out;
}

rt::Value Interpreter::eval_indice_method(const std::string& indice_name, const std::string& method,
                                         const Expr& call, Env& env) {
  auto it = entities_.find(indice_name);
  const ast::Block& b = *it->second->block;
  const std::string armazenamento = field_str(b, "armazenamento");
  // Formatos: "qdrant://host:porta/colecao" (ver run-indice-armazenamento) e
  // "pgvector://colecao" (Postgres + extensao pgvector; connection string no
  // campo "url", como em fonte postgres).
  std::string qdrant_base, qdrant_col;
  std::string pgv_table, pgv_url;
  const bool qdrant = armazenamento.rfind("qdrant://", 0) == 0;
  const bool pgvector = armazenamento.rfind("pgvector://", 0) == 0;
  if (!armazenamento.empty() && armazenamento != "memoria" && !qdrant && !pgvector) {
    fail(call.span, "indice '" + indice_name + "': armazenamento '" + armazenamento +
                        "' nao implementado; use \"memoria\", \"qdrant://host:porta/colecao\" "
                        "ou \"pgvector://colecao\" (com campo \"url\")",
         DiagCode::ConnectorNotImplemented);
  }
  if (qdrant) {
    const std::string rest = armazenamento.substr(9);  // depois de qdrant://
    const std::size_t slash = rest.find('/');
    if (slash == std::string::npos || slash == 0 || slash == rest.size() - 1) {
      fail(call.span, "indice '" + indice_name +
                          "': armazenamento qdrant deve ser 'qdrant://host:porta/colecao'");
    }
    qdrant_base = "http://" + rest.substr(0, slash);
    qdrant_col = rest.substr(slash + 1);
  }
  if (pgvector) {
    pgv_table = armazenamento.substr(11);  // depois de pgvector://
    pgv_url = field_str(b, "url");
    if (pgv_url.empty()) {
      fail(call.span, "indice '" + indice_name +
                          "': armazenamento pgvector precisa do campo \"url\" "
                          "(connection string libpq, ex.: \"host=... port=... dbname=... user=...\")");
    }
  }
  const std::string emb_model = field_str(b, "embeddings");
  // Serializa o indice em memoria entre as rotas paralelas. Vive ate o fim
  // da funcao: cobre inserir/buscar e as chamadas de embedding no meio.
  std::unique_lock<std::mutex> index_lk(index_stores_mutex_);
  rt::MemoryIndex& store = index_stores_[indice_name];

  if (method == "inserir") {
    if (call.args.empty()) fail(call.span, "inserir espera uma lista ou tabela");
    Value v = eval(*call.args[0].value, env);
    int added = 0;
    auto add_one = [&](const Value& item) {
      const std::string text = row_text(item);
      std::string id = std::to_string(store.size() + 1);
      if (item.kind == ValueKind::Mapa && item.map) {
        if (const Value* i = item.map->find("id")) id = to_display(*i);
      }
      const std::vector<float> vec = rt::llm_embed(emb_model, text);
      if (qdrant) {
        try {
          rt::qdrant_upsert(qdrant_base, qdrant_col, id, text, vec);
        } catch (const std::exception& e) {
          fail(call.span, std::string(e.what()));
        }
      } else if (pgvector) {
        try {
          rt::pgvector_upsert(pgv_url, pgv_table, id, text, vec);
        } catch (const std::exception& e) {
          fail(call.span, std::string(e.what()));
        }
      } else {
        store.insert(id, text, vec);
      }
      ++added;
    };
    if ((v.kind == ValueKind::Lista || v.kind == ValueKind::Tabela) && v.list) {
      for (const Value& e : *v.list) add_one(e);
    } else {
      add_one(v);
    }
    return Value::inteiro(added);
  }

  if (method == "buscar") {
    if (call.args.empty()) fail(call.span, "buscar espera um texto de consulta");
    Value q = eval(*call.args[0].value, env);
    rt::ValueMap kw;
    for (const auto& a : call.args) {
      if (!a.name.empty()) kw.set(a.name, eval(*a.value, env));
    }
    std::size_t k = 5;
    if (const Value* tk = kw.find("top_k")) k = static_cast<std::size_t>(tk->as_number());
    const std::string qt = q.kind == ValueKind::Texto ? q.s : to_display(q);
    Value out = Value::lista();
    if (qdrant || pgvector) {
      std::vector<std::pair<std::string, double>> hits;
      try {
        if (qdrant) {
          hits = rt::qdrant_search(qdrant_base, qdrant_col, rt::llm_embed(emb_model, qt), k);
        } else {
          hits = rt::pgvector_search(pgv_url, pgv_table, rt::llm_embed(emb_model, qt), k);
        }
      } catch (const std::exception& e) {
        fail(call.span, std::string(e.what()));
      }
      for (const auto& h : hits) {
        Value row = Value::mapa();
        row.map->set("id", Value::texto(h.first));
        row.map->set("score", Value::decimal(h.second));
        out.list->push_back(std::move(row));
      }
      return out;
    }
    auto hits = store.search(rt::llm_embed(emb_model, qt), k);
    for (const auto& h : hits) {
      Value row = Value::mapa();
      row.map->set("id", Value::texto(h.id));
      row.map->set("texto", Value::texto(h.text));
      row.map->set("score", Value::decimal(h.score));
      out.list->push_back(std::move(row));
    }
    return out;
  }

  fail(call.span, "indice: metodo '" + method + "' desconhecido (use inserir / buscar)");
}

// ------------------------------------------------------------------ agents

rt::Value Interpreter::run_tool(const Item& tool_decl, const rt::ValueMap& args, Span span) {
  Env env;
  env.parent = &root_;
  if (tool_decl.block) {
    if (const Item* entrada = find_field(*tool_decl.block, "entrada"); entrada && entrada->block) {
      for (const auto& f : entrada->block->items) {
        if (f && f->kind == ItemKind::Field) {
          const Value* a = args.find(f->key);
          env.vars[f->key] = a ? *a : Value::nulo();
        }
      }
    }
    if (const Item* exec = find_field(*tool_decl.block, "executar"); exec && exec->block) {
      try {
        exec_block(*exec->block, env);
      } catch (const ReturnSignal& r) {
        return r.value;
      }
    }
  }
  (void)span;
  return Value::nulo();
}

namespace {

struct PlannerAction {
  enum Kind { Tool, Answer } kind = Answer;
  std::string tool;
  rt::ValueMap args;
  std::string answer;
};

std::string trim_copy(const std::string& s) {
  std::size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
  return s.substr(a, b - a);
}

// Protocolo do planner (uma linha por turno):
//   chamar <nome> {<json de argumentos>}  — executa uma ferramenta
//   responder: <resposta final>           — encerra o loop
// Qualquer outro texto vira resposta final (LLMs reais ignoram protocolo com
// frequencia; e' o fallback tolerante).
PlannerAction parse_planner_action(const std::string& raw) {
  PlannerAction a;
  const std::string s = trim_copy(raw);
  if (s.rfind("chamar ", 0) == 0) {
    const std::string rest = trim_copy(s.substr(7));
    const std::size_t brace = rest.find('{');
    PlannerAction tool;
    tool.kind = PlannerAction::Tool;
    tool.tool = trim_copy(rest.substr(0, brace));
    if (brace != std::string::npos) {
      try {
        const Value v = rt::json_parse(rest.substr(brace));
        if (v.kind == ValueKind::Mapa && v.map) {
          for (const auto& [k, val] : v.map->items) tool.args.set(k, val);
        }
      } catch (...) {
        // argumentos malformados: o interpretador completa com best-effort
      }
    }
    if (!tool.tool.empty()) return tool;
  }
  if (s.rfind("responder:", 0) == 0) {
    a.answer = trim_copy(s.substr(10));
  } else {
    a.answer = s;
  }
  return a;
}

Value args_to_value(const rt::ValueMap& args) {
  Value out = Value::mapa();
  for (const auto& [k, v] : args.items) out.map->set(k, v);
  return out;
}

// Entradas best-effort: campos 'texto' recebem a mensagem; os demais, o
// padrao do tipo declarado.
rt::ValueMap best_effort_args(const Item& tool_decl, const std::string& message) {
  rt::ValueMap targs;
  if (tool_decl.block) {
    if (const Item* entrada = find_field(*tool_decl.block, "entrada"); entrada && entrada->block) {
      for (const auto& f : entrada->block->items) {
        if (!f || f->kind != ItemKind::Field) continue;
        if (f->value && f->value->kind == ExprKind::Name && f->value->text == "texto") {
          targs.set(f->key, Value::texto(message));
        } else {
          targs.set(f->key, default_for_type(f->value.get()));
        }
      }
    }
  }
  return targs;
}

std::string tool_params_desc(const Item& tool_decl) {
  std::string out;
  if (tool_decl.block) {
    if (const Item* entrada = find_field(*tool_decl.block, "entrada"); entrada && entrada->block) {
      bool first = true;
      for (const auto& f : entrada->block->items) {
        if (!f || f->kind != ItemKind::Field) continue;
        if (!first) out += ", ";
        out += f->key;
        if (f->value && f->value->kind == ExprKind::Name) out += ": " + f->value->text;
        first = false;
      }
    }
  }
  return out;
}

}  // namespace

rt::Value Interpreter::eval_agente_responder(const std::string& agent_name, const Expr& call,
                                             Env& env) {
  auto it = entities_.find(agent_name);
  const ast::Block& cfg = *it->second->block;

  std::string message;
  if (!call.args.empty()) {
    Value m = eval(*call.args[0].value, env);
    message = m.kind == ValueKind::Texto ? m.s : to_display(m);
  }

  const std::string papel = field_str(cfg, "papel");
  const std::string llm_name = field_word(cfg, "llm", "");
  const std::string memoria = field_word(cfg, "memoria", "nenhuma");
  const int max_passos = field_int(cfg, "max_passos", 6);

  // Ferramentas declaradas, ja resolvidas e validadas.
  std::vector<std::pair<std::string, const Item*>> tools;
  if (const Item* tf = find_field(cfg, "ferramentas")) {
    std::vector<std::string> names;
    if (tf->value && tf->value->kind == ExprKind::ListLit) {
      for (const auto& e : tf->value->elems) {
        if (e && e->kind == ExprKind::Name) names.push_back(e->text);
      }
    }
    if (tf->block) {
      for (const auto& raw : tf->block->items) {
        const Item* c = (raw && raw->kind == ItemKind::ListEntry && raw->child) ? raw->child.get()
                                                                                : raw.get();
        if (!c) continue;
        if (c->kind == ItemKind::Stmt && c->stmt && c->stmt->a &&
            c->stmt->a->kind == ExprKind::Name) {
          names.push_back(c->stmt->a->text);
        } else if (c->kind == ItemKind::Field) {
          names.push_back(c->key);
        }
      }
    }
    for (const std::string& name : names) {
      auto tit = entities_.find(name);
      if (tit == entities_.end() || tit->second->key != "ferramenta") {
        fail(call.span, "agente '" + agent_name + "': ferramenta '" + name + "' nao declarada");
      }
      tools.emplace_back(name, tit->second);
    }
  }

  std::string prompt = message;
  if (memoria == "conversa") {
    std::string mem;
    {
      std::lock_guard<std::mutex> lk(agent_memory_mutex_);
      mem = agent_memory_[agent_name];
    }
    if (!mem.empty()) prompt = mem + "\n" + message;
  }

  Value rastro = Value::lista();
  std::string answer;

  if (llm_name.empty()) {
    // Sem LLM: cada ferramenta roda uma vez com entradas best-effort e a
    // resposta e' local.
    int step = 0;
    for (const auto& [tname, tdecl] : tools) {
      if (step >= max_passos) break;
      rt::ValueMap targs = best_effort_args(*tdecl, prompt);
      Value obs = run_tool(*tdecl, targs, call.span);
      ++step;
      Value entry = Value::mapa();
      entry.map->set("passo", Value::inteiro(step));
      entry.map->set("ferramenta", Value::texto(tname));
      entry.map->set("argumentos", args_to_value(targs));
      entry.map->set("observacao", Value::texto(to_display(obs)));
      rastro.list->push_back(std::move(entry));
    }
    answer = "[sem llm] " + message;
  } else {
    rt::LlmConfig lc = llm_config(llm_name, call.span);
    std::string system = papel;

    if (tools.empty()) {
      try {
        answer = rt::llm_chat(lc, system, prompt);
      } catch (const std::exception& e) {
        fail(call.span, std::string("agente '") + agent_name + "': LLM: " + e.what());
      }
    } else {
      // Planner iterativo (M9.2): a cada passo o LLM escolhe a proxima acao
      // — chamar uma ferramenta (com argumentos em JSON) ou responder.
      // O bloco "Ferramentas disponiveis:" seguido de linhas "- <nome>:"
      // tambem e' o que o modo mock usa para simular o planner.
      system += "\n\nFerramentas disponiveis:\n";
      for (const auto& [tname, tdecl] : tools) {
        system += "- " + tname + ": " + field_str(*tdecl->block, "descricao");
        const std::string params = tool_params_desc(*tdecl);
        if (!params.empty()) system += " (parametros: " + params + ")";
        system += "\n";
      }
      system += "\nResponda EXATAMENTE uma linha por turno:\n";
      system += "chamar <nome> {<json de argumentos>}  — usa uma ferramenta\n";
      system += "responder: <resposta final>           — quando nao precisar mais de ferramentas\n";

      std::string observations;
      for (int step = 1; step <= max_passos && answer.empty(); ++step) {
        std::string user = "Pedido do usuario: " + prompt + "\n";
        if (!observations.empty()) user += "\nObservacoes ate agora:\n" + observations;
        std::string raw;
        try {
          raw = rt::llm_chat(lc, system, user);
        } catch (const std::exception& e) {
          fail(call.span, std::string("agente '") + agent_name + "': LLM: " + e.what());
        }
        const PlannerAction action = parse_planner_action(raw);
        if (action.kind == PlannerAction::Answer) {
          answer = action.answer;
          break;
        }
        auto tit = entities_.find(action.tool);
        if (tit == entities_.end() || tit->second->key != "ferramenta") {
          fail(call.span, "agente '" + agent_name + "': o LLM pediu a ferramenta '" +
                              action.tool + "', que nao esta declarada");
        }
        rt::ValueMap targs = best_effort_args(*tit->second, prompt);
        for (const auto& [k, v] : action.args.items) targs.set(k, v);  // JSON sobrescreve
        Value obs = run_tool(*tit->second, targs, call.span);

        Value entry = Value::mapa();
        entry.map->set("passo", Value::inteiro(step));
        entry.map->set("ferramenta", Value::texto(action.tool));
        entry.map->set("argumentos", args_to_value(targs));
        entry.map->set("observacao", Value::texto(to_display(obs)));
        rastro.list->push_back(std::move(entry));
        observations += "- " + action.tool + ": " + to_display(obs) + "\n";
      }

      if (answer.empty()) {
        // Estourou max_passos sem resposta final: uma ultima chamada pede a
        // sintese com o que foi observado.
        try {
          const std::string user = "Pedido do usuario: " + prompt + "\n\nObservacoes ate agora:\n" +
                                   observations;
          const std::string raw = rt::llm_chat(
              lc, system + "\n\nLimite de passos atingido. Responda agora no formato responder: <sintese>.",
              user);
          const PlannerAction action = parse_planner_action(raw);
          answer = action.kind == PlannerAction::Answer
                       ? action.answer
                       : "[agente] limite de passos atingido sem resposta final";
        } catch (...) {
          answer = "[agente] limite de passos atingido sem resposta final";
        }
      }
    }
  }

  if (memoria == "conversa") {
    std::lock_guard<std::mutex> lk(agent_memory_mutex_);
    std::string& mem = agent_memory_[agent_name];
    mem += (mem.empty() ? "" : "\n") + ("usuario: " + message) + "\nagente: " + answer;
  }

  Value out = Value::mapa();
  out.map->set("texto", Value::texto(answer));
  out.map->set("rastro", std::move(rastro));
  return out;
}

rt::Value Interpreter::eval_equipe_call(const std::string& team_name, const Expr& call, Env& env) {
  auto it = entities_.find(team_name);
  const ast::Block& cfg = *it->second->block;
  const std::string estrategia = field_word(cfg, "estrategia", "sequencial");

  std::vector<std::pair<std::string, std::string>> members;  // (rotulo, agente)
  if (const Item* af = find_field(cfg, "agentes"); af && af->block) {
    for (const auto& raw : af->block->items) {
      const Item* c = (raw && raw->kind == ItemKind::ListEntry && raw->child) ? raw->child.get()
                                                                              : raw.get();
      if (c && c->kind == ItemKind::Field && c->value && c->value->kind == ExprKind::Name) {
        members.emplace_back(c->key, c->value->text);
      } else if (c && c->kind == ItemKind::Stmt && c->stmt && c->stmt->a &&
                 c->stmt->a->kind == ExprKind::Name) {
        members.emplace_back(c->stmt->a->text, c->stmt->a->text);
      }
    }
  }

  std::string message;
  if (!call.args.empty()) {
    Value m = eval(*call.args[0].value, env);
    message = m.kind == ValueKind::Texto ? m.s : to_display(m);
  }

  if (estrategia == "supervisor") {
    // Supervisor (M9.2): um LLM orquestra, delegando tarefas aos agentes por
    // rotulo ate decidir responder.
    const std::string sup_name = field_word(cfg, "supervisor", "");
    if (sup_name.empty()) {
      fail(call.span, "equipe '" + team_name + "': estrategia 'supervisor' exige 'supervisor: <llm>'");
    }
    const std::string objetivo = field_str(cfg, "objetivo");
    const int max_passos = field_int(cfg, "max_passos", 6);
    rt::LlmConfig lc = llm_config(sup_name, call.span);

    std::string system = objetivo;
    system += "\n\nAgentes disponiveis:\n";
    for (const auto& [rotulo, agente] : members) {
      std::string papel_membro;
      if (auto ait = entities_.find(agente); ait != entities_.end() && ait->second->block) {
        papel_membro = field_str(*ait->second->block, "papel");
      }
      system += "- " + rotulo + ": " + agente;
      if (!papel_membro.empty()) system += " — " + papel_membro;
      system += "\n";
    }
    system += "\nResponda EXATAMENTE uma linha por turno:\n";
    system += "delegar <rotulo> <tarefa>  — delega a tarefa ao agente\n";
    system += "responder: <resposta final>\n";

    Value sup_rastro = Value::lista();
    std::string history;
    std::string final_text;
    std::string last_raw;
    for (int step = 1; step <= max_passos; ++step) {
      std::string user = "Pedido: " + message + "\n";
      if (!history.empty()) user += "\nResultados ate agora:\n" + history;
      std::string raw;
      try {
        raw = rt::llm_chat(lc, system, user);
      } catch (const std::exception& e) {
        fail(call.span, std::string("equipe '") + team_name + "': supervisor: " + e.what());
      }
      last_raw = raw;
      const std::string line = trim_copy(raw);
      if (line.rfind("delegar ", 0) == 0) {
        const std::string rest = trim_copy(line.substr(8));
        const std::size_t sp = rest.find(' ');
        const std::string rotulo = rest.substr(0, sp);
        const std::string tarefa = sp == std::string::npos ? message : trim_copy(rest.substr(sp + 1));
        auto mit = std::find_if(members.begin(), members.end(),
                                [&](const auto& m) { return m.first == rotulo; });
        if (mit == members.end()) {
          fail(call.span, "equipe '" + team_name + "': o supervisor delegou para o rotulo '" +
                              rotulo + "', que nao esta em 'agentes:'");
        }
        Expr fake;  // synthesize a `<agente>.responder <tarefa>` call
        fake.kind = ExprKind::Call;
        fake.span = call.span;
        ast::Arg arg;
        arg.value = std::make_unique<Expr>();
        arg.value->kind = ExprKind::TextLit;
        arg.value->text = tarefa;
        fake.args.push_back(std::move(arg));

        Value r = eval_agente_responder(mit->second, fake, env);
        std::string texto = (r.kind == ValueKind::Mapa && r.map && r.map->find("texto"))
                                ? r.map->find("texto")->s
                                : "";
        Value entry = Value::mapa();
        entry.map->set("agente", Value::texto(rotulo));
        entry.map->set("tarefa", Value::texto(tarefa));
        entry.map->set("texto", Value::texto(texto));
        sup_rastro.list->push_back(std::move(entry));
        history += "- " + rotulo + ": " + texto + "\n";
        continue;
      }
      if (line.rfind("responder:", 0) == 0) {
        final_text = trim_copy(line.substr(10));
      } else {
        final_text = line;  // resposta nao estruturada: usa o texto cru
      }
      break;
    }
    if (final_text.empty()) {
      final_text = last_raw.empty() ? "[supervisor] nenhuma resposta" : last_raw;
    }

    Value out = Value::mapa();
    out.map->set("texto", Value::texto(final_text));
    out.map->set("rastro", std::move(sup_rastro));
    return out;
  }

  Value rastro = Value::lista();
  std::string current = message;
  std::string combined;

  for (const auto& [rotulo, agente] : members) {
    Expr fake;  // synthesize a `<agente>.responder <texto>` call
    fake.kind = ExprKind::Call;
    fake.span = call.span;
    ast::Arg arg;
    arg.value = std::make_unique<Expr>();
    arg.value->kind = ExprKind::TextLit;
    arg.value->text = (estrategia == "paralelo") ? message : current;
    fake.args.push_back(std::move(arg));

    Value r = eval_agente_responder(agente, fake, env);
    std::string texto = (r.kind == ValueKind::Mapa && r.map && r.map->find("texto"))
                            ? r.map->find("texto")->s
                            : "";
    Value entry = Value::mapa();
    entry.map->set("agente", Value::texto(rotulo));
    entry.map->set("texto", Value::texto(texto));
    rastro.list->push_back(std::move(entry));

    current = texto;
    combined += rotulo + ": " + texto + "\n";
  }

  Value out = Value::mapa();
  out.map->set("texto", Value::texto(estrategia == "paralelo" ? combined : current));
  out.map->set("rastro", std::move(rastro));
  return out;
}

// ------------------------------------------------------------------ HTTP service

namespace {

struct Route {
  std::string method;
  std::string path;
  const Item* field = nullptr;  // the `rota` Field (has `entrada:` / `passos:`)
};

std::vector<Route> collect_routes(const ast::Block& block) {
  std::vector<Route> routes;
  for (const auto& it : block.items) {
    if (!it || it->kind != ItemKind::Field || it->key != "rota") continue;
    Route r;
    r.field = it.get();
    if (it->header.size() >= 1 && it->header[0] && it->header[0]->kind == ExprKind::Name) {
      r.method = it->header[0]->text;
      for (char& c : r.method) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    if (it->header.size() >= 2 && it->header[1] && it->header[1]->kind == ExprKind::TextLit) {
      r.path = it->header[1]->text;
    }
    routes.push_back(std::move(r));
  }
  return routes;
}

}  // namespace

int Interpreter::serve(int port_override, int max_requests, int threads) {
  register_decls();

  const Item* svc = nullptr;
  for (const auto& item : program_.items) {
    if (item && item->kind == ItemKind::Decl && item->key == "servico") {
      svc = item.get();
      break;
    }
  }
  if (!svc || !svc->block) {
    out_ << "nenhum 'servico' declarado\n";
    return 1;
  }

  int port = port_override > 0 ? port_override : field_int(*svc->block, "porta", 8080);
  const std::vector<Route> routes = collect_routes(*svc->block);
  // Middleware: todos os blocos `meio:` do servico, em ordem de declaracao.
  // Cada um roda (no Env da rota casada) antes dos `passos:`; se algum
  // executar `responder:`, a resposta dele vale e a rota nao executa.
  std::vector<const Item*> meios;
  for (const auto& it : svc->block->items) {
    if (it && it->kind == ItemKind::Field && it->key == "meio") meios.push_back(it.get());
  }

  rt::HttpServer server;
  const std::string err = server.listen_on("127.0.0.1", port);
  if (!err.empty()) {
    Diagnostic d;
    d.severity = Severity::Error;
    d.code = DiagCode::RuntimeError;
    d.span = svc->span;
    d.message = "servico " + decl_name(*svc) + ": " + err;
    diag_.report(std::move(d));
    return 1;
  }
  out_ << "servico " << decl_name(*svc) << ": escutando 127.0.0.1:" << port << "\n" << std::flush;

  // O tratamento das rotas roda num pool de workers (padrao: ate 4); os
  // estados mutaveis compartilhados do interpretador foram tornados
  // reentrantes (thread_locals + mutexes nos caches).
  if (threads <= 0) {
    const unsigned hc = std::thread::hardware_concurrency();
    threads = hc > 0 ? static_cast<int>(std::min(4u, hc)) : 1;
  }
  const int served = server.run(
      [&](const rt::HttpRequest& req) -> rt::HttpResponse {
        rt::HttpResponse resp;
        const Route* match = nullptr;
        for (const Route& r : routes) {
          if (r.method == req.method && r.path == req.path) {
            match = &r;
            break;
          }
        }

        if (!match) {
          resp.status = 404;
          resp.body = R"({"erro":"rota nao encontrada"})";
        } else {
          Value parsed = Value::mapa();
          bool bad = false;
          if (!req.body.empty()) {
            try {
              parsed = rt::json_parse(req.body);
            } catch (...) {
              bad = true;
            }
          }
          const Item* entrada = match->field->block ? find_field(*match->field->block, "entrada") : nullptr;
          if (!bad && entrada && entrada->value && entrada->value->kind == ExprKind::Name) {
            if (auto t = entities_.find(entrada->value->text);
                t != entities_.end() && t->second->key == "tipo" && t->second->block &&
                parsed.kind == ValueKind::Mapa) {
              for (const auto& f : t->second->block->items) {
                if (f && f->kind == ItemKind::Field && parsed.map && !parsed.map->find(f->key)) {
                  resp.status = 400;
                  resp.body = R"({"erro":"campo ')" + f->key + R"(' ausente"})";
                  bad = true;
                  break;
                }
              }
            }
          }
          if (bad && resp.status != 400) {
            resp.status = 400;
            resp.body = R"({"erro":"corpo JSON invalido"})";
          }

          if (!bad) {
            RouteResponse rr;
            route_resp_ = &rr;
            Env env;
            env.parent = &root_;
            env.vars["entrada"] = parsed;
            const Item* passos = match->field->block ? find_field(*match->field->block, "passos") : nullptr;
            try {
              bool abortado = false;
              for (const Item* meio : meios) {
                if (!meio->block) continue;
                exec_block(*meio->block, env);
                if (rr.set) {  // meio respondeu (ex.: recusa de autenticacao)
                  abortado = true;
                  break;
                }
              }
              if (!abortado && passos && passos->block) exec_block(*passos->block, env);
              resp.status = rr.set ? rr.status : 200;
              resp.body = json_dump(rr.dados.kind == ValueKind::Nulo ? Value::mapa() : rr.dados);
            } catch (const RuntimeAbort& a) {
              resp.status = 500;
              resp.body = R"({"erro":)" + std::string("\"") + a.message + "\"}";
            }
            route_resp_ = nullptr;
          }
        }

        {
          std::lock_guard<std::mutex> lk(log_mutex_);
          out_ << req.method << " " << req.path << " -> " << resp.status << "\n" << std::flush;
        }
        return resp;
      },
      max_requests, threads);
  return served < 0 ? 1 : 0;
}

// ------------------------------------------------------------------ statements

void Interpreter::exec_block(const ast::Block& block, Env& env) {
  // Desembrulha item de lista (`- x`) ate o conteudo.
  auto unwrap = [](const Item* it) -> const Item* {
    while (it && it->kind == ItemKind::ListEntry) {
      it = it->child ? it->child.get()
                     : (it->block && !it->block->items.empty() ? it->block->items[0].get()
                                                               : nullptr);
    }
    return it;
  };
  const auto& items = block.items;
  for (std::size_t k = 0; k < items.size(); ++k) {
    const Item* it = items[k].get();
    if (!it) continue;
    // `- senao:` solto em 'passos:' (o parser nao o anexa ao 'se' quando ele
    // vem como item de lista separado): trata como o 'senao' do 'se' imediato
    // anterior — mas so se esse 'se' nao pegou nenhum ramo. Fora desse par,
    // mantem o legado: executa o bloco incondicionalmente.
    const Item* bare = unwrap(it);
    const Item* prev = k > 0 ? unwrap(items[k - 1].get()) : nullptr;
    const bool prev_if_bare =
        prev && prev->kind == ItemKind::Stmt && prev->stmt &&
        prev->stmt->kind == StmtKind::If && !prev->stmt->else_body;
    if (bare && bare->kind == ItemKind::Field && bare->key == "senao" &&
        bare->header.empty() && bare->block) {
      if (prev_if_bare) {
        if (!last_if_taken_) {
          Env inner;
          inner.parent = &env;
          exec_block(*bare->block, inner);
        }
      } else {
        Env inner;
        inner.parent = &env;
        exec_block(*bare->block, inner);
      }
      continue;
    }
    exec_item(*it, env);
  }
}

void Interpreter::exec_item(const Item& item, Env& env) {
  switch (item.kind) {
    case ItemKind::Stmt:
      if (item.stmt) exec_stmt(*item.stmt, env);
      return;
    case ItemKind::ListEntry:
      if (item.block) exec_block(*item.block, env);
      if (item.child) exec_item(*item.child, env);
      return;
    case ItemKind::Field:
      if (item.key == "verificar") {
        run_verificar(item, env);
        return;
      }
      if ((item.key == "responder" || item.key == "responder_em_fluxo") && route_resp_ &&
          item.block) {
        for (const auto& sub : item.block->items) {
          if (!sub || sub->kind != ItemKind::Field) continue;
          if (sub->key == "status" && sub->value) {
            route_resp_->status = static_cast<int>(eval(*sub->value, env).as_number());
          } else if (sub->key == "dados") {
            if (sub->block) {
              Value m = Value::mapa();
              for (const auto& d : sub->block->items) {
                if (d && d->kind == ItemKind::Field && d->value) {
                  m.map->set(d->key, eval(*d->value, env));
                }
              }
              route_resp_->dados = std::move(m);
            } else if (sub->value) {
              route_resp_->dados = eval(*sub->value, env);
            }
          }
        }
        route_resp_->set = true;
        return;
      }
      if (item.value) eval(*item.value, env);
      if (item.block) {
        // e.g. `- responder:` inside `passos:` — evaluate nested field values
        Env inner;
        inner.parent = &env;
        exec_block(*item.block, inner);
      }
      return;
    case ItemKind::Decl:
      return;
  }
}

void Interpreter::exec_stmt(const Stmt& stmt, Env& env) {
  switch (stmt.kind) {
    case StmtKind::Expr:
      if (stmt.a) eval(*stmt.a, env);
      return;
    case StmtKind::Assign: {
      Value v = stmt.b ? eval(*stmt.b, env) : Value::nulo();
      if (stmt.a && stmt.a->kind == ExprKind::Name) {
        env.set(stmt.a->text, std::move(v));
      } else if (stmt.a) {
        eval(*stmt.a, env);  // evaluate target for side effects; member assign unsupported
      }
      return;
    }
    case StmtKind::Return:
      throw ReturnSignal{stmt.a ? eval(*stmt.a, env) : Value::nulo()};
    case StmtKind::If: {
      if (stmt.a && eval(*stmt.a, env).truthy()) {
        Env inner;
        inner.parent = &env;
        exec_block(stmt.body, inner);
        last_if_taken_ = true;
        return;
      }
      for (const auto& ei : stmt.elifs) {
        if (ei.cond && eval(*ei.cond, env).truthy()) {
          Env inner;
          inner.parent = &env;
          exec_block(ei.body, inner);
          last_if_taken_ = true;
          return;
        }
      }
      last_if_taken_ = false;
      if (stmt.else_body) {
        Env inner;
        inner.parent = &env;
        exec_block(*stmt.else_body, inner);
      }
      return;
    }
    case StmtKind::ForEach: {
      Value seq = stmt.a ? eval(*stmt.a, env) : Value::nulo();
      if (seq.kind != ValueKind::Lista && seq.kind != ValueKind::Tabela) {
        fail(stmt.span, std::string("'para cada' espera uma lista, recebeu ") + seq.type_name());
      }
      if (seq.list) {
        for (const Value& element : *seq.list) {
          Env inner;
          inner.parent = &env;
          inner.vars[stmt.name] = element;
          exec_block(stmt.body, inner);
        }
      }
      return;
    }
    case StmtKind::While: {
      std::int64_t guard = 0;
      while (stmt.a && eval(*stmt.a, env).truthy()) {
        if (++guard > kLoopGuard) fail(stmt.span, "laco 'enquanto' excedeu o limite de iteracoes");
        Env inner;
        inner.parent = &env;
        exec_block(stmt.body, inner);
      }
      return;
    }
    case StmtKind::Try: {
      try {
        Env inner;
        inner.parent = &env;
        exec_block(stmt.body, inner);
      } catch (const RuntimeAbort& a) {
        if (!stmt.catch_body) return;
        Env inner;
        inner.parent = &env;
        if (!stmt.name.empty()) inner.vars[stmt.name] = Value::texto(a.message);
        exec_block(*stmt.catch_body, inner);
      }
      return;
    }
  }
}

// ------------------------------------------------------------------ expressions

std::string Interpreter::interpolate(const std::string& text, Env& env) {
  std::string out;
  for (std::size_t k = 0; k < text.size();) {
    if (k + 1 < text.size() && text[k] == '{' && text[k + 1] == '{') {
      std::size_t end = text.find("}}", k + 2);
      if (end != std::string::npos) {
        std::string name = text.substr(k + 2, end - (k + 2));
        while (!name.empty() && name.front() == ' ') name.erase(name.begin());
        while (!name.empty() && name.back() == ' ') name.pop_back();
        if (Value* v = env.lookup(name)) {
          out += to_display(*v);
        } else {
          out += "{{" + name + "}}";
        }
        k = end + 2;
        continue;
      }
    }
    out += text[k++];
  }
  return out;
}

Value Interpreter::eval(const Expr& expr, Env& env) {
  switch (expr.kind) {
    case ExprKind::IntLit:
      return Value::inteiro(std::strtoll(expr.text.c_str(), nullptr, 10));
    case ExprKind::DecimalLit:
      return Value::decimal(std::strtod(expr.text.c_str(), nullptr));
    case ExprKind::TextLit:
      return Value::texto(interpolate(expr.text, env));
    case ExprKind::BoolLit:
      return Value::logico(expr.boolean);
    case ExprKind::NullLit:
      return Value::nulo();
    case ExprKind::Name: {
      if (Value* v = env.lookup(expr.text)) return *v;
      if (auto it = entities_.find(expr.text); it != entities_.end()) return Value::texto(expr.text);
      fail(expr.span, "nome '" + expr.text + "' nao definido");
    }
    case ExprKind::Member: {
      Value base = eval(*expr.lhs, env);
      if (base.kind == ValueKind::Tensor && base.tensor) {
        const rt::Tensor& t = *base.tensor;
        const std::string& m = expr.text;
        try {
          if (m == "forma") {
            rt::ValueList dims;
            for (std::int64_t d : t.shape) dims.push_back(Value::inteiro(d));
            return Value::lista(std::move(dims));
          }
          if (m == "soma") return Value::decimal(rt::sum_all(t));
          if (m == "media") return Value::decimal(rt::mean_all(t));
          if (m == "argmax") return Value::inteiro(rt::argmax_last(t));
          if (m == "transposta") return Value::tensor_de(rt::transpose2d(t));
          if (m == "softmax") return Value::tensor_de(rt::softmax_last(t));
          if (word_in(m, {"relu", "gelu", "silu", "sigmoide", "tanh"})) {
            return Value::tensor_de(rt::apply_unary(t, m));
          }
          if (m == "item") {
            if (t.size() != 1) fail(expr.span, "item espera um tensor de 1 elemento");
            return Value::decimal(t.data[0]);
          }
          if (m == "tamanho") return Value::inteiro(t.size());
        } catch (const std::exception& e) {
          fail(expr.span, std::string(e.what()));
        }
      }
      if ((base.kind == ValueKind::Mapa || base.kind == ValueKind::Tabela) && base.map) {
        if (Value* f = base.map->find(expr.text)) return *f;
      }
      if (base.kind == ValueKind::Mapa && base.map) {
        if (Value* f = base.map->find(expr.text)) return *f;
      }
      if (expr.text == "tamanho") {
        if (base.kind == ValueKind::Lista || base.kind == ValueKind::Tabela) {
          return Value::inteiro(base.list ? static_cast<std::int64_t>(base.list->size()) : 0);
        }
        if (base.kind == ValueKind::Texto) {
          return Value::inteiro(static_cast<std::int64_t>(base.s.size()));
        }
      }
      if (expr.optional) return Value::nulo();
      fail(expr.span, std::string("'") + base.type_name() + "' nao tem o campo '" + expr.text + "'");
    }
    case ExprKind::Index: {
      // `tensor [ ... ]` / `zeros [ ... ]` etc. — constructor sugar, unambiguous
      // because these names are never bound as variables.
      if (expr.lhs->kind == ExprKind::Name &&
          (expr.lhs->text == "tensor" || expr.lhs->text == "zeros" || expr.lhs->text == "uns" ||
           expr.lhs->text == "aleatorio")) {
        Value elems = Value::lista();
        for (const auto& el : expr.elems) elems.list->push_back(eval(*el, env));
        const std::string& n = expr.lhs->text;
        if (n == "tensor") return Value::tensor_de(value_to_tensor(elems, expr.span));
        std::vector<std::int64_t> shape;
        for (const Value& v : *elems.list) shape.push_back(static_cast<std::int64_t>(v.as_number()));
        if (shape.empty()) fail(expr.span, n + " precisa de uma forma");
        if (n == "zeros") return Value::tensor_de(rt::Tensor::zeros(shape));
        if (n == "uns") return Value::tensor_de(rt::Tensor::ones(shape));
        return Value::tensor_de(rt::Tensor::xavier(shape, shape.front(), shape.back(), 42));
      }
      Value base = eval(*expr.lhs, env);
      if (expr.elems.empty()) fail(expr.span, "indice vazio");
      Value idx = eval(*expr.elems[0], env);
      if (base.kind == ValueKind::Lista || base.kind == ValueKind::Tabela) {
        if (!base.list) return Value::nulo();
        std::int64_t n = static_cast<std::int64_t>(base.list->size());
        std::int64_t i = idx.is_number() ? static_cast<std::int64_t>(idx.as_number()) : 0;
        if (i < 0) i += n;
        if (i < 0 || i >= n) fail(expr.span, "indice fora dos limites");
        return (*base.list)[static_cast<std::size_t>(i)];
      }
      if (base.kind == ValueKind::Mapa && base.map) {
        Value* f = base.map->find(idx.kind == ValueKind::Texto ? idx.s : to_display(idx));
        return f ? *f : Value::nulo();
      }
      fail(expr.span, std::string("nao e possivel indexar '") + base.type_name() + "'");
    }
    case ExprKind::Slice: {
      Value base = eval(*expr.lhs, env);
      if (base.kind != ValueKind::Lista || !base.list) {
        fail(expr.span, "fatia so funciona em listas");
      }
      std::int64_t n = static_cast<std::int64_t>(base.list->size());
      std::int64_t lo = expr.rhs ? static_cast<std::int64_t>(eval(*expr.rhs, env).as_number()) : 0;
      std::int64_t hi = expr.extra ? static_cast<std::int64_t>(eval(*expr.extra, env).as_number()) : n;
      lo = std::clamp<std::int64_t>(lo, 0, n);
      hi = std::clamp<std::int64_t>(hi, lo, n);
      rt::ValueList out(base.list->begin() + lo, base.list->begin() + hi);
      return Value::lista(std::move(out));
    }
    case ExprKind::Unary: {
      Value v = eval(*expr.rhs, env);
      if (expr.text == "nao") return Value::logico(!v.truthy());
      if (expr.text == "-") {
        if (v.kind == ValueKind::Inteiro) return Value::inteiro(-v.i);
        return Value::decimal(-v.as_number());
      }
      return v;
    }
    case ExprKind::Binary:
      return eval_binary(expr, env);
    case ExprKind::ListLit: {
      rt::ValueList out;
      out.reserve(expr.elems.size());
      for (const auto& el : expr.elems) out.push_back(eval(*el, env));
      return Value::lista(std::move(out));
    }
    case ExprKind::MapLit: {
      Value m = Value::mapa();
      for (const auto& en : expr.entries) m.map->set(en.key, eval(*en.value, env));
      return m;
    }
    case ExprKind::Assign: {
      Value v = eval(*expr.rhs, env);
      if (expr.lhs && expr.lhs->kind == ExprKind::Name) env.set(expr.lhs->text, v);
      return v;
    }
    case ExprKind::Device:
      return eval(*expr.lhs, env);
    case ExprKind::Call:
      return eval_call(expr, env);
  }
  return Value::nulo();
}

Value Interpreter::eval_binary(const Expr& expr, Env& env) {
  const std::string& op = expr.text;

  if (op == "e") return Value::logico(eval(*expr.lhs, env).truthy() && eval(*expr.rhs, env).truthy());
  if (op == "ou") return Value::logico(eval(*expr.lhs, env).truthy() || eval(*expr.rhs, env).truthy());

  Value a = eval(*expr.lhs, env);
  Value b = eval(*expr.rhs, env);

  if ((a.kind == ValueKind::Tensor || b.kind == ValueKind::Tensor) &&
      (op == "+" || op == "-" || op == "*" || op == "/")) {
    try {
      if (a.kind == ValueKind::Tensor && b.kind == ValueKind::Tensor) {
        const rt::Tensor& x = *a.tensor;
        const rt::Tensor& y = *b.tensor;
        if (op == "+") return Value::tensor_de(rt::add(x, y));
        if (op == "-") return Value::tensor_de(rt::sub(x, y));
        if (op == "*") return Value::tensor_de(rt::mul(x, y));
        return Value::tensor_de(rt::div(x, y));
      }
      const bool tensor_left = a.kind == ValueKind::Tensor;
      const rt::Tensor& t = tensor_left ? *a.tensor : *b.tensor;
      const float s = tensor_left ? static_cast<float>(b.as_number())
                                  : static_cast<float>(a.as_number());
      if (!tensor_left && (op == "-" || op == "/")) {
        // scalar (op) tensor
        rt::Tensor lhs = rt::Tensor::filled(t.shape, s);
        return Value::tensor_de(op == "-" ? rt::sub(lhs, t) : rt::div(lhs, t));
      }
      return Value::tensor_de(rt::scalar_op(t, s, op[0]));
    } catch (const std::exception& e) {
      fail(expr.span, std::string(e.what()));
    }
  }

  if (op == "|") return Value::texto(to_display(a) + " | " + to_display(b));

  bool ok = false;
  Value r = rt::apply_binop(op, a, b, &ok);
  if (!ok) fail(expr.span, "operador desconhecido '" + op + "'");
  return r;
}

std::vector<Value> Interpreter::eval_args(const Expr& call, Env& env) {
  std::vector<Value> out;
  for (const auto& a : call.args) {
    if (a.name.empty()) out.push_back(eval(*a.value, env));
  }
  return out;
}

rt::ValueMap Interpreter::eval_kwargs(const Expr& call, Env& env) {
  rt::ValueMap kw;
  for (const auto& a : call.args) {
    if (!a.name.empty()) kw.set(a.name, eval(*a.value, env));
  }
  if (call.block) {
    for (const auto& it : call.block->items) {
      if (it && it->kind == ItemKind::Field && it->value) {
        kw.set(it->key, eval(*it->value, env));
      }
    }
  }
  return kw;
}

// `particionar_por:` aceita um nome de coluna (texto) ou uma lista de nomes
// para particao composta. Retorna vazio quando o argumento ausente.
std::vector<std::string> Interpreter::parse_particionar_por(const rt::ValueMap& kw,
                                                           const char* builtin,
                                                           const Span& span) {
  std::vector<std::string> cols;
  const Value* p = kw.find("particionar_por");
  if (!p) return cols;
  if (p->kind == ValueKind::Texto) {
    cols.push_back(p->s);
    return cols;
  }
  if (p->kind == ValueKind::Lista && p->list) {
    for (const Value& item : *p->list) {
      if (item.kind != ValueKind::Texto) {
        fail(span, std::string(builtin) +
                       ": 'particionar_por' deve ser texto ou lista de textos "
                       "(ex.: particionar_por: [\"estado\", \"mes\"])");
      }
      cols.push_back(item.s);
    }
    return cols;
  }
  fail(span, std::string(builtin) +
                 ": 'particionar_por' deve ser texto ou lista de textos "
                 "(ex.: particionar_por: [\"estado\", \"mes\"])");
}

Value Interpreter::eval_call(const Expr& expr, Env& env) {
  const Expr& callee = *expr.lhs;

  if (callee.kind == ExprKind::Member) {
    Value receiver = eval(*callee.lhs, env);
    return eval_method(callee.text, std::move(receiver), expr, env);
  }

  if (callee.kind == ExprKind::Name) {
    const std::string& name = callee.text;
    if (auto it = functions_.find(name); it != functions_.end()) {
      return call_function(*it->second, eval_args(expr, env), expr.span);
    }
    return eval_builtin(name, expr, env);
  }

  fail(expr.span, "chamada invalida");
}

Value Interpreter::call_function(const Item& fn, std::vector<Value> args, Span span) {
  // Try the bytecode VM for functions in its pure subset; fall back otherwise.
  // A compilacao e lazy e cacheada: o mutex so cobre o mapa; o Chunk em si
  // e imutavel durante a execucao e pode ser rodado por varias threads.
  std::shared_ptr<vm::Chunk> chunk;
  {
    std::lock_guard<std::mutex> lk(vm_chunks_mutex_);
    auto cit = vm_chunks_.find(&fn);
    if (cit == vm_chunks_.end()) {
      try {
        std::unordered_set<std::string> names;
        for (const auto& kv : functions_) names.insert(kv.first);
        chunk = std::make_shared<vm::Chunk>(vm::compile_function(fn, names));
      } catch (const vm::NotCompilable&) {
        chunk = nullptr;
      }
      cit = vm_chunks_.emplace(&fn, chunk).first;
      if (std::getenv("TILT_VM_DEBUG") && chunk) {
        const vm::Chunk& c = *chunk;
        std::lock_guard<std::mutex> log_lk(log_mutex_);
        out_ << "; chunk " << decl_name(fn) << " locals=" << c.num_locals << "\n";
        for (std::size_t i = 0; i < c.code.size(); ++i) {
          out_ << ";  " << i << ": op=" << static_cast<int>(c.code[i].op) << " a=" << c.code[i].a
               << " b=" << c.code[i].b << "\n";
        }
      }
    }
    chunk = cit->second;
  }
  if (chunk) {
    vm::Vm machine(out_, [this](const std::string& name, std::vector<Value>& a, bool* handled) {
      auto f = functions_.find(name);
      if (f == functions_.end()) {
        *handled = false;
        return Value::nulo();
      }
      *handled = true;
      return call_function(*f->second, std::move(a), Span{});
    });
    try {
      return machine.run(*chunk, std::move(args));
    } catch (const std::exception& e) {
      fail(fn.span, std::string("VM: ") + e.what());
    }
  }

  Env env;
  env.parent = &root_;
  for (std::size_t k = 0; k < fn.params.size(); ++k) {
    env.vars[fn.params[k].name] = k < args.size() ? args[k] : Value::nulo();
  }
  if (!fn.block) return Value::nulo();
  try {
    exec_block(*fn.block, env);
  } catch (const ReturnSignal& r) {
    return r.value;
  }
  (void)span;
  return Value::nulo();
}

// ------------------------------------------------------------------ builtins

Value Interpreter::eval_builtin(const std::string& name, const Expr& call, Env& env) {
  auto args = [&] { return eval_args(call, env); };

  if (name == "imprimir" || name == "imprima" || name == "print") {
    auto a = args();
    for (std::size_t k = 0; k < a.size(); ++k) {
      if (k) out_ << ' ';
      out_ << to_display(a[k]);
    }
    out_ << '\n';
    return Value::nulo();
  }
  if (name == "registrar" || name == "log") {
    auto a = args();
    out_ << "[log]";
    for (const Value& v : a) out_ << ' ' << to_display(v);
    out_ << '\n';
    return Value::nulo();
  }
  if (name == "env") {
    auto a = args();
    if (a.empty() || a[0].kind != ValueKind::Texto) return Value::nulo();
    const char* v = std::getenv(a[0].s.c_str());
    return v ? Value::texto(v) : Value::nulo();
  }
  if (name == "tamanho" || name == "contar") {
    auto a = args();
    if (a.empty()) return Value::inteiro(0);
    const Value& v = a[0];
    if (v.kind == ValueKind::Lista || v.kind == ValueKind::Tabela) {
      return Value::inteiro(v.list ? static_cast<std::int64_t>(v.list->size()) : 0);
    }
    if (v.kind == ValueKind::Texto) return Value::inteiro(static_cast<std::int64_t>(v.s.size()));
    if (v.kind == ValueKind::Mapa) {
      return Value::inteiro(v.map ? static_cast<std::int64_t>(v.map->items.size()) : 0);
    }
    return Value::inteiro(0);
  }
  if (name == "somar" || name == "media" || name == "min" || name == "max") {
    auto a = args();
    if (a.empty() || a[0].kind != ValueKind::Lista || !a[0].list || a[0].list->empty()) {
      return Value::inteiro(0);
    }
    const auto& xs = *a[0].list;
    double acc = xs[0].as_number();
    for (std::size_t k = 1; k < xs.size(); ++k) {
      double n = xs[k].as_number();
      if (name == "somar" || name == "media") acc += n;
      else if (name == "min") acc = std::min(acc, n);
      else acc = std::max(acc, n);
    }
    if (name == "media") acc /= static_cast<double>(xs.size());
    if (acc == static_cast<double>(static_cast<std::int64_t>(acc)) && name != "media") {
      return Value::inteiro(static_cast<std::int64_t>(acc));
    }
    return Value::decimal(acc);
  }
  if (name == "intervalo" || name == "ate") {
    auto a = args();
    std::int64_t lo = 0;
    std::int64_t hi = 0;
    if (a.size() == 1) hi = static_cast<std::int64_t>(a[0].as_number());
    else if (a.size() >= 2) {
      lo = static_cast<std::int64_t>(a[0].as_number());
      hi = static_cast<std::int64_t>(a[1].as_number());
    }
    rt::ValueList out;
    for (std::int64_t k = lo; k < hi; ++k) out.push_back(Value::inteiro(k));
    return Value::lista(std::move(out));
  }
  if (name == "dividir") {
    auto a = args();
    if (a.size() < 2 || a[0].kind != ValueKind::Texto || a[1].kind != ValueKind::Texto) {
      fail(call.span, "dividir espera (texto, separador)");
    }
    rt::ValueList out;
    const std::string& s = a[0].s;
    const std::string& sep = a[1].s;
    if (sep.empty()) {
      out.push_back(Value::texto(s));
    } else {
      std::size_t start = 0;
      std::size_t pos;
      while ((pos = s.find(sep, start)) != std::string::npos) {
        out.push_back(Value::texto(s.substr(start, pos - start)));
        start = pos + sep.size();
      }
      out.push_back(Value::texto(s.substr(start)));
    }
    return Value::lista(std::move(out));
  }
  if (name == "tensor" || name == "zeros" || name == "uns" || name == "aleatorio") {
    auto a = args();
    rt::ValueMap kw = eval_kwargs(call, env);
    auto to_shape = [&](const Value& v) {
      std::vector<std::int64_t> shape;
      if (v.kind == ValueKind::Lista && v.list) {
        for (const Value& e : *v.list) shape.push_back(static_cast<std::int64_t>(e.as_number()));
      }
      return shape;
    };
    if (name == "tensor" && !a.empty() && a[0].kind == ValueKind::Lista) {
      return Value::tensor_de(value_to_tensor(a[0], call.span));
    }
    std::vector<std::int64_t> shape = a.empty() ? std::vector<std::int64_t>{} : to_shape(a[0]);
    if (const Value* f = kw.find("forma")) shape = to_shape(*f);
    if (shape.empty()) fail(call.span, name + " espera uma forma, ex.: " + name + " [2, 3]");
    if (name == "zeros" || name == "tensor") {
      const Value* fill = kw.find("valor");
      return Value::tensor_de(rt::Tensor::filled(shape, fill ? static_cast<float>(fill->as_number()) : 0.0F));
    }
    if (name == "uns") return Value::tensor_de(rt::Tensor::ones(shape));
    const Value* seed = kw.find("semente");
    std::uint64_t s = seed ? static_cast<std::uint64_t>(seed->as_number()) : 42;
    return Value::tensor_de(rt::Tensor::xavier(shape, shape.front(), shape.back(), s));
  }
  if (name == "ler_csv") {
    auto a = args();
    if (a.empty() || a[0].kind != ValueKind::Texto) fail(call.span, "ler_csv espera um caminho");
    return read_csv_file(a[0].s, call.span);
  }
  if (name == "ler_parquet") {
    auto a = args();
    if (a.empty() || a[0].kind != ValueKind::Texto) fail(call.span, "ler_parquet espera um caminho");
    try {
      Value t = rt::parquet_read(a[0].s);
      t.kind = ValueKind::Tabela;
      return t;
    } catch (const std::exception& e) {
      fail(call.span, std::string(e.what()));
    }
  }
  if (name == "ler_delta") {
    auto a = args();
    rt::ValueMap kw = eval_kwargs(call, env);
    if (a.empty() || a[0].kind != ValueKind::Texto) fail(call.span, "ler_delta espera um diretorio");
    const Value* onde = kw.find("onde");
    if (onde && onde->kind != ValueKind::Mapa) {
      fail(call.span, "ler_delta: 'onde' deve ser um mapa de colunas e valores "
                      "(ex.: onde: { estado: \"SP\" })");
    }
    try {
      Value t = rt::delta_read(a[0].s, onde);
      t.kind = ValueKind::Tabela;
      return t;
    } catch (const std::exception& e) {
      fail(call.span, std::string(e.what()));
    }
  }
  if (name == "carregador") {
    auto a = args();
    rt::ValueMap kw = eval_kwargs(call, env);
    if (a.empty() || a[0].kind != ValueKind::Texto) fail(call.span, "carregador espera um caminho");
    const Value* alvo = kw.find("alvo");
    if (!alvo || alvo->kind != ValueKind::Texto) fail(call.span, "carregador espera 'alvo: \"coluna\"'");
    Value tbl = read_csv_file(a[0].s, call.span);
    const rt::ValueList& rows = tbl.list ? *tbl.list : rt::ValueList{};
    std::vector<std::string> feats;
    if (!rows.empty() && rows[0].map) {
      for (const auto& kv : rows[0].map->items) {
        if (kv.first != alvo->s) feats.push_back(kv.first);
      }
    }
    rt::Tensor x;
    x.shape = {static_cast<std::int64_t>(rows.size()), static_cast<std::int64_t>(feats.size())};
    x.data.reserve(rows.size() * feats.size());
    Value ys = Value::lista();
    for (const Value& row : rows) {
      for (const std::string& c : feats) {
        const Value* cell = row.map ? row.map->find(c) : nullptr;
        x.data.push_back(cell ? static_cast<float>(cell->as_number()) : 0.0F);
      }
      const Value* t = row.map ? row.map->find(alvo->s) : nullptr;
      ys.list->push_back(Value::inteiro(t ? static_cast<std::int64_t>(t->as_number()) : 0));
    }
    Value out = Value::mapa();
    out.map->set("x", Value::tensor_de(std::move(x)));
    out.map->set("y", std::move(ys));
    rt::ValueList fl;
    for (const std::string& c : feats) fl.push_back(Value::texto(c));
    out.map->set("atributos", Value::lista(std::move(fl)));
    return out;
  }
  if (name == "ler_json") {
    auto a = args();
    if (a.empty() || a[0].kind != ValueKind::Texto) fail(call.span, "ler_json espera um caminho");
    std::ifstream in(a[0].s);
    if (!in) fail(call.span, "nao foi possivel abrir '" + a[0].s + "'");
    std::ostringstream ss;
    ss << in.rdbuf();
    Value parsed;
    try {
      parsed = rt::json_parse(ss.str());
    } catch (const std::exception& e) {
      fail(call.span, std::string(e.what()));
    }
    if (parsed.kind == ValueKind::Lista) parsed.kind = ValueKind::Tabela;
    return parsed;
  }
  if (name == "escrever_json") {
    auto a = args();
    if (a.size() < 2 || a[1].kind != ValueKind::Texto) {
      fail(call.span, "escrever_json espera (valor, caminho)");
    }
    std::ofstream out(a[1].s);
    if (!out) fail(call.span, "nao foi possivel escrever '" + a[1].s + "'");
    out << rt::json_dump(a[0]);
    return Value::nulo();
  }
  if (name == "escrever_csv") {
    auto a = args();
    if (a.size() < 2 || (a[0].kind != ValueKind::Tabela && a[0].kind != ValueKind::Lista)) {
      fail(call.span, "escrever espera (tabela, caminho)");
    }
    std::ofstream outf(a[1].s);
    if (!outf) fail(call.span, "nao foi possivel escrever '" + a[1].s + "'");
    const auto& rows = *a[0].list;
    if (!rows.empty() && rows[0].kind == ValueKind::Mapa && rows[0].map) {
      const auto& hdr = rows[0].map->items;
      for (std::size_t k = 0; k < hdr.size(); ++k) outf << (k ? "," : "") << hdr[k].first;
      outf << '\n';
      for (const Value& r : rows) {
        for (std::size_t k = 0; k < hdr.size(); ++k) {
          const Value* c = r.map ? r.map->find(hdr[k].first) : nullptr;
          outf << (k ? "," : "") << (c ? to_display(*c) : "");
        }
        outf << '\n';
      }
    }
    return Value::nulo();
  }
  if (name == "escrever_parquet") {
    auto a = args();
    rt::ValueMap kw = eval_kwargs(call, env);
    if (a.size() < 2 || (a[0].kind != ValueKind::Tabela && a[0].kind != ValueKind::Lista)) {
      fail(call.span, "escrever_parquet espera (tabela, caminho)");
    }
    if (a[1].kind != ValueKind::Texto) {
      fail(call.span, "escrever_parquet espera (tabela, caminho)");
    }
    rt::ParquetWriteOpts opts;
    const Value* codec = kw.find("codec");
    if (codec) {
      if (codec->kind != ValueKind::Texto) {
        fail(call.span, "escrever_parquet: 'codec' deve ser \"gzip\" ou \"snappy\"");
      }
      if (codec->s == "gzip") {
        opts.codec = 2;
      } else if (codec->s == "snappy") {
        opts.codec = 1;
      } else {
        fail(call.span, "escrever_parquet: codec '" + codec->s +
                            "' invalido (use \"gzip\" ou \"snappy\")");
      }
    }
    const Value* paginas = kw.find("paginas");
    if (paginas) {
      if (paginas->kind != ValueKind::Texto) {
        fail(call.span, "escrever_parquet: 'paginas' deve ser \"v1\" ou \"v2\"");
      }
      if (paginas->s == "v1") {
        opts.paginas_v2 = false;
      } else if (paginas->s == "v2") {
        opts.paginas_v2 = true;
      } else {
        fail(call.span, "escrever_parquet: paginas '" + paginas->s +
                            "' invalido (use \"v1\" ou \"v2\")");
      }
    }
    try {
      rt::parquet_write(a[1].s, a[0], nullptr, opts);
    } catch (const std::exception& e) {
      fail(call.span, std::string(e.what()));
    }
    return Value::nulo();
  }
  if (name == "escrever_delta") {
    auto a = args();
    rt::ValueMap kw = eval_kwargs(call, env);
    if (a.size() < 2 || (a[0].kind != ValueKind::Tabela && a[0].kind != ValueKind::Lista)) {
      fail(call.span, "escrever_delta espera (tabela, diretorio)");
    }
    std::vector<std::string> part_cols = parse_particionar_por(kw, "escrever_delta", call.span);
    try {
      rt::delta_write(a[1].s, a[0], part_cols);
    } catch (const std::exception& e) {
      fail(call.span, std::string(e.what()));
    }
    return Value::nulo();
  }
  if (name == "anexar_delta") {
    auto a = args();
    rt::ValueMap kw = eval_kwargs(call, env);
    if (a.size() < 2 || (a[0].kind != ValueKind::Tabela && a[0].kind != ValueKind::Lista)) {
      fail(call.span, "anexar_delta espera (tabela, diretorio)");
    }
    std::vector<std::string> part_cols = parse_particionar_por(kw, "anexar_delta", call.span);
    try {
      rt::delta_append(a[1].s, a[0], part_cols);
    } catch (const std::exception& e) {
      fail(call.span, std::string(e.what()));
    }
    return Value::nulo();
  }
  if (name == "ler_iceberg") {
    auto a = args();
    rt::ValueMap kw = eval_kwargs(call, env);
    if (a.empty() || a[0].kind != ValueKind::Texto) fail(call.span, "ler_iceberg espera um diretorio");
    const Value* onde = kw.find("onde");
    if (onde && onde->kind != ValueKind::Mapa) {
      fail(call.span, "ler_iceberg: 'onde' deve ser um mapa de colunas e valores "
                      "(ex.: onde: { estado: \"SP\" })");
    }
    try {
      Value t = rt::iceberg_read(a[0].s, onde);
      t.kind = ValueKind::Tabela;
      return t;
    } catch (const std::exception& e) {
      fail(call.span, std::string(e.what()));
    }
  }
  if (name == "escrever_iceberg") {
    auto a = args();
    rt::ValueMap kw = eval_kwargs(call, env);
    if (a.size() < 2 || (a[0].kind != ValueKind::Tabela && a[0].kind != ValueKind::Lista)) {
      fail(call.span, "escrever_iceberg espera (tabela, diretorio)");
    }
    std::vector<std::string> part_cols = parse_particionar_por(kw, "escrever_iceberg", call.span);
    try {
      rt::iceberg_write(a[1].s, a[0], part_cols);
    } catch (const std::exception& e) {
      fail(call.span, std::string(e.what()));
    }
    return Value::nulo();
  }
  if (name == "anexar_iceberg") {
    auto a = args();
    rt::ValueMap kw = eval_kwargs(call, env);
    if (a.size() < 2 || (a[0].kind != ValueKind::Tabela && a[0].kind != ValueKind::Lista)) {
      fail(call.span, "anexar_iceberg espera (tabela, diretorio)");
    }
    std::vector<std::string> part_cols = parse_particionar_por(kw, "anexar_iceberg", call.span);
    try {
      rt::iceberg_append(a[1].s, a[0], part_cols);
    } catch (const std::exception& e) {
      fail(call.span, std::string(e.what()));
    }
    return Value::nulo();
  }
  if (name == "responder") {
    rt::ValueMap kw = eval_kwargs(call, env);
    out_ << "resposta:";
    for (const auto& kv : kw.items) out_ << ' ' << kv.first << '=' << to_display(kv.second);
    out_ << '\n';
    return Value::nulo();
  }

  if (name == "perguntar" || name == "perguntar_em_fluxo") {
    return eval_perguntar(call, env);
  }
  if (name == "incorporar") {
    auto a = args();
    const std::string model = !a.empty() && a[0].kind == ValueKind::Texto ? a[0].s : "";
    std::string text;
    if (a.size() > 1) text = a[1].kind == ValueKind::Texto ? a[1].s : to_display(a[1]);
    try {
      std::vector<float> v = rt::llm_embed(model, text);
      rt::Tensor t;
      t.shape = {static_cast<std::int64_t>(v.size())};
      t.data = std::move(v);
      return Value::tensor_de(std::move(t));
    } catch (const std::exception& e) {
      fail(call.span, std::string("incorporar: ") + e.what());
    }
  }
  if (name == "checar_tilt") {
    auto a = args();
    if (a.empty() || a[0].kind != ValueKind::Texto) {
      fail(call.span, "checar_tilt espera o caminho de um arquivo .tilt");
    }
    Value result = Value::mapa();
    std::optional<SourceFile> src;
    try {
      src = SourceFile::load(a[0].s);
    } catch (const std::exception& e) {
      result.map->set("ok", Value::logico(false));
      result.map->set("erro", Value::texto(std::string(e.what())));
      return result;
    }
    DiagnosticEngine d(&src.value());
    Lexer lx(src.value(), d);
    std::vector<Token> toks = lx.tokenize();
    Parser ps(toks, d);
    ast::Program prog = ps.parse_program();
    check_program(prog, d);

    result.map->set("ok", Value::logico(!d.has_errors()));
    Value errs = Value::lista();
    for (const Diagnostic& e : d.all()) {
      Value m = Value::mapa();
      m.map->set("codigo", Value::texto(std::string(diag_code_string(e.code))));
      m.map->set("linha", Value::inteiro(e.span.line));
      m.map->set("coluna", Value::inteiro(e.span.column));
      m.map->set("mensagem", Value::texto(e.message));
      Value ns = Value::lista();
      for (const std::string& n : e.notes) ns.list->push_back(Value::texto(n));
      m.map->set("notas", std::move(ns));
      errs.list->push_back(std::move(m));
    }
    result.map->set("erros", std::move(errs));
    return result;
  }
  if (name == "dividir_texto") {
    auto a = args();
    rt::ValueMap kw = eval_kwargs(call, env);
    const std::string src = !a.empty() && a[0].kind == ValueKind::Texto ? a[0].s : "";
    std::size_t win = 800;
    std::size_t overlap = 100;
    if (const Value* t = kw.find("tamanho")) win = static_cast<std::size_t>(t->as_number());
    if (const Value* o = kw.find("sobreposicao")) overlap = static_cast<std::size_t>(o->as_number());
    if (win == 0) win = 1;
    const std::size_t step = win > overlap ? win - overlap : 1;
    rt::ValueList chunks;
    for (std::size_t start = 0; start < src.size(); start += step) {
      chunks.push_back(Value::texto(src.substr(start, win)));
      if (start + win >= src.size()) break;
    }
    if (chunks.empty()) chunks.push_back(Value::texto(src));
    return Value::lista(std::move(chunks));
  }
  if (name == "ler") {
    auto a = args();
    if (!a.empty() && a[0].kind == ValueKind::Texto && entities_.count(a[0].s)) {
      return read_fonte(a[0].s, call.span);
    }
    fail(call.span, "ler: esperava uma 'fonte' declarada", DiagCode::ConnectorNotImplemented);
  }
  // Opcoes {senha:, banco:, tls:} comuns a ler_redis/escrever_redis; senha e
  // banco vencem a URL, tls liga TLS (rediss:// tambem liga).
  auto redis_opts = [&](const std::vector<Value>& a, std::size_t idx) -> rt::RedisOpts {
    rt::RedisOpts opts;
    if (a.size() <= idx) return opts;
    if (a[idx].kind != ValueKind::Mapa || !a[idx].map) {
      fail(call.span, "redis: opcoes devem ser um mapa {senha:, banco:, tls:}");
    }
    if (const Value* sv = a[idx].map->find("senha")) {
      if (sv->kind != ValueKind::Texto) fail(call.span, "redis: 'senha' deve ser texto");
      opts.auth = sv->s;
    }
    if (const Value* bv = a[idx].map->find("banco")) {
      if (bv->kind != ValueKind::Inteiro) fail(call.span, "redis: 'banco' deve ser inteiro");
      opts.db = static_cast<int>(bv->i);
    }
    if (const Value* tv = a[idx].map->find("tls")) {
      if (tv->kind != ValueKind::Logico) fail(call.span, "redis: 'tls' deve ser logico");
      opts.tls = tv->b;
    }
    return opts;
  };
  if (name == "ler_redis") {
    auto a = args();
    if (a.size() < 2 || a[0].kind != ValueKind::Texto || a[1].kind != ValueKind::Texto) {
      fail(call.span,
           "ler_redis espera (url, chave, {senha:, banco:}), ex.: ler_redis "
           "\"redis://:segredo@localhost:6379/2\", \"chave\"");
    }
    try {
      return rt::redis_get(a[0].s, a[1].s, redis_opts(a, 2));
    } catch (const std::exception& e) {
      fail(call.span, std::string(e.what()));
    }
  }
  if (name == "escrever_redis") {
    auto a = args();
    if (a.size() < 3 || a[0].kind != ValueKind::Texto || a[1].kind != ValueKind::Texto) {
      fail(call.span,
           "escrever_redis espera (url, chave, valor, {senha:, banco:}), ex.: escrever_redis "
           "url, \"chave\", valor");
    }
    try {
      rt::redis_set(a[0].s, a[1].s, a[2], redis_opts(a, 3));
    } catch (const std::exception& e) {
      fail(call.span, std::string(e.what()));
    }
    return Value::nulo();
  }
  if (name == "ler_kafka") {
    auto a = args();
    if (a.empty() || a[0].kind != ValueKind::Texto) {
      fail(call.span,
           "ler_kafka espera (topico, {desde:, max:, grupo:, broker:}), ex.: ler_kafka "
           "\"pedidos\", {desde: \"inicio\", max: 100}");
    }
    bool do_fim = false;
    std::int64_t max = 100;
    std::string grupo, broker;
    bool tls = false;
    if (a.size() >= 2) {
      if (a[1].kind != ValueKind::Mapa || !a[1].map) {
        fail(call.span, "ler_kafka: opcoes devem ser um mapa {desde:, max:, grupo:, broker:, tls:}");
      }
      if (const Value* dv = a[1].map->find("desde")) {
        if (dv->kind != ValueKind::Texto || (dv->s != "inicio" && dv->s != "fim")) {
          fail(call.span, "ler_kafka: 'desde' deve ser \"inicio\" ou \"fim\"");
        }
        do_fim = dv->s == "fim";
      }
      if (const Value* mv = a[1].map->find("max")) {
        if (mv->kind != ValueKind::Inteiro) {
          fail(call.span, "ler_kafka: 'max' deve ser inteiro");
        }
        max = mv->i;
      }
      if (const Value* gv = a[1].map->find("grupo")) {
        if (gv->kind != ValueKind::Texto || gv->s.empty()) {
          fail(call.span, "ler_kafka: 'grupo' deve ser texto nao vazio");
        }
        grupo = gv->s;
      }
      if (const Value* bv = a[1].map->find("broker")) {
        if (bv->kind != ValueKind::Texto) {
          fail(call.span, "ler_kafka: 'broker' deve ser texto (ex.: \"host:9092\")");
        }
        broker = bv->s;
      }
      if (const Value* tv = a[1].map->find("tls")) {
        if (tv->kind != ValueKind::Logico) fail(call.span, "ler_kafka: 'tls' deve ser logico");
        tls = tv->b;
      }
    }
    try {
      if (!grupo.empty()) {
        // Consumer group: coordenacao + checkpoint por commit de offset.
        Value out = Value::lista();
        for (const auto& [part, valor] : rt::kafka_consume_group(broker, grupo, a[0].s,
                                                                 static_cast<int>(max), tls)) {
          (void)part;
          out.list->push_back(Value::texto(valor));
        }
        return out;
      }
      return rt::kafka_ler(a[0].s, do_fim, max, broker, tls);
    } catch (const std::exception& e) {
      fail(call.span, std::string(e.what()));
    }
  }
  if (name == "escrever_kafka") {
    auto a = args();
    if (a.size() < 2 || a[0].kind != ValueKind::Texto) {
      fail(call.span,
           "escrever_kafka espera (topico, valor, {particao:}), ex.: escrever_kafka "
           "\"pedidos\", valor");
    }
    std::int64_t particao = 0;
    bool tls = false;
    if (a.size() >= 3) {
      if (a[2].kind != ValueKind::Mapa || !a[2].map) {
        fail(call.span, "escrever_kafka: opcoes devem ser um mapa {particao:, tls:}");
      }
      if (const Value* pv = a[2].map->find("particao")) {
        if (pv->kind != ValueKind::Inteiro) {
          fail(call.span, "escrever_kafka: 'particao' deve ser inteiro");
        }
        particao = pv->i;
      }
      if (const Value* tv = a[2].map->find("tls")) {
        if (tv->kind != ValueKind::Logico) fail(call.span, "escrever_kafka: 'tls' deve ser logico");
        tls = tv->b;
      }
    }
    try {
      const std::string body = a[1].kind == ValueKind::Texto ? a[1].s : rt::json_dump(a[1]);
      rt::kafka_produzir(a[0].s, body, static_cast<std::int32_t>(particao), tls);
    } catch (const std::exception& e) {
      fail(call.span, std::string(e.what()));
    }
    return Value::nulo();
  }
  if (name == "mongo_inserir") {
    auto a = args();
    if (a.size() < 2 || a[0].kind != ValueKind::Texto) {
      fail(call.span,
           "mongo_inserir espera (colecao, documento, {banco:}), ex.: mongo_inserir \"pedidos\", "
           "{ cliente: \"ana\" }");
    }
    std::string banco;
    if (a.size() >= 3) {
      if (a[2].kind != ValueKind::Mapa || !a[2].map) {
        fail(call.span, "mongo_inserir: opcoes devem ser um mapa {banco: \"x\"}");
      }
      if (const Value* bv = a[2].map->find("banco")) {
        if (bv->kind != ValueKind::Texto) {
          fail(call.span, "mongo_inserir: 'banco' deve ser texto");
        }
        banco = bv->s;
      }
    }
    try {
      rt::mongo_inserir(a[0].s, a[1], banco);
    } catch (const std::exception& e) {
      fail(call.span, std::string(e.what()));
    }
    return Value::nulo();
  }
  if (name == "mongo_buscar") {
    auto a = args();
    if (a.empty() || a[0].kind != ValueKind::Texto) {
      fail(call.span,
           "mongo_buscar espera (colecao, {filtro:, max:, somente:, lote:, banco:}), ex.: "
           "mongo_buscar \"pedidos\", { filtro: { cliente: \"ana\" } }");
    }
    Value filtro = Value::mapa();
    std::int64_t max = 100;
    std::int64_t lote = 0;
    Value somente = Value::lista();
    std::string banco;
    if (a.size() >= 2) {
      if (a[1].kind != ValueKind::Mapa || !a[1].map) {
        fail(call.span, "mongo_buscar: opcoes devem ser um mapa {filtro:, max:, somente:, lote:, banco:}");
      }
      if (const Value* fv = a[1].map->find("filtro")) {
        if (fv->kind != ValueKind::Mapa || !fv->map) {
          fail(call.span, "mongo_buscar: 'filtro' deve ser um mapa de igualdade");
        }
        filtro = *fv;
      }
      if (const Value* mv = a[1].map->find("max")) {
        if (mv->kind != ValueKind::Inteiro) {
          fail(call.span, "mongo_buscar: 'max' deve ser inteiro");
        }
        max = mv->i;
      }
      if (const Value* sv = a[1].map->find("somente")) {
        if (sv->kind != ValueKind::Lista || !sv->list || sv->list->empty()) {
          fail(call.span, "mongo_buscar: 'somente' deve ser uma lista de textos nao vazia");
        }
        for (const Value& c : *sv->list) {
          if (c.kind != ValueKind::Texto || c.s.empty()) {
            fail(call.span, "mongo_buscar: 'somente' deve ser uma lista de textos nao vazios");
          }
        }
        somente = *sv;
      }
      if (const Value* lv = a[1].map->find("lote")) {
        if (lv->kind != ValueKind::Inteiro || lv->i <= 0) {
          fail(call.span, "mongo_buscar: 'lote' deve ser inteiro > 0");
        }
        lote = lv->i;
      }
      if (const Value* bv = a[1].map->find("banco")) {
        if (bv->kind != ValueKind::Texto) {
          fail(call.span, "mongo_buscar: 'banco' deve ser texto");
        }
        banco = bv->s;
      }
    }
    try {
      return rt::mongo_buscar(a[0].s, filtro, max, banco, somente, lote);
    } catch (const std::exception& e) {
      fail(call.span, std::string(e.what()));
    }
  }
  if (name == "mongo_atualizar") {
    auto a = args();
    if (a.size() < 3 || a[0].kind != ValueKind::Texto) {
      fail(call.span,
           "mongo_atualizar espera (colecao, filtro, mudancas, {banco:, multi:}), ex.: "
           "mongo_atualizar \"pedidos\", {cliente: \"ana\"}, {$set: {valor: 999}, $inc: {acessos: 1}}");
    }
    if (a[1].kind != ValueKind::Mapa || !a[1].map) {
      fail(call.span, "mongo_atualizar: 'filtro' deve ser um mapa de igualdade");
    }
    if (a[2].kind != ValueKind::Mapa || !a[2].map) {
      fail(call.span, "mongo_atualizar: 'mudancas' deve ser um mapa {$set: {...}, $inc: {...}}");
    }
    std::string banco;
    bool multi = false;
    if (a.size() >= 4) {
      if (a[3].kind != ValueKind::Mapa || !a[3].map) {
        fail(call.span, "mongo_atualizar: opcoes devem ser um mapa {banco:, multi:}");
      }
      if (const Value* bv = a[3].map->find("banco")) {
        if (bv->kind != ValueKind::Texto) {
          fail(call.span, "mongo_atualizar: 'banco' deve ser texto");
        }
        banco = bv->s;
      }
      if (const Value* mv = a[3].map->find("multi")) {
        if (mv->kind != ValueKind::Logico) {
          fail(call.span, "mongo_atualizar: 'multi' deve ser logico");
        }
        multi = mv->b;
      }
    }
    try {
      return Value::inteiro(rt::mongo_atualizar(a[0].s, a[1], a[2], multi, banco));
    } catch (const std::exception& e) {
      fail(call.span, std::string(e.what()));
    }
  }
  if (name == "mongo_deletar") {
    auto a = args();
    if (a.size() < 2 || a[0].kind != ValueKind::Texto) {
      fail(call.span,
           "mongo_deletar espera (colecao, filtro, {banco:}), ex.: mongo_deletar \"pedidos\", "
           "{cliente: \"bob\"}");
    }
    if (a[1].kind != ValueKind::Mapa || !a[1].map) {
      fail(call.span, "mongo_deletar: 'filtro' deve ser um mapa de igualdade");
    }
    std::string banco;
    if (a.size() >= 3) {
      if (a[2].kind != ValueKind::Mapa || !a[2].map) {
        fail(call.span, "mongo_deletar: opcoes devem ser um mapa {banco: \"x\"}");
      }
      if (const Value* bv = a[2].map->find("banco")) {
        if (bv->kind != ValueKind::Texto) {
          fail(call.span, "mongo_deletar: 'banco' deve ser texto");
        }
        banco = bv->s;
      }
    }
    try {
      return Value::inteiro(rt::mongo_deletar(a[0].s, a[1], banco));
    } catch (const std::exception& e) {
      fail(call.span, std::string(e.what()));
    }
  }
  if (name == "mongo_criar_indice") {
    auto a = args();
    if (a.size() < 2 || a[0].kind != ValueKind::Texto) {
      fail(call.span,
           "mongo_criar_indice espera (colecao, {campos:, banco:}), ex.: mongo_criar_indice "
           "\"pedidos\", {campos: [\"cliente\"]}");
    }
    if (a[1].kind != ValueKind::Mapa || !a[1].map) {
      fail(call.span, "mongo_criar_indice: opcoes devem ser um mapa {campos:, banco:}");
    }
    Value campos;
    if (const Value* cv = a[1].map->find("campos")) {
      if (cv->kind != ValueKind::Lista || !cv->list) {
        fail(call.span, "mongo_criar_indice: 'campos' deve ser uma lista de textos");
      }
      campos = *cv;
    } else {
      fail(call.span, "mongo_criar_indice: falta 'campos: [\"a\", ...]'");
    }
    std::string banco;
    if (const Value* bv = a[1].map->find("banco")) {
      if (bv->kind != ValueKind::Texto) {
        fail(call.span, "mongo_criar_indice: 'banco' deve ser texto");
      }
      banco = bv->s;
    }
    try {
      return Value::texto(rt::mongo_criar_indice(a[0].s, campos, banco));
    } catch (const std::exception& e) {
      fail(call.span, std::string(e.what()));
    }
  }
  if (name == "mongo_agregar") {
    auto a = args();
    if (a.size() < 2 || a[0].kind != ValueKind::Texto) {
      fail(call.span,
           "mongo_agregar espera (colecao, [etapas], {banco:}), ex.: mongo_agregar \"pedidos\", "
           "[{$group: {_id: \"$cliente\", total: {$sum: 1}}}]");
    }
    if (a[1].kind != ValueKind::Lista || !a[1].list) {
      fail(call.span, "mongo_agregar: 'etapas' deve ser uma lista de mapas [{operador: {...}}]");
    }
    std::string banco;
    if (a.size() >= 3) {
      if (a[2].kind != ValueKind::Mapa || !a[2].map) {
        fail(call.span, "mongo_agregar: opcoes devem ser um mapa {banco: \"x\"}");
      }
      if (const Value* bv = a[2].map->find("banco")) {
        if (bv->kind != ValueKind::Texto) {
          fail(call.span, "mongo_agregar: 'banco' deve ser texto");
        }
        banco = bv->s;
      }
    }
    try {
      return rt::mongo_agregar(a[0].s, a[1], banco);
    } catch (const std::exception& e) {
      fail(call.span, std::string(e.what()));
    }
  }
  if (name == "ler_s3") {
    auto a = args();
    if (a.size() < 1 || a[0].kind != ValueKind::Texto) {
      fail(call.span, "ler_s3 espera (url), ex.: ler_s3 \"s3://bucket/chave\"");
    }
    try {
      return Value::texto(rt::s3_get(a[0].s));
    } catch (const std::exception& e) {
      fail(call.span, std::string(e.what()));
    }
  }
  if (name == "escrever_s3") {
    auto a = args();
    if (a.size() < 2 || a[0].kind != ValueKind::Texto) {
      fail(call.span, "escrever_s3 espera (url, valor), ex.: escrever_s3 \"s3://bucket/chave\", valor");
    }
    try {
      const std::string body =
          a[1].kind == ValueKind::Texto ? a[1].s : rt::json_dump(a[1]);
      rt::s3_put(a[0].s, body);
    } catch (const std::exception& e) {
      fail(call.span, std::string(e.what()));
    }
    return Value::nulo();
  }
  if (name == "listar_s3") {
    auto a = args();
    if (a.empty() || a[0].kind != ValueKind::Texto) {
      fail(call.span,
           "listar_s3 espera (url_do_bucket, {prefixo:, max:}), ex.: listar_s3 "
           "\"s3://bucket\", {prefixo: \"x\", max: 100}");
    }
    std::string prefixo;
    int max = 1000;
    if (a.size() >= 2) {
      if (a[1].kind != ValueKind::Mapa || !a[1].map) {
        fail(call.span, "listar_s3: opcoes devem ser um mapa {prefixo:, max:}");
      }
      if (const Value* p = a[1].map->find("prefixo")) {
        if (p->kind != ValueKind::Texto) {
          fail(call.span, "listar_s3: 'prefixo' deve ser texto");
        }
        prefixo = p->s;
      }
      if (const Value* m = a[1].map->find("max")) {
        max = static_cast<int>(m->as_number());
      }
    }
    try {
      const auto chaves = rt::s3_list(a[0].s, prefixo, max);
      rt::ValueList lst;
      for (const auto& c : chaves) lst.push_back(Value::texto(c));
      return Value::lista(std::move(lst));
    } catch (const std::exception& e) {
      fail(call.span, std::string(e.what()));
    }
  }
  if (name == "apagar_s3") {
    auto a = args();
    if (a.empty() || a[0].kind != ValueKind::Texto) {
      fail(call.span, "apagar_s3 espera (url), ex.: apagar_s3 \"s3://bucket/chave\"");
    }
    try {
      rt::s3_delete(a[0].s);
    } catch (const std::exception& e) {
      fail(call.span, std::string(e.what()));
    }
    return Value::nulo();
  }
  if (word_in(name, {"escrever"}) || (name.rfind("ler_", 0) == 0) ||
      (name.rfind("escrever_", 0) == 0)) {
    fail(call.span, "'" + name + "': conector/formato nao implementado (M5.2)",
         DiagCode::ConnectorNotImplemented);
  }
  if (name == "modelo") return eval_modelo_call(call, env);

  // Direct tool call: `<ferramenta> arg: valor`.
  if (auto tit = entities_.find(name);
      tit != entities_.end() && tit->second->key == "ferramenta") {
    rt::ValueMap targs;
    for (const auto& a : call.args) {
      if (!a.name.empty()) targs.set(a.name, eval(*a.value, env));
    }
    if (call.block) {
      for (const auto& it : call.block->items) {
        if (it && it->kind == ItemKind::Field && it->value) targs.set(it->key, eval(*it->value, env));
      }
    }
    return run_tool(*tit->second, targs, call.span);
  }
  if (word_in(name, {"treinar", "incorporador"})) {
    fail(call.span, "execucao de '" + name + "' nao implementada", DiagCode::NotImplemented);
  }

  fail(call.span, "funcao '" + name + "' nao definida");
}

// ------------------------------------------------------------------ methods

Value Interpreter::eval_method(const std::string& method, Value receiver, const Expr& call,
                               Env& env) {
  if (receiver.kind == ValueKind::Texto) {
    if (auto e = entities_.find(receiver.s); e != entities_.end() && e->second->block) {
      const std::string& ekind = e->second->key;
      if (ekind == "indice") return eval_indice_method(receiver.s, method, call, env);
      if (ekind == "agente" && (method == "responder" || method == "perguntar")) {
        return eval_agente_responder(receiver.s, call, env);
      }
      if (ekind == "equipe" && (method == "executar" || method == "responder")) {
        return eval_equipe_call(receiver.s, call, env);
      }
      if (ekind == "ferramenta" && method == "executar") {
        rt::ValueMap targs;
        for (const auto& a : call.args) {
          if (!a.name.empty()) targs.set(a.name, eval(*a.value, env));
        }
        if (call.block) {
          for (const auto& it : call.block->items) {
            if (it && it->kind == ItemKind::Field && it->value) {
              targs.set(it->key, eval(*it->value, env));
            }
          }
        }
        return run_tool(*e->second, targs, call.span);
      }
    }
  }

  const bool is_table = receiver.kind == ValueKind::Tabela || receiver.kind == ValueKind::Lista;

  if (is_table && method == "filtrar") {
    if (call.args.empty()) fail(call.span, "filtrar espera uma condicao");
    rt::ValueList kept;
    for (const Value& row : (receiver.list ? *receiver.list : rt::ValueList{})) {
      Env inner;
      inner.parent = &env;
      inner.vars["linha"] = row;
      if (eval(*call.args[0].value, inner).truthy()) kept.push_back(row);
    }
    return Value::tabela(std::move(kept));
  }
  if (is_table && (method == "derivar" || method == "mapear")) {
    if (call.args.empty() || call.args[0].value->kind != ExprKind::MapLit) {
      fail(call.span, method + " espera um mapa { coluna: expressao }");
    }
    const Expr& spec = *call.args[0].value;
    rt::ValueList out;
    for (const Value& row : (receiver.list ? *receiver.list : rt::ValueList{})) {
      Env inner;
      inner.parent = &env;
      inner.vars["linha"] = row;
      Value nr = Value::mapa();
      if (row.map) {
        for (const auto& kv : row.map->items) nr.map->set(kv.first, kv.second);
      }
      for (const auto& en : spec.entries) nr.map->set(en.key, eval(*en.value, inner));
      out.push_back(std::move(nr));
    }
    return Value::tabela(std::move(out));
  }
  if (is_table && method == "agrupar_por") {
    if (call.args.size() < 2) fail(call.span, "agrupar_por espera (coluna, agregacoes)");
    Value key_v = eval(*call.args[0].value, env);
    std::string key_col = key_v.kind == ValueKind::Texto ? key_v.s : "";
    const Expr& aggs = *call.args[1].value;
    if (aggs.kind != ExprKind::MapLit) fail(call.span, "agregacoes devem ser um mapa");

    std::vector<std::string> order;
    std::unordered_map<std::string, rt::ValueList> groups;
    for (const Value& row : (receiver.list ? *receiver.list : rt::ValueList{})) {
      const Value* k = row.map ? row.map->find(key_col) : nullptr;
      std::string gk = k ? to_display(*k) : "";
      if (!groups.count(gk)) order.push_back(gk);
      groups[gk].push_back(row);
    }

    rt::ValueList out;
    for (const std::string& gk : order) {
      Value r = Value::mapa();
      r.map->set(key_col.empty() ? "grupo" : key_col, Value::texto(gk));
      for (const auto& en : aggs.entries) {
        const Expr& spec = *en.value;
        std::string fn = "contar";
        std::string col;
        if (spec.kind == ExprKind::Call && spec.lhs->kind == ExprKind::Name) {
          fn = spec.lhs->text;
          if (!spec.args.empty() && spec.args[0].value->kind == ExprKind::TextLit) {
            col = spec.args[0].value->text;
          }
        } else if (spec.kind == ExprKind::Name) {
          fn = spec.text;
        }
        const auto& rows = groups[gk];
        double acc = 0;
        if (fn == "contar") {
          acc = static_cast<double>(rows.size());
        } else {
          bool init = false;
          for (const Value& rr : rows) {
            const Value* c = rr.map ? rr.map->find(col) : nullptr;
            double n = c ? c->as_number() : 0;
            if (!init) {
              acc = n;
              init = true;
            } else if (fn == "somar" || fn == "media") {
              acc += n;
            } else if (fn == "min") {
              acc = std::min(acc, n);
            } else if (fn == "max") {
              acc = std::max(acc, n);
            }
          }
          if (fn == "media" && !rows.empty()) acc /= static_cast<double>(rows.size());
        }
        if (acc == static_cast<double>(static_cast<std::int64_t>(acc)) && fn != "media") {
          r.map->set(en.key, Value::inteiro(static_cast<std::int64_t>(acc)));
        } else {
          r.map->set(en.key, Value::decimal(acc));
        }
      }
      out.push_back(std::move(r));
    }
    return Value::tabela(std::move(out));
  }
  if (is_table && method == "selecionar") {
    auto cols = eval_args(call, env);
    rt::ValueList out;
    for (const Value& row : (receiver.list ? *receiver.list : rt::ValueList{})) {
      Value nr = Value::mapa();
      for (const Value& c : cols) {
        if (c.kind == ValueKind::Texto && row.map) {
          if (Value* f = row.map->find(c.s)) nr.map->set(c.s, *f);
        }
      }
      out.push_back(std::move(nr));
    }
    return Value::tabela(std::move(out));
  }
  if (is_table && method == "ordenar_por") {
    auto a = eval_args(call, env);
    rt::ValueMap kw = eval_kwargs(call, env);
    if (a.empty() || a[0].kind != ValueKind::Texto) fail(call.span, "ordenar_por espera uma coluna");
    const std::string col = a[0].s;
    const Value* desc = kw.find("desc");
    const bool descending = desc && desc->truthy();
    rt::ValueList out = receiver.list ? *receiver.list : rt::ValueList{};
    std::stable_sort(out.begin(), out.end(), [&](const Value& x, const Value& y) {
      const Value* xa = x.map ? x.map->find(col) : nullptr;
      const Value* ya = y.map ? y.map->find(col) : nullptr;
      bool less;
      if (xa && ya && xa->is_number() && ya->is_number()) {
        less = xa->as_number() < ya->as_number();
      } else {
        less = (xa ? to_display(*xa) : "") < (ya ? to_display(*ya) : "");
      }
      return descending ? !less : less;
    });
    return Value::tabela(std::move(out));
  }
  if (is_table && (method == "limite" || method == "primeiros")) {
    auto a = eval_args(call, env);
    std::int64_t n = a.empty() ? 0 : static_cast<std::int64_t>(a[0].as_number());
    rt::ValueList out;
    if (receiver.list) {
      for (const Value& row : *receiver.list) {
        if (static_cast<std::int64_t>(out.size()) >= n) break;
        out.push_back(row);
      }
    }
    return Value::tabela(std::move(out));
  }
  if (is_table && method == "distinto") {
    auto a = eval_args(call, env);
    const std::string col = (!a.empty() && a[0].kind == ValueKind::Texto) ? a[0].s : "";
    rt::ValueList out;
    std::vector<std::string> seen;
    for (const Value& row : (receiver.list ? *receiver.list : rt::ValueList{})) {
      std::string key;
      if (col.empty()) {
        key = to_display(row);
      } else {
        const Value* c = row.map ? row.map->find(col) : nullptr;
        key = c ? to_display(*c) : "";
      }
      if (std::find(seen.begin(), seen.end(), key) == seen.end()) {
        seen.push_back(key);
        out.push_back(row);
      }
    }
    return Value::tabela(std::move(out));
  }

  if (receiver.kind == ValueKind::Tensor && receiver.tensor) {
    const rt::Tensor& t = *receiver.tensor;
    try {
      if (method == "forma") {
        rt::ValueList dims;
        for (std::int64_t d : t.shape) dims.push_back(Value::inteiro(d));
        return Value::lista(std::move(dims));
      }
      if (method == "matmul" || method == "mais") {
        auto a = eval_args(call, env);
        if (a.empty() || a[0].kind != ValueKind::Tensor) {
          fail(call.span, method + " espera outro tensor");
        }
        return Value::tensor_de(method == "matmul" ? rt::matmul(t, *a[0].tensor)
                                                   : rt::add(t, *a[0].tensor));
      }
      if (method == "transposta") return Value::tensor_de(rt::transpose2d(t));
      if (method == "reformar") {
        auto a = eval_args(call, env);
        std::vector<std::int64_t> shape;
        if (!a.empty() && a[0].kind == ValueKind::Lista && a[0].list) {
          for (const Value& e : *a[0].list) shape.push_back(static_cast<std::int64_t>(e.as_number()));
        }
        return Value::tensor_de(rt::reshape(t, shape));
      }
      if (word_in(method, {"relu", "gelu", "silu", "sigmoide", "tanh"})) {
        return Value::tensor_de(rt::apply_unary(t, method));
      }
      if (method == "softmax") return Value::tensor_de(rt::softmax_last(t));
      if (method == "soma") return Value::decimal(rt::sum_all(t));
      if (method == "media") return Value::decimal(rt::mean_all(t));
      if (method == "argmax") return Value::inteiro(rt::argmax_last(t));
      if (method == "item") {
        if (t.size() != 1) fail(call.span, "item espera um tensor de 1 elemento");
        return Value::decimal(t.data[0]);
      }
    } catch (const std::exception& e) {
      fail(call.span, std::string(e.what()));
    }
    fail(call.span, "tensor nao tem o metodo '" + method + "'");
  }

  if (receiver.kind == ValueKind::Texto) {
    if (method == "maiusculas") {
      std::string s = receiver.s;
      for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
      return Value::texto(s);
    }
    if (method == "minusculas") {
      std::string s = receiver.s;
      for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      return Value::texto(s);
    }
  }

  fail(call.span, std::string("'") + receiver.type_name() + "' nao tem o metodo '" + method + "'");
}

}  // namespace tilt
