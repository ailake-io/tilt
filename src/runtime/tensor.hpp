#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace tilt::rt {

// Dense row-major f32 tensor. Naive scalar kernels (SIMD/threads: M6.2).
// Shape-mismatched operations throw std::runtime_error.
struct Tensor {
  std::vector<std::int64_t> shape;
  std::vector<float> data;

  std::int64_t size() const;
  std::int64_t rank() const { return static_cast<std::int64_t>(shape.size()); }
  std::string shape_str() const;  // e.g. "f32, 2, 3"

  static Tensor filled(std::vector<std::int64_t> shape, float value);
  static Tensor zeros(std::vector<std::int64_t> shape) { return filled(std::move(shape), 0.0F); }
  static Tensor ones(std::vector<std::int64_t> shape) { return filled(std::move(shape), 1.0F); }
  // Deterministic Xavier-uniform fill seeded by `seed`.
  static Tensor xavier(std::vector<std::int64_t> shape, std::int64_t fan_in, std::int64_t fan_out,
                       std::uint64_t seed);
};

Tensor add(const Tensor& a, const Tensor& b);  // supports scalar and last-dim bias broadcast
Tensor sub(const Tensor& a, const Tensor& b);
Tensor mul(const Tensor& a, const Tensor& b);
Tensor div(const Tensor& a, const Tensor& b);
Tensor scalar_op(const Tensor& a, float s, char op);  // op in {+,-,*,/}

Tensor matmul(const Tensor& a, const Tensor& b);
Tensor transpose2d(const Tensor& a);
Tensor reshape(const Tensor& a, std::vector<std::int64_t> shape);

Tensor apply_unary(const Tensor& a, const std::string& fn);  // relu/gelu/silu/sigmoide/tanh
Tensor softmax_last(const Tensor& a);

float sum_all(const Tensor& a);
float mean_all(const Tensor& a);
std::int64_t argmax_last(const Tensor& a);  // index of max in the last dim (rank-1 tensor)

}  // namespace tilt::rt
