// Sorteio e embaralhamento deterministicos entre plataformas.
//
// std::mt19937/std::mt19937_64 produzem a mesma sequencia em toda libstdc++/
// libc++/MSVC, mas std::shuffle e std::uniform_int_distribution nao: o padrao
// deixa o algoritmo a criterio da biblioteca. Como `semente:` da Tilt promete
// resultados reproduziveis (e os testes de ouro dependem disso), o codigo que
// usa semente deve chamar estes helpers em vez das versoes da std.
#pragma once

#include <cstddef>
#include <cstdint>
#include <iterator>

namespace tilt {

// Inteiro uniforme em [0, n) sem vies (amostragem por rejeicao). Requer n > 0
// e n - 1 <= Rng::max() - Rng::min().
template <class Rng>
std::uint64_t sortear_indice(Rng& rng, std::uint64_t n) {
  const std::uint64_t faixa = static_cast<std::uint64_t>(Rng::max()) -
                              static_cast<std::uint64_t>(Rng::min());
  // (faixa + 1) % n valores no topo da faixa seriam desiguais: rejeita-os.
  const std::uint64_t excesso = (faixa % n + 1) % n;
  const std::uint64_t limite = faixa - excesso;
  std::uint64_t v;
  do {
    v = static_cast<std::uint64_t>(rng()) - static_cast<std::uint64_t>(Rng::min());
  } while (v > limite);
  return v % n;
}

// Fisher-Yates de tras para frente; equivalente a std::shuffle, mas com
// resultado identico em qualquer biblioteca padrao.
template <class It, class Rng>
void embaralhar_portavel(It first, It last, Rng& rng) {
  const auto n = static_cast<std::uint64_t>(std::distance(first, last));
  for (std::uint64_t i = n; i > 1; --i) {
    const std::uint64_t j = sortear_indice(rng, i);
    std::iter_swap(first + static_cast<std::ptrdiff_t>(i - 1),
                   first + static_cast<std::ptrdiff_t>(j));
  }
}

}  // namespace tilt
