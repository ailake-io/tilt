#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace tilt::rt {

// In-memory vector index for RAG (`armazenamento: "memoria"`).
class MemoryIndex {
 public:
  struct Hit {
    std::string id;
    std::string text;
    float score = 0.0F;
  };

  void insert(std::string id, std::string text, std::vector<float> vec);
  std::vector<Hit> search(const std::vector<float>& query, std::size_t k) const;
  std::size_t size() const { return entries_.size(); }

 private:
  struct Entry {
    std::string id;
    std::string text;
    std::vector<float> vec;
  };
  std::vector<Entry> entries_;
};

// Resultado de `buscar` nos backends externos (qdrant, pgvector, weaviate,
// pinecone, chroma): id, similaridade (maior = melhor) e o texto guardado no
// `inserir` (vazio se o ponto foi gravado sem texto).
struct VectorHit {
  std::string id;
  double score = 0.0;
  std::string texto;
};

float cosine(const std::vector<float>& a, const std::vector<float>& b);

}  // namespace tilt::rt
