#include "runtime/tensor.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iostream>

int main() {
  using tilt::rt::Tensor;
  // Large enough to select im2col + CBLAS when an optimized provider is
  // present; padding and dilation exercise the coordinate mapping.
  Tensor x = Tensor::zeros({1, 8, 48, 48});
  Tensor k = Tensor::zeros({32, 8, 3, 3});
  for (std::size_t i = 0; i < x.data.size(); ++i)
    x.data[i] = static_cast<float>(static_cast<int>(i % 19) - 9) * 0.01F;
  for (std::size_t i = 0; i < k.data.size(); ++i)
    k.data[i] = static_cast<float>(static_cast<int>(i % 13) - 6) * 0.005F;
  const Tensor got = tilt::rt::conv2d(x, k, 1, 2, 2);
  if (got.shape != (std::vector<std::int64_t>{1, 32, 48, 48})) return 1;
  for (int co = 0; co < 32; ++co) {
    for (int yy = 0; yy < 48; ++yy) {
      for (int xx = 0; xx < 48; ++xx) {
        float expected = 0.0F;
        for (int ci = 0; ci < 8; ++ci) {
          for (int u = 0; u < 3; ++u) {
            for (int v = 0; v < 3; ++v) {
              const int h = yy + u * 2 - 2;
              const int w = xx + v * 2 - 2;
              if (h >= 0 && h < 48 && w >= 0 && w < 48)
                expected += x.data[static_cast<std::size_t>((ci * 48 + h) * 48 + w)] *
                            k.data[static_cast<std::size_t>(((co * 8 + ci) * 3 + u) * 3 + v)];
            }
          }
        }
        const float actual = got.data[static_cast<std::size_t>((co * 48 + yy) * 48 + xx)];
        if (std::abs(actual - expected) > 1e-4F * std::max(1.0F, std::abs(expected))) {
          std::cerr << "conv2d " << co << "," << yy << "," << xx << ": "
                    << actual << " != " << expected << "\n";
          return 2;
        }
      }
    }
  }
  return 0;
}
