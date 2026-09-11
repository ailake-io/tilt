#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace tilt::rt {

// Indice vetorial no Chroma (REST via curl, HTTP puro, mesmo padrao do
// weaviate.cpp). Chroma open-source padrao: sem auth. A colecao e
// get-or-create (POST /api/v1/collections {name, get_or_create}) e o id
// devolvido endereca add/query. A query devolve distances (hnsw:space
// cosine: distance = 1 - cosseno); o score tilt e 1 - distance para ficar
// consistente com os demais backends (maior = melhor).
void chroma_upsert(const std::string& base, const std::string& colecao,
                   const std::string& id, const std::string& text,
                   const std::vector<float>& vec);
std::vector<std::pair<std::string, double>> chroma_search(const std::string& base,
                                                          const std::string& colecao,
                                                          const std::vector<float>& vec,
                                                          std::size_t k);

}  // namespace tilt::rt
