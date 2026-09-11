#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace tilt::rt {

// Indice vetorial remoto no Weaviate (REST via curl, mesmo padrao do
// qdrant.cpp). A classe e criada automaticamente na primeira escrita
// (vectorizer "none", propriedade "texto"); a busca e GraphQL nearVector
// (distancia de cosseno) e devolve score = 1 - distance.
void weaviate_upsert(const std::string& base, const std::string& classe,
                     const std::string& id, const std::string& text,
                     const std::vector<float>& vec);
std::vector<std::pair<std::string, double>> weaviate_search(const std::string& base,
                                                            const std::string& classe,
                                                            const std::vector<float>& vec,
                                                            std::size_t k);

}  // namespace tilt::rt
