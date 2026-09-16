#include "runtime/gguf.hpp"

#include <cstring>
#include <fstream>
#include <stdexcept>

namespace tilt::rt {

namespace {

// Escritor little-endian minimo para o GGUF.
struct Escritor {
  std::string out;
  void u8(std::uint8_t v) { out.push_back(static_cast<char>(v)); }
  void u32(std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
  }
  void u64(std::uint64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
  }
  void f32(float v) {
    std::uint32_t u = 0;
    std::memcpy(&u, &v, 4);
    u32(u);
  }
  void texto(const std::string& s) {
    u64(static_cast<std::uint64_t>(s.size()));
    out.append(s);
  }
};

// Tipos de metadado do GGUF.
enum : std::uint32_t {
  GGUF_UINT8 = 0,
  GGUF_INT8 = 1,
  GGUF_UINT16 = 2,
  GGUF_INT16 = 3,
  GGUF_UINT32 = 4,
  GGUF_INT32 = 5,
  GGUF_FLOAT32 = 6,
  GGUF_BOOL = 7,
  GGUF_STRING = 8,
  GGUF_ARRAY = 9,
  GGUF_UINT64 = 10,
  GGUF_INT64 = 11,
  GGUF_FLOAT64 = 12,
};

void kv_texto(Escritor& w, const std::string& chave, const std::string& valor) {
  w.texto(chave);
  w.u32(GGUF_STRING);
  w.texto(valor);
}

void kv_u32(Escritor& w, const std::string& chave, std::uint32_t valor) {
  w.texto(chave);
  w.u32(GGUF_UINT32);
  w.u32(valor);
}

[[noreturn]] void die(const std::string& m) { throw std::runtime_error("gguf: " + m); }

}  // namespace

bool gguf_salvar(const std::string& path, const std::vector<GgufTensor>& tensores,
                 const std::string& nome_modelo, std::string& err) {
  try {
    if (tensores.empty()) die("modelo sem tensores");
    for (const GgufTensor& t : tensores) {
      if (t.nome.empty()) die("tensor sem nome");
      if (t.forma.empty() || t.forma.size() > 4) die("tensor '" + t.nome + "' com rank invalido");
      std::int64_t n = 1;
      for (std::int64_t d : t.forma) {
        if (d <= 0) die("tensor '" + t.nome + "' com dimensao invalida");
        n *= d;
      }
      if (n != static_cast<std::int64_t>(t.dados.size())) {
        die("tensor '" + t.nome + "' com dados incompativeis com a forma");
      }
    }

    Escritor w;
    w.out.append("GGUF", 4);
    w.u32(3);  // versao
    w.u64(static_cast<std::uint64_t>(tensores.size()));
    w.u64(3);  // metadados
    kv_texto(w, "general.architecture", "tilt");
    kv_texto(w, "general.name", nome_modelo.empty() ? "tilt" : nome_modelo);
    kv_u32(w, "tilt.tensores", static_cast<std::uint32_t>(tensores.size()));

    // Infos dos tensores (offsets relativos ao inicio da secao de dados).
    std::uint64_t desloc = 0;
    for (const GgufTensor& t : tensores) {
      w.texto(t.nome);
      w.u32(static_cast<std::uint32_t>(t.forma.size()));
      // GGUF grava as dimensoes invertidas (da mais rapida para a mais lenta).
      for (auto it = t.forma.rbegin(); it != t.forma.rend(); ++it) {
        w.u64(static_cast<std::uint64_t>(*it));
      }
      w.u32(0);  // F32
      w.u64(desloc);
      desloc += static_cast<std::uint64_t>(t.dados.size() * 4);
      desloc = (desloc + 31) & ~static_cast<std::uint64_t>(31);  // alinha em 32
    }

    // Alinha o cabecalho em 32 antes dos dados.
    while (w.out.size() % 32 != 0) w.u8(0);

    std::string dados;
    std::uint64_t pos = 0;
    for (const GgufTensor& t : tensores) {
      while (pos % 32 != 0) {
        dados.push_back(0);
        ++pos;
      }
      const std::string bruto(reinterpret_cast<const char*>(t.dados.data()),
                              t.dados.size() * sizeof(float));
      dados.append(bruto);
      pos += static_cast<std::uint64_t>(bruto.size());
    }

    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) die("nao foi possivel gravar '" + path + "'");
    f.write(w.out.data(), static_cast<std::streamsize>(w.out.size()));
    f.write(dados.data(), static_cast<std::streamsize>(dados.size()));
    if (!f) die("falha ao gravar '" + path + "'");
    return true;
  } catch (const std::exception& e) {
    err = e.what();
    return false;
  }
}

}  // namespace tilt::rt
