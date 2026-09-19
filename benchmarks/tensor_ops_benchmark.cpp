#include "runtime/tensor.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace {

struct Stats {
  double median_ms = 0.0;
  double mean_ms = 0.0;
  double checksum = 0.0;
};

std::size_t elements(const std::vector<std::int64_t>& shape) {
  std::size_t n = 1;
  for (std::int64_t dim : shape) n *= static_cast<std::size_t>(dim);
  return n;
}

tilt::rt::Tensor linear_tensor(const std::vector<std::int64_t>& shape, int period,
                               float scale) {
  tilt::rt::Tensor out;
  out.shape = shape;
  out.data.resize(elements(shape));
  for (std::size_t i = 0; i < out.data.size(); ++i) {
    out.data[i] = static_cast<float>(static_cast<int>(i % static_cast<std::size_t>(period)) -
                                     period / 2) *
                  scale;
  }
  return out;
}

template <typename Fn>
Stats measure(Fn&& fn, int warmup, int iterations) {
  for (int i = 0; i < warmup; ++i) {
    tilt::rt::Tensor out = fn();
    (void)tilt::rt::sum_all(out);
  }
  std::vector<double> samples;
  samples.reserve(static_cast<std::size_t>(iterations));
  double checksum = 0.0;
  for (int i = 0; i < iterations; ++i) {
    const auto start = std::chrono::steady_clock::now();
    tilt::rt::Tensor out = fn();
    checksum += static_cast<double>(tilt::rt::sum_all(out));
    const auto stop = std::chrono::steady_clock::now();
    samples.push_back(
        std::chrono::duration<double, std::milli>(stop - start).count());
  }
  std::sort(samples.begin(), samples.end());
  const std::size_t middle = samples.size() / 2;
  const double median =
      samples.size() % 2 == 0 ? (samples[middle - 1] + samples[middle]) / 2.0
                              : samples[middle];
  double mean = 0.0;
  for (double sample : samples) mean += sample;
  mean /= static_cast<double>(samples.size());
  return {median, mean, checksum / static_cast<double>(iterations)};
}

void print_result(const std::string& operation, const std::string& shape, int iterations,
                  const Stats& stats) {
  std::cout << std::setprecision(10) << "{\"backend\":\"tilt\",\"operation\":\""
            << operation << "\",\"shape\":\"" << shape << "\",\"iterations\":"
            << iterations << ",\"median_ms\":" << stats.median_ms
            << ",\"mean_ms\":" << stats.mean_ms << ",\"checksum\":"
            << stats.checksum << "}\n";
}

int parse_count(const char* text, const char* option, bool allow_zero) {
  char* end = nullptr;
  const long value = std::strtol(text, &end, 10);
  const long minimum = allow_zero ? 0 : 1;
  if (*text == '\0' || *end != '\0' || value < minimum || value > 100000) {
    std::cerr << option << " deve ser um inteiro nao negativo\n";
    std::exit(2);
  }
  return static_cast<int>(value);
}

}  // namespace

int main(int argc, char** argv) {
  int iterations = 20;
  int warmup = 5;
  bool quick = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--iterations" && i + 1 < argc) {
      iterations = parse_count(argv[++i], "--iterations", false);
    } else if (arg == "--warmup" && i + 1 < argc) {
      warmup = parse_count(argv[++i], "--warmup", true);
    } else if (arg == "--quick") {
      quick = true;
    } else {
      std::cerr << "uso: tensor_ops_benchmark [--iterations N] [--warmup N] [--quick]\n";
      return 2;
    }
  }

  const std::int64_t matrix = quick ? 64 : 256;
  const std::int64_t channels_in = quick ? 4 : 16;
  const std::int64_t channels_out = quick ? 8 : 32;
  const std::int64_t height = quick ? 16 : 64;
  const std::int64_t width = quick ? 16 : 64;
  const std::string matmul_shape = std::to_string(matrix) + "x" + std::to_string(matrix) +
                                   "x" + std::to_string(matrix);
  const std::string conv_shape =
      std::to_string(height) + "x" + std::to_string(width) + "x" +
      std::to_string(channels_in) + "x" + std::to_string(channels_out);

  const tilt::rt::Tensor mat_a = linear_tensor({matrix, matrix}, 17, 0.01F);
  const tilt::rt::Tensor mat_b = linear_tensor({matrix, matrix}, 23, 0.007F);
  const tilt::rt::Tensor conv_x =
      linear_tensor({1, channels_in, height, width}, 19, 0.01F);
  const tilt::rt::Tensor conv_k =
      linear_tensor({channels_out, channels_in, 3, 3}, 13, 0.005F);

  const Stats matmul_stats =
      measure([&] { return tilt::rt::matmul(mat_a, mat_b); }, warmup, iterations);
  print_result("matmul", matmul_shape, iterations, matmul_stats);
  const Stats conv_stats =
      measure([&] { return tilt::rt::conv2d(conv_x, conv_k); }, warmup, iterations);
  print_result("conv2d", conv_shape, iterations, conv_stats);
  return 0;
}
