#include "runtime/vectorstore.hpp"

#include <algorithm>
#include <cmath>
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

}  // namespace tilt::rt
