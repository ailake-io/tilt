#pragma once

#include <cstddef>

namespace tilt::rt::metal {

// A implementação Objective-C++ é compilada somente no macOS. O stub C++
// preserva o mesmo ABI nas demais plataformas e retorna indisponível.
void* create();
void destroy(void* state);
bool gemm(void* state, const float* a, const float* b, float* c, int m, int k, int n);
bool relu(void* state, float* data, std::size_t n);
bool gelu(void* state, float* data, std::size_t n);
bool add(void* state, const float* a, const float* b, float* c, std::size_t n);
bool conv2d(void* state, const float* x, const float* weights, float* y, int batch,
            int in_channels, int height, int width, int out_channels, int kh, int kw,
            int out_h, int out_w, int stride, int padding, int dilation);
bool upload(void* state, const float* data, std::size_t bytes, void*& handle);
bool download(void* state, const void* handle, float* data, std::size_t bytes);
bool release_buffer(void* state, void* handle);
bool gemm_resident(void* state, const void* a, const void* b, void*& c,
                   int m, int k, int n);

}  // namespace tilt::rt::metal
