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
bool normalize(void* state, const float* input, const float* mean, const float* variance,
              const float* gamma, const float* beta, float* output, int batches,
              int channels, int spatial, float epsilon);
bool maxpool2d(void* state, const float* input, float* output, int batches, int channels,
               int height, int width, int window, int stride);
bool reduce_sum(void* state, const float* input, float* output, std::size_t count);
bool upload(void* state, const float* data, std::size_t bytes, void*& handle);
bool download(void* state, const void* handle, float* data, std::size_t bytes);
bool release_buffer(void* state, void* handle);
bool gemm_resident(void* state, const void* a, const void* b, void*& c,
                   int m, int k, int n);
bool batch_gemm_resident(void* state, const void* a, const void* b, void*& c,
                         int batches, int m, int k, int n);
bool conv2d_resident(void* state, const void* x, const void* weights, void*& y, int batch,
                     int in_channels, int height, int width, int out_channels, int kh, int kw,
                     int out_h, int out_w, int stride, int padding, int dilation);
bool relu_resident(void* state, void* data, std::size_t n);
bool gelu_resident(void* state, void* data, std::size_t n);
bool add_resident(void* state, const void* a, const void* b, void*& c, std::size_t n);
bool add_channel_bias_resident(void* state, const void* input, const void* bias, void*& output,
                               int batches, int channels, int spatial);
bool normalize_resident(void* state, const void* input, const void* mean,
                        const void* variance, const void* gamma, const void* beta,
                        void*& output, int batches, int channels, int spatial, float epsilon);
bool maxpool2d_resident(void* state, const void* input, void*& output, int batches,
                        int channels, int height, int width, int window, int stride);
bool reduce_sum_resident(void* state, const void* input, void*& output, std::size_t count);

}  // namespace tilt::rt::metal
