#pragma once

#include <functional>
#include <string>

namespace tilt::rt {

// Pool de conexoes para bancos relacionais (postgres, mysql, duckdb).
//
// Uso: uma chamada = um PooledConn no escopo. O handle e exclusivo do dono;
// o destructor devolve ao pool (idle, limitado) ou fecha. Conexoes quebradas
// sao descartadas na proxima aquisicao (validacao `alive`) — o caminho de
// erro existente (die/throw) nao precisa mudar.
//
// Contrato (1a passada):
// - So para statements avulsos. `transacao` usa conexao dedicada (fora).
// - Statement que abre estado de sessao/transacao (BEGIN/START/SET no inicio
//   do SQL, case-insensitive) nao volta ao pool: e fechado no release.
// - sqlite fica fora (open de arquivo e barato; risco de lock/estado nao
//   compensa) e clickhouse tambem (HTTP sem conexao persistente).
//
// Backend vazio ("") = bypass total: abre no ctor, fecha no dtor, sem
// registro (para `transacao`, que exige conexao dedicada).
//
// Env: TILT_SQL_POOL=0 desliga (abre/fecha por chamada, como antes);
// TILT_SQL_POOL_MAX limita ociosas por chave (default 8);
// TILT_SQL_POOL_DEBUG=1 loga hit/miss/discard no stderr (sem DSN, que pode
// ter senha).
class PooledConn {
 public:
  using OpenFn = std::function<void*()>;        // abre ou lanca (die)
  using AliveFn = std::function<bool(void*)>;   // barato, nunca lanca
  using CloseFn = std::function<void(void*)>;   // fecha, nunca lanca
  PooledConn(std::string backend, std::string dsn, OpenFn open, AliveFn alive, CloseFn close,
             const std::string& sql_hint = {});
  ~PooledConn();
  PooledConn(const PooledConn&) = delete;
  PooledConn& operator=(const PooledConn&) = delete;
  void* get() const { return handle_; }
  // Marca como suja (fecha em vez de devolver): use se o statement deixou
  // estado na sessao alem do coberto pelo sql_hint.
  void discard() { pool_back_ = false; }

 private:
  std::string backend_;
  std::string key_;
  CloseFn close_;
  void* handle_ = nullptr;
  bool pool_back_ = true;
};

// Fecha todas as conexoes ociosas do pool. Chame no fim do main, com as
// bibliotecas de cliente (libpq, libmysqlclient) ainda carregadas: o destrutor
// estatico do registro nao pode fazer isso com seguranca (ordem de teardown
// indefinida), e sem esta chamada as ociosas aparecem como vazamento no
// LeakSanitizer.
void sql_pool_fechar_ociosas();

}  // namespace tilt::rt
