#include "runtime/tls.hpp"

#include <dlfcn.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <stdexcept>

namespace tilt::rt {

namespace {

[[noreturn]] void die(const std::string& m) { throw std::runtime_error("tls: " + m); }

// Codigos numericos das macros do openssl/ssl.h e openssl/tls1.h (as macros
// nao sao exportadas como simbolos; chamamos as funcoes por tras com os
// codigos). Verificados contra /usr/include/openssl.
constexpr int kSslCtrlSetTlsextHostname = 55;  // SSL_CTRL_SET_TLSEXT_HOSTNAME
constexpr int kTlsextNametypeHostName = 0;     // TLSEXT_NAMETYPE_host_name
constexpr int kSslVerifyNone = 0x00;           // SSL_VERIFY_NONE
constexpr int kSslVerifyPeer = 0x01;           // SSL_VERIFY_PEER
constexpr int kSslErrorSsl = 1;                // SSL_ERROR_SSL
constexpr int kSslErrorSyscall = 5;            // SSL_ERROR_SYSCALL
constexpr int kSslErrorZeroReturn = 6;         // SSL_ERROR_ZERO_RETURN

// OpenSSL carregada via dlopen — mesmo padrao de sqlite.cpp/parquet.cpp.
struct SslApi {
  void* ssl_lib = nullptr;
  void* crypto_lib = nullptr;
  const void* (*tls_client_method)(void) = nullptr;
  void* (*ssl_ctx_new)(const void*) = nullptr;
  void (*ssl_ctx_free)(void*) = nullptr;
  long (*ssl_ctrl)(void*, int, long, void*) = nullptr;  // p/ SNI (macro)
  void (*ssl_ctx_set_verify)(void*, int, void*) = nullptr;
  int (*ssl_ctx_set_default_verify_paths)(void*) = nullptr;
  void* (*ssl_new)(void*) = nullptr;
  void (*ssl_free)(void*) = nullptr;
  int (*ssl_set_fd)(void*, int) = nullptr;
  int (*ssl_connect)(void*) = nullptr;
  int (*ssl_write)(void*, const void*, int) = nullptr;
  int (*ssl_read)(void*, void*, int) = nullptr;
  int (*ssl_get_error)(const void*, int) = nullptr;
  int (*ssl_shutdown)(void*) = nullptr;
  void* (*ssl_get0_param)(void*) = nullptr;
  unsigned long (*err_get_error)(void) = nullptr;
  void (*err_error_string_n)(unsigned long, char*, std::size_t) = nullptr;
  int (*x509_verify_param_set1_host)(void*, const char*, std::size_t) = nullptr;
};

template <typename F>
bool bind_sym(void* lib, F& fn, const char* name) {
  fn = reinterpret_cast<F>(::dlsym(lib, name));
  return fn != nullptr;
}

const SslApi& openssl() {
  static const SslApi instance = [] {
    SslApi a;
    a.ssl_lib = ::dlopen("libssl.so.3", RTLD_NOW | RTLD_LOCAL);
    if (!a.ssl_lib) a.ssl_lib = ::dlopen("libssl.so", RTLD_NOW | RTLD_LOCAL);
    a.crypto_lib = ::dlopen("libcrypto.so.3", RTLD_NOW | RTLD_LOCAL);
    if (!a.crypto_lib) a.crypto_lib = ::dlopen("libcrypto.so", RTLD_NOW | RTLD_LOCAL);
    if (!a.ssl_lib || !a.crypto_lib) {
      if (a.ssl_lib) ::dlclose(a.ssl_lib);
      if (a.crypto_lib) ::dlclose(a.crypto_lib);
      return SslApi{};
    }
    const bool ok = bind_sym(a.ssl_lib, a.tls_client_method, "TLS_client_method") &&
                    bind_sym(a.ssl_lib, a.ssl_ctx_new, "SSL_CTX_new") &&
                    bind_sym(a.ssl_lib, a.ssl_ctx_free, "SSL_CTX_free") &&
                    bind_sym(a.ssl_lib, a.ssl_ctrl, "SSL_ctrl") &&
                    bind_sym(a.ssl_lib, a.ssl_ctx_set_verify, "SSL_CTX_set_verify") &&
                    bind_sym(a.ssl_lib, a.ssl_ctx_set_default_verify_paths,
                             "SSL_CTX_set_default_verify_paths") &&
                    bind_sym(a.ssl_lib, a.ssl_new, "SSL_new") &&
                    bind_sym(a.ssl_lib, a.ssl_free, "SSL_free") &&
                    bind_sym(a.ssl_lib, a.ssl_set_fd, "SSL_set_fd") &&
                    bind_sym(a.ssl_lib, a.ssl_connect, "SSL_connect") &&
                    bind_sym(a.ssl_lib, a.ssl_write, "SSL_write") &&
                    bind_sym(a.ssl_lib, a.ssl_read, "SSL_read") &&
                    bind_sym(a.ssl_lib, a.ssl_get_error, "SSL_get_error") &&
                    bind_sym(a.ssl_lib, a.ssl_shutdown, "SSL_shutdown") &&
                    bind_sym(a.ssl_lib, a.ssl_get0_param, "SSL_get0_param") &&
                    bind_sym(a.crypto_lib, a.err_get_error, "ERR_get_error") &&
                    bind_sym(a.crypto_lib, a.err_error_string_n, "ERR_error_string_n") &&
                    bind_sym(a.crypto_lib, a.x509_verify_param_set1_host,
                             "X509_VERIFY_PARAM_set1_host");
    if (!ok) {
      ::dlclose(a.ssl_lib);
      ::dlclose(a.crypto_lib);
      return SslApi{};
    }
    return a;
  }();
  return instance;
}

bool skip_verify() {
  const char* env = std::getenv("TILT_TLS_SKIP_VERIFY");
  return env != nullptr && env[0] == '1';
}

// Mensagem do erro mais recente da fila do OpenSSL (libcrypto).
std::string erro_openssl(const SslApi& api) {
  const unsigned long code = api.err_get_error();
  if (code == 0) return "erro desconhecido do OpenSSL";
  char buf[256] = {0};
  api.err_error_string_n(code, buf, sizeof(buf) - 1);
  return buf;
}

}  // namespace

TlsStream::TlsStream(int fd, const std::string& host) : fd_(fd) {
  const SslApi& api = openssl();
  if (!api.ssl_lib) {
    die("OpenSSL nao encontrado: instale libssl3 (libssl.so.3 / libcrypto.so.3)");
  }

  ctx_ = api.ssl_ctx_new(api.tls_client_method());
  if (!ctx_) die("falha ao criar o SSL_CTX (" + erro_openssl(api) + ")");
  if (api.ssl_ctx_set_default_verify_paths(ctx_) != 1) {
    api.ssl_ctx_free(ctx_);
    ctx_ = nullptr;
    die("nao foi possivel carregar os certificados CAs do sistema "
        "(SSL_CTX_set_default_verify_paths); verifique a instalacao do OpenSSL");
  }
  if (skip_verify()) {
    api.ssl_ctx_set_verify(ctx_, kSslVerifyNone, nullptr);
  } else {
    api.ssl_ctx_set_verify(ctx_, kSslVerifyPeer, nullptr);
  }

  ssl_ = api.ssl_new(ctx_);
  if (!ssl_) {
    api.ssl_ctx_free(ctx_);
    ctx_ = nullptr;
    die("falha ao criar o SSL (" + erro_openssl(api) + ")");
  }
  if (api.ssl_set_fd(ssl_, fd_) != 1) {
    api.ssl_free(ssl_);
    api.ssl_ctx_free(ctx_);
    ssl_ = nullptr;
    ctx_ = nullptr;
    die("falha ao associar o socket ao SSL");
  }

  // SNI: a macro SSL_set_tlsext_host_name vira SSL_ctrl com estes codigos.
  if (!host.empty()) {
    api.ssl_ctrl(ssl_, kSslCtrlSetTlsextHostname, kTlsextNametypeHostName,
                 const_cast<char*>(host.c_str()));
    // Verificacao de hostname no certificado do peer.
    if (!skip_verify()) {
      api.x509_verify_param_set1_host(api.ssl_get0_param(ssl_), host.c_str(), 0);
    }
  }

  const int ret = api.ssl_connect(ssl_);
  if (ret != 1) {
    const int err = api.ssl_get_error(ssl_, ret);
    std::string msg = "handshake falhou";
    if (err == kSslErrorSsl) {
      msg += ": " + erro_openssl(api);
      if (!skip_verify()) {
        msg += " (certificado auto-assinado ou host divergente? defina TILT_TLS_SKIP_VERIFY=1 "
               "para ignorar a verificacao em testes)";
      }
    } else if (err == kSslErrorSyscall && errno != 0) {
      msg += ": " + std::string(std::strerror(errno));
    } else {
      msg += " (codigo " + std::to_string(err) + ")";
    }
    api.ssl_free(ssl_);
    api.ssl_ctx_free(ctx_);
    ssl_ = nullptr;
    ctx_ = nullptr;
    die(msg);
  }
}

TlsStream::~TlsStream() {
  if (!ssl_) return;
  const SslApi& api = openssl();
  api.ssl_shutdown(ssl_);  // best-effort: o socket fecha logo em seguida
  api.ssl_free(ssl_);
  api.ssl_ctx_free(ctx_);
}

TlsStream::TlsStream(TlsStream&& outro) noexcept
    : fd_(outro.fd_), ssl_(outro.ssl_), ctx_(outro.ctx_) {
  outro.fd_ = -1;
  outro.ssl_ = nullptr;
  outro.ctx_ = nullptr;
}

TlsStream& TlsStream::operator=(TlsStream&& outro) noexcept {
  if (this != &outro) {
    this->~TlsStream();
    fd_ = outro.fd_;
    ssl_ = outro.ssl_;
    ctx_ = outro.ctx_;
    outro.fd_ = -1;
    outro.ssl_ = nullptr;
    outro.ctx_ = nullptr;
  }
  return *this;
}

void TlsStream::write_all(const char* dados, std::size_t n) {
  const SslApi& api = openssl();
  std::size_t off = 0;
  while (off < n) {
    const int chunk = static_cast<int>(std::min<std::size_t>(n - off, 1u << 30));
    const int w = api.ssl_write(ssl_, dados + off, chunk);
    if (w <= 0) {
      const int err = api.ssl_get_error(ssl_, w);
      if (err == kSslErrorSyscall && errno != 0) {
        die("falha ao enviar dados: " + std::string(std::strerror(errno)));
      }
      die("falha ao enviar dados (codigo SSL " + std::to_string(err) + ")");
    }
    off += static_cast<std::size_t>(w);
  }
}

std::size_t TlsStream::read_some(char* out, std::size_t n) {
  const SslApi& api = openssl();
  const int r = api.ssl_read(ssl_, out, static_cast<int>(std::min<std::size_t>(n, 1u << 30)));
  if (r <= 0) {
    const int err = api.ssl_get_error(ssl_, r);
    if (err == kSslErrorZeroReturn) return 0;  // fechamento ordenado pelo peer
    if (err == kSslErrorSyscall && errno != 0) {
      die("falha ao ler dados: " + std::string(std::strerror(errno)));
    }
    if (err == kSslErrorSsl) die("falha ao ler dados: " + erro_openssl(api));
    die("falha ao ler dados (codigo SSL " + std::to_string(err) + ")");
  }
  return static_cast<std::size_t>(r);
}

}  // namespace tilt::rt
