#include "runtime/gguf.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace tilt::rt {
namespace {

struct Writer {
  std::string out;
  void u8(std::uint8_t v) { out.push_back(static_cast<char>(v)); }
  void u32(std::uint32_t v) { for (int i = 0; i < 4; ++i) u8(static_cast<std::uint8_t>(v >> (8 * i))); }
  void u64(std::uint64_t v) { for (int i = 0; i < 8; ++i) u8(static_cast<std::uint8_t>(v >> (8 * i))); }
  void text(const std::string& s) { u64(s.size()); out.append(s); }
};

enum : std::uint32_t {
  GGUF_UINT8 = 0, GGUF_INT8 = 1, GGUF_UINT16 = 2, GGUF_INT16 = 3,
  GGUF_UINT32 = 4, GGUF_INT32 = 5, GGUF_FLOAT32 = 6, GGUF_BOOL = 7,
  GGUF_STRING = 8, GGUF_ARRAY = 9, GGUF_UINT64 = 10, GGUF_INT64 = 11,
  GGUF_FLOAT64 = 12
};

[[noreturn]] void die(const std::string& m) { throw std::runtime_error("gguf: " + m); }

void kv_text(Writer& w, const std::string& key, const std::string& value) {
  w.text(key); w.u32(GGUF_STRING); w.text(value);
}
void kv_u32(Writer& w, const std::string& key, std::uint32_t value) {
  w.text(key); w.u32(GGUF_UINT32); w.u32(value);
}

std::uint16_t f32_to_f16(float value) {
  std::uint32_t bits = 0; std::memcpy(&bits, &value, sizeof(bits));
  const std::uint32_t sign = (bits >> 16) & 0x8000U;
  const std::uint32_t exp = (bits >> 23) & 0xffU;
  const std::uint32_t mant = bits & 0x7fffffU;
  if (exp == 0xffU) return static_cast<std::uint16_t>(sign | (mant ? 0x7e00U : 0x7c00U));
  const int e = static_cast<int>(exp) - 127 + 15;
  if (e >= 31) return static_cast<std::uint16_t>(sign | 0x7c00U);
  if (e <= 0) {
    if (e < -10) return static_cast<std::uint16_t>(sign);
    return static_cast<std::uint16_t>(sign | ((mant | 0x800000U) >> (14 - e)));
  }
  return static_cast<std::uint16_t>(sign | (static_cast<std::uint32_t>(e) << 10) | (mant >> 13));
}

float f16_to_f32(std::uint16_t h) {
  const std::uint32_t sign = static_cast<std::uint32_t>(h & 0x8000U) << 16;
  const std::uint32_t exp = (h >> 10) & 0x1fU;
  const std::uint32_t mant = h & 0x3ffU;
  std::uint32_t bits = 0;
  if (exp == 0) {
    if (mant == 0) bits = sign;
    else {
      std::uint32_t m = mant; int e = -14;
      while ((m & 0x400U) == 0) { m <<= 1; --e; }
      bits = sign | (static_cast<std::uint32_t>(e + 127) << 23) | ((m & 0x3ffU) << 13);
    }
  } else if (exp == 0x1fU) bits = sign | 0x7f800000U | (mant << 13);
  else bits = sign | ((exp + 112U) << 23) | (mant << 13);
  float out = 0.0F; std::memcpy(&out, &bits, sizeof(out)); return out;
}

std::string encode_f32(const GgufTensor& t) {
  return std::string(reinterpret_cast<const char*>(t.dados.data()), t.dados.size() * sizeof(float));
}
std::string encode_q8(const GgufTensor& t) {
  if (t.dados.size() % 32 != 0) die("Q8_0 exige quantidade de elementos multipla de 32");
  std::string out; out.reserve((t.dados.size() / 32) * 34);
  for (std::size_t base = 0; base < t.dados.size(); base += 32) {
    float max_abs = 0.0F;
    for (std::size_t i = 0; i < 32; ++i) max_abs = std::max(max_abs, std::fabs(t.dados[base + i]));
    const float scale = max_abs > 0.0F ? max_abs / 127.0F : 0.0F;
    const std::uint16_t h = f32_to_f16(scale);
    out.push_back(static_cast<char>(h & 0xffU)); out.push_back(static_cast<char>(h >> 8));
    for (std::size_t i = 0; i < 32; ++i) {
      const int q = scale == 0.0F ? 0 : static_cast<int>(std::lrint(std::clamp(
          t.dados[base + i] / scale, -127.0F, 127.0F)));
      out.push_back(static_cast<char>(static_cast<std::int8_t>(q)));
    }
  }
  return out;
}

struct Reader {
  const std::string& bytes; std::size_t pos = 0;
  std::uint8_t u8() { if (pos >= bytes.size()) throw std::runtime_error("arquivo GGUF truncado"); return static_cast<std::uint8_t>(bytes[pos++]); }
  std::uint32_t u32() { std::uint32_t v = 0; for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(u8()) << (8 * i); return v; }
  std::uint64_t u64() { std::uint64_t v = 0; for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(u8()) << (8 * i); return v; }
  std::string text() { const auto n = u64(); if (n > bytes.size() - pos) throw std::runtime_error("texto GGUF truncado"); std::string s = bytes.substr(pos, static_cast<std::size_t>(n)); pos += static_cast<std::size_t>(n); return s; }
  void skip(std::size_t n) { if (n > bytes.size() - pos) throw std::runtime_error("valor GGUF truncado"); pos += n; }
};

void skip_value(Reader& r, std::uint32_t type) {
  switch (type) {
    case GGUF_UINT8: case GGUF_INT8: case GGUF_BOOL: r.skip(1); return;
    case GGUF_UINT16: case GGUF_INT16: r.skip(2); return;
    case GGUF_UINT32: case GGUF_INT32: case GGUF_FLOAT32: r.skip(4); return;
    case GGUF_UINT64: case GGUF_INT64: case GGUF_FLOAT64: r.skip(8); return;
    case GGUF_STRING: (void)r.text(); return;
    case GGUF_ARRAY: {
      const auto element = r.u32(); const auto n = r.u64();
      if (n > 100000000ULL) throw std::runtime_error("array GGUF grande demais");
      for (std::uint64_t i = 0; i < n; ++i) skip_value(r, element);
      return;
    }
    default: throw std::runtime_error("tipo de metadata GGUF desconhecido");
  }
}

std::size_t count_elements(const std::vector<std::int64_t>& shape) {
  std::size_t n = 1;
  for (const auto d : shape) {
    if (d <= 0 || n > std::numeric_limits<std::size_t>::max() / static_cast<std::size_t>(d)) die("forma GGUF invalida");
    n *= static_cast<std::size_t>(d);
  }
  return n;
}

}  // namespace

bool gguf_salvar(const std::string& path, const std::vector<GgufTensor>& tensors,
                 const std::string& model_name, std::string& err) {
  try {
    if (tensors.empty()) die("modelo sem tensores");
    std::vector<std::string> blobs;
    for (const auto& t : tensors) {
      if (t.nome.empty() || t.forma.empty() || t.forma.size() > 4) die("tensor com nome/forma invalida");
      if (count_elements(t.forma) != t.dados.size()) die("tensor '" + t.nome + "' com dados incompativeis");
      if (t.tipo == GgufTensor::Tipo::Q8_0 && t.dados.size() % 32 != 0) die("tensor '" + t.nome + "' nao e multiplo de 32 para Q8_0");
      blobs.push_back(t.tipo == GgufTensor::Tipo::Q8_0 ? encode_q8(t) : encode_f32(t));
    }
    Writer w; w.out.append("GGUF", 4); w.u32(3); w.u64(tensors.size()); w.u64(3);
    kv_text(w, "general.architecture", "tilt"); kv_text(w, "general.name", model_name.empty() ? "tilt" : model_name);
    kv_u32(w, "tilt.tensores", static_cast<std::uint32_t>(tensors.size()));
    std::uint64_t offset = 0;
    for (std::size_t i = 0; i < tensors.size(); ++i) {
      const auto& t = tensors[i]; w.text(t.nome); w.u32(static_cast<std::uint32_t>(t.forma.size()));
      for (auto it = t.forma.rbegin(); it != t.forma.rend(); ++it) w.u64(static_cast<std::uint64_t>(*it));
      w.u32(static_cast<std::uint32_t>(t.tipo)); w.u64(offset);
      offset += blobs[i].size(); offset = (offset + 31) & ~static_cast<std::uint64_t>(31);
    }
    while (w.out.size() % 32 != 0) w.u8(0);
    std::string data;
    for (const auto& blob : blobs) { while (data.size() % 32 != 0) data.push_back(0); data.append(blob); }
    std::ofstream f(path, std::ios::binary | std::ios::trunc); if (!f) die("nao foi possivel gravar '" + path + "'");
    f.write(w.out.data(), static_cast<std::streamsize>(w.out.size())); f.write(data.data(), static_cast<std::streamsize>(data.size()));
    if (!f) die("falha ao gravar '" + path + "'");
    return true;
  } catch (const std::exception& e) { err = e.what(); return false; }
}

bool gguf_carregar_tensores(const std::string& path, std::vector<GgufTensor>& tensors, std::string& err) {
  tensors.clear();
  try {
    std::ifstream f(path, std::ios::binary); if (!f) die("nao foi possivel abrir o arquivo");
    const std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (bytes.size() < 24 || bytes.compare(0, 4, "GGUF") != 0) die("magic invalido");
    Reader r{bytes}; (void)r.u32(); const auto version = r.u32(); if (version < 2 || version > 3) die("versao nao suportada");
    const auto nt = r.u64(); const auto nkv = r.u64(); if (nt == 0 || nt > 1000000ULL || nkv > 1000000ULL) die("cabecalho invalido");
    for (std::uint64_t i = 0; i < nkv; ++i) { (void)r.text(); skip_value(r, r.u32()); }
    struct Info { GgufTensor tensor; std::uint64_t offset; };
    std::vector<Info> infos; infos.reserve(static_cast<std::size_t>(nt));
    for (std::uint64_t i = 0; i < nt; ++i) {
      Info info; info.tensor.nome = r.text(); const auto rank = r.u32(); if (rank == 0 || rank > 4) die("rank invalido");
      info.tensor.forma.resize(rank); for (std::uint32_t d = 0; d < rank; ++d) { const auto dim = r.u64(); if (!dim || dim > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) die("dimensao invalida"); info.tensor.forma[rank - d - 1] = static_cast<std::int64_t>(dim); }
      const auto type = r.u32(); if (type != 0 && type != 8) die("tipo de tensor nao suportado");
      if (type == 8 && count_elements(info.tensor.forma) % 32 != 0) die("Q8_0 com forma nao multipla de 32");
      info.tensor.tipo = type == 8 ? GgufTensor::Tipo::Q8_0 : GgufTensor::Tipo::F32; info.offset = r.u64(); infos.push_back(std::move(info));
    }
    const std::size_t data_start = (r.pos + 31) & ~static_cast<std::size_t>(31); if (data_start > bytes.size()) die("dados ausentes");
    for (auto& info : infos) {
      const std::size_t n = count_elements(info.tensor.forma); const std::size_t bytes_n = info.tensor.tipo == GgufTensor::Tipo::Q8_0 ? (n / 32) * 34 : n * 4;
      if (info.offset > bytes.size() - data_start || bytes_n > bytes.size() - data_start - info.offset) die("dados truncados");
      const std::size_t at = data_start + static_cast<std::size_t>(info.offset); info.tensor.dados.resize(n);
      if (info.tensor.tipo == GgufTensor::Tipo::F32) {
        for (std::size_t i = 0; i < n; ++i) { std::uint32_t bits = 0; for (int b = 0; b < 4; ++b) bits |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + i * 4 + b])) << (8 * b); std::memcpy(&info.tensor.dados[i], &bits, sizeof(float)); }
      } else {
        for (std::size_t base = 0; base < n; base += 32) { const std::size_t block = at + (base / 32) * 34; const auto h = static_cast<std::uint16_t>(static_cast<unsigned char>(bytes[block])) | (static_cast<std::uint16_t>(static_cast<unsigned char>(bytes[block + 1])) << 8); const float scale = f16_to_f32(h); for (std::size_t i = 0; i < 32; ++i) info.tensor.dados[base + i] = scale * static_cast<float>(static_cast<std::int8_t>(bytes[block + 2 + i])); }
      }
      info.tensor.tipo = GgufTensor::Tipo::F32; tensors.push_back(std::move(info.tensor));
    }
    return true;
  } catch (const std::exception& e) { tensors.clear(); err = e.what(); return false; }
}

}  // namespace tilt::rt
