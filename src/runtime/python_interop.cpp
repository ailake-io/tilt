#include "runtime/python_interop.hpp"

#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include "runtime/compat.hpp"
#include "runtime/json.hpp"

namespace tilt::rt {

namespace {

// Roda no Python: le o pedido, importa, chama e grava {ok, resultado|erro}.
constexpr const char* kRunner = R"PY(
import importlib, importlib.util, json, math, os, sys, traceback

def _json(v):
    if v is None or isinstance(v, (bool, int, str)):
        return v
    if isinstance(v, float):
        return None if math.isnan(v) or math.isinf(v) else v
    if isinstance(v, dict):
        return {str(k): _json(x) for k, x in v.items()}
    if isinstance(v, (list, tuple, set, frozenset)):
        return [_json(x) for x in v]
    if hasattr(v, "isoformat"):
        return v.isoformat()
    if hasattr(v, "to_dict"):
        try:
            return _json(v.to_dict("records"))
        except TypeError:
            return _json(v.to_dict())
    if hasattr(v, "to_pylist"):
        return _json(v.to_pylist())
    if hasattr(v, "tolist"):
        return _json(v.tolist())
    if hasattr(v, "item"):
        return _json(v.item())
    raise TypeError("resultado do tipo %s nao vira JSON" % type(v).__name__)

def _carregar(modulo):
    if modulo.endswith(".py"):
        caminho = os.path.abspath(modulo)
        sys.path.insert(0, os.path.dirname(caminho))
        spec = importlib.util.spec_from_file_location(
            os.path.splitext(os.path.basename(caminho))[0], caminho)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        return mod
    sys.path.insert(0, os.getcwd())
    return importlib.import_module(modulo)

def _main(req_path, out_path):
    real_stdout = sys.stdout
    sys.stdout = sys.stderr  # print() do modulo nao pode sujar o stdout do Tilt
    try:
        with open(req_path, encoding="utf-8") as f:
            req = json.load(f)
        alvo = _carregar(req["modulo"])
        for parte in req["funcao"].split("."):
            alvo = getattr(alvo, parte)
        resultado = alvo(*req["args"], **req["nomeados"])
        saida = {"ok": True, "resultado": _json(resultado)}
    except SystemExit as e:
        saida = {"ok": False, "erro": "SystemExit(%s)" % (e.code,)}
    except BaseException as e:
        tb = traceback.extract_tb(e.__traceback__)
        onde = ""
        if tb:
            ultimo = tb[-1]
            onde = " (%s:%d)" % (os.path.basename(ultimo.filename), ultimo.lineno)
        saida = {"ok": False, "erro": "%s: %s%s" % (type(e).__name__, e, onde)}
    sys.stdout = real_stdout
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(saida, f, ensure_ascii=False, allow_nan=False)

_main(sys.argv[1], sys.argv[2])
)PY";

// Nome de modulo importavel ("math", "pacote.sub") ou caminho de .py sem
// caracteres que uma shell interprete.
bool caminho_seguro(const std::string& s) {
  if (s.empty()) return false;
  for (const char ch : s) {
    const auto c = static_cast<unsigned char>(ch);
    if (std::isalnum(c) != 0 || c == '_' || c == '.' || c == '/' || c == '-' || c == ':' ||
        c == '\\' || c == '+' || c == '~') {
      continue;
    }
    return false;
  }
  return true;
}

bool nome_de_modulo(const std::string& s) {
  if (s.empty() || std::isdigit(static_cast<unsigned char>(s[0])) != 0) return false;
  for (const char ch : s) {
    const auto c = static_cast<unsigned char>(ch);
    if (std::isalnum(c) == 0 && c != '_' && c != '.') return false;
  }
  return true;
}

std::string aspas(const std::string& s) {
#if defined(_WIN32)
  return "\"" + s + "\"";  // s ja foi validado: sem aspas, %, ^ nem &
#else
  return "'" + s + "'";  // s ja foi validado: sem aspas simples
#endif
}

// Cria um arquivo temporario com o texto dado; devolve o caminho.
std::string escrever_temp(const char* tag, const std::string& conteudo) {
  std::string caminho;
  const int fd = tilt_tempfile(tag, caminho);
  if (fd < 0) throw std::runtime_error("nao foi possivel criar arquivo temporario");
  tilt_close_file(fd);
  std::ofstream out(caminho, std::ios::binary | std::ios::trunc);
  out << conteudo;
  out.close();
  if (!out) {
    std::remove(caminho.c_str());
    throw std::runtime_error("falha ao escrever arquivo temporario");
  }
  return caminho;
}

struct Apagador {
  std::vector<std::string> caminhos;
  ~Apagador() {
    for (const std::string& c : caminhos) std::remove(c.c_str());
  }
};

// Uma lista em que todo item e um mapa e uma tabela.
Value como_tabela(Value v) {
  if (v.kind != ValueKind::Lista || !v.list || v.list->empty()) return v;
  for (const Value& item : *v.list) {
    if (item.kind != ValueKind::Mapa) return v;
  }
  return Value::tabela(*v.list);
}

}  // namespace

Value chamar_python(const std::string& modulo, const std::string& funcao,
                    const std::vector<Value>& args, const ValueMap& nomeados,
                    const std::string& python) {
  const bool eh_arquivo = modulo.size() > 3 && modulo.compare(modulo.size() - 3, 3, ".py") == 0;
  if (eh_arquivo ? !caminho_seguro(modulo) : !nome_de_modulo(modulo)) {
    throw std::runtime_error("chamar_python: modulo invalido '" + modulo +
                             "' (use um nome importavel como \"math\" ou o caminho de um .py)");
  }
  if (!nome_de_modulo(funcao)) {
    throw std::runtime_error("chamar_python: nome de funcao invalido '" + funcao + "'");
  }
  std::string exe = python;
  if (exe.empty()) {
    const char* env = std::getenv("TILT_PYTHON");
    if (env != nullptr && *env != '\0') {
      exe = env;
    } else {
#if defined(_WIN32)
      exe = "python";
#else
      exe = "python3";
#endif
    }
  }
  if (!caminho_seguro(exe)) {
    throw std::runtime_error("chamar_python: executavel Python invalido '" + exe + "'");
  }

  Value pedido = Value::mapa();
  pedido.map->items.emplace_back("modulo", Value::texto(modulo));
  pedido.map->items.emplace_back("funcao", Value::texto(funcao));
  pedido.map->items.emplace_back("args", Value::lista(args));
  Value nom = Value::mapa();
  for (const auto& kv : nomeados.items) nom.map->items.emplace_back(kv.first, kv.second);
  pedido.map->items.emplace_back("nomeados", nom);

  Apagador limpeza;
  const std::string script = escrever_temp("py", kRunner);
  limpeza.caminhos.push_back(script);
  const std::string req = escrever_temp("pyreq", json_dump_compacto(pedido));
  limpeza.caminhos.push_back(req);
  const std::string resp = escrever_temp("pyout", "");
  limpeza.caminhos.push_back(resp);

  const std::string cmd = aspas(exe) + " " + aspas(script) + " " + aspas(req) + " " + aspas(resp);
  FILE* pipe = tilt_popen(cmd.c_str(), "r");
  if (pipe == nullptr) throw std::runtime_error("chamar_python: nao foi possivel executar " + exe);
  std::array<char, 1024> lixo{};
  while (std::fread(lixo.data(), 1, lixo.size(), pipe) > 0) {
  }
  tilt_pclose(pipe);

  std::ifstream in(resp, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  if (ss.str().empty()) {
    throw std::runtime_error("chamar_python: o Python nao respondeu (" + exe +
                             " esta instalado? defina TILT_PYTHON para escolher o executavel)");
  }
  Value saida;
  try {
    saida = json_parse(ss.str());
  } catch (const std::exception& e) {
    throw std::runtime_error(std::string("chamar_python: resposta invalida: ") + e.what());
  }
  const Value* ok = saida.map ? saida.map->find("ok") : nullptr;
  if (ok == nullptr || ok->kind != ValueKind::Logico || !ok->b) {
    const Value* erro = saida.map ? saida.map->find("erro") : nullptr;
    throw std::runtime_error("python: " + (erro != nullptr && erro->kind == ValueKind::Texto
                                               ? erro->s
                                               : std::string("erro desconhecido")));
  }
  const Value* r = saida.map->find("resultado");
  return r != nullptr ? como_tabela(*r) : Value::nulo();
}

}  // namespace tilt::rt
