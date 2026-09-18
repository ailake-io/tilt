#include "runtime/safetensors.hpp"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

#include "runtime/json.hpp"

namespace tilt::rt {
namespace {

void u64_le(std::string& out, std::uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    out.push_back(static_cast<char>(v & 0xffU));
    v >>= 8;
  }
}
std::uint64_t ler_u64(const std::string& s, std::size_t pos) {
  if (pos + 8 > s.size()) throw std::runtime_error("cabecalho truncado");
  std::uint64_t v = 0;
  for (int i = 7; i >= 0; --i)
    v = (v << 8) | static_cast<unsigned char>(s[pos + static_cast<std::size_t>(i)]);
  return v;
}
void f32_le(std::string& out, float v) {
  std::uint32_t u = 0;
  std::memcpy(&u, &v, sizeof(u));
  for (int i = 0; i < 4; ++i) {
    out.push_back(static_cast<char>(u & 0xffU));
    u >>= 8;
  }
}
float ler_f32(const std::string& s, std::size_t pos) {
  if (pos + 4 > s.size()) throw std::runtime_error("dados truncados");
  std::uint32_t u = 0;
  for (int i = 3; i >= 0; --i)
    u = (u << 8) | static_cast<unsigned char>(s[pos + static_cast<std::size_t>(i)]);
  float v = 0.0F;
  std::memcpy(&v, &u, sizeof(v));
  return v;
}
std::string json_string(const std::string& s) {
  std::string out = "\"";
  for (char c : s) {
    if (c == '\\')
      out += "\\\\";
    else if (c == '"')
      out += "\\\"";
    else if (c == '\n')
      out += "\\n";
    else if (c == '\r')
      out += "\\r";
    else if (c == '\t')
      out += "\\t";
    else
      out.push_back(c);
  }
  out += "\"";
  return out;
}
std::int64_t number(const Value& v) {
  if (!v.is_number()) throw std::runtime_error("metadado numerico invalido");
  return static_cast<std::int64_t>(v.as_number());
}
}  // namespace

bool safetensors_salvar(const std::string& path, const std::map<std::string, Tensor>& tensores,
                        const std::map<std::string, std::string>& metadata, std::string& erro) {
  try {
    if (tensores.empty()) throw std::runtime_error("nenhum tensor para salvar");
    std::string header = "{";
    bool first = true;
    if (!metadata.empty()) {
      header += json_string("__metadata__") + ":{";
      bool mf = true;
      for (const auto& [k, v] : metadata) {
        if (!mf) header += ",";
        mf = false;
        header += json_string(k) + ":" + json_string(v);
      }
      header += "}";
      first = false;
    }
    std::uint64_t offset = 0;
    for (const auto& [name, tensor] : tensores) {
      if (tensor.shape.empty() || tensor.size() < 0 ||
          static_cast<std::uint64_t>(tensor.size()) * 4ULL > 0xffffffffffffff00ULL)
        throw std::runtime_error("forma de tensor invalida em '" + name + "'");
      if (!first) header += ",";
      first = false;
      header += json_string(name) + ":{\"dtype\":\"F32\",\"shape\":[";
      for (std::size_t i = 0; i < tensor.shape.size(); ++i) {
        if (i) header += ",";
        header += std::to_string(tensor.shape[i]);
      }
      const std::uint64_t bytes = static_cast<std::uint64_t>(tensor.data.size()) * 4ULL;
      header += "],\"data_offsets\":[" + std::to_string(offset) + "," +
                std::to_string(offset + bytes) + "]}";
      offset += bytes;
    }
    header += "}";
    while ((header.size() + 8) % 8 != 0) header.push_back(' ');
    std::string out;
    out.reserve(8 + header.size() + static_cast<std::size_t>(offset));
    u64_le(out, static_cast<std::uint64_t>(header.size()));
    out += header;
    for (const auto& [name, tensor] : tensores)
      for (float v : tensor.data) f32_le(out, v);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) throw std::runtime_error("nao foi possivel gravar '" + path + "'");
    file.write(out.data(), static_cast<std::streamsize>(out.size()));
    if (!file) throw std::runtime_error("falha ao gravar '" + path + "'");
    return true;
  } catch (const std::exception& e) {
    erro = e.what();
    return false;
  }
}

bool safetensors_carregar(const std::string& path, std::map<std::string, Tensor>& tensores,
                          std::map<std::string, std::string>& metadata, std::string& erro) {
  try {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("nao foi possivel abrir '" + path + "'");
    std::ostringstream ss;
    ss << file.rdbuf();
    const std::string bytes = ss.str();
    const std::uint64_t header_size = ler_u64(bytes, 0);
    if (header_size > bytes.size() - 8) throw std::runtime_error("cabecalho fora do arquivo");
    const std::string header = bytes.substr(8, static_cast<std::size_t>(header_size));
    const Value doc = json_parse(header);
    if (doc.kind != ValueKind::Mapa || !doc.map)
      throw std::runtime_error("cabecalho JSON invalido");
    if (const Value* mv = doc.map->find("__metadata__");
        mv && mv->kind == ValueKind::Mapa && mv->map) {
      for (const auto& [k, v] : mv->map->items)
        if (v.kind == ValueKind::Texto) metadata[k] = v.s;
    }
    const std::size_t data_begin = 8 + static_cast<std::size_t>(header_size);
    for (const auto& [name, spec] : doc.map->items) {
      if (name == "__metadata__") continue;
      if (spec.kind != ValueKind::Mapa || !spec.map)
        throw std::runtime_error("tensor '" + name + "' invalido");
      const Value* dtype = spec.map->find("dtype");
      const Value* shape = spec.map->find("shape");
      const Value* offsets = spec.map->find("data_offsets");
      if (!dtype || dtype->kind != ValueKind::Texto || dtype->s != "F32" || !shape ||
          shape->kind != ValueKind::Lista || !shape->list || !offsets ||
          offsets->kind != ValueKind::Lista || !offsets->list || offsets->list->size() != 2)
        throw std::runtime_error("tensor '" + name + "' precisa ser F32 com shape/data_offsets");
      Tensor tensor;
      for (const Value& d : *shape->list) {
        const std::int64_t dim = number(d);
        if (dim <= 0) throw std::runtime_error("dimensao invalida em '" + name + "'");
        tensor.shape.push_back(dim);
      }
      const std::uint64_t begin = static_cast<std::uint64_t>(number((*offsets->list)[0]));
      const std::uint64_t end = static_cast<std::uint64_t>(number((*offsets->list)[1]));
      if (end < begin || end - begin != static_cast<std::uint64_t>(tensor.size()) * 4ULL ||
          end > bytes.size() - data_begin)
        throw std::runtime_error("offsets invalidos em '" + name + "'");
      tensor.data.resize(static_cast<std::size_t>(tensor.size()));
      for (std::size_t i = 0; i < tensor.data.size(); ++i)
        tensor.data[i] = ler_f32(bytes, data_begin + static_cast<std::size_t>(begin) + i * 4);
      tensores[name] = std::move(tensor);
    }
    return true;
  } catch (const std::exception& e) {
    erro = e.what();
    return false;
  }
}

}  // namespace tilt::rt
