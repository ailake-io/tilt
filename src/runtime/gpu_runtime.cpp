#include "runtime/gpu_runtime.hpp"

#include <cstdlib>
#include <cstring>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <dlfcn.h>
#define TILT_HAS_DLOPEN 1
#else
#define TILT_HAS_DLOPEN 0
#endif

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
)cuda";

// --- CUDA Driver API + NVRTC, bound lazily via dlopen -----------------------

struct CudaState {
#if TILT_HAS_DLOPEN
  void* lib_cuda = nullptr;
  void* lib_nvrtc = nullptr;

  int (*cuInit)(unsigned) = nullptr;
  int (*cuDeviceGetCount)(int*) = nullptr;
  int (*cuDeviceGet)(int*, int) = nullptr;
  int (*cuCtxCreate)(void**, unsigned, int) = nullptr;
  int (*cuMemAlloc)(void**, std::size_t) = nullptr;
  int (*cuMemFree)(void*) = nullptr;
  int (*cuMemcpyHtoD)(void*, const void*, std::size_t) = nullptr;
  int (*cuMemcpyDtoH)(void*, const void*, std::size_t) = nullptr;
  int (*cuModuleLoadData)(void**, const void*) = nullptr;
  int (*cuModuleGetFunction)(void**, void*, const char*) = nullptr;
  int (*cuLaunchKernel)(void*, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned,
                        void*, void**, void**) = nullptr;
  int (*cuCtxSynchronize)() = nullptr;

  int (*nvrtcCreateProgram)(void**, const char*, const char*, int, const char**,
                            const char**) = nullptr;
  int (*nvrtcCompileProgram)(void*, int, const char**) = nullptr;
  int (*nvrtcGetPTXSize)(void*, std::size_t*) = nullptr;
  int (*nvrtcGetPTX)(void*, char*) = nullptr;

  void* ctx = nullptr;
  void* module = nullptr;
  void* fn_gemm = nullptr;
  void* fn_relu = nullptr;

  template <typename T>
  bool bind(void* lib, T& fp, const char* name) {
    fp = reinterpret_cast<T>(::dlsym(lib, name));
    return fp != nullptr;
  }

  bool init() {
    lib_cuda = ::dlopen("libcuda.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (!lib_cuda) lib_cuda = ::dlopen("libcuda.so", RTLD_NOW | RTLD_GLOBAL);
    if (!lib_cuda) lib_cuda = ::dlopen("nvcuda.dll", RTLD_NOW | RTLD_GLOBAL);
    if (!lib_cuda) return false;

    bool ok = bind(lib_cuda, cuInit, "cuInit") && bind(lib_cuda, cuDeviceGetCount, "cuDeviceGetCount") &&
              bind(lib_cuda, cuDeviceGet, "cuDeviceGet") &&
              bind(lib_cuda, cuCtxCreate, "cuCtxCreate_v2") &&
              bind(lib_cuda, cuMemAlloc, "cuMemAlloc_v2") && bind(lib_cuda, cuMemFree, "cuMemFree_v2") &&
              bind(lib_cuda, cuMemcpyHtoD, "cuMemcpyHtoD_v2") &&
              bind(lib_cuda, cuMemcpyDtoH, "cuMemcpyDtoH_v2") &&
              bind(lib_cuda, cuModuleLoadData, "cuModuleLoadData") &&
              bind(lib_cuda, cuModuleGetFunction, "cuModuleGetFunction") &&
              bind(lib_cuda, cuLaunchKernel, "cuLaunchKernel") &&
              bind(lib_cuda, cuCtxSynchronize, "cuCtxSynchronize");
    if (!ok) return false;

    if (cuInit(0) != 0) return false;
    int count = 0;
    if (cuDeviceGetCount(&count) != 0 || count <= 0) return false;
    int dev = 0;
    if (cuDeviceGet(&dev, 0) != 0) return false;
    if (cuCtxCreate(&ctx, 0, dev) != 0) return false;

    lib_nvrtc = ::dlopen("libnvrtc.so", RTLD_NOW);
    if (!lib_nvrtc) return false;  // no runtime compiler -> stay on CPU
    if (!(bind(lib_nvrtc, nvrtcCreateProgram, "nvrtcCreateProgram") &&
          bind(lib_nvrtc, nvrtcCompileProgram, "nvrtcCompileProgram") &&
          bind(lib_nvrtc, nvrtcGetPTXSize, "nvrtcGetPTXSize") &&
          bind(lib_nvrtc, nvrtcGetPTX, "nvrtcGetPTX"))) {
      return false;
    }

    void* prog = nullptr;
    if (nvrtcCreateProgram(&prog, kKernelSrc, "tilt.cu", 0, nullptr, nullptr) != 0) return false;
    if (nvrtcCompileProgram(prog, 0, nullptr) != 0) return false;
    std::size_t ptx_size = 0;
    if (nvrtcGetPTXSize(prog, &ptx_size) != 0) return false;
    std::vector<char> ptx(ptx_size);
    if (nvrtcGetPTX(prog, ptx.data()) != 0) return false;
    if (cuModuleLoadData(&module, ptx.data()) != 0) return false;
    if (cuModuleGetFunction(&fn_gemm, module, "tilt_sgemm") != 0) return false;
    if (cuModuleGetFunction(&fn_relu, module, "tilt_relu") != 0) return false;
    return true;
  }

  bool gemm(const float* a, const float* b, float* c, int m, int k, int n) {
    void *da = nullptr, *db = nullptr, *dc = nullptr;
    const std::size_t sa = sizeof(float) * static_cast<std::size_t>(m) * k;
    const std::size_t sb = sizeof(float) * static_cast<std::size_t>(k) * n;
    const std::size_t sc = sizeof(float) * static_cast<std::size_t>(m) * n;
    if (cuMemAlloc(&da, sa) || cuMemAlloc(&db, sb) || cuMemAlloc(&dc, sc)) return false;
    cuMemcpyHtoD(da, a, sa);
    cuMemcpyHtoD(db, b, sb);
    unsigned bx = 16, by = 16;
    unsigned gx = (static_cast<unsigned>(n) + bx - 1) / bx;
    unsigned gy = (static_cast<unsigned>(m) + by - 1) / by;
    void* params[] = {&da, &db, &dc, &m, &k, &n};
    int rc = cuLaunchKernel(fn_gemm, gx, gy, 1, bx, by, 1, 0, nullptr, params, nullptr);
    cuCtxSynchronize();
    if (rc == 0) cuMemcpyDtoH(c, dc, sc);
    cuMemFree(da);
    cuMemFree(db);
    cuMemFree(dc);
    return rc == 0;
  }

  bool relu(float* d, std::size_t n) {
    void* dd = nullptr;
    const std::size_t bytes = sizeof(float) * n;
    if (cuMemAlloc(&dd, bytes)) return false;
    cuMemcpyHtoD(dd, d, bytes);
    int ni = static_cast<int>(n);
    unsigned threads = 256;
    unsigned blocks = (static_cast<unsigned>(n) + threads - 1) / threads;
    void* params[] = {&dd, &ni};
    int rc = cuLaunchKernel(fn_relu, blocks, 1, 1, threads, 1, 1, 0, nullptr, params, nullptr);
    cuCtxSynchronize();
    if (rc == 0) cuMemcpyDtoH(d, dd, bytes);
    cuMemFree(dd);
    return rc == 0;
  }
#else
  bool init() { return false; }
  bool gemm(const float*, const float*, float*, int, int, int) { return false; }
  bool relu(float*, std::size_t) { return false; }
#endif
};

std::string env_gpu() {
  const char* v = std::getenv("TILT_GPU");
  return v ? std::string(v) : std::string("off");
}

bool wants_gpu(const std::string& device) {
  return device == "auto" || device == "gpu" || device == "cuda" || device.rfind("cuda:", 0) == 0;
}

}  // namespace

GpuRuntime& GpuRuntime::instance() {
  static GpuRuntime rt;
  return rt;
}

bool GpuRuntime::ensure(const std::string& device) {
  if (tried_) return available();
  tried_ = true;

  const std::string mode = env_gpu();
  if (mode == "off") return false;
  if (mode == "fake") {
    backend_ = GpuBackend::Fake;
    info_ = "fake (kernels de CPU pelo caminho de dispatch da GPU)";
    return true;
  }
  // "auto": honour it only when the program asked for a GPU device.
  if (!wants_gpu(device)) return false;

  auto* st = new CudaState();
  if (st->init()) {
    cuda_ctx_ = st;
    backend_ = GpuBackend::Cuda;
    info_ = "cuda (libcuda + nvrtc)";
    return true;
  }
  delete st;
  return false;
}

bool GpuRuntime::gemm(const float* a, const float* b, float* c, int m, int k, int n) {
  if (backend_ == GpuBackend::Fake) {
    cpu_gemm(a, b, c, m, k, n);
    return true;
  }
  if (backend_ == GpuBackend::Cuda && cuda_ctx_) {
    return static_cast<CudaState*>(cuda_ctx_)->gemm(a, b, c, m, k, n);
  }
  return false;
}

bool GpuRuntime::relu(float* data, std::size_t n) {
  if (backend_ == GpuBackend::Fake) {
    cpu_relu(data, n);
    return true;
  }
  if (backend_ == GpuBackend::Cuda && cuda_ctx_) {
    return static_cast<CudaState*>(cuda_ctx_)->relu(data, n);
  }
  return false;
}

}  // namespace tilt::rt
