#include <cassert>
#include <iostream>

#include "runtime/value.hpp"

static_assert(sizeof(tilt::rt::Value) <= 64,
              "Value deve manter o armazenamento complexo fora do objeto principal");

int main() {
  using tilt::rt::Value;
  using tilt::rt::ValueKind;

  // O teste também cobre que a consolidação preserva os construtores e os
  // acessos usados pelos valores heap-backed.
  Value mapa = Value::mapa();
  mapa.map_ref()->set("n", Value::inteiro(7));
  assert(mapa.map_ref()->find("n") != nullptr);

  Value lista = Value::lista({Value::inteiro(1), Value::inteiro(2)});
  assert(lista.list_ref() && lista.list_ref()->size() == 2);

  Value escalar = Value::inteiro(42);
  assert(escalar.kind == ValueKind::Inteiro);
  assert(!escalar.storage);

  Value curto = Value::texto("tilt sso");
  assert(curto.s.size() == 8);
  assert(curto.s == "tilt sso");
  Value longo = Value::texto("texto suficientemente longo para sair do SSO");
  assert(longo.s.size() > 22);
  assert(longo.s.find("SSO") != std::string::npos);
  Value copia = longo;
  assert(copia.s == longo.s);
  longo.s.secure_clear();
  assert(longo.s.empty());
  assert(!copia.s.empty());

  std::cout << sizeof(Value) << "\n";
  return 0;
}
