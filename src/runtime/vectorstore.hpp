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
  // Busca hibrida: funde (RRF) o ranking vetorial com o de palavras-chave
  // (BM25 sobre o texto). `score` do hit e o score fundido, nao um cosseno.
  std::vector<Hit> search_hibrido(const std::string& consulta, const std::vector<float>& query,
                                  std::size_t k) const;
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

// BM25 (k1 = 1.2, b = 0.75) de cada documento contra a consulta. Tokens: letras
// e digitos em minusculas (bytes UTF-8 >= 0x80 contam como letra). Devolve um
// score por documento (0 se nao ha termo em comum).
std::vector<double> bm25_scores(const std::string& consulta, const std::vector<std::string>& docs);

// Fusao por ranking reciproco (RRF, k = 60) de dois scores por item (maior =
// melhor): 1/(60 + posicao_a) + 1/(60 + posicao_b), com posicao a partir de 1.
std::vector<double> fusao_rrf(const std::vector<double>& a, const std::vector<double>& b);

}  // namespace tilt::rt
