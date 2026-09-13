#include "runtime/checkpoint.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

#include "runtime/json.hpp"
#include "runtime/kafka.hpp"
#include "runtime/s3.hpp"

namespace tilt::rt {

namespace {

bool comeca_com(const std::string& s, const std::string& prefixo) {
  return s.rfind(prefixo, 0) == 0;
}

std::string base(const std::string& p) {
  return std::filesystem::path(p).filename().string();
}

// "kafka:<topico>/<chave>" -> {topico, chave}. Formato gerado pelo resolve;
// topico de Kafka nao contem '/'.
std::pair<std::string, std::string> parte_kafka(const std::string& r) {
  const std::string resto = r.substr(6);
  const std::size_t barra = resto.find('/');
  if (barra == std::string::npos || barra == 0 || barra + 1 >= resto.size()) {
    throw std::runtime_error("checkpoint: URI kafka invalida '" + r + "'");
  }
  return {resto.substr(0, barra), resto.substr(barra + 1)};
}

Value junta_mapas(const Value& acumulado, const Value& novo) {
  Value out = Value::mapa();
  if (acumulado.kind == ValueKind::Mapa && acumulado.map) {
    for (const auto& kv : acumulado.map->items) out.map->set(kv.first, kv.second);
  }
  if (novo.kind == ValueKind::Mapa && novo.map) {
    for (const auto& kv : novo.map->items) out.map->set(kv.first, kv.second);
  }
  return out;
}

}  // namespace

std::string checkpoint_resolve(const std::string& offset_local) {
  const char* dir = std::getenv("TILT_CHECKPOINT_DIR");
  if (!dir || !*dir) return offset_local;
  const std::string d = dir;
  if (comeca_com(d, "s3://")) {
    std::string p = d;
    while (p.size() > 5 && p.back() == '/') p.pop_back();
    return p + "/" + base(offset_local);
  }
  if (comeca_com(d, "kafka:")) {
    std::string t = d.substr(6);
    while (!t.empty() && t.back() == '/') t.pop_back();
    if (t.empty() || t.find('/') != std::string::npos) {
      throw std::runtime_error("checkpoint: TILT_CHECKPOINT_DIR kafka invalido '" + d +
                               "' (use kafka:<topico>)");
    }
    return "kafka:" + t + "/" + base(offset_local);
  }
  std::error_code ec;
  std::filesystem::create_directories(d, ec);
  return (std::filesystem::path(d) / base(offset_local)).string();
}

std::string checkpoint_ler(const std::string& resolvido) {
  if (comeca_com(resolvido, "s3://")) {
    try {
      return s3_get(resolvido);
    } catch (const std::exception& e) {
      const std::string msg = e.what();
      // Ausente (404/NoSuchKey) = sem checkpoint ainda; o resto e erro real.
      if (msg.find("404") != std::string::npos || msg.find("NoSuchKey") != std::string::npos ||
          msg.find("nao encontrado") != std::string::npos) {
        return "";
      }
      throw;
    }
  }
  if (comeca_com(resolvido, "kafka:")) {
    const auto [topico, chave] = parte_kafka(resolvido);
    Value acumulado = Value::mapa();
    // Sem grupo: le do inicio e junta (last-wins); historico curto por
    // construcao (1 record por save, mapa inteiro).
    const Value lista = kafka_ler(topico, false, 500, "", false);
    if (lista.kind == ValueKind::Lista && lista.list) {
      for (const Value& item : *lista.list) {
        if (item.kind != ValueKind::Texto) continue;
        try {
          acumulado = junta_mapas(acumulado, json_parse(item.s));
        } catch (const std::exception&) {
          // record estranho no topico: ignora e segue
        }
      }
    }
    if (acumulado.kind != ValueKind::Mapa || !acumulado.map) return "";
    const Value* v = acumulado.map->find(chave);
    if (!v) return "";
    Value out = Value::mapa();
    out.map->set(chave, *v);
    return json_dump(out);
  }
  std::ifstream in(resolvido);
  if (!in) return "";
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

void checkpoint_gravar(const std::string& resolvido, const std::string& corpo) {
  if (comeca_com(resolvido, "s3://")) {
    s3_put(resolvido, corpo);
    return;
  }
  if (comeca_com(resolvido, "kafka:")) {
    const auto [topico, chave] = parte_kafka(resolvido);
    // Single-writer (eleicao de lider): le o mapa vigente, atualiza a chave
    // e produz o mapa inteiro como 1 record.
    Value atual;
    try {
      const std::string bruto = checkpoint_ler(resolvido);
      atual = bruto.empty() ? Value::mapa() : json_parse(bruto);
    } catch (const std::exception&) {
      atual = Value::mapa();
    }
    Value novo;
    try {
      novo = json_parse(corpo);
    } catch (const std::exception& e) {
      throw std::runtime_error(std::string("checkpoint: corpo invalido: ") + e.what());
    }
    Value merged = junta_mapas(atual, novo);
    ProduceOptions opt;
    opt.tentativas = 3;
    kafka_produzir(topico, json_dump(merged), 0, opt, false);
    return;
  }
  const std::string tmp = resolvido + ".tmp";
  {
    std::ofstream out(tmp, std::ios::trunc);
    if (!out) throw std::runtime_error("checkpoint: nao foi possivel gravar '" + tmp + "'");
    out << corpo;
    if (!out) throw std::runtime_error("checkpoint: falha ao gravar '" + tmp + "'");
  }
  if (std::rename(tmp.c_str(), resolvido.c_str()) != 0) {
    std::remove(tmp.c_str());
    throw std::runtime_error("checkpoint: falha ao publicar '" + resolvido + "'");
  }
}

}  // namespace tilt::rt
