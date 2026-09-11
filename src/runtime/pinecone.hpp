#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace tilt::rt {

// Indice vetorial hospedado no Pinecone (REST via curl sobre HTTPS, mesmo
// padrao do weaviate.cpp). Data plane apenas: o indice ja deve existir na
// conta (criar indice e control plane, fora de escopo). Auth obrigatoria por
// env PINECONE_API_KEY (header Api-Key). Upsert: POST /vectors/upsert
// {namespace, vectors: [{id, values, metadata: {texto}}]}; busca:
// POST /query {namespace, vector, topK} -> matches[] com id/score (o score do
// Pinecone ja e similaridade de cosseno: quanto maior, melhor).
void pinecone_upsert(const std::string& base, const std::string& ns,
                     const std::string& id, const std::string& text,
                     const std::vector<float>& vec);
std::vector<std::pair<std::string, double>> pinecone_search(const std::string& base,
                                                            const std::string& ns,
                                                            const std::vector<float>& vec,
                                                            std::size_t k);

}  // namespace tilt::rt
