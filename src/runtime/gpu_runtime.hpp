#pragma once

#include <cstddef>
#include <string>

namespace tilt::rt {

enum class GpuBackend { Cpu, Cuda, Fake };

// GPU dispatch. The CUDA path binds libcuda + libnvrtc at runtime (dlopen) and
// compiles small CUDA-C kernels on first use; when no driver is present it
// reports unavailable and callers fall back to the CPU kernels in tensor.cpp.
// `TILT_GPU` overrides detection: off | auto | fake.
class GpuRuntime {
 public:
  static GpuRuntime& instance();

  // Attempts initialization for the given device string ("auto", "gpu",
  // "cuda:N", "cuda"). Returns true if a GPU backend is active.
  bool ensure(const std::string& device);

  bool available() const { return backend_ != GpuBackend::Cpu; }
  GpuBackend backend() const { return backend_; }
  const std::string& info() const { return info_; }

  // C = A(m x k) * B(k x n), row-major f32. Returns false if it could not run
  // on the GPU (caller should use the CPU path).
  bool gemm(const float* a, const float* b, float* c, int m, int k, int n);
  bool relu(float* data, std::size_t n);

 private:
  GpuRuntime() = default;

  GpuBackend backend_ = GpuBackend::Cpu;
  std::string info_ = "cpu";
  bool tried_ = false;
  void* cuda_ctx_ = nullptr;  // opaque CudaState*, allocated by the .cpp
};

}  // namespace tilt::rt
