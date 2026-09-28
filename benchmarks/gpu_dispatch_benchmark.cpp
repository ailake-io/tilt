#include "runtime/gpu_runtime.hpp"
#include "runtime/tensor.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

template <typename F>
double median_ms(F&& fn, int repetitions) {
  fn();
  fn();
  std::vector<double> samples;
  samples.reserve(static_cast<std::size_t>(repetitions));
  for (int i = 0; i < repetitions; ++i) {
    const auto begin = std::chrono::steady_clock::now();
    fn();
    const auto end = std::chrono::steady_clock::now();
    samples.push_back(std::chrono::duration<double, std::milli>(end - begin).count());
  }
  std::sort(samples.begin(), samples.end());
  return samples[samples.size() / 2];
}

float checksum(const std::vector<float>& data) {
  return std::accumulate(data.begin(), data.end(), 0.0F);
}

void fill(tilt::rt::Tensor& t, int period) {
  for (std::size_t i = 0; i < t.data.size(); ++i)
    t.data[i] = static_cast<float>(static_cast<int>(i % static_cast<std::size_t>(period)) -
                                    period / 2) * 0.003F;
}

bool close(const tilt::rt::Tensor& expected, const std::vector<float>& actual) {
  if (expected.data.size() != actual.size()) return false;
  for (std::size_t i = 0; i < actual.size(); ++i) {
    const float tolerance = 1e-3F * std::max(1.0F, std::abs(expected.data[i]));
    if (!std::isfinite(actual[i]) || std::abs(expected.data[i] - actual[i]) > tolerance)
      return false;
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  const int repetitions = argc > 1 ? std::atoi(argv[1]) : 5;
  if (repetitions < 1 || repetitions > 100) return 2;
  auto& gpu = tilt::rt::GpuRuntime::instance();
  const bool has_gpu = gpu.ensure("cuda:0") && gpu.backend() == tilt::rt::GpuBackend::Cuda;
  std::cout << "backend,operation,shape,median_ms,checksum\n" << std::fixed << std::setprecision(6);
  for (int n : {32, 64, 128, 256, 512, 1024}) {
    auto a = tilt::rt::Tensor::zeros({n, n});
    auto b = tilt::rt::Tensor::zeros({n, n});
    fill(a, 17);
    fill(b, 23);
    tilt::rt::Tensor expected;
    const double cpu_ms = median_ms([&] { expected = tilt::rt::matmul(a, b); }, repetitions);
    std::vector<float> cpu_values(expected.data.begin(), expected.data.end());
    std::cout << "cpu,gemm," << n << "x" << n << "," << cpu_ms << ","
              << checksum(cpu_values) << "\n";
    if (has_gpu) {
      std::vector<float> output(static_cast<std::size_t>(n) * n);
      const auto run = [&] {
        if (!gpu.gemm(a.data.data(), b.data.data(), output.data(), n, n, n))
          throw std::runtime_error("GEMM CUDA falhou");
      };
      const double gpu_ms = median_ms(run, repetitions);
      if (!close(expected, output)) return 3;
      std::cout << "cuda,gemm," << n << "x" << n << "," << gpu_ms << ","
                << checksum(output) << "\n";
    }
  }

  for (int side : {16, 64}) {
    const int ci = side == 16 ? 4 : 16;
    const int co = side == 16 ? 8 : 32;
    const int out_side = side - 2;
    auto x = tilt::rt::Tensor::zeros({1, ci, side, side});
    auto w = tilt::rt::Tensor::zeros({co, ci, 3, 3});
    fill(x, 19);
    fill(w, 13);
    tilt::rt::Tensor expected;
    const double cpu_ms = median_ms([&] { expected = tilt::rt::conv2d(x, w); }, repetitions);
    std::vector<float> cpu_values(expected.data.begin(), expected.data.end());
    std::cout << "cpu,conv2d," << side << "x" << side << "," << cpu_ms << ","
              << checksum(cpu_values) << "\n";
    if (has_gpu) {
      std::vector<float> output(expected.data.size());
      const auto run = [&] {
        if (!gpu.conv2d(x.data.data(), w.data.data(), output.data(), 1, ci, side, side,
                        co, 3, 3, out_side, out_side, 1, 0, 1))
          throw std::runtime_error("conv2d CUDA falhou");
      };
      const double gpu_ms = median_ms(run, repetitions);
      if (!close(expected, output)) return 4;
      std::cout << "cuda,conv2d," << side << "x" << side << "," << gpu_ms << ","
                << checksum(output) << "\n";
    }
  }
  if (!has_gpu) std::cerr << "CUDA indisponivel: resultados CPU apenas\n";
  return 0;
}
