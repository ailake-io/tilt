#include "runtime/tensor.hpp"

#include <cstdio>

int main() {
  float* released = nullptr;
  {
    tilt::rt::Tensor first = tilt::rt::Tensor::zeros({32});
    released = first.data.data();
    first.data[0] = 42.0F;
  }

  tilt::rt::Tensor reused = tilt::rt::Tensor::zeros({32});
  if (reused.data.data() != released) {
    std::fprintf(stderr, "tensor pool did not reuse the exact-size buffer\n");
    return 1;
  }
  if (reused.data[0] != 0.0F) {
    std::fprintf(stderr, "Tensor::zeros exposed stale pooled contents\n");
    return 1;
  }

  std::puts("tensor_pool_unit ok");
  return 0;
}
