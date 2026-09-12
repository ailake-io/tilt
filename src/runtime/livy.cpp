#include "runtime/livy.hpp"

#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include "runtime/http_client.hpp"
#include "runtime/json.hpp"

namespace tilt::rt {

namespace {

constexpr int kTimeoutPollS = 120;  // orcamento total do polling (sessao/statement)
constexpr int kTimeoutHttpS = 30;   // por requisicao HTTP
constexpr int kPollMs = 500;        // intervalo entre polls

[[noreturn]] void die(const std::string& m) { throw std::runtime_error("livy: " + m); }

std::string truncar(const std::string& s, std::size_t n) {
  return s.size() <= n ? s : s.substr(0, n);
}

// Erros do http_client chegam com prefixo "http: "; reemite com o prefixo
// do conector para o capturar do tilt mostrar a origem.
template <typename F>
Value json_ou_die(F&& fn) {
  try {
    return fn();
  } catch (const std::exception& e) {
    die(e.what());
  }
}

std::string base_da_url(std::string url) {
  while (!url.empty() && url.back() == '/') url.pop_back();
  return url;
}

// Kind da sessao Livy: o servidor chama a sessao Scala de "spark" e a de
// Python de "pyspark".
std::string kind_da_lingua(const std::string& lingua) {
  if (lingua == "scala" || lingua.empty()) return "spark";
  if (lingua == "pyspark") return "pyspark";
  die("lingua '" + lingua + "' invalida (use \"scala\" ou \"pyspark\")");
}

std::string campo_texto(const Value& m, const char* chave) {
  if (m.kind != ValueKind::Mapa || !m.map) return "";
  const Value* v = m.map->find(chave);
  return v && v->kind == ValueKind::Texto ? v->s : "";
}

std::int64_t campo_inteiro(const Value& m, const char* chave) {
  if (m.kind != ValueKind::Mapa || !m.map) return -1;
  const Value* v = m.map->find(chave);
  return v && v->kind == ValueKind::Inteiro ? v->i : -1;
}

// GET /sessions/{id}/statements/{st}: estado + output (data.text/plain).
struct Statement {
  std::string estado;
  std::string status_saida;  // "ok" / "error" (campo output.status)
  std::string texto;         // data.text/plain
  std::string ename;
  std::string evalue;
};

Statement le_statement(const std::string& base, std::int64_t sessao, std::int64_t st) {
  const Value v = json_ou_die([&] {
    return http_get_json(
        base + "/sessions/" + std::to_string(sessao) + "/statements/" + std::to_string(st), {},
        kTimeoutHttpS);
  });
  Statement out;
  out.estado = campo_texto(v, "state");
  if (v.kind == ValueKind::Mapa && v.map) {
    const Value* output = v.map->find("output");
    if (output && output->kind == ValueKind::Mapa && output->map) {
      out.status_saida = campo_texto(*output, "status");
      out.ename = campo_texto(*output, "ename");
      out.evalue = campo_texto(*output, "evalue");
      if (const Value* data = output->map->find("data");
          data && data->kind == ValueKind::Mapa && data->map) {
        const Value* txt = data->map->find("text/plain");
        if (txt && txt->kind == ValueKind::Texto) out.texto = txt->s;
      }
    }
  }
  return out;
}

// Mensagem de erro de um statement em error: ename + evalue do output do Livy.
[[noreturn]] void die_statement(const Statement& st) {
  std::string msg = st.ename;
  if (!st.evalue.empty()) msg += (msg.empty() ? "" : ": ") + st.evalue;
  if (msg.empty()) msg = "statement falhou sem detalhe do erro";
  die(msg);
}

// Sessao pronta para receber statement: reutiliza a primeira idle com o kind
// alvo; se nao houver, cria (POST /sessions com {kind, conf}) e espera ficar
// idle. A sessao NAO e fechada no fim — fica no pool do Livy para o proximo
// uso (o proprio Livy derruba por inatividade conforme o conf do servidor).
std::int64_t obter_sessao(const std::string& base, const std::string& kind, const Value* conf) {
  const Value lista =
      json_ou_die([&] { return http_get_json(base + "/sessions", {}, kTimeoutHttpS); });
  if (lista.kind == ValueKind::Mapa && lista.map) {
    const Value* sessoes = lista.map->find("sessions");
    if (sessoes && sessoes->kind == ValueKind::Lista && sessoes->list) {
      for (const Value& s : *sessoes->list) {
        if (campo_texto(s, "kind") == kind && campo_texto(s, "state") == "idle") {
          const std::int64_t id = campo_inteiro(s, "id");
          if (id >= 0) return id;
        }
      }
    }
  }
  Value corpo = Value::mapa();
  corpo.map->set("kind", Value::texto(kind));
  if (conf && conf->kind == ValueKind::Mapa && conf->map && !conf->map->items.empty()) {
    corpo.map->set("conf", *conf);
  }
  const Value criada =
      json_ou_die([&] { return http_post_json(base + "/sessions", corpo, {}, kTimeoutHttpS); });
  const std::int64_t id = campo_inteiro(criada, "id");
  if (id < 0) die("resposta de POST /sessions sem 'id' de sessao");
  // Sessao nova sobe "starting": espera ficar idle (ou morrer) com timeout.
  const auto limite = std::chrono::steady_clock::now() + std::chrono::seconds(kTimeoutPollS);
  while (std::chrono::steady_clock::now() < limite) {
    const Value s = json_ou_die(
        [&] { return http_get_json(base + "/sessions/" + std::to_string(id), {}, kTimeoutHttpS); });
    const std::string estado = campo_texto(s, "state");
    if (estado == "idle") return id;
    if (estado == "error" || estado == "dead" || estado == "killed") {
      die("sessao " + std::to_string(id) + " morreu ao iniciar (state: " + estado + ")");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(kPollMs));
  }
  die("timeout de " + std::to_string(kTimeoutPollS) +
      "s esperando a sessao iniciar (state diferente de idle)");
}

// POST /sessions/{id}/statements + polling ate available/error (~120s).
Statement executar_statement(const std::string& base, const std::string& codigo,
                             const std::string& lingua, const Value* conf) {
  const std::int64_t sessao = obter_sessao(base, kind_da_lingua(lingua), conf);
  Value corpo = Value::mapa();
  corpo.map->set("code", Value::texto(codigo));
  const Value submetido = json_ou_die([&] {
    return http_post_json(base + "/sessions/" + std::to_string(sessao) + "/statements", corpo, {},
                          kTimeoutHttpS);
  });
  const std::int64_t st = campo_inteiro(submetido, "id");
  if (st < 0) die("resposta de POST /statements sem 'id' do statement");
  const auto limite = std::chrono::steady_clock::now() + std::chrono::seconds(kTimeoutPollS);
  while (true) {
    const Statement s = le_statement(base, sessao, st);
    if (s.estado == "available") return s;
    if (s.estado == "error" || s.status_saida == "error") die_statement(s);
    if (s.estado == "cancelled" || s.estado == "cancelling") {
      die("statement cancelado pelo servidor");
    }
    if (std::chrono::steady_clock::now() >= limite) {
      die("timeout de " + std::to_string(kTimeoutPollS) +
          "s aguardando o statement (state: " + (s.estado.empty() ? "?" : s.estado) + ")");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(kPollMs));
  }
}

// Estados transientes passados ao polling sao: sessao starting -> idle;
// statement waiting/running -> available/error.
}  // namespace

Value livy_sql(const std::string& url, const std::string& codigo_sql, const std::string& lingua,
               const Value* conf) {
  const std::string base = base_da_url(url);
  std::string codigo;
  if (kind_da_lingua(lingua) == "pyspark") {
    codigo = "__import__(\"json\").dumps([__import__(\"json\").loads(r) for r in spark.sql(\"\"\"" +
             codigo_sql + "\"\"\").toJSON().collect()])";
  } else {
    codigo = "spark.sql(\"\"\"" + codigo_sql + "\"\"\").toJSON.collectAsList().toString()";
  }
  const Statement st = executar_statement(base, codigo, lingua, conf);
  Value parsed;
  try {
    parsed = json_parse(st.texto);
  } catch (const std::exception& e) {
    die("saida do Spark nao e um JSON valido: " + std::string(e.what()) +
        " (recebido: " + truncar(st.texto, 120) + ")");
  }
  if (parsed.kind != ValueKind::Lista || !parsed.list) {
    die("saida do Spark nao e uma lista de linhas (recebido: " + truncar(st.texto, 120) + ")");
  }
  Value tabela = Value::tabela();
  for (const Value& linha : *parsed.list) {
    if (linha.kind != ValueKind::Mapa || !linha.map) {
      die("linha da saida do Spark nao e um objeto JSON (recebido: " + truncar(st.texto, 120) +
          ")");
    }
    tabela.list->push_back(linha);
  }
  return tabela;
}

Value livy_executar(const std::string& url, const std::string& codigo, const std::string& lingua,
                    const Value* conf) {
  const std::string base = base_da_url(url);
  const Statement st = executar_statement(base, codigo, lingua, conf);
  return Value::texto(st.texto);
}

}  // namespace tilt::rt
