#pragma once

#include <cstddef>
#include <string>

namespace tilt::rt {

// Camada TLS minima sobre um socket conectado (fd). O OpenSSL e carregado em
// runtime via tilt_dlopen ("libssl.so.3" no POSIX, "libssl-3-x64.dll" no
// Windows; ver runtime/compat.hpp) com fallback para o nome curto — zero
// dependencia de link, mesmo padrao de sqlite.cpp (libsqlite3) e
// parquet.cpp (libz).
//
// Handshake de cliente (TLS_client_method), verificacao de certificado com o
// trust store padrao do sistema (SSL_CTX_set_default_verify_paths) e
// verificacao de hostname (SNI + X509_VERIFY_PARAM_set1_host). Defina
// TILT_TLS_SKIP_VERIFY=1 para desligar a verificacao (testes com certificado
// auto-assinado). Sem suporte a client cert / SASL nesta fase.
//
// O TlsStream nao fecha nem toma posse do fd: o dono do socket (as classes
// Conn de redis/kafka/mongo) continua responsavel por close(2).

class TlsStream {
 public:
  // Faz o handshake cliente sobre `fd` (ja conectado). `host` e usado para
  // SNI e verificacao de hostname. Lanca std::runtime_error com mensagem
  // acionavel em falhas (OpenSSL ausente, handshake, certificado).
  TlsStream(int fd, const std::string& host);
  ~TlsStream();

  TlsStream(TlsStream&& outro) noexcept;
  TlsStream& operator=(TlsStream&& outro) noexcept;
  TlsStream(const TlsStream&) = delete;
  TlsStream& operator=(const TlsStream&) = delete;

  // Escreve todos os bytes; lanca em erro de escrita/timeout.
  void write_all(const char* dados, std::size_t n);

  // Le ate `n` bytes; retorna o numero lido (> 0) ou 0 em fechamento
  // ordenado pelo peer. Lanca em erro de leitura/timeout.
  std::size_t read_some(char* out, std::size_t n);

  // Encerramento ordenado (SSL_shutdown) no destrutor, best-effort.

 private:
  int fd_ = -1;
  void* ssl_ = nullptr;   // SSL*
  void* ctx_ = nullptr;   // SSL_CTX*
};

}  // namespace tilt::rt
