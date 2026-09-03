#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace tilt::rt {

// Linear allocator with instant reset: bump `offset` on alloc, zero out on
// reset. Owns a fixed-capacity buffer; alocar returns nullptr when full.
// Typical use: one arena per request — alloc scratch during handling, reset
// when the response goes out.
class TiltArena {
 public:
  explicit TiltArena(std::size_t capacidade) : buffer_(capacidade) {}

  TiltArena(const TiltArena&) = delete;
  TiltArena& operator=(const TiltArena&) = delete;

  void* alocar(std::size_t tamanho, std::size_t alinhamento = alignof(std::max_align_t)) {
    if (alinhamento == 0) alinhamento = 1;
    const std::size_t mask = alinhamento - 1;
    const std::size_t aligned = (offset_ + mask) & ~mask;
    if (aligned + tamanho > buffer_.size()) return nullptr;
    offset_ = aligned + tamanho;
    return buffer_.data() + aligned;
  }

  void resetar() { offset_ = 0; }

  std::size_t usado() const { return offset_; }
  std::size_t capacidade() const { return buffer_.size(); }

 private:
  std::vector<char> buffer_;
  std::size_t offset_ = 0;
};

}  // namespace tilt::rt
