#include "runtime/vectorstore.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <numeric>
#include <unordered_map>
#include <utility>

namespace tilt::rt {

float cosine(const std::vector<float>& a, const std::vector<float>& b) {
  const std::size_t n = std::min(a.size(), b.size());
  float dot = 0.0F;
  float na = 0.0F;
  float nb = 0.0F;
  for (std::size_t i = 0; i < n; ++i) {
    dot += a[i] * b[i];
    na += a[i] * a[i];
    nb += b[i] * b[i];
  }
  if (na == 0.0F || nb == 0.0F) return 0.0F;
  return dot / (std::sqrt(na) * std::sqrt(nb));
}

void MemoryIndex::insert(std::string id, std::string text, std::vector<float> vec) {
  entries_.push_back({std::move(id), std::move(text), std::move(vec)});
}

std::vector<MemoryIndex::Hit> MemoryIndex::search(const std::vector<float>& query,
                                                  std::size_t k) const {
  std::vector<Hit> hits;
  hits.reserve(entries_.size());
  for (const Entry& e : entries_) {
    hits.push_back({e.id, e.text, cosine(query, e.vec)});
  }
  std::stable_sort(hits.begin(), hits.end(),
                   [](const Hit& x, const Hit& y) { return x.score > y.score; });
  if (hits.size() > k) hits.resize(k);
  return hits;
}

namespace {

std::vector<std::string> tokens_de(const std::string& s) {
  std::vector<std::string> out;
  std::string atual;
  for (unsigned char c : s) {
    if (std::isalnum(c) || c >= 0x80) {
      atual += static_cast<char>(c < 0x80 ? std::tolower(c) : c);
    } else if (!atual.empty()) {
      out.push_back(std::move(atual));
      atual.clear();
    }
  }
  if (!atual.empty()) out.push_back(std::move(atual));
  return out;
}

// Posicao (1 = melhor) de cada item por score decrescente; empates mantem a
// ordem original.
std::vector<std::size_t> posicoes(const std::vector<double>& scores) {
  std::vector<std::size_t> ordem(scores.size());
  std::iota(ordem.begin(), ordem.end(), 0);
  std::stable_sort(ordem.begin(), ordem.end(),
                   [&](std::size_t x, std::size_t y) { return scores[x] > scores[y]; });
  std::vector<std::size_t> pos(scores.size());
  for (std::size_t r = 0; r < ordem.size(); ++r) pos[ordem[r]] = r + 1;
  return pos;
}

}  // namespace

std::vector<double> bm25_scores(const std::string& consulta, const std::vector<std::string>& docs) {
  constexpr double kK1 = 1.2;
  constexpr double kB = 0.75;
  const std::size_t n = docs.size();
  std::vector<double> scores(n, 0.0);
  if (n == 0) return scores;
  std::vector<std::unordered_map<std::string, int>> freq(n);
  std::unordered_map<std::string, int> df;
  double total = 0.0;
  std::vector<double> tam(n, 0.0);
  for (std::size_t i = 0; i < n; ++i) {
    const std::vector<std::string> toks = tokens_de(docs[i]);
    tam[i] = static_cast<double>(toks.size());
    total += tam[i];
    for (const std::string& t : toks) ++freq[i][t];
    for (const auto& kv : freq[i]) ++df[kv.first];
  }
  const double media = total / static_cast<double>(n);
  std::vector<std::string> termos = tokens_de(consulta);
  std::sort(termos.begin(), termos.end());
  termos.erase(std::unique(termos.begin(), termos.end()), termos.end());
  for (const std::string& termo : termos) {
    const auto d = df.find(termo);
    if (d == df.end()) continue;
    const double idf =
        std::log(1.0 + (static_cast<double>(n) - d->second + 0.5) / (d->second + 0.5));
    for (std::size_t i = 0; i < n; ++i) {
      const auto f = freq[i].find(termo);
      if (f == freq[i].end()) continue;
      const double tf = f->second;
      const double norma = kK1 * (1.0 - kB + kB * (media > 0 ? tam[i] / media : 1.0));
      scores[i] += idf * (tf * (kK1 + 1.0)) / (tf + norma);
    }
  }
  return scores;
}

std::vector<double> fusao_rrf(const std::vector<double>& a, const std::vector<double>& b) {
  const std::vector<std::size_t> pa = posicoes(a);
  const std::vector<std::size_t> pb = posicoes(b);
  std::vector<double> out(a.size(), 0.0);
  for (std::size_t i = 0; i < a.size(); ++i) {
    out[i] = 1.0 / (60.0 + static_cast<double>(pa[i])) +
             1.0 / (60.0 + static_cast<double>(i < pb.size() ? pb[i] : pa[i]));
  }
  return out;
}

std::vector<MemoryIndex::Hit> MemoryIndex::search_hibrido(const std::string& consulta,
                                                          const std::vector<float>& query,
                                                          std::size_t k) const {
  std::vector<double> vetorial;
  std::vector<std::string> textos;
  vetorial.reserve(entries_.size());
  textos.reserve(entries_.size());
  for (const Entry& e : entries_) {
    vetorial.push_back(static_cast<double>(cosine(query, e.vec)));
    textos.push_back(e.text);
  }
  const std::vector<double> fundido = fusao_rrf(vetorial, bm25_scores(consulta, textos));
  std::vector<Hit> hits;
  hits.reserve(entries_.size());
  for (std::size_t i = 0; i < entries_.size(); ++i) {
    hits.push_back({entries_[i].id, entries_[i].text, static_cast<float>(fundido[i])});
  }
  std::stable_sort(hits.begin(), hits.end(),
                   [](const Hit& x, const Hit& y) { return x.score > y.score; });
  if (hits.size() > k) hits.resize(k);
  return hits;
}

}  // namespace tilt::rt
