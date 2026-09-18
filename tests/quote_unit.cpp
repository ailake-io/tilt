// Teste unitario standalone do quoting de shell (sem SO especifico):
// tilt_posix_quote e tilt_win_quote sao funcoes puras, testaveis no Linux.
// Uso: g++ -std=c++20 -I src tests/quote_unit.cpp src/runtime/compat.cpp
//      -o /tmp/quote_unit && /tmp/quote_unit
//
// A regra Win espelha Python list2cmdline (CommandLineToArgvW do curl MSVC).
// Casos verificados a mao contra a semantica do cmd.exe.
#include <cassert>
#include <cstdio>
#include <string>

#include "runtime/compat.hpp"

using tilt::rt::tilt_posix_quote;
using tilt::rt::tilt_win_quote;

void eq(const std::string& got, const std::string& want, const char* what) {
  if (got != want) {
    std::printf("FALHA %s:\n  obtido   %s\n  esperado %s\n", what, got.c_str(), want.c_str());
    std::abort();
  }
}

int main() {
  // POSIX: aspas simples, ' vira '\''.
  eq(tilt_posix_quote("abc"), "'abc'", "posix simples");
  eq(tilt_posix_quote(""), "''", "posix vazio");
  eq(tilt_posix_quote("it's"), "'it'\\''s'", "posix apóstrofo");
  eq(tilt_posix_quote("a b&c"), "'a b&c'", "posix especiais passam");
  eq(tilt_posix_quote("%{http_code}"), "'%{http_code}'", "posix formato curl");

  // Win: sem metachars, volta puro.
  eq(tilt_win_quote("abc"), "abc", "win puro");
  eq(tilt_win_quote("https://x/y?a=b"), "https://x/y?a=b", "win url simples");
  eq(tilt_win_quote(""), "\"\"", "win vazio");
  // Espaco agrupa.
  eq(tilt_win_quote("a b"), "\"a b\"", "win espaco");
  eq(tilt_win_quote("C:\\Temp\\arq x.json"), "\"C:\\Temp\\arq x.json\"", "win path espaco");
  // Aspas internas escapam com \ dobrada (CommandLineToArgvW).
  eq(tilt_win_quote("a\"b"), "\"a\\\"b\"", "win aspa");
  eq(tilt_win_quote("a\\\"b"), "\"a\\\\\\\"b\"", "win barra+aspa");
  // Sem metachars, volta pura mesmo com barra final (cmd nao trata \ fora
  // de aspas; so dobra quando ha aspas — caso abaixo).
  eq(tilt_win_quote("C:\\Temp\\"), "C:\\Temp\\", "win barra final pura");
  eq(tilt_win_quote("C:\\Temp dir\\"), "\"C:\\Temp dir\\\\\"", "win espaco+barra final");
  // & | < > ^ agrupam (senao o cmd interpreta).
  eq(tilt_win_quote("a&b"), "\"a&b\"", "win ecomercial");
  eq(tilt_win_quote("a|b"), "\"a|b\"", "win pipe");
  eq(tilt_win_quote("--data @C:\\T\\a.json"), "\"--data @C:\\T\\a.json\"", "win data");
  // Formato -w do curl: sem par %...% valido, segue agrupado e intacto.
  eq(tilt_win_quote("%{http_code}"), "\"%{http_code}\"", "win formato curl");
  eq(tilt_win_quote("\n%{http_code}"), "\"\n%{http_code}\"", "win formato curl nl");

  std::printf("quote_unit ok\n");
  return 0;
}
