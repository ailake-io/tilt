#include "runtime/gpu_runtime.hpp"

#include "runtime/compat.hpp"
#include "runtime/metal_runtime.hpp"
#include "runtime/tensor.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <numeric>
#include <vector>

namespace tilt::rt {

namespace {

// Reference row-major SGEMM / ReLU used by the Fake backend and as the shape
// the CUDA kernel must match.
void cpu_gemm(const float* a, const float* b, float* c, int m, int k, int n) {
  for (int i = 0; i < m; ++i) {
    for (int j = 0; j < n; ++j) c[i * n + j] = 0.0F;
    for (int p = 0; p < k; ++p) {
      const float av = a[i * k + p];
      for (int j = 0; j < n; ++j) c[i * n + j] += av * b[p * n + j];
    }
  }
}

void cpu_relu(float* d, std::size_t n) {
  for (std::size_t i = 0; i < n; ++i) {
    if (d[i] < 0.0F) d[i] = 0.0F;
  }
}

void cpu_gelu(float* d, std::size_t n) {
  for (std::size_t i = 0; i < n; ++i) {
    const float v = d[i];
    d[i] = 0.5F * v * (1.0F + std::tanh(0.7978845608F * (v + 0.044715F * v * v * v)));
  }
}

void cpu_conv2d(const float* x, const float* w, float* y, int batch, int ci, int h, int width,
                int co, int kh, int kw, int oh, int ow, int stride, int pad, int dilation) {
  for (int b = 0; b < batch; ++b) for (int o = 0; o < co; ++o)
    for (int row = 0; row < oh; ++row) for (int col = 0; col < ow; ++col) {
      float acc = 0.0F;
      for (int c = 0; c < ci; ++c) for (int u = 0; u < kh; ++u) for (int v = 0; v < kw; ++v) {
        const int yy = row * stride + u * dilation - pad;
        const int xx = col * stride + v * dilation - pad;
        if (yy >= 0 && yy < h && xx >= 0 && xx < width)
          acc += x[((b * ci + c) * h + yy) * width + xx] *
                 w[((o * ci + c) * kh + u) * kw + v];
      }
      y[((b * co + o) * oh + row) * ow + col] = acc;
    }
}

bool cpu_embedding_backward(const float* indices, const float* grad, float* table,
                            int count, int vocab, int dim) {
  std::fill(table, table + static_cast<std::size_t>(vocab) * dim, 0.0F);
  for (int i = 0; i < count; ++i) {
    const float value = indices[i];
    const int row = static_cast<int>(std::llround(value));
    if (std::fabs(value - static_cast<float>(row)) > 1e-5F || row < 0 || row >= vocab) return false;
    for (int d = 0; d < dim; ++d) table[row * dim + d] += grad[i * dim + d];
  }
  return true;
}

void cpu_conv2d_backward(const float* x, const float* w, const float* grad, float* gx,
                         float* gw, float* gb, int batch, int ci, int h, int width, int co,
                         int kh, int kw, int oh, int ow, int stride, int pad, int dilation) {
  std::fill(gx, gx + static_cast<std::size_t>(batch) * ci * h * width, 0.0F);
  std::fill(gw, gw + static_cast<std::size_t>(co) * ci * kh * kw, 0.0F);
  std::fill(gb, gb + co, 0.0F);
  for (int b = 0; b < batch; ++b) for (int o = 0; o < co; ++o)
    for (int oy = 0; oy < oh; ++oy) for (int ox = 0; ox < ow; ++ox) {
      const float gy = grad[((b * co + o) * oh + oy) * ow + ox];
      gb[o] += gy;
      for (int c = 0; c < ci; ++c) for (int u = 0; u < kh; ++u) for (int v = 0; v < kw; ++v) {
        const int yy = oy * stride + u * dilation - pad, xx = ox * stride + v * dilation - pad;
        if (yy < 0 || yy >= h || xx < 0 || xx >= width) continue;
        const std::size_t xi = ((b * ci + c) * h + yy) * width + xx;
        const std::size_t wi = ((o * ci + c) * kh + u) * kw + v;
        gw[wi] += x[xi] * gy;
        gx[xi] += w[wi] * gy;
      }
    }
}

void cpu_recurrent_backward(const float* x, const float* w, const float* u, const float* b,
                            const float* hs, const float* cs, const float* gates, const float* gy,
                            float* gx, float* gw, float* gu, float* gb, int kind, int batch,
                            int time, int input_size, int hidden) {
  const RecurrentKind rk = kind == 0 ? RecurrentKind::Rnn
      : (kind == 1 ? RecurrentKind::Lstm : RecurrentKind::Gru);
  const int gate_count = kind == 0 ? 1 : (kind == 1 ? 4 : 3);
  Tensor tx = Tensor::zeros({batch, time, input_size});
  Tensor tw = Tensor::zeros({input_size, gate_count * hidden});
  Tensor tu = Tensor::zeros({hidden, gate_count * hidden});
  Tensor tb = Tensor::zeros({gate_count * hidden});
  Tensor tgy = Tensor::zeros({batch, hidden});
  std::copy(x, x + tx.data.size(), tx.data.begin());
  std::copy(w, w + tw.data.size(), tw.data.begin());
  std::copy(u, u + tu.data.size(), tu.data.begin());
  std::copy(b, b + tb.data.size(), tb.data.begin());
  std::copy(gy, gy + tgy.data.size(), tgy.data.begin());
  RecurrentCache cache;
  cache.kind = rk; cache.lote = batch; cache.tempo = time;
  cache.entrada = input_size; cache.oculta = hidden;
  cache.estados_h.assign(hs, hs + static_cast<std::size_t>(time + 1) * batch * hidden);
  if (kind == 1)
    cache.estados_c.assign(cs, cs + static_cast<std::size_t>(time + 1) * batch * hidden);
  if (kind != 0)
    cache.portas.assign(gates, gates + static_cast<std::size_t>(time) * batch * gate_count * hidden);
  Tensor tgx, tgw, tgu, tgb;
  recorrente_backward(tx, tw, tu, tb, rk, cache, tgy, tgx, tgw, tgu, tgb);
  std::copy(tgx.data.begin(), tgx.data.end(), gx);
  std::copy(tgw.data.begin(), tgw.data.end(), gw);
  std::copy(tgu.data.begin(), tgu.data.end(), gu);
  std::copy(tgb.data.begin(), tgb.data.end(), gb);
}

void cpu_normalize(const float* input, const float* mean, const float* variance,
                   const float* gamma, const float* beta, float* output, int batches,
                   int channels, int spatial, float epsilon) {
  for (int b = 0; b < batches; ++b) for (int c = 0; c < channels; ++c)
    for (int s = 0; s < spatial; ++s) {
      const std::size_t i = (static_cast<std::size_t>(b) * channels + c) * spatial + s;
      output[i] = (input[i] - mean[c]) / std::sqrt(variance[c] + epsilon) * gamma[c] + beta[c];
    }
}

void cpu_maxpool2d(const float* input, float* output, int batches, int channels, int height,
                   int width, int window, int stride) {
  const int out_h = height < window ? 0 : (height - window) / stride + 1;
  const int out_w = width < window ? 0 : (width - window) / stride + 1;
  for (int b = 0; b < batches; ++b) for (int c = 0; c < channels; ++c)
    for (int y = 0; y < out_h; ++y) for (int x = 0; x < out_w; ++x) {
      float best = -std::numeric_limits<float>::max();
      for (int dy = 0; dy < window; ++dy) for (int dx = 0; dx < window; ++dx)
        best = std::max(best, input[((b * channels + c) * height + y * stride + dy) * width + x * stride + dx]);
      output[((b * channels + c) * out_h + y) * out_w + x] = best;
    }
}

// IEEE-754 round-to-nearest-even. Keep the master weights and gradients f32;
// only GEMM operands are reduced to f16 in mixed precision.
std::uint16_t to_half(float value) {
  const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
  const std::uint16_t sign = static_cast<std::uint16_t>((bits >> 16) & 0x8000U);
  const int exponent = static_cast<int>((bits >> 23) & 0xffU) - 127 + 15;
  const std::uint32_t mantissa = bits & 0x7fffffU;
  if (exponent >= 31) return static_cast<std::uint16_t>(sign | 0x7c00U);
  if (exponent <= 0) {
    if (exponent < -10) return sign;
    const std::uint32_t full = mantissa | 0x800000U;
    const int shift = 14 - exponent;
    const std::uint32_t rounded = (full + ((1U << (shift - 1)) - 1U) + ((full >> shift) & 1U)) >> shift;
    return static_cast<std::uint16_t>(sign | rounded);
  }
  const std::uint32_t rounded = mantissa + 0xfffU + ((mantissa >> 13) & 1U);
  return static_cast<std::uint16_t>(sign | ((exponent + (rounded >> 23)) << 10) |
                                    ((rounded >> 13) & 0x3ffU));
}

float from_half(std::uint16_t value) {
  const std::uint32_t sign = (static_cast<std::uint32_t>(value) & 0x8000U) << 16;
  int exponent = (value >> 10) & 31;
  std::uint32_t mantissa = value & 0x3ffU;
  if (exponent == 0 && mantissa) {
    exponent = 1;
    while ((mantissa & 0x400U) == 0) { mantissa <<= 1; --exponent; }
    mantissa &= 0x3ffU;
  }
  const std::uint32_t bits = exponent == 31 ? sign | 0x7f800000U | (mantissa << 13)
      : exponent == 0 ? sign : sign | (static_cast<std::uint32_t>(exponent + 112) << 23) | (mantissa << 13);
  return std::bit_cast<float>(bits);
}

std::vector<std::uint16_t> half_operands(const float* input, std::size_t count) {
  std::vector<std::uint16_t> result(count);
  for (std::size_t i = 0; i < count; ++i) result[i] = to_half(input[i]);
  return result;
}

const char* kKernelSrc = R"cuda(
extern "C" __global__ void tilt_sgemm(const float* A, const float* B, float* C,
                                      int M, int K, int N) {
  int row = blockIdx.y * blockDim.y + threadIdx.y;
  int col = blockIdx.x * blockDim.x + threadIdx.x;
  if (row >= M || col >= N) return;
  float acc = 0.0f;
  for (int p = 0; p < K; ++p) acc += A[row * K + p] * B[p * N + col];
  C[row * N + col] = acc;
}
extern "C" __global__ void tilt_relu(float* D, int n) {
  int i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i < n && D[i] < 0.0f) D[i] = 0.0f;
}
extern "C" __global__ void tilt_gelu(float* D, int n) {
  int i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i < n) {
    float v = D[i];
    D[i] = 0.5f * v * (1.0f + tanhf(0.7978845608f * (v + 0.044715f * v * v * v)));
  }
}
extern "C" __global__ void tilt_add(const float* A, const float* B, float* C, int n) {
  int i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i < n) C[i] = A[i] + B[i];
}
extern "C" __global__ void tilt_conv2d(const float* X, const float* W, float* Y,
                                          int B, int CI, int H, int Width, int CO,
                                          int KH, int KW, int OH, int OW,
                                          int Stride, int Pad, int Dilation) {
  int i = blockIdx.x * blockDim.x + threadIdx.x;
  int total = B * CO * OH * OW;
  if (i >= total) return;
  int col = i % OW;
  int row = (i / OW) % OH;
  int o = (i / (OW * OH)) % CO;
  int b = i / (OW * OH * CO);
  float acc = 0.0f;
  for (int c = 0; c < CI; ++c) for (int u = 0; u < KH; ++u) for (int v = 0; v < KW; ++v) {
    int yy = row * Stride + u * Dilation - Pad;
    int xx = col * Stride + v * Dilation - Pad;
    if (yy >= 0 && yy < H && xx >= 0 && xx < Width)
      acc += X[((b * CI + c) * H + yy) * Width + xx] *
             W[((o * CI + c) * KH + u) * KW + v];
  }
  Y[i] = acc;
}
__device__ float tilt_half_to_float(unsigned short value) {
  unsigned int sign = ((unsigned int)value & 0x8000u) << 16;
  int exponent = (value >> 10) & 31;
  unsigned int mantissa = value & 0x3ffu;
  if (exponent == 0 && mantissa) {
    exponent = 1;
    while (!(mantissa & 0x400u)) { mantissa <<= 1; --exponent; }
    mantissa &= 0x3ffu;
  }
  unsigned int bits = exponent == 31 ? sign | 0x7f800000u | (mantissa << 13)
      : exponent == 0 ? sign : sign | ((unsigned int)(exponent + 112) << 23) | (mantissa << 13);
  return __uint_as_float(bits);
}
extern "C" __global__ void tilt_sgemm_mixed(const unsigned short* A, const unsigned short* B,
                                               float* C, int M, int K, int N) {
  int row = blockIdx.y * blockDim.y + threadIdx.y;
  int col = blockIdx.x * blockDim.x + threadIdx.x;
  if (row >= M || col >= N) return;
  float acc = 0.0f;
  for (int p = 0; p < K; ++p)
    acc += tilt_half_to_float(A[row * K + p]) * tilt_half_to_float(B[p * N + col]);
  C[row * N + col] = acc;
}
extern "C" __global__ void tilt_batch_gemm(const float* A, const float* B, float* C,
                                             int Batches, int M, int K, int N) {
  int col = blockIdx.x * blockDim.x + threadIdx.x;
  int row = blockIdx.y * blockDim.y + threadIdx.y;
  int batch = blockIdx.z;
  if (batch >= Batches || row >= M || col >= N) return;
  float acc = 0.0f;
  for (int p = 0; p < K; ++p)
    acc += A[(batch * M + row) * K + p] * B[(batch * K + p) * N + col];
  C[(batch * M + row) * N + col] = acc;
}
extern "C" __global__ void tilt_normalize(const float* X, const float* Mean,
                                             const float* Variance, const float* Gamma,
                                             const float* Beta, float* Y, int Batches,
                                             int Channels, int Spatial, float Epsilon) {
  int i = blockIdx.x * blockDim.x + threadIdx.x;
  int total = Batches * Channels * Spatial;
  if (i >= total) return;
  int channel = (i / Spatial) % Channels;
  Y[i] = (X[i] - Mean[channel]) * rsqrtf(Variance[channel] + Epsilon) * Gamma[channel] + Beta[channel];
}
extern "C" __global__ void tilt_maxpool2d(const float* X, float* Y, int Batches,
                                             int Channels, int Height, int Width,
                                             int Window, int Stride, int OutHeight,
                                             int OutWidth) {
  int i = blockIdx.x * blockDim.x + threadIdx.x;
  int total = Batches * Channels * OutHeight * OutWidth;
  if (i >= total) return;
  int ox = i % OutWidth;
  int oy = (i / OutWidth) % OutHeight;
  int c = (i / (OutWidth * OutHeight)) % Channels;
  int b = i / (OutWidth * OutHeight * Channels);
  float best = -3.402823466e+38F;
  for (int dy = 0; dy < Window; ++dy) for (int dx = 0; dx < Window; ++dx) {
    int y = oy * Stride + dy, x = ox * Stride + dx;
    if (y < Height && x < Width) best = fmaxf(best, X[((b * Channels + c) * Height + y) * Width + x]);
  }
  Y[i] = best;
}
extern "C" __global__ void tilt_reduce_sum(const float* X, float* Y, int N) {
  extern __shared__ float partial[];
  const unsigned tid = threadIdx.x;
  float total = 0.0f;
  for (int i = static_cast<int>(tid); i < N; i += blockDim.x) total += X[i];
  partial[tid] = total;
  __syncthreads();
  for (unsigned stride = blockDim.x / 2; stride > 0; stride >>= 1) {
    if (tid < stride) partial[tid] += partial[tid + stride];
    __syncthreads();
  }
  if (tid == 0) Y[0] = partial[0];
}
extern "C" __global__ void tilt_embedding_backward(const float* Indices, const float* G,
                                                     float* dTable, int Count, int Vocab,
                                                     int Dim) {
  int i = blockIdx.x * blockDim.x + threadIdx.x;
  int total = Count * Dim;
  if (i >= total) return;
  float value = Indices[i / Dim];
  int row = (int)(value >= 0.0f ? value + 0.5f : value - 0.5f);
  if (row >= 0 && row < Vocab) atomicAdd(&dTable[row * Dim + (i % Dim)], G[i]);
}
extern "C" __global__ void tilt_conv2d_backward_input(
    const float* X, const float* W, const float* G, float* dX,
    int B, int CI, int H, int Width, int CO, int KH, int KW, int OH, int OW,
    int Stride, int Pad, int Dilation) {
  int i = blockIdx.x * blockDim.x + threadIdx.x;
  int total = B * CI * H * Width;
  if (i >= total) return;
  int xx = i % Width, yy = (i / Width) % H, ci = (i / (Width * H)) % CI;
  int b = i / (Width * H * CI);
  float acc = 0.0f;
  for (int co = 0; co < CO; ++co) for (int oy = 0; oy < OH; ++oy) for (int ox = 0; ox < OW; ++ox)
    for (int u = 0; u < KH; ++u) for (int v = 0; v < KW; ++v) {
      int y = oy * Stride + u * Dilation - Pad;
      int x = ox * Stride + v * Dilation - Pad;
      if (y == yy && x == xx)
        acc += W[((co * CI + ci) * KH + u) * KW + v] *
               G[((b * CO + co) * OH + oy) * OW + ox];
    }
  dX[i] = acc;
}
extern "C" __global__ void tilt_conv2d_backward_weight(
    const float* X, const float* G, float* dW,
    int B, int CI, int H, int Width, int CO, int KH, int KW, int OH, int OW,
    int Stride, int Pad, int Dilation) {
  int i = blockIdx.x * blockDim.x + threadIdx.x;
  int total = CO * CI * KH * KW;
  if (i >= total) return;
  int v = i % KW, u = (i / KW) % KH, ci = (i / (KW * KH)) % CI;
  int co = i / (KW * KH * CI);
  float acc = 0.0f;
  for (int b = 0; b < B; ++b) for (int oy = 0; oy < OH; ++oy) for (int ox = 0; ox < OW; ++ox) {
    int y = oy * Stride + u * Dilation - Pad;
    int x = ox * Stride + v * Dilation - Pad;
    if (y >= 0 && y < H && x >= 0 && x < Width)
      acc += X[((b * CI + ci) * H + y) * Width + x] *
             G[((b * CO + co) * OH + oy) * OW + ox];
  }
  dW[i] = acc;
}
extern "C" __global__ void tilt_conv2d_backward_bias(const float* G, float* dB,
                                                       int B, int CO, int OH, int OW) {
  int co = blockIdx.x * blockDim.x + threadIdx.x;
  if (co >= CO) return;
  float acc = 0.0f;
  for (int b = 0; b < B; ++b) for (int oy = 0; oy < OH; ++oy) for (int ox = 0; ox < OW; ++ox)
    acc += G[((b * CO + co) * OH + oy) * OW + ox];
  dB[co] = acc;
}
extern "C" __global__ void tilt_recurrent_backward(
    const float* X, const float* W, const float* U, const float* Bias,
    const float* Hs, const float* Cs, const float* Gates, const float* Gy,
    float* Gx, float* GW, float* GU, float* GB, float* Dh, float* Dc,
    float* DhNext, float* DcNext, int Kind, int B, int T, int F, int H) {
  int bn = blockIdx.x * blockDim.x + threadIdx.x;
  if (bn >= B) return;
  int G = Kind == 0 ? H : (Kind == 1 ? 4 * H : 3 * H);
  for (int j = 0; j < H; ++j) { Dh[bn * H + j] = 0.0f; Dc[bn * H + j] = 0.0f; }
  for (int t = T - 1; t >= 0; --t) {
    for (int j = 0; j < H; ++j) { DhNext[bn * H + j] = 0.0f; DcNext[bn * H + j] = 0.0f; }
    for (int j = 0; j < H; ++j) {
      const float dht = Dh[bn * H + j] + (t == T - 1 ? Gy[bn * H + j] : 0.0f);
      if (Kind == 0) {
        float hout = Hs[((t + 1) * B + bn) * H + j];
        float d = dht * (1.0f - hout * hout);
        atomicAdd(&GB[j], d);
        for (int q = 0; q < F; ++q) {
          float xv = X[(bn * T + t) * F + q];
          atomicAdd(&GW[q * H + j], xv * d);
          Gx[(bn * T + t) * F + q] += W[q * H + j] * d;
        }
        for (int q = 0; q < H; ++q) {
          float hp = Hs[(t * B + bn) * H + q];
          atomicAdd(&GU[q * H + j], hp * d);
          DhNext[bn * H + q] += U[q * H + j] * d;
        }
      } else if (Kind == 1) {
        int base = ((t * B + bn) * G) + j;
        float ii = Gates[base], ff = Gates[base + H], gg = Gates[base + 2 * H], oo = Gates[base + 3 * H];
        float cc = Cs[((t + 1) * B + bn) * H + j];
        float tc = tanhf(cc);
        float dct = Dc[bn * H + j] + dht * oo * (1.0f - tc * tc);
        float cp = Cs[(t * B + bn) * H + j];
        float dz[4] = {dct * gg * ii * (1.0f - ii), dct * cp * ff * (1.0f - ff),
                       dct * ii * (1.0f - gg * gg), dht * tc * oo * (1.0f - oo)};
        DcNext[bn * H + j] = dct * ff;
        for (int gate = 0; gate < 4; ++gate) {
          int col = gate * H + j;
          atomicAdd(&GB[col], dz[gate]);
          for (int q = 0; q < F; ++q) {
            float xv = X[(bn * T + t) * F + q];
            atomicAdd(&GW[q * G + col], xv * dz[gate]);
            Gx[(bn * T + t) * F + q] += W[q * G + col] * dz[gate];
          }
          for (int q = 0; q < H; ++q) {
            float hp = Hs[(t * B + bn) * H + q];
            atomicAdd(&GU[q * G + col], hp * dz[gate]);
            DhNext[bn * H + q] += U[q * G + col] * dz[gate];
          }
        }
      } else {
        int base = ((t * B + bn) * G) + j;
        float z = Gates[base], r = Gates[base + H], nh = Gates[base + 2 * H];
        float hpj = Hs[(t * B + bn) * H + j];
        float dz = dht * (hpj - nh) * z * (1.0f - z);
        float dn = dht * (1.0f - z) * (1.0f - nh * nh);
        float qn = 0.0f;
        for (int q = 0; q < H; ++q) qn += Hs[(t * B + bn) * H + q] * U[q * G + 2 * H + j];
        float dr = dn * qn * r * (1.0f - r), dnr = dn * r;
        atomicAdd(&GB[j], dz); atomicAdd(&GB[H + j], dr); atomicAdd(&GB[2 * H + j], dn);
        for (int q = 0; q < F; ++q) {
          float xv = X[(bn * T + t) * F + q];
          Gx[(bn * T + t) * F + q] += W[q * G + j] * dz + W[q * G + H + j] * dr + W[q * G + 2 * H + j] * dn;
          atomicAdd(&GW[q * G + j], xv * dz); atomicAdd(&GW[q * G + H + j], xv * dr);
          atomicAdd(&GW[q * G + 2 * H + j], xv * dn);
        }
        for (int q = 0; q < H; ++q) {
          int uz = q * G + j, ur = q * G + H + j, un = q * G + 2 * H + j;
          float hp = Hs[(t * B + bn) * H + q];
          atomicAdd(&GU[uz], hp * dz); atomicAdd(&GU[ur], hp * dr); atomicAdd(&GU[un], hp * dnr);
          DhNext[bn * H + q] += U[uz] * dz + U[ur] * dr + U[un] * dnr;
        }
      }
    }
    for (int j = 0; j < H; ++j) { Dh[bn * H + j] = DhNext[bn * H + j]; Dc[bn * H + j] = DcNext[bn * H + j]; }
  }
}
)cuda";

// --- CUDA Driver API + NVRTC, bound lazily via tilt_dlopen (dlopen no POSIX,
// LoadLibrary no Windows) ----------------------------------------------------

struct CudaState {
  void* lib_cuda = nullptr;
  void* lib_nvrtc = nullptr;
  void* lib_cublas = nullptr;

  int (*cuInit)(unsigned) = nullptr;
  int (*cuDeviceGetCount)(int*) = nullptr;
  int (*cuDeviceGet)(int*, int) = nullptr;
  int (*cuCtxCreate)(void**, unsigned, int) = nullptr;
  int (*cuCtxDestroy)(void*) = nullptr;
  int (*cuCtxGetCurrent)(void**) = nullptr;
  int (*cuCtxSetCurrent)(void*) = nullptr;
  int (*cuMemAlloc)(void**, std::size_t) = nullptr;
  int (*cuMemFree)(void*) = nullptr;
  int (*cuMemcpyHtoD)(void*, const void*, std::size_t) = nullptr;
  int (*cuMemcpyDtoH)(void*, const void*, std::size_t) = nullptr;
  using CudaDevicePtr = std::uintptr_t;
  int (*cuMemcpyDtoHDevice)(void*, CudaDevicePtr, std::size_t) = nullptr;
  int (*cuModuleLoadData)(void**, const void*) = nullptr;
  int (*cuModuleGetFunction)(void**, void*, const char*) = nullptr;
  int (*cuModuleUnload)(void*) = nullptr;
  int (*cuLaunchKernel)(void*, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned,
                        void*, void**, void**) = nullptr;
  int (*cuCtxSynchronize)() = nullptr;

  int (*nvrtcCreateProgram)(void**, const char*, const char*, int, const char**,
                            const char**) = nullptr;
  int (*nvrtcCompileProgram)(void*, int, const char**) = nullptr;
  int (*nvrtcGetPTXSize)(void*, std::size_t*) = nullptr;
  int (*nvrtcGetPTX)(void*, char*) = nullptr;
  int (*nvrtcGetProgramLogSize)(void*, std::size_t*) = nullptr;
  int (*nvrtcGetProgramLog)(void*, char*) = nullptr;
  int (*nvrtcDestroyProgram)(void**) = nullptr;

  // cuBLAS is optional. Tilt still runs with its NVRTC kernels when the
  // library is absent (and with tensor.cpp when CUDA itself is absent).
  int (*cublasCreate)(void**) = nullptr;
  int (*cublasDestroy)(void*) = nullptr;
  int (*cublasSetMathMode)(void*, int) = nullptr;
  int (*cublasSgemm)(void*, int, int, int, int, int, const float*, const float*, int,
                     const float*, int, const float*, float*, int) = nullptr;
  int (*cublasGemmEx)(void*, int, int, int, int, int, const void*, const void*, int, int,
                      const void*, int, int, const void*, void*, int, int, int, int) = nullptr;

  void* ctx = nullptr;
  void* module = nullptr;
  void* fn_gemm = nullptr;
  void* fn_relu = nullptr;
  void* fn_gemm_mixed = nullptr;
  void* fn_gelu = nullptr;
  void* fn_add = nullptr;
  void* fn_conv2d = nullptr;
  void* fn_batch_gemm = nullptr;
  void* fn_normalize = nullptr;
  void* fn_maxpool2d = nullptr;
  void* fn_reduce_sum = nullptr;
  void* fn_embedding_backward = nullptr;
  void* fn_conv2d_backward_input = nullptr;
  void* fn_conv2d_backward_weight = nullptr;
  void* fn_conv2d_backward_bias = nullptr;
  void* fn_recurrent_backward = nullptr;
  void* blas = nullptr;
  bool tensor_core = false;
  std::array<void*, 5> scratch{};
  std::array<std::size_t, 5> scratch_bytes{};
  // Pesos de camadas costumam ser reutilizados em varias chamadas GEMM. O
  // cache evita HtoD repetido; checksum detecta atualizacoes do otimizador.
  void* resident_rhs = nullptr;
  const float* resident_rhs_host = nullptr;
  std::size_t resident_rhs_bytes = 0;
  std::uint64_t resident_rhs_hash = 0;

  ~CudaState() {
    if (ctx && cuCtxSetCurrent) {
      void* previous = nullptr;
      if (cuCtxGetCurrent) (void)cuCtxGetCurrent(&previous);
      (void)cuCtxSetCurrent(ctx);
      for (void* p : scratch) if (p && cuMemFree) (void)cuMemFree(p);
      if (resident_rhs && cuMemFree) (void)cuMemFree(resident_rhs);
      if (blas && cublasDestroy) (void)cublasDestroy(blas);
      if (module && cuModuleUnload) (void)cuModuleUnload(module);
      (void)cuCtxSetCurrent(previous == ctx ? nullptr : previous);
      if (cuCtxDestroy) (void)cuCtxDestroy(ctx);
    }
    if (lib_nvrtc) (void)tilt_dlclose(lib_nvrtc);
    if (lib_cublas) (void)tilt_dlclose(lib_cublas);
    if (lib_cuda) (void)tilt_dlclose(lib_cuda);
  }

  static std::uint64_t hash_host(const float* data, std::size_t bytes) {
    const auto* p = reinterpret_cast<const unsigned char*>(data);
    std::uint64_t h = 1469598103934665603ULL;
    for (std::size_t i = 0; i < bytes; ++i) {
      h ^= p[i];
      h *= 1099511628211ULL;
    }
    return h;
  }

  bool reserve_scratch(std::size_t slot, std::size_t bytes) {
    if (bytes == 0 || slot >= scratch.size()) return false;
    if (scratch_bytes[slot] >= bytes) return true;
    void* next = nullptr;
    if (cuMemAlloc(&next, bytes) != 0) return false;
    if (scratch[slot]) (void)cuMemFree(scratch[slot]);
    scratch[slot] = next;
    scratch_bytes[slot] = bytes;
    return true;
  }

  struct ContextScope {
    CudaState& state;
    void* previous = nullptr;
    bool active = false;
    explicit ContextScope(CudaState& s) : state(s) {
      active = s.cuCtxGetCurrent(&previous) == 0 && s.cuCtxSetCurrent(s.ctx) == 0;
    }
    ~ContextScope() { if (active) (void)state.cuCtxSetCurrent(previous); }
  };

  template <typename T>
  bool bind(void* lib, T& fp, const char* name) {
    fp = reinterpret_cast<T>(tilt_dlsym(lib, name));
    return fp != nullptr;
  }

  bool init(int index) {
#if defined(_WIN32)
    lib_cuda = tilt_dlopen("nvcuda.dll", true);
#else
    // Em WSL a libcuda do driver é um shim em /usr/lib/wsl/lib. O loader
    // também pode encontrar uma libcuda Linux em /lib, que não enxerga a GPU
    // exposta pelo host e retorna CUDA_ERROR_NO_DEVICE.
    lib_cuda = tilt_dlopen("/usr/lib/wsl/lib/libcuda.so.1", true);
    if (!lib_cuda) lib_cuda = tilt_dlopen("/usr/lib/wsl/lib/libcuda.so", true);
    if (!lib_cuda) lib_cuda = tilt_dlopen("libcuda.so.1", true);
    if (!lib_cuda) lib_cuda = tilt_dlopen("libcuda.so", true);
    if (!lib_cuda) lib_cuda = tilt_dlopen("nvcuda.dll", true);
#endif
    if (!lib_cuda) return false;
    if (std::getenv("TILT_GPU_DEBUG"))
      std::fprintf(stderr, "[gpu] libcuda carregada: %p\n", lib_cuda);

    bool ok = bind(lib_cuda, cuInit, "cuInit") && bind(lib_cuda, cuDeviceGetCount, "cuDeviceGetCount") &&
              bind(lib_cuda, cuDeviceGet, "cuDeviceGet") &&
              bind(lib_cuda, cuCtxCreate, "cuCtxCreate_v2") &&
              bind(lib_cuda, cuCtxDestroy, "cuCtxDestroy_v2") &&
              bind(lib_cuda, cuCtxGetCurrent, "cuCtxGetCurrent") &&
              bind(lib_cuda, cuCtxSetCurrent, "cuCtxSetCurrent") &&
              bind(lib_cuda, cuMemAlloc, "cuMemAlloc_v2") && bind(lib_cuda, cuMemFree, "cuMemFree_v2") &&
              bind(lib_cuda, cuMemcpyHtoD, "cuMemcpyHtoD_v2") &&
              bind(lib_cuda, cuMemcpyDtoH, "cuMemcpyDtoH_v2") &&
              bind(lib_cuda, cuModuleLoadData, "cuModuleLoadData") &&
              bind(lib_cuda, cuModuleGetFunction, "cuModuleGetFunction") &&
              bind(lib_cuda, cuModuleUnload, "cuModuleUnload") &&
              bind(lib_cuda, cuLaunchKernel, "cuLaunchKernel") &&
              bind(lib_cuda, cuCtxSynchronize, "cuCtxSynchronize");
    if (!ok) {
      if (std::getenv("TILT_GPU_DEBUG")) std::fprintf(stderr, "[gpu] bind do driver falhou\n");
      return false;
    }
    if (!bind(lib_cuda, cuMemcpyDtoHDevice, "cuMemcpyDtoH_v2")) return false;

    const int init_rc = cuInit(0);
    if (init_rc != 0) {
      if (std::getenv("TILT_GPU_DEBUG")) std::fprintf(stderr, "[gpu] cuInit=%d\n", init_rc);
      return false;
    }
    int count = 0;
    const int count_rc = cuDeviceGetCount(&count);
    if (count_rc != 0 || index < 0 || index >= count) {
      if (std::getenv("TILT_GPU_DEBUG"))
        std::fprintf(stderr, "[gpu] cuDeviceGetCount=%d count=%d index=%d\n", count_rc, count, index);
      return false;
    }
    int dev = 0;
    const int device_rc = cuDeviceGet(&dev, index);
    if (device_rc != 0) {
      if (std::getenv("TILT_GPU_DEBUG")) std::fprintf(stderr, "[gpu] cuDeviceGet=%d\n", device_rc);
      return false;
    }
    void* previous = nullptr;
    const int current_rc = cuCtxGetCurrent(&previous);
    if (current_rc != 0) {
      if (std::getenv("TILT_GPU_DEBUG")) std::fprintf(stderr, "[gpu] cuCtxGetCurrent=%d\n", current_rc);
      return false;
    }
    const int create_rc = cuCtxCreate(&ctx, 0, dev);
    if (create_rc != 0) {
      if (std::getenv("TILT_GPU_DEBUG")) std::fprintf(stderr, "[gpu] cuCtxCreate=%d\n", create_rc);
      return false;
    }
    ContextScope scope(*this);
    // cuCtxCreate makes the context current on this thread; restore the
    // caller's context once initialization finishes.
    scope.previous = previous;

#if defined(_WIN32)
    // NVRTC no Windows vem como nvrtc64_<major><minor>_0.dll; versões CUDA
    // recentes instalam também um nvrtc.dll alias. Sem NVRTC o estado fica
    // no CPU (fallback já existente).
    lib_nvrtc = tilt_dlopen("nvrtc.dll");
    if (!lib_nvrtc) lib_nvrtc = tilt_dlopen("nvrtc64_124_0.dll");
    if (!lib_nvrtc) lib_nvrtc = tilt_dlopen("nvrtc64_120_0.dll");
    if (!lib_nvrtc) lib_nvrtc = tilt_dlopen("nvrtc64_110_0.dll");
#else
    lib_nvrtc = tilt_dlopen("libnvrtc.so", true);
    if (!lib_nvrtc) lib_nvrtc = tilt_dlopen("libnvrtc.so.12", true);
#endif
    if (!lib_nvrtc) {
      if (std::getenv("TILT_GPU_DEBUG")) std::fprintf(stderr, "[gpu] NVRTC nao encontrado\n");
      return false;
    }  // no runtime compiler -> stay on CPU
    if (!(bind(lib_nvrtc, nvrtcCreateProgram, "nvrtcCreateProgram") &&
          bind(lib_nvrtc, nvrtcCompileProgram, "nvrtcCompileProgram") &&
          bind(lib_nvrtc, nvrtcGetPTXSize, "nvrtcGetPTXSize") &&
          bind(lib_nvrtc, nvrtcGetPTX, "nvrtcGetPTX") &&
          bind(lib_nvrtc, nvrtcDestroyProgram, "nvrtcDestroyProgram"))) {
      if (std::getenv("TILT_GPU_DEBUG")) std::fprintf(stderr, "[gpu] bind NVRTC falhou\n");
      return false;
    }
    (void)bind(lib_nvrtc, nvrtcGetProgramLogSize, "nvrtcGetProgramLogSize");
    (void)bind(lib_nvrtc, nvrtcGetProgramLog, "nvrtcGetProgramLog");

    void* prog = nullptr;
    const int create_program_rc = nvrtcCreateProgram(&prog, kKernelSrc, "tilt.cu", 0, nullptr, nullptr);
    if (create_program_rc != 0) {
      if (std::getenv("TILT_GPU_DEBUG"))
        std::fprintf(stderr, "[gpu] nvrtcCreateProgram=%d\n", create_program_rc);
      return false;
    }
    if (nvrtcCompileProgram(prog, 0, nullptr) != 0) {
      if (std::getenv("TILT_GPU_DEBUG") && nvrtcGetProgramLogSize && nvrtcGetProgramLog) {
        std::size_t log_size = 0;
        if (nvrtcGetProgramLogSize(prog, &log_size) == 0 && log_size > 0) {
          std::vector<char> log(log_size);
          if (nvrtcGetProgramLog(prog, log.data()) == 0)
            std::fprintf(stderr, "[gpu] NVRTC: %s\n", log.data());
        }
      }
      (void)nvrtcDestroyProgram(&prog);
      return false;
    }
    std::size_t ptx_size = 0;
    if (nvrtcGetPTXSize(prog, &ptx_size) != 0 || ptx_size == 0) {
      (void)nvrtcDestroyProgram(&prog);
      return false;
    }
    std::vector<char> ptx(ptx_size);
    const int ptx_rc = nvrtcGetPTX(prog, ptx.data());
    (void)nvrtcDestroyProgram(&prog);
    if (ptx_rc != 0) return false;
    if (cuModuleLoadData(&module, ptx.data()) != 0) return false;
    if (cuModuleGetFunction(&fn_gemm, module, "tilt_sgemm") != 0) return false;
    if (cuModuleGetFunction(&fn_relu, module, "tilt_relu") != 0) return false;
    if (cuModuleGetFunction(&fn_gemm_mixed, module, "tilt_sgemm_mixed") != 0) return false;
    if (cuModuleGetFunction(&fn_gelu, module, "tilt_gelu") != 0) return false;
    if (cuModuleGetFunction(&fn_add, module, "tilt_add") != 0) return false;
    if (cuModuleGetFunction(&fn_conv2d, module, "tilt_conv2d") != 0) return false;
    if (cuModuleGetFunction(&fn_batch_gemm, module, "tilt_batch_gemm") != 0) return false;
    if (cuModuleGetFunction(&fn_normalize, module, "tilt_normalize") != 0) return false;
    if (cuModuleGetFunction(&fn_maxpool2d, module, "tilt_maxpool2d") != 0) return false;
    if (cuModuleGetFunction(&fn_reduce_sum, module, "tilt_reduce_sum") != 0) return false;
    if (cuModuleGetFunction(&fn_embedding_backward, module, "tilt_embedding_backward") != 0) return false;
    if (cuModuleGetFunction(&fn_conv2d_backward_input, module, "tilt_conv2d_backward_input") != 0) return false;
    if (cuModuleGetFunction(&fn_conv2d_backward_weight, module, "tilt_conv2d_backward_weight") != 0) return false;
    if (cuModuleGetFunction(&fn_conv2d_backward_bias, module, "tilt_conv2d_backward_bias") != 0) return false;
    if (cuModuleGetFunction(&fn_recurrent_backward, module, "tilt_recurrent_backward") != 0) return false;

#if defined(_WIN32)
    lib_cublas = tilt_dlopen("cublas64_12.dll", true);
#else
    lib_cublas = tilt_dlopen("libcublas.so.12", true);
    if (!lib_cublas) lib_cublas = tilt_dlopen("libcublas.so", true);
#endif
    if (lib_cublas && bind(lib_cublas, cublasCreate, "cublasCreate_v2") &&
        bind(lib_cublas, cublasDestroy, "cublasDestroy_v2") &&
        bind(lib_cublas, cublasSgemm, "cublasSgemm_v2") &&
        bind(lib_cublas, cublasGemmEx, "cublasGemmEx")) {
      if (cublasCreate(&blas) != 0) blas = nullptr;
      // CUBLAS_TENSOR_OP_MATH=1. A missing symbol or an older driver simply
      // keeps the regular cuBLAS path, with no hard dependency on this API.
      if (blas && bind(lib_cublas, cublasSetMathMode, "cublasSetMathMode") &&
          cublasSetMathMode(blas, 1) == 0) tensor_core = true;
    }
    return true;
  }

  bool gemm(const float* a, const float* b, float* c, int m, int k, int n, bool mixed) {
    if (m < 0 || k < 0 || n < 0) return false;
    if (m == 0 || n == 0) return true;
    if (k == 0) {
      std::fill_n(c, static_cast<std::size_t>(m) * n, 0.0F);
      return true;
    }
    ContextScope scope(*this);
    if (!scope.active) return false;
    const std::size_t sa = (mixed ? sizeof(std::uint16_t) : sizeof(float)) * static_cast<std::size_t>(m) * k;
    const std::size_t sb = (mixed ? sizeof(std::uint16_t) : sizeof(float)) * static_cast<std::size_t>(k) * n;
    const std::size_t sc = sizeof(float) * static_cast<std::size_t>(m) * n;
    const auto ah = mixed ? half_operands(a, static_cast<std::size_t>(m) * k) : std::vector<std::uint16_t>{};
    const auto bh = mixed ? half_operands(b, static_cast<std::size_t>(k) * n) : std::vector<std::uint16_t>{};
    bool ok = reserve_scratch(0, sa) && reserve_scratch(2, sc);
    void* da = scratch[0];
    void* db = nullptr;
    std::uint64_t rhs_hash = 0;
    if (ok && !mixed) {
      rhs_hash = hash_host(b, sb);
      if (!resident_rhs || resident_rhs_bytes < sb || resident_rhs_host != b ||
          resident_rhs_hash != rhs_hash) {
        if (!resident_rhs || resident_rhs_bytes < sb) {
          void* next = nullptr;
          if (cuMemAlloc(&next, sb) != 0) ok = false;
          if (ok) {
            if (resident_rhs) (void)cuMemFree(resident_rhs);
            resident_rhs = next;
            resident_rhs_bytes = sb;
          }
        }
        if (ok) ok = cuMemcpyHtoD(resident_rhs, b, sb) == 0;
        if (ok) {
          resident_rhs_host = b;
          resident_rhs_hash = rhs_hash;
        }
      }
      db = resident_rhs;
    } else if (ok) {
      ok = reserve_scratch(1, sb);
      db = scratch[1];
    }
    void* dc = scratch[2];
    if (ok) ok = cuMemcpyHtoD(da, mixed ? static_cast<const void*>(ah.data()) : a, sa) == 0;
    if (ok && mixed) ok = cuMemcpyHtoD(db, bh.data(), sb) == 0;
    bool launched = false;
    if (ok && blas) {
      const float alpha = 1.0F, beta = 0.0F;
      // Row-major C=A*B is column-major C^T=B^T*A^T without copying.
      if (mixed) {
        // CUDA_R_16F=2, CUDA_R_32F=0, CUBLAS_COMPUTE_32F=68,
        // CUBLAS_GEMM_DEFAULT=-1 (CUDA 11+ ABI).
        // CUBLAS_GEMM_DEFAULT_TENSOR_OP=99; the explicit algorithm keeps the
        // request effective on drivers that ignore math mode for GemmEx.
        const int algorithm = tensor_core ? 99 : -1;
        launched = cublasGemmEx(blas, 0, 0, n, m, k, &alpha, db, 2, n, da, 2, k,
                                &beta, dc, 0, n, 68, algorithm) == 0;
      } else {
        launched = cublasSgemm(blas, 0, 0, n, m, k, &alpha,
                               static_cast<const float*>(db), n,
                               static_cast<const float*>(da), k, &beta,
                               static_cast<float*>(dc), n) == 0;
      }
    }
    if (ok && !launched) {
      unsigned bx = 16, by = 16;
      unsigned gx = (static_cast<unsigned>(n) + bx - 1) / bx;
      unsigned gy = (static_cast<unsigned>(m) + by - 1) / by;
      void* params[] = {&da, &db, &dc, &m, &k, &n};
      ok = cuLaunchKernel(mixed ? fn_gemm_mixed : fn_gemm, gx, gy, 1, bx, by, 1, 0,
                          nullptr, params, nullptr) == 0;
    }
    if (ok) ok = cuCtxSynchronize() == 0;
    if (ok) ok = cuMemcpyDtoH(c, dc, sc) == 0;
    return ok;
  }

  bool unary(float* d, std::size_t n, void* fn) {
    if (n == 0) return true;
    if (n > static_cast<std::size_t>(std::numeric_limits<int>::max())) return false;
    ContextScope scope(*this);
    if (!scope.active) return false;
    const std::size_t bytes = sizeof(float) * n;
    if (!reserve_scratch(0, bytes)) return false;
    void* dd = scratch[0];
    bool ok = cuMemcpyHtoD(dd, d, bytes) == 0;
    int ni = static_cast<int>(n);
    unsigned threads = 256;
    unsigned blocks = (static_cast<unsigned>(n) + threads - 1) / threads;
    void* params[] = {&dd, &ni};
    if (ok) ok = cuLaunchKernel(fn, blocks, 1, 1, threads, 1, 1, 0, nullptr, params, nullptr) == 0;
    if (ok) ok = cuCtxSynchronize() == 0;
    if (ok) ok = cuMemcpyDtoH(d, dd, bytes) == 0;
    return ok;
  }

  bool add(const float* a, const float* b, float* c, std::size_t n) {
    if (n == 0) return true;
    if (n > static_cast<std::size_t>(std::numeric_limits<int>::max())) return false;
    ContextScope scope(*this);
    if (!scope.active) return false;
    const std::size_t bytes = n * sizeof(float);
    bool ok = reserve_scratch(0, bytes) && reserve_scratch(1, bytes) &&
              reserve_scratch(2, bytes);
    void* da = scratch[0];
    void* db = scratch[1];
    void* dc = scratch[2];
    if (ok) ok = cuMemcpyHtoD(da, a, bytes) == 0 && cuMemcpyHtoD(db, b, bytes) == 0;
    int ni = static_cast<int>(n);
    unsigned threads = 256;
    unsigned blocks = (static_cast<unsigned>(n) + threads - 1) / threads;
    void* params[] = {&da, &db, &dc, &ni};
    if (ok) ok = cuLaunchKernel(fn_add, blocks, 1, 1, threads, 1, 1, 0, nullptr, params, nullptr) == 0;
    if (ok) ok = cuCtxSynchronize() == 0;
    if (ok) ok = cuMemcpyDtoH(c, dc, bytes) == 0;
    return ok;
  }

  bool conv2d(const float* x, const float* w, float* y, int batch, int ci, int h, int width,
              int co, int kh, int kw, int oh, int ow, int stride, int pad, int dilation) {
    const std::int64_t total = static_cast<std::int64_t>(batch) * co * oh * ow;
    if (total == 0) return true;
    if (total < 0 || total > std::numeric_limits<int>::max()) return false;
    ContextScope scope(*this);
    if (!scope.active) return false;
    const std::size_t sx = sizeof(float) * static_cast<std::size_t>(batch) * ci * h * width;
    const std::size_t sw = sizeof(float) * static_cast<std::size_t>(co) * ci * kh * kw;
    const std::size_t sy = sizeof(float) * static_cast<std::size_t>(total);
    bool ok = reserve_scratch(0, sx) && reserve_scratch(1, sw) &&
              reserve_scratch(2, sy);
    void* dx = scratch[0];
    void* dw = scratch[1];
    void* dy = scratch[2];
    if (ok) ok = cuMemcpyHtoD(dx, x, sx) == 0 && cuMemcpyHtoD(dw, w, sw) == 0;
    int ni = static_cast<int>(total);
    unsigned threads = 256;
    unsigned blocks = (static_cast<unsigned>(ni) + threads - 1) / threads;
    void* params[] = {&dx, &dw, &dy, &batch, &ci, &h, &width, &co, &kh, &kw,
                      &oh, &ow, &stride, &pad, &dilation};
    if (ok) ok = cuLaunchKernel(fn_conv2d, blocks, 1, 1, threads, 1, 1, 0, nullptr, params, nullptr) == 0;
    if (ok) ok = cuCtxSynchronize() == 0;
    if (ok) ok = cuMemcpyDtoH(y, dy, sy) == 0;
    return ok;
  }

  bool embedding_backward(const float* indices, const float* grad, float* table,
                          int count, int vocab, int dim) {
    if (!indices || !grad || !table || count < 0 || vocab <= 0 || dim <= 0) return false;
    if (static_cast<std::size_t>(count) >
      static_cast<std::size_t>(std::numeric_limits<int>::max()) / static_cast<std::size_t>(dim))
      return false;
    for (int i = 0; i < count; ++i) {
      const float value = indices[i];
      const int row = static_cast<int>(std::llround(value));
      if (std::fabs(value - static_cast<float>(row)) > 1e-5F || row < 0 || row >= vocab)
        return false;
    }
    const std::size_t si = sizeof(float) * static_cast<std::size_t>(count);
    const std::size_t sg = sizeof(float) * static_cast<std::size_t>(count) * dim;
    const std::size_t st = sizeof(float) * static_cast<std::size_t>(vocab) * dim;
    ContextScope scope(*this);
    if (!scope.active || !reserve_scratch(0, si) || !reserve_scratch(1, sg) ||
        !reserve_scratch(2, st)) return false;
    void* di = scratch[0]; void* dg = scratch[1]; void* dt = scratch[2];
    std::vector<float> zeros(static_cast<std::size_t>(vocab) * dim, 0.0F);
    if (cuMemcpyHtoD(di, indices, si) != 0 || cuMemcpyHtoD(dg, grad, sg) != 0 ||
        cuMemcpyHtoD(dt, zeros.data(), st) != 0) return false;
    int total = count * dim;
    void* params[] = {&di, &dg, &dt, &count, &vocab, &dim};
    const unsigned threads = 256;
    if (cuLaunchKernel(fn_embedding_backward,
                       (static_cast<unsigned>(total) + threads - 1) / threads, 1, 1,
                       threads, 1, 1, 0, nullptr, params, nullptr) != 0 ||
        cuCtxSynchronize() != 0) return false;
    return cuMemcpyDtoH(table, dt, st) == 0;
  }

  bool conv2d_backward(const float* x, const float* w, const float* grad, float* gx,
                       float* gw, float* gb, int batch, int ci, int h, int width, int co,
                       int kh, int kw, int oh, int ow, int stride, int pad, int dilation) {
    if (!x || !w || !grad || !gx || !gw || !gb || batch < 0 || ci <= 0 || h <= 0 ||
        width <= 0 || co <= 0 || kh <= 0 || kw <= 0 || oh < 0 || ow < 0 || stride <= 0 ||
        pad < 0 || dilation <= 0) return false;
    const std::size_t sx = sizeof(float) * static_cast<std::size_t>(batch) * ci * h * width;
    const std::size_t sw = sizeof(float) * static_cast<std::size_t>(co) * ci * kh * kw;
    const std::size_t sg = sizeof(float) * static_cast<std::size_t>(batch) * co * oh * ow;
    const std::size_t total_x = static_cast<std::size_t>(batch) * ci * h * width;
    const std::size_t total_w = static_cast<std::size_t>(co) * ci * kh * kw;
    if (total_x > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        total_w > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        static_cast<std::size_t>(batch) * co * oh * ow >
            static_cast<std::size_t>(std::numeric_limits<int>::max())) return false;
    ContextScope scope(*this);
    if (!scope.active || !reserve_scratch(0, sx) || !reserve_scratch(1, sw) ||
        !reserve_scratch(2, sg) || !reserve_scratch(3, sx) || !reserve_scratch(4, sw))
      return false;
    void* dx = scratch[0]; void* dw = scratch[1]; void* dg = scratch[2];
    void* dgx = scratch[3]; void* dgw = scratch[4]; void* dgb = nullptr;
    const std::size_t sb = sizeof(float) * static_cast<std::size_t>(co);
    if (cuMemAlloc(&dgb, sb) != 0) return false;
    std::vector<float> zeros_b(static_cast<std::size_t>(co), 0.0F);
    bool ok = cuMemcpyHtoD(dx, x, sx) == 0 && cuMemcpyHtoD(dw, w, sw) == 0 &&
              cuMemcpyHtoD(dg, grad, sg) == 0 && cuMemcpyHtoD(dgb, zeros_b.data(), sb) == 0;
    int total_x_i = static_cast<int>(total_x), total_w_i = static_cast<int>(total_w);
    const unsigned threads = 256;
    void* p_x[] = {&dx, &dw, &dg, &dgx, &batch, &ci, &h, &width, &co, &kh, &kw, &oh, &ow,
                   &stride, &pad, &dilation};
    void* p_w[] = {&dx, &dg, &dgw, &batch, &ci, &h, &width, &co, &kh, &kw, &oh, &ow, &stride,
                   &pad, &dilation};
    void* p_b[] = {&dg, &dgb, &batch, &co, &oh, &ow};
    if (ok) ok = cuLaunchKernel(fn_conv2d_backward_input,
                                (static_cast<unsigned>(total_x_i) + threads - 1) / threads,
                                1, 1, threads, 1, 1, 0, nullptr, p_x, nullptr) == 0;
    if (ok) ok = cuLaunchKernel(fn_conv2d_backward_weight,
                                (static_cast<unsigned>(total_w_i) + threads - 1) / threads,
                                1, 1, threads, 1, 1, 0, nullptr, p_w, nullptr) == 0;
    if (ok) ok = cuLaunchKernel(fn_conv2d_backward_bias,
                                (static_cast<unsigned>(co) + threads - 1) / threads,
                                1, 1, threads, 1, 1, 0, nullptr, p_b, nullptr) == 0;
    if (ok) ok = cuCtxSynchronize() == 0;
    if (ok) ok = cuMemcpyDtoH(gx, dgx, sx) == 0 && cuMemcpyDtoH(gw, dgw, sw) == 0 &&
                  cuMemcpyDtoH(gb, dgb, sb) == 0;
    (void)cuMemFree(dgb);
    return ok;
  }

  bool recurrent_backward(const float* x, const float* w, const float* u, const float* b,
                          const float* hs, const float* cs, const float* gates, const float* gy,
                          float* gx, float* gw, float* gu, float* gb, int kind, int batch,
                          int time, int input_size, int hidden) {
    if (!x || !w || !u || !b || !hs || !gy || !gx || !gw || !gu || !gb ||
        batch <= 0 || time <= 0 || input_size <= 0 || hidden <= 0 || kind < 0 || kind > 2)
      return false;
    const int gate_count = kind == 0 ? 1 : (kind == 1 ? 4 : 3);
    const int gates_size = gate_count * hidden;
    const std::size_t sx = sizeof(float) * static_cast<std::size_t>(batch) * time * input_size;
    const std::size_t sw = sizeof(float) * static_cast<std::size_t>(input_size) * gates_size;
    const std::size_t su = sizeof(float) * static_cast<std::size_t>(hidden) * gates_size;
    const std::size_t sb = sizeof(float) * static_cast<std::size_t>(gates_size);
    const std::size_t sh = sizeof(float) * static_cast<std::size_t>(time + 1) * batch * hidden;
    const std::size_t sc = kind == 1 ? sh : 0;
    const std::size_t sp = kind == 0 ? 0
        : sizeof(float) * static_cast<std::size_t>(time) * batch * gates_size;
    const std::size_t sg = sizeof(float) * static_cast<std::size_t>(batch) * hidden;
    const std::size_t swork = sg;
    std::vector<void*> dev;
    auto alloc = [&](std::size_t bytes, void*& p) {
      p = nullptr;
      if (bytes == 0) return true;
      if (cuMemAlloc(&p, bytes) != 0) return false;
      dev.push_back(p);
      return true;
    };
    ContextScope scope(*this);
    if (!scope.active) return false;
    void *dx, *dw, *du, *db, *dhs, *dcs, *dpg, *dgy, *dgx, *dgw, *dgu, *dgb;
    void *ddh, *ddc, *ddhn, *ddcn;
    if (!alloc(sx, dx) || !alloc(sw, dw) || !alloc(su, du) || !alloc(sb, db) ||
        !alloc(sh, dhs) || !alloc(sc, dcs) || !alloc(sp, dpg) || !alloc(sg, dgy) ||
        !alloc(sx, dgx) || !alloc(sw, dgw) || !alloc(su, dgu) || !alloc(sb, dgb) ||
        !alloc(swork, ddh) || !alloc(swork, ddc) || !alloc(swork, ddhn) || !alloc(swork, ddcn)) {
      for (void* p : dev) (void)cuMemFree(p);
      return false;
    }
    std::vector<float> zero_x(static_cast<std::size_t>(batch) * time * input_size, 0.0F);
    std::vector<float> zero_w(static_cast<std::size_t>(input_size) * gates_size, 0.0F);
    std::vector<float> zero_u(static_cast<std::size_t>(hidden) * gates_size, 0.0F);
    std::vector<float> zero_b(static_cast<std::size_t>(gates_size), 0.0F);
    bool ok = cuMemcpyHtoD(dx, x, sx) == 0 && cuMemcpyHtoD(dw, w, sw) == 0 &&
              cuMemcpyHtoD(du, u, su) == 0 && cuMemcpyHtoD(db, b, sb) == 0 &&
              cuMemcpyHtoD(dhs, hs, sh) == 0 &&
              (kind != 1 || cuMemcpyHtoD(dcs, cs, sc) == 0) &&
              (kind == 0 || cuMemcpyHtoD(dpg, gates, sp) == 0) &&
              cuMemcpyHtoD(dgy, gy, sg) == 0 && cuMemcpyHtoD(dgx, zero_x.data(), sx) == 0 &&
              cuMemcpyHtoD(dgw, zero_w.data(), sw) == 0 &&
              cuMemcpyHtoD(dgu, zero_u.data(), su) == 0 &&
              cuMemcpyHtoD(dgb, zero_b.data(), sb) == 0;
    void* params[] = {&dx, &dw, &du, &db, &dhs, &dcs, &dpg, &dgy, &dgx, &dgw, &dgu, &dgb,
                      &ddh, &ddc, &ddhn, &ddcn, &kind, &batch, &time, &input_size, &hidden};
    const unsigned threads = 128;
    if (ok) ok = cuLaunchKernel(fn_recurrent_backward,
                                (static_cast<unsigned>(batch) + threads - 1) / threads,
                                1, 1, threads, 1, 1, 0, nullptr, params, nullptr) == 0;
    if (ok) ok = cuCtxSynchronize() == 0;
    if (ok) ok = cuMemcpyDtoH(gx, dgx, sx) == 0 && cuMemcpyDtoH(gw, dgw, sw) == 0 &&
                  cuMemcpyDtoH(gu, dgu, su) == 0 && cuMemcpyDtoH(gb, dgb, sb) == 0;
    for (void* p : dev) (void)cuMemFree(p);
    return ok;
  }

  bool upload(const float* data, std::size_t bytes, GpuBuffer& buffer) {
    if (!data || bytes == 0) return false;
    ContextScope scope(*this);
    if (!scope.active) return false;
    if (buffer.handle) (void)release(buffer);
    void* device = nullptr;
    if (cuMemAlloc(&device, bytes) != 0 || cuMemcpyHtoD(device, data, bytes) != 0) {
      if (device) (void)cuMemFree(device);
      return false;
    }
    buffer.handle = device; buffer.bytes = bytes; buffer.backend = GpuBackend::Cuda;
    return true;
  }

  bool download(const GpuBuffer& buffer, float* data, std::size_t bytes) {
    if (!buffer.handle || !data || bytes > buffer.bytes || buffer.backend != GpuBackend::Cuda) return false;
    ContextScope scope(*this);
    return scope.active && cuMemcpyDtoHDevice(data, reinterpret_cast<CudaDevicePtr>(buffer.handle),
                                               bytes) == 0;
  }

  bool release(GpuBuffer& buffer) {
    if (!buffer.handle) return true;
    ContextScope scope(*this);
    if (!scope.active || cuMemFree(buffer.handle) != 0) return false;
    buffer = {};
    return true;
  }

  bool gemm_resident(const GpuBuffer& a, const GpuBuffer& b, GpuBuffer& c,
                     int m, int k, int n) {
    if (!a.handle || !b.handle || a.backend != GpuBackend::Cuda || b.backend != GpuBackend::Cuda ||
        m < 0 || k < 0 || n < 0 || a.bytes < sizeof(float) * static_cast<std::size_t>(m) * k ||
        b.bytes < sizeof(float) * static_cast<std::size_t>(k) * n) return false;
    ContextScope scope(*this);
    if (!scope.active) return false;
    const std::size_t out_bytes = sizeof(float) * static_cast<std::size_t>(m) * n;
    if (!c.handle || c.backend != GpuBackend::Cuda || c.bytes < out_bytes) {
      if (c.handle) (void)release(c);
      if (cuMemAlloc(&c.handle, out_bytes) != 0) return false;
      c.bytes = out_bytes; c.backend = GpuBackend::Cuda;
    }
    if (m == 0 || n == 0) return true;
    if (k == 0) return false;
    unsigned bx = 16, by = 16;
    unsigned gx = (static_cast<unsigned>(n) + bx - 1) / bx;
    unsigned gy = (static_cast<unsigned>(m) + by - 1) / by;
    void* da = a.handle; void* db = b.handle; void* dc = c.handle;
    void* params[] = {&da, &db, &dc, &m, &k, &n};
    if (cuLaunchKernel(fn_gemm, gx, gy, 1, bx, by, 1, 0, nullptr, params, nullptr) != 0) return false;
    return cuCtxSynchronize() == 0;
  }

  bool batch_gemm(const float* a, const float* b, float* c, int batches, int m, int k, int n) {
    if (!a || !b || !c || batches < 0 || m < 0 || k < 0 || n < 0 || batches == 0 || m == 0 || n == 0)
      return batches == 0 || m == 0 || n == 0;
    if (batches > 65535 || m > 65535 || n > 65535 || k > std::numeric_limits<int>::max()) return false;
    const std::size_t sa = sizeof(float) * static_cast<std::size_t>(batches) * m * k;
    const std::size_t sb = sizeof(float) * static_cast<std::size_t>(batches) * k * n;
    const std::size_t sc = sizeof(float) * static_cast<std::size_t>(batches) * m * n;
    ContextScope scope(*this);
    if (!scope.active || !reserve_scratch(0, sa) || !reserve_scratch(1, sb) || !reserve_scratch(2, sc)) return false;
    void* da = scratch[0]; void* db = scratch[1]; void* dc = scratch[2];
    if (cuMemcpyHtoD(da, a, sa) != 0 || cuMemcpyHtoD(db, b, sb) != 0) return false;
    void* params[] = {&da, &db, &dc, &batches, &m, &k, &n};
    const unsigned bx = 16, by = 16;
    const unsigned gx = (static_cast<unsigned>(n) + bx - 1) / bx;
    const unsigned gy = (static_cast<unsigned>(m) + by - 1) / by;
    if (cuLaunchKernel(fn_batch_gemm, gx, gy, static_cast<unsigned>(batches), bx, by, 1,
                       0, nullptr, params, nullptr) != 0 || cuCtxSynchronize() != 0) return false;
    return cuMemcpyDtoH(c, dc, sc) == 0;
  }

  bool normalize(const float* input, const float* mean, const float* variance,
                 const float* gamma, const float* beta, float* output, int batches,
                 int channels, int spatial, float epsilon) {
    if (!input || !mean || !variance || !gamma || !beta || !output || batches < 0 ||
        channels < 0 || spatial < 0 || batches == 0 || channels == 0 || spatial == 0) return false;
    const std::size_t total = static_cast<std::size_t>(batches) * channels * spatial;
    const std::size_t data_bytes = sizeof(float) * total;
    const std::size_t channel_bytes = sizeof(float) * static_cast<std::size_t>(channels);
    ContextScope scope(*this);
    if (!scope.active || !reserve_scratch(0, data_bytes) || !reserve_scratch(1, channel_bytes) ||
        !reserve_scratch(2, data_bytes) || !reserve_scratch(3, channel_bytes) ||
        !reserve_scratch(4, channel_bytes)) return false;
    void* dx = scratch[0]; void* dmean = scratch[1]; void* dy = scratch[2];
    void* dvar = scratch[3]; void* dgamma = scratch[4];
    // Beta shares the input scratch only after the input copy has completed;
    // allocate a short-lived device buffer for it to keep the pool bounded.
    void* dbeta = nullptr;
    if (cuMemAlloc(&dbeta, channel_bytes) != 0) return false;
    bool ok = cuMemcpyHtoD(dx, input, data_bytes) == 0 &&
              cuMemcpyHtoD(dmean, mean, channel_bytes) == 0 &&
              cuMemcpyHtoD(dvar, variance, channel_bytes) == 0 &&
              cuMemcpyHtoD(dgamma, gamma, channel_bytes) == 0 &&
              cuMemcpyHtoD(dbeta, beta, channel_bytes) == 0;
    void* params[] = {&dx, &dmean, &dvar, &dgamma, &dbeta, &dy, &batches, &channels,
                      &spatial, &epsilon};
    const int total_i = static_cast<int>(total);
    const unsigned threads = 256;
    if (ok && total <= static_cast<std::size_t>(std::numeric_limits<int>::max())) {
      const unsigned blocks = (static_cast<unsigned>(total_i) + threads - 1) / threads;
      ok = cuLaunchKernel(fn_normalize, blocks, 1, 1, threads, 1, 1, 0, nullptr, params, nullptr) == 0;
    } else ok = false;
    if (ok) ok = cuCtxSynchronize() == 0;
    if (ok) ok = cuMemcpyDtoH(output, dy, data_bytes) == 0;
    (void)cuMemFree(dbeta);
    return ok;
  }

  bool maxpool2d(const float* input, float* output, int batches, int channels,
                 int height, int width, int window, int stride) {
    if (!input || !output || batches < 0 || channels < 0 || height < 0 || width < 0 ||
        window <= 0 || stride <= 0) return false;
    int out_h = height < window ? 0 : (height - window) / stride + 1;
    int out_w = width < window ? 0 : (width - window) / stride + 1;
    const std::int64_t total = static_cast<std::int64_t>(batches) * channels * out_h * out_w;
    if (total <= 0 || total > std::numeric_limits<int>::max()) return total == 0;
    const std::size_t sx = sizeof(float) * static_cast<std::size_t>(batches) * channels * height * width;
    const std::size_t sy = sizeof(float) * static_cast<std::size_t>(total);
    ContextScope scope(*this);
    if (!scope.active || !reserve_scratch(0, sx) || !reserve_scratch(2, sy)) return false;
    void* dx = scratch[0]; void* dy = scratch[2];
    if (cuMemcpyHtoD(dx, input, sx) != 0) return false;
    int total_i = static_cast<int>(total);
    void* params[] = {&dx, &dy, &batches, &channels, &height, &width, &window, &stride,
                      &out_h, &out_w};
    const unsigned threads = 256;
    if (cuLaunchKernel(fn_maxpool2d, (static_cast<unsigned>(total_i) + threads - 1) / threads,
                       1, 1, threads, 1, 1, 0, nullptr, params, nullptr) != 0 ||
        cuCtxSynchronize() != 0) return false;
    return cuMemcpyDtoH(output, dy, sy) == 0;
  }

  bool reduce_sum(const float* input, float* output, std::size_t count) {
    if (!input || !output || count == 0 || count > static_cast<std::size_t>(std::numeric_limits<int>::max())) return false;
    ContextScope scope(*this);
    if (!scope.active || !reserve_scratch(0, count * sizeof(float)) || !reserve_scratch(2, sizeof(float))) return false;
    void* dx = scratch[0]; void* dy = scratch[2];
    if (cuMemcpyHtoD(dx, input, count * sizeof(float)) != 0) return false;
    int n = static_cast<int>(count); void* params[] = {&dx, &dy, &n};
    constexpr unsigned threads = 256;
    if (cuLaunchKernel(fn_reduce_sum, 1, 1, 1, threads, 1, 1,
                       threads * sizeof(float), nullptr, params, nullptr) != 0 ||
        cuCtxSynchronize() != 0) return false;
    return cuMemcpyDtoH(output, dy, sizeof(float)) == 0;
  }
};

std::string env_gpu() {
  const char* v = std::getenv("TILT_GPU");
  return v ? std::string(v) : std::string("off");
}

int requested_device(const std::string& device) {
  if (device == "auto" || device == "gpu" || device == "cuda") return 0;
  if (device.rfind("cuda:", 0) != 0 || device.size() == 5) return -1;
  int index = 0;
  for (std::size_t i = 5; i < device.size(); ++i) {
    if (device[i] < '0' || device[i] > '9' || index > 1000000) return -1;
    index = index * 10 + device[i] - '0';
  }
  return index;
}

bool requested_metal(const std::string& device) { return device == "metal" || device == "metal:0"; }

}  // namespace

GpuRuntime& GpuRuntime::instance() {
  static GpuRuntime rt;
  return rt;
}

GpuRuntime::~GpuRuntime() {
  delete static_cast<CudaState*>(cuda_ctx_);
  metal::destroy(metal_ctx_);
}

bool GpuRuntime::ensure(const std::string& device) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (device == "cpu") return false;
  const std::string mode = env_gpu();
  if (mode == "off") return false;
  if (mode == "fake") {
    backend_ = GpuBackend::Fake;
    info_ = "fake (kernels de CPU pelo caminho de dispatch da GPU)";
    return true;
  }
  if (requested_metal(device) || mode == "metal") {
    if (backend_ == GpuBackend::Metal) return true;
    if (metal_ctx_) return false;
    metal_ctx_ = metal::create();
    if (!metal_ctx_) return false;
    backend_ = GpuBackend::Metal;
    info_ = "metal (Metal Shading Language)";
    return true;
  }
  const int index = requested_device(device);
  if (index < 0) return false;
  if (backend_ == GpuBackend::Cuda) return index == device_index_;
  if (attempted_index_ == index) return false;
  attempted_index_ = index;

  auto* st = new CudaState();
  if (st->init(index)) {
    cuda_ctx_ = st;
    backend_ = GpuBackend::Cuda;
    device_index_ = index;
    tensor_core_active_ = st->tensor_core;
    info_ = "cuda:" + std::to_string(index) + " (libcuda + nvrtc" +
            (tensor_core_active_ ? " + tensor-cores" : "") + ")";
    return true;
  }
  delete st;
  return false;
}

bool GpuRuntime::gemm(const float* a, const float* b, float* c, int m, int k, int n) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (backend_ == GpuBackend::Fake) {
    cpu_gemm(a, b, c, m, k, n);
    return true;
  }
  if (backend_ == GpuBackend::Cuda && cuda_ctx_) {
    return static_cast<CudaState*>(cuda_ctx_)->gemm(a, b, c, m, k, n, false);
  }
  if (backend_ == GpuBackend::Metal && metal_ctx_) return metal::gemm(metal_ctx_, a, b, c, m, k, n);
  return false;
}

bool GpuRuntime::gemm_mixed(const float* a, const float* b, float* c, int m, int k, int n) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (m < 0 || k < 0 || n < 0) return false;
  constexpr float kHalfMax = 65504.0F;
  for (std::size_t i = 0; i < static_cast<std::size_t>(m) * k; ++i) {
    if (!std::isfinite(a[i]) || std::abs(a[i]) > kHalfMax) return false;
  }
  for (std::size_t i = 0; i < static_cast<std::size_t>(k) * n; ++i) {
    if (!std::isfinite(b[i]) || std::abs(b[i]) > kHalfMax) return false;
  }
  if (backend_ == GpuBackend::Fake) {
    std::vector<float> ah(static_cast<std::size_t>(m) * k), bh(static_cast<std::size_t>(k) * n);
    for (std::size_t i = 0; i < ah.size(); ++i) ah[i] = from_half(to_half(a[i]));
    for (std::size_t i = 0; i < bh.size(); ++i) bh[i] = from_half(to_half(b[i]));
    cpu_gemm(ah.data(), bh.data(), c, m, k, n);
    return true;
  }
  if (backend_ == GpuBackend::Cuda && cuda_ctx_) {
    return static_cast<CudaState*>(cuda_ctx_)->gemm(a, b, c, m, k, n, true);
  }
  if (backend_ == GpuBackend::Metal && metal_ctx_)
    return metal::gemm(metal_ctx_, a, b, c, m, k, n);
  return false;
}

bool GpuRuntime::relu(float* data, std::size_t n) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (backend_ == GpuBackend::Fake) {
    cpu_relu(data, n);
    return true;
  }
  if (backend_ == GpuBackend::Cuda && cuda_ctx_) {
    auto* state = static_cast<CudaState*>(cuda_ctx_);
    return state->unary(data, n, state->fn_relu);
  }
  if (backend_ == GpuBackend::Metal && metal_ctx_) return metal::relu(metal_ctx_, data, n);
  return false;
}

bool GpuRuntime::gelu(float* data, std::size_t n) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (backend_ == GpuBackend::Fake) {
    cpu_gelu(data, n);
    return true;
  }
  if (backend_ == GpuBackend::Cuda && cuda_ctx_) {
    auto* state = static_cast<CudaState*>(cuda_ctx_);
    return state->unary(data, n, state->fn_gelu);
  }
  if (backend_ == GpuBackend::Metal && metal_ctx_) return metal::gelu(metal_ctx_, data, n);
  return false;
}

bool GpuRuntime::add(const float* a, const float* b, float* c, std::size_t n) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (backend_ == GpuBackend::Fake) {
    for (std::size_t i = 0; i < n; ++i) c[i] = a[i] + b[i];
    return true;
  }
  if (backend_ == GpuBackend::Cuda && cuda_ctx_) {
    return static_cast<CudaState*>(cuda_ctx_)->add(a, b, c, n);
  }
  if (backend_ == GpuBackend::Metal && metal_ctx_) return metal::add(metal_ctx_, a, b, c, n);
  return false;
}

bool GpuRuntime::conv2d(const float* x, const float* weights, float* y, int batch, int in_channels,
                        int height, int width, int out_channels, int kh, int kw, int out_h,
                        int out_w, int stride, int padding, int dilation) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (backend_ == GpuBackend::Fake) {
    cpu_conv2d(x, weights, y, batch, in_channels, height, width, out_channels, kh, kw,
               out_h, out_w, stride, padding, dilation);
    return true;
  }
  if (backend_ == GpuBackend::Cuda && cuda_ctx_) {
    return static_cast<CudaState*>(cuda_ctx_)->conv2d(x, weights, y, batch, in_channels, height,
                                                       width, out_channels, kh, kw, out_h, out_w,
                                                       stride, padding, dilation);
  }
  if (backend_ == GpuBackend::Metal && metal_ctx_) {
    return metal::conv2d(metal_ctx_, x, weights, y, batch, in_channels, height, width,
                         out_channels, kh, kw, out_h, out_w, stride, padding, dilation);
  }
  return false;
}

bool GpuRuntime::conv2d_backward(const float* x, const float* weights, const float* grad_output,
                                 float* grad_input, float* grad_weights, float* grad_bias,
                                 int batch, int in_channels, int height, int width,
                                 int out_channels, int kh, int kw, int out_h, int out_w,
                                 int stride, int padding, int dilation) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (backend_ == GpuBackend::Fake) {
    cpu_conv2d_backward(x, weights, grad_output, grad_input, grad_weights, grad_bias, batch,
                        in_channels, height, width, out_channels, kh, kw, out_h, out_w, stride,
                        padding, dilation);
    return true;
  }
  if (backend_ == GpuBackend::Cuda && cuda_ctx_)
    return static_cast<CudaState*>(cuda_ctx_)->conv2d_backward(
        x, weights, grad_output, grad_input, grad_weights, grad_bias, batch, in_channels, height,
        width, out_channels, kh, kw, out_h, out_w, stride, padding, dilation);
  return false;
}

bool GpuRuntime::embedding_backward(const float* indices, const float* grad_output,
                                    float* grad_table, int index_count, int vocabulary,
                                    int dimension) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (backend_ == GpuBackend::Fake) {
    return cpu_embedding_backward(indices, grad_output, grad_table, index_count, vocabulary,
                                  dimension);
  }
  if (backend_ == GpuBackend::Cuda && cuda_ctx_)
    return static_cast<CudaState*>(cuda_ctx_)->embedding_backward(
        indices, grad_output, grad_table, index_count, vocabulary, dimension);
  return false;
}

bool GpuRuntime::recurrent_backward(
    const float* x, const float* weights, const float* recurrent_weights, const float* bias,
    const float* cache_h, const float* cache_c, const float* cache_gates,
    const float* grad_output, float* grad_input, float* grad_weights,
    float* grad_recurrent_weights, float* grad_bias, int kind, int batch, int time,
    int input_size, int hidden_size) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (backend_ == GpuBackend::Fake) {
    cpu_recurrent_backward(x, weights, recurrent_weights, bias, cache_h, cache_c, cache_gates,
                           grad_output, grad_input, grad_weights, grad_recurrent_weights,
                           grad_bias, kind, batch, time, input_size, hidden_size);
    return true;
  }
  if (backend_ == GpuBackend::Cuda && cuda_ctx_)
    return static_cast<CudaState*>(cuda_ctx_)->recurrent_backward(
        x, weights, recurrent_weights, bias, cache_h, cache_c, cache_gates, grad_output,
        grad_input, grad_weights, grad_recurrent_weights, grad_bias, kind, batch, time,
        input_size, hidden_size);
  return false;
}

bool GpuRuntime::batch_gemm(const float* a, const float* b, float* c, int batches, int m, int k, int n) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (backend_ == GpuBackend::Fake) {
    const std::size_t sa = static_cast<std::size_t>(m) * k;
    const std::size_t sb = static_cast<std::size_t>(k) * n;
    const std::size_t sc = static_cast<std::size_t>(m) * n;
    for (int batch = 0; batch < batches; ++batch)
      cpu_gemm(a + batch * sa, b + batch * sb, c + batch * sc, m, k, n);
    return true;
  }
  if (backend_ == GpuBackend::Cuda && cuda_ctx_)
    return static_cast<CudaState*>(cuda_ctx_)->batch_gemm(a, b, c, batches, m, k, n);
  return false;
}

bool GpuRuntime::normalize(const float* input, const float* mean, const float* variance,
                           const float* gamma, const float* beta, float* output, int batches,
                           int channels, int spatial, float epsilon) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (backend_ == GpuBackend::Fake) {
    cpu_normalize(input, mean, variance, gamma, beta, output, batches, channels, spatial, epsilon);
    return true;
  }
  if (backend_ == GpuBackend::Cuda && cuda_ctx_)
    return static_cast<CudaState*>(cuda_ctx_)->normalize(input, mean, variance, gamma, beta,
                                                          output, batches, channels, spatial, epsilon);
  return false;
}

bool GpuRuntime::maxpool2d(const float* input, float* output, int batches, int channels,
                           int height, int width, int window, int stride) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (backend_ == GpuBackend::Fake) {
    cpu_maxpool2d(input, output, batches, channels, height, width, window, stride);
    return true;
  }
  if (backend_ == GpuBackend::Cuda && cuda_ctx_)
    return static_cast<CudaState*>(cuda_ctx_)->maxpool2d(input, output, batches, channels,
                                                           height, width, window, stride);
  return false;
}

bool GpuRuntime::reduce_sum(const float* input, float* output, std::size_t count) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (backend_ == GpuBackend::Fake) {
    *output = std::accumulate(input, input + count, 0.0F);
    return true;
  }
  if (backend_ == GpuBackend::Cuda && cuda_ctx_)
    return static_cast<CudaState*>(cuda_ctx_)->reduce_sum(input, output, count);
  return false;
}

bool GpuRuntime::dense_backward(const float* input, const float* weights, const float* grad_output,
                                float* grad_input, float* grad_weights, float* grad_bias,
                                int batches, int input_features, int output_features) {
  if (!input || !weights || !grad_output || !grad_input || !grad_weights || !grad_bias ||
      batches < 0 || input_features < 0 || output_features < 0) return false;
  std::vector<float> weights_transposed(static_cast<std::size_t>(output_features) * input_features);
  for (int i = 0; i < input_features; ++i)
    for (int o = 0; o < output_features; ++o)
      weights_transposed[static_cast<std::size_t>(o) * input_features + i] =
          weights[static_cast<std::size_t>(i) * output_features + o];
  if (!gemm(grad_output, weights_transposed.data(), grad_input,
            batches, output_features, input_features)) return false;
  std::vector<float> input_transposed(static_cast<std::size_t>(input_features) * batches);
  for (int b = 0; b < batches; ++b)
    for (int i = 0; i < input_features; ++i)
      input_transposed[static_cast<std::size_t>(i) * batches + b] =
          input[static_cast<std::size_t>(b) * input_features + i];
  if (!gemm(input_transposed.data(), grad_output, grad_weights,
            input_features, batches, output_features)) return false;
  std::vector<float> column(static_cast<std::size_t>(batches));
  for (int o = 0; o < output_features; ++o) {
    for (int b = 0; b < batches; ++b) column[static_cast<std::size_t>(b)] =
        grad_output[static_cast<std::size_t>(b) * output_features + o];
    if (!reduce_sum(column.data(), grad_bias + o, column.size())) return false;
  }
  return true;
}

bool GpuRuntime::upload(const float* data, std::size_t count, GpuBuffer& buffer) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (count > std::numeric_limits<std::size_t>::max() / sizeof(float)) return false;
  const std::size_t bytes = count * sizeof(float);
  if (backend_ == GpuBackend::Cuda && cuda_ctx_)
    return static_cast<CudaState*>(cuda_ctx_)->upload(data, bytes, buffer);
  if (backend_ == GpuBackend::Metal && metal_ctx_) {
    if (buffer.handle) (void)metal::release_buffer(metal_ctx_, buffer.handle);
    buffer = {};
    if (!metal::upload(metal_ctx_, data, bytes, buffer.handle)) return false;
    buffer.bytes = bytes;
    buffer.backend = GpuBackend::Metal;
    return true;
  }
  return false;
}

bool GpuRuntime::download(const GpuBuffer& buffer, float* data, std::size_t count) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (count > std::numeric_limits<std::size_t>::max() / sizeof(float)) return false;
  const std::size_t bytes = count * sizeof(float);
  if (buffer.backend == GpuBackend::Cuda && cuda_ctx_)
    return static_cast<CudaState*>(cuda_ctx_)->download(buffer, data, bytes);
  if (buffer.backend == GpuBackend::Metal && metal_ctx_)
    return metal::download(metal_ctx_, buffer.handle, data, bytes);
  return false;
}

bool GpuRuntime::release(GpuBuffer& buffer) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!buffer.handle) return true;
  bool ok = false;
  if (buffer.backend == GpuBackend::Cuda && cuda_ctx_)
    ok = static_cast<CudaState*>(cuda_ctx_)->release(buffer);
  else if (buffer.backend == GpuBackend::Metal && metal_ctx_)
    ok = metal::release_buffer(metal_ctx_, buffer.handle);
  if (ok) buffer = {};
  return ok;
}

bool GpuRuntime::gemm_resident(const GpuBuffer& a, const GpuBuffer& b, GpuBuffer& c,
                               int m, int k, int n) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (m < 0 || k < 0 || n < 0) return false;
  const auto elements = static_cast<std::size_t>(m) * static_cast<std::size_t>(n);
  if ((n != 0 && elements / static_cast<std::size_t>(n) != static_cast<std::size_t>(m)) ||
      elements > std::numeric_limits<std::size_t>::max() / sizeof(float)) return false;
  if (backend_ == GpuBackend::Cuda && cuda_ctx_)
    return static_cast<CudaState*>(cuda_ctx_)->gemm_resident(a, b, c, m, k, n);
  if (backend_ == GpuBackend::Metal && metal_ctx_) {
    const std::size_t bytes = sizeof(float) * elements;
    if (c.handle && (c.backend != GpuBackend::Metal || c.bytes < bytes)) {
      (void)metal::release_buffer(metal_ctx_, c.handle);
      c = {};
    }
    if (!metal::gemm_resident(metal_ctx_, a.handle, b.handle, c.handle, m, k, n)) return false;
    c.bytes = bytes;
    c.backend = GpuBackend::Metal;
    return true;
  }
  return false;
}

}  // namespace tilt::rt
