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
bool upload(void*, const float*, std::size_t, void*&) { return false; }
bool download(void*, const void*, float*, std::size_t) { return false; }
bool release_buffer(void*, void*) { return false; }
bool gemm_resident(void*, const void*, const void*, void*&, int, int, int) { return false; }

}  // namespace tilt::rt::metal
