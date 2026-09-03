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

float cosine(const std::vector<float>& a, const std::vector<float>& b);

}  // namespace tilt::rt
