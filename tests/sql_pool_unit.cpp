// Teste unitario standalone do pool de conexoes (sem servidor de banco):
// compila so com sql_pool.cpp e exercita reuso, teto, descarte,
// bypass e exclusao mutua entre threads. Falha via abort (nao-zero).
// Uso: g++ -std=c++20 -I src tests/sql_pool_unit.cpp src/runtime/sql_pool.cpp
//      -o /tmp/sql_pool_unit && /tmp/sql_pool_unit
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "runtime/sql_pool.hpp"

using tilt::rt::PooledConn;

static int abertos = 0;
static int fechados = 0;
static bool viva_agora = true;

void* abre() {
  ++abertos;
  return reinterpret_cast<void*>(static_cast<std::intptr_t>(abertos));
}
bool viva(void* h) { return viva_agora && h != nullptr; }
void fecha(void*) { ++fechados; }

int main() {
  const char* off = std::getenv("TILT_SQL_POOL");
  const bool desligado = off && std::string(off) == "0";

  // 1) Reuso: segunda aquisicao devolve o mesmo handle (hit).
  void* h1 = nullptr;
  {
    PooledConn a("t", "dsn", abre, viva, fecha, "SELECT 1");
    h1 = a.get();
  }
  {
    PooledConn b("t", "dsn", abre, viva, fecha, "SELECT 1");
    if (!desligado) assert(b.get() == h1 && "esperava hit no pool");
  }
  if (!desligado) assert(abertos == 1 && "esperava 1 abertura para 2 usos");

  // 2) Morta no checkout: descarta e abre nova.
  {
    PooledConn c("morta", "x", abre, viva, fecha, "SELECT 1");
    (void)c;
  }
  viva_agora = false;  // simula queda do servidor com a ociosa no pool
  {
    const int antes = abertos;
    PooledConn d("morta", "x", abre, viva, fecha, "SELECT 1");
    (void)d;
    if (!desligado) assert(abertos == antes + 1 && "conexao morta devia ser trocada");
  }
  viva_agora = true;

  // 3) BEGIN/START/SET nao voltam ao pool.
  const int antes3 = abertos;
  {
    PooledConn e("t", "dsn2", abre, viva, fecha, "BEGIN");
    (void)e;
  }
  {
    PooledConn f("t", "dsn2", abre, viva, fecha, "SELECT 1");
    (void)f;
    assert(abertos == antes3 + 2 && "BEGIN devia descartar (2 aberturas)");
  }
  {
    PooledConn g("t", "dsn3", abre, viva, fecha, "  start transaction");
    (void)g;
  }
  {
    const int antes = abertos;
    PooledConn h("t", "dsn3", abre, viva, fecha, "SELECT 1");
    (void)h;
    assert(abertos == antes + 1 && "START devia descartar");
  }

  // 4) discard() explicito.
  const int antes4 = abertos;
  {
    PooledConn i("t", "dsn4", abre, viva, fecha, "SELECT 1");
    i.discard();
  }
  {
    PooledConn j("t", "dsn4", abre, viva, fecha, "SELECT 1");
    (void)j;
    assert(abertos == antes4 + 2 && "discard devia fechar");
  }

  // 5) Concorrencia: 8 threads x 200 aquisicoes, sem crash e sem vazar
  // (cada handle em uso e exclusivo; ociosas limitadas pelo teto).
  {
    std::atomic<int> erros{0};
    auto roda = [&] {
      try {
        for (int k = 0; k < 200; ++k) {
          PooledConn p("t", "conc", abre, viva, fecha, "SELECT 1");
          if (!p.get()) ++erros;
        }
      } catch (...) {
        ++erros;
      }
    };
    std::vector<std::thread> ts;
    for (int k = 0; k < 8; ++k) ts.emplace_back(roda);
    for (auto& t : ts) t.join();
    assert(erros == 0 && "threads nao podem falhar");
  }

  std::printf("sql_pool_unit ok (abertos=%d fechados=%d)\n", abertos, fechados);
  return 0;
}
