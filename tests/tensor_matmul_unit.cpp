#include "runtime/tensor.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <tuple>
#include <vector>

int main() {
  using tilt::rt::Tensor;
  const std::vector<std::tuple<int, int, int>> cases = {
      {1, 1, 1}, {3, 5, 7}, {8, 33, 37}, {64, 64, 64}, {128, 128, 128}};
  for (const auto& [m, k, n] : cases) {
    Tensor a = Tensor::zeros({m, k});
    Tensor b = Tensor::zeros({k, n});
    for (std::size_t i = 0; i < a.data.size(); ++i)
      a.data[i] = static_cast<float>(static_cast<int>(i % 17) - 8) * 0.0625F;
    for (std::size_t i = 0; i < b.data.size(); ++i)
      b.data[i] = static_cast<float>(static_cast<int>(i % 23) - 11) * 0.03125F;
    const Tensor c = tilt::rt::matmul(a, b);
    if (c.shape != std::vector<std::int64_t>{m, n}) return 1;
    for (int row = 0; row < m; ++row) {
      for (int col = 0; col < n; ++col) {
        float expected = 0.0F;
        for (int p = 0; p < k; ++p)
          expected += a.data[static_cast<std::size_t>(row * k + p)] *
                      b.data[static_cast<std::size_t>(p * n + col)];
        const float got = c.data[static_cast<std::size_t>(row * n + col)];
        if (std::abs(got - expected) > 1e-4F * std::max(1.0F, std::abs(expected))) {
          std::cerr << "matmul " << m << "x" << k << "x" << n << " @ " << row << "," << col
                    << ": esperado " << expected << ", obtido " << got << "\n";
          return 2;
        }
      }
    }
  }
  return 0;
}
