#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace tilt::rt {

// Indice vetorial remoto em Postgres + pgvector (SQL montado sobre o
// conector libpq ja existente). A tabela <tabela> é criada automaticamente
// na primeira escrita: (id TEXT PRIMARY KEY, texto TEXT, embedding vector(N)).
// Distancia de cosseno via operador <=> do pgvector.
void pgvector_upsert(const std::string& url, const std::string& tabela, const std::string& id,
                     const std::string& text, const std::vector<float>& vec);
std::vector<std::pair<std::string, double>> pgvector_search(const std::string& url,
                                                            const std::string& tabela,
                                                            const std::vector<float>& vec,
                                                            std::size_t k);

}  // namespace tilt::rt
