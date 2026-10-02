#include "runtime/metal_runtime.hpp"

namespace tilt::rt::metal {

void* create() { return nullptr; }
void destroy(void*) {}
bool gemm(void*, const float*, const float*, float*, int, int, int) { return false; }
bool relu(void*, float*, std::size_t) { return false; }
bool gelu(void*, float*, std::size_t) { return false; }
bool add(void*, const float*, const float*, float*, std::size_t) { return false; }
bool conv2d(void*, const float*, const float*, float*, int, int, int, int, int, int, int,
            int, int, int, int, int) { return false; }
bool normalize(void*, const float*, const float*, const float*, const float*, const float*, float*, int, int, int, float) { return false; }
bool maxpool2d(void*, const float*, float*, int, int, int, int, int, int) { return false; }
bool reduce_sum(void*, const float*, float*, std::size_t) { return false; }
bool upload(void*, const float*, std::size_t, void*&) { return false; }
bool download(void*, const void*, float*, std::size_t) { return false; }
bool release_buffer(void*, void*) { return false; }
bool gemm_resident(void*, const void*, const void*, void*&, int, int, int) { return false; }
bool batch_gemm_resident(void*, const void*, const void*, void*&, int, int, int, int) { return false; }
bool conv2d_resident(void*, const void*, const void*, void*&, int, int, int, int, int, int, int,
                     int, int, int, int, int) { return false; }
bool relu_resident(void*, void*, std::size_t) { return false; }
bool gelu_resident(void*, void*, std::size_t) { return false; }
bool add_resident(void*, const void*, const void*, void*&, std::size_t) { return false; }
bool add_channel_bias_resident(void*, const void*, const void*, void*&, int, int, int) { return false; }
bool normalize_resident(void*, const void*, const void*, const void*, const void*, const void*,
                        void*&, int, int, int, float) { return false; }
bool maxpool2d_resident(void*, const void*, void*&, int, int, int, int, int, int) { return false; }
bool reduce_sum_resident(void*, const void*, void*&, std::size_t) { return false; }

}  // namespace tilt::rt::metal
