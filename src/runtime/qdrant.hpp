#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace tilt::rt {

// Indice vetorial remoto no Qdrant (REST via curl, mesmo padrao do llm.cpp).
// O ID tilt (texto) e mapeado para UUID deterministico, pois o Qdrant so
// aceita inteiros ou UUID como id de ponto.
void qdrant_upsert(const std::string& base, const std::string& collection,
                   const std::string& id, const std::string& text,
                   const std::vector<float>& vec);
std::vector<std::pair<std::string, double>> qdrant_search(const std::string& base,
                                                          const std::string& collection,
                                                          const std::vector<float>& vec,
                                                          std::size_t k);

}  // namespace tilt::rt
