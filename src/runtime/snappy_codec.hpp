#pragma once

// Codec snappy (formato de bloco, como usado pelo Parquet), header-only e sem
// dependencias de link:
// - snappy_decompress(): descompressor completo do formato de bloco snappy
//   (varint de tamanho nao comprimido + tags literal / copy-1 / copy-2 /
//   copy-4), capaz de ler blocos gerados por qualquer implementacao real
//   (pyarrow, parquet-cpp etc., que usam matching com sobreposicao);
// - snappy_compress_literals(): compressor "literal-only" — emite um bloco
//   snappy VALIDO composto exclusivamente de literais (sem back-references).
//   Qualquer leitor snappy descomprime o resultado corretamente; o custo e a
//   ausencia de compressao de fato (cabeçalho + tags por bloco de 64 KiB).
//
// Lanca std::runtime_error com mensagem "snappy: ..." em stream invalido.

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace tilt::rt {
namespace snappy_detail {

[[noreturn]] inline void sfail(const std::string& m) {
  throw std::runtime_error("snappy: " + m);
}

inline std::uint64_t read_varint(const std::uint8_t*& p, const std::uint8_t* end) {
  std::uint64_t v = 0;
  unsigned shift = 0;
  while (true) {
    if (p >= end) sfail("varint de tamanho truncado");
    const std::uint8_t b = *p++;
    v |= static_cast<std::uint64_t>(b & 0x7F) << shift;
    if (!(b & 0x80)) return v;
    shift += 7;
    if (shift > 63) sfail("varint de tamanho invalido");
  }
}

}  // namespace snappy_detail

// Descomprime um bloco snappy generico. Retorna o conteudo original.
inline std::string snappy_decompress(const std::string& in) {
  const std::uint8_t* p = reinterpret_cast<const std::uint8_t*>(in.data());
  const std::uint8_t* end = p + in.size();
  const std::uint64_t out_len = snappy_detail::read_varint(p, end);
  std::string out;
  out.reserve(static_cast<std::size_t>(out_len));
  while (out.size() < out_len) {
    if (p >= end) snappy_detail::sfail("bloco truncado (tags em falta)");
    const std::uint8_t tag = *p++;
    switch (tag & 0x03) {
      case 0: {  // literal
        std::uint64_t len = tag >> 2;
        if (len >= 60) {  // tamanho em 1..4 bytes little-endian (len - 59 bytes)
          const unsigned extra = static_cast<unsigned>(len) - 59;
          if (static_cast<std::size_t>(end - p) < extra) {
            snappy_detail::sfail("literal com tamanho truncado");
          }
          std::uint64_t v = 0;
          for (unsigned k = 0; k < extra; ++k) {
            v |= static_cast<std::uint64_t>(*p++) << (8 * k);
          }
          len = v;
        }
        ++len;  // tamanho armazenado = len - 1
        if (static_cast<std::uint64_t>(end - p) < len || out.size() + len > out_len) {
          snappy_detail::sfail("literal fora dos limites do bloco");
        }
        out.append(reinterpret_cast<const char*>(p), static_cast<std::size_t>(len));
        p += len;
        break;
      }
      case 1: {  // copy com offset de 1 byte: len 4..11
        if (p >= end) snappy_detail::sfail("copy-1 truncado");
        const std::uint64_t len = 4 + ((tag >> 2) & 0x07);
        const std::uint64_t offset = ((tag >> 5) << 8) | *p++;
        if (offset == 0 || offset > out.size()) {
          snappy_detail::sfail("copy com offset invalido");
        }
        for (std::uint64_t k = 0; k < len; ++k) {
          if (out.size() >= out_len) snappy_detail::sfail("copia excede o tamanho declarado");
          out.push_back(out[out.size() - offset]);
        }
        break;
      }
      case 2: {  // copy com offset de 2 bytes: len 1..64
        if (static_cast<std::size_t>(end - p) < 2) snappy_detail::sfail("copy-2 truncado");
        const std::uint64_t len = 1 + (tag >> 2);
        const std::uint64_t offset = static_cast<std::uint64_t>(p[0]) | (static_cast<std::uint64_t>(p[1]) << 8);
        p += 2;
        if (offset == 0 || offset > out.size()) {
          snappy_detail::sfail("copy com offset invalido");
        }
        for (std::uint64_t k = 0; k < len; ++k) {
          if (out.size() >= out_len) snappy_detail::sfail("copia excede o tamanho declarado");
          out.push_back(out[out.size() - offset]);
        }
        break;
      }
      default: {  // copy com offset de 4 bytes: len 1..64
        if (static_cast<std::size_t>(end - p) < 4) snappy_detail::sfail("copy-4 truncado");
        const std::uint64_t len = 1 + (tag >> 2);
        std::uint64_t offset = 0;
        for (unsigned k = 0; k < 4; ++k) {
          offset |= static_cast<std::uint64_t>(*p++) << (8 * k);
        }
        if (offset == 0 || offset > out.size()) {
          snappy_detail::sfail("copy com offset invalido");
        }
        for (std::uint64_t k = 0; k < len; ++k) {
          if (out.size() >= out_len) snappy_detail::sfail("copia excede o tamanho declarado");
          out.push_back(out[out.size() - offset]);
        }
        break;
      }
    }
  }
  if (p != end) snappy_detail::sfail("bytes sobrando apos o fim do bloco");
  return out;
}

// Comprime em um bloco snappy valido composto so de literais. Nao reduz o
// tamanho (ao contrario: adiciona ~0,03% de tags), mas produz um stream que
// qualquer leitor snappy descomprime para o conteudo original.
inline std::string snappy_compress_literals(const std::string& in) {
  std::string out;
  std::uint64_t len = in.size();
  while (len >= 0x80) {  // varint do tamanho nao comprimido
    out.push_back(static_cast<char>((len & 0x7F) | 0x80));
    len >>= 7;
  }
  out.push_back(static_cast<char>(len));

  std::size_t pos = 0;
  while (pos < in.size()) {
    const std::size_t chunk = in.size() - pos < 65536 ? in.size() - pos : 65536;
    const std::uint32_t stored = static_cast<std::uint32_t>(chunk) - 1;  // len - 1
    // tag literal com tamanho em 2 bytes (61 -> 60 + 2 bytes de tamanho)
    out.push_back(static_cast<char>(61 << 2));
    out.push_back(static_cast<char>(stored & 0xFF));
    out.push_back(static_cast<char>((stored >> 8) & 0xFF));
    out.append(in.data() + pos, chunk);
    pos += chunk;
  }
  return out;
}

}  // namespace tilt::rt
