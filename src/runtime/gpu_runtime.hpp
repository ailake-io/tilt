#pragma once

#include <cstddef>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace tilt::rt {

enum class GpuBackend { Cpu, Cuda, Metal, Fake };

class GpuRuntime;

// Opaque armazenamento persistente no acelerador. O buffer pertence ao
// GpuRuntime que o criou e deve ser liberado com release(). A API continua
// válida em máquinas sem GPU: upload retorna false e o chamador pode manter o
// Tensor em CPU.
struct GpuBuffer {
  void* handle = nullptr;
  std::size_t bytes = 0;
  // Número de elementos float válidos. Mantido separado de bytes para que
  // operações residentes possam validar dimensões sem repetir metadados.
  std::size_t elements = 0;
  GpuBackend backend = GpuBackend::Cpu;
};

struct GpuTensorStorage {
  GpuBuffer buffer;
  GpuRuntime* runtime = nullptr;
  const float* host_data = nullptr;
  std::size_t host_elements = 0;
  ~GpuTensorStorage();
};

// GPU dispatch. The CUDA path binds libcuda + libnvrtc at runtime (tilt_dlopen:
// dlopen no POSIX, LoadLibrary no Windows) and compiles small CUDA-C kernels
// on first use; when no driver is present it
// reports unavailable and callers fall back to the CPU kernels in tensor.cpp.
// `TILT_GPU` overrides detection: off | auto | metal (macOS) | fake.
class GpuRuntime {
 public:
  static GpuRuntime& instance();

  // Attempts initialization for the given device string ("auto", "gpu",
  // "cuda:N", "cuda" ou "metal"). Returns true if a GPU backend is active.
  bool ensure(const std::string& device);

  bool available() const { return backend_ != GpuBackend::Cpu; }
  GpuBackend backend() const { return backend_; }
  const std::string& info() const { return info_; }
  bool tensor_core_active() const { return tensor_core_active_; }
  bool residency_available() const {
    return backend_ == GpuBackend::Cuda || backend_ == GpuBackend::Metal || backend_ == GpuBackend::Fake;
  }

  // Mantém um tensor no device entre operações. O ponteiro de host só é lido
  // durante upload; operações posteriores usam o buffer sem novas cópias.
  bool upload(const float* data, std::size_t count, GpuBuffer& buffer);
  bool download(const GpuBuffer& buffer, float* data, std::size_t count);
  bool release(GpuBuffer& buffer);
  bool gemm_resident(const GpuBuffer& a, const GpuBuffer& b, GpuBuffer& c,
                     int m, int k, int n);
  bool batch_gemm_resident(const GpuBuffer& a, const GpuBuffer& b, GpuBuffer& c,
                           int batches, int m, int k, int n);
  bool conv2d_resident(const GpuBuffer& input, const GpuBuffer& weights, GpuBuffer& output,
                       int batch, int in_channels, int height, int width, int out_channels,
                       int kh, int kw, int out_h, int out_w, int stride, int padding,
                       int dilation);
  bool relu_resident(GpuBuffer& data, std::size_t count);
  bool gelu_resident(GpuBuffer& data, std::size_t count);
  bool add_resident(const GpuBuffer& a, const GpuBuffer& b, GpuBuffer& c,
                    std::size_t count);
  bool add_channel_bias_resident(const GpuBuffer& input, const GpuBuffer& bias,
                                 GpuBuffer& output, int batches, int channels, int spatial);
  bool normalize_resident(const GpuBuffer& input, const GpuBuffer& mean,
                          const GpuBuffer& variance, const GpuBuffer& gamma,
                          const GpuBuffer& beta, GpuBuffer& output,
                          int batches, int channels, int spatial, float epsilon);
  bool maxpool2d_resident(const GpuBuffer& input, GpuBuffer& output,
                          int batches, int channels, int height, int width,
                          int window, int stride);
  bool reduce_sum_resident(const GpuBuffer& input, GpuBuffer& output,
                           std::size_t count);
  bool batch_gemm(const float* a, const float* b, float* c, int batches, int m, int k, int n);
  bool normalize(const float* input, const float* mean, const float* variance,
                 const float* gamma, const float* beta, float* output,
                 int batches, int channels, int spatial, float epsilon);
  bool maxpool2d(const float* input, float* output, int batches, int channels,
                 int height, int width, int window, int stride);
  bool reduce_sum(const float* input, float* output, std::size_t count);
  // Backward de uma camada densa: todas as três reduções/GEMMs usam o backend
  // ativo; retorna false para o chamador manter o backward CPU.
  bool dense_backward(const float* input, const float* weights, const float* grad_output,
                      float* grad_input, float* grad_weights, float* grad_bias,
                      int batches, int input_features, int output_features);

  // C = A(m x k) * B(k x n), row-major f32. Returns false if it could not run
  // on the GPU (caller should use the CPU path).
  bool gemm(const float* a, const float* b, float* c, int m, int k, int n);
  // FP16 operands with FP32 accumulation/output; intended for treino AMP.
  bool gemm_mixed(const float* a, const float* b, float* c, int m, int k, int n);
  bool relu(float* data, std::size_t n);
  bool gelu(float* data, std::size_t n);
  bool add(const float* a, const float* b, float* c, std::size_t n);
  // NCHW convolution; output dimensions are supplied by the validated caller.
  bool conv2d(const float* x, const float* weights, float* y, int batch, int in_channels,
              int height, int width, int out_channels, int kh, int kw, int out_h, int out_w,
              int stride, int padding, int dilation);
  // Backward kernels for layers whose reference implementation lives in
  // tensor.cpp.  A false return keeps the caller on the validated CPU path.
  bool conv2d_backward(const float* x, const float* weights, const float* grad_output,
                       float* grad_input, float* grad_weights, float* grad_bias,
                       int batch, int in_channels, int height, int width, int out_channels,
                       int kh, int kw, int out_h, int out_w, int stride, int padding,
                       int dilation);
  bool embedding_backward(const float* indices, const float* grad_output, float* grad_table,
                          int index_count, int vocabulary, int dimension);
  bool recurrent_backward(const float* x, const float* weights, const float* recurrent_weights,
                          const float* bias, const float* cache_h, const float* cache_c,
                          const float* cache_gates, const float* grad_output,
                          float* grad_input, float* grad_weights, float* grad_recurrent_weights,
                          float* grad_bias, int kind, int batch, int time, int input_size,
                          int hidden_size);

 private:
  GpuRuntime() = default;
  ~GpuRuntime();

  GpuBackend backend_ = GpuBackend::Cpu;
  std::string info_ = "cpu";
  int attempted_index_ = -1;
  int device_index_ = -1;
  std::mutex mutex_;
  void* cuda_ctx_ = nullptr;  // opaque CudaState*, allocated by the .cpp
  void* metal_ctx_ = nullptr; // opaque MetalState*, allocated on macOS
  bool tensor_core_active_ = false;
};

// Grafo pequeno e explícito para encadear operações sem materializar no host.
// Os buffers pertencem ao chamador e precisam permanecer vivos até execute().
class GpuGraph {
 public:
  explicit GpuGraph(GpuRuntime& runtime = GpuRuntime::instance()) : runtime_(runtime) {}
  void clear() { operations_.clear(); }
  std::size_t size() const { return operations_.size(); }
  void add_relu(GpuBuffer& data, std::size_t count) {
    operations_.emplace_back([this, &data, count] { return runtime_.relu_resident(data, count); });
  }
  void add_gelu(GpuBuffer& data, std::size_t count) {
    operations_.emplace_back([this, &data, count] { return runtime_.gelu_resident(data, count); });
  }
  void add_add(const GpuBuffer& a, const GpuBuffer& b, GpuBuffer& c, std::size_t count) {
    operations_.emplace_back([this, &a, &b, &c, count] { return runtime_.add_resident(a, b, c, count); });
  }
  void add_gemm(const GpuBuffer& a, const GpuBuffer& b, GpuBuffer& c, int m, int k, int n) {
    operations_.emplace_back([this, &a, &b, &c, m, k, n] { return runtime_.gemm_resident(a, b, c, m, k, n); });
  }
  void add_batch_gemm(const GpuBuffer& a, const GpuBuffer& b, GpuBuffer& c,
                      int batches, int m, int k, int n) {
    operations_.emplace_back([this, &a, &b, &c, batches, m, k, n] {
      return runtime_.batch_gemm_resident(a, b, c, batches, m, k, n);
    });
  }
  void add_conv2d(const GpuBuffer& input, const GpuBuffer& weights, GpuBuffer& output,
                  int batch, int in_channels, int height, int width, int out_channels,
                  int kh, int kw, int out_h, int out_w, int stride, int padding, int dilation) {
    operations_.emplace_back([this, &input, &weights, &output, batch, in_channels, height, width,
                              out_channels, kh, kw, out_h, out_w, stride, padding, dilation] {
      return runtime_.conv2d_resident(input, weights, output, batch, in_channels, height, width,
                                      out_channels, kh, kw, out_h, out_w, stride, padding,
                                      dilation);
    });
  }
  void add_channel_bias(const GpuBuffer& input, const GpuBuffer& bias, GpuBuffer& output,
                        int batches, int channels, int spatial) {
    operations_.emplace_back([this, &input, &bias, &output, batches, channels, spatial] {
      return runtime_.add_channel_bias_resident(input, bias, output, batches, channels, spatial);
    });
  }
  void add_normalize(const GpuBuffer& input, const GpuBuffer& mean,
                     const GpuBuffer& variance, const GpuBuffer& gamma,
                     const GpuBuffer& beta, GpuBuffer& output, int batches,
                     int channels, int spatial, float epsilon) {
    operations_.emplace_back([this, &input, &mean, &variance, &gamma, &beta, &output,
                              batches, channels, spatial, epsilon] {
      return runtime_.normalize_resident(input, mean, variance, gamma, beta, output,
                                         batches, channels, spatial, epsilon);
    });
  }
  void add_maxpool2d(const GpuBuffer& input, GpuBuffer& output, int batches,
                     int channels, int height, int width, int window, int stride) {
    operations_.emplace_back([this, &input, &output, batches, channels, height, width,
                              window, stride] {
      return runtime_.maxpool2d_resident(input, output, batches, channels, height, width,
                                         window, stride);
    });
  }
  void add_reduce_sum(const GpuBuffer& input, GpuBuffer& output, std::size_t count) {
    operations_.emplace_back([this, &input, &output, count] {
      return runtime_.reduce_sum_resident(input, output, count);
    });
  }
  bool execute() {
    for (auto& operation : operations_)
      if (!operation()) return false;
    return true;
  }

 private:
  GpuRuntime& runtime_;
  std::vector<std::function<bool()>> operations_;
};

}  // namespace tilt::rt
