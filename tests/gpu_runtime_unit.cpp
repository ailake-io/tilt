#include "runtime/gpu_runtime.hpp"
#include "runtime/tensor.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>

int main() {
  auto& gpu = tilt::rt::GpuRuntime::instance();
  if (gpu.ensure("cpu")) return 1;
  const char* env = std::getenv("TILT_GPU");
  const std::string mode = env ? env : "off";
  if (mode == "metal") {
    if (!gpu.ensure("metal")) {
      std::cout << "Metal indisponivel; teste de hardware ignorado\n";
      return 0;
    }
  }
  if (mode == "auto" && gpu.ensure("cuda:999")) return 2;
  if (mode != "metal" && !gpu.ensure("cuda:0")) {
    if (mode == "auto") {
      std::cout << "CUDA indisponivel; teste de hardware ignorado\n";
      return 0;
    }
    return 3;
  }
  if (mode == "auto" && gpu.backend() != tilt::rt::GpuBackend::Cuda) return 4;
  if (mode == "fake" && gpu.backend() != tilt::rt::GpuBackend::Fake) return 5;
  if (mode == "metal" && gpu.backend() != tilt::rt::GpuBackend::Metal) return 21;
  const float a[] = {1, 2, 3, 4, 5, 6};
  const float b[] = {7, 8, 9, 10, 11, 12};
  float out[4] = {};
  if (!gpu.gemm(a, b, out, 2, 3, 2)) return 6;
  const float expected[] = {58, 64, 139, 154};
  for (int i = 0; i < 4; ++i) {
    if (std::abs(out[i] - expected[i]) > 0.001F) return 7;
  }
  float mixed_a[] = {1.125F, 2.5F};
  float mixed_b[] = {0.25F, 4.0F};
  float mixed_out = 0;
  if (!gpu.gemm_mixed(mixed_a, mixed_b, &mixed_out, 1, 2, 1) ||
      std::abs(mixed_out - 10.28125F) > 0.001F) return 8;
  const float rounded_input = 1.1F;
  const float one = 1.0F;
  if (!gpu.gemm_mixed(&rounded_input, &one, &mixed_out, 1, 1, 1) ||
      std::abs(mixed_out - 1.099609375F) > 0.000001F) return 11;
  const float too_large = 70000.0F;
  if (gpu.gemm_mixed(&too_large, &one, &mixed_out, 1, 1, 1)) return 12;
  float values[] = {-2, 0, 3};
  if (!gpu.relu(values, 3) || values[0] != 0 || values[1] != 0 || values[2] != 3) return 9;
  float gelu_values[] = {-1, 0, 1};
  if (!gpu.gelu(gelu_values, 3) || std::abs(gelu_values[0] + 0.158808F) > 0.002F ||
      std::abs(gelu_values[1]) > 0.001F || std::abs(gelu_values[2] - 0.841192F) > 0.002F) return 14;
  float added[3] = {};
  const float lhs[] = {1, -2, 3};
  const float rhs[] = {4, 2, -5};
  if (!gpu.add(lhs, rhs, added, 3) || added[0] != 5 || added[1] != 0 || added[2] != -2) return 15;
  const float image[] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
  const float filter[] = {1, 0, 0, -1};
  float convolution[4] = {};
  if (!gpu.conv2d(image, filter, convolution, 1, 1, 3, 3, 1, 2, 2, 2, 2, 1, 0, 1)) return 16;
  for (float v : convolution) if (std::abs(v + 4.0F) > 0.001F) return 17;
  float dilated = 0;
  if (!gpu.conv2d(image, filter, &dilated, 1, 1, 3, 3, 1, 2, 2, 1, 1, 1, 0, 2) ||
      std::abs(dilated + 8.0F) > 0.001F) return 18;
  tilt::rt::Tensor x = tilt::rt::Tensor::zeros({2, 2, 4, 5});
  tilt::rt::Tensor w = tilt::rt::Tensor::zeros({3, 2, 2, 3});
  for (std::size_t i = 0; i < x.data.size(); ++i) x.data[i] = static_cast<float>(i % 13) * 0.1F;
  for (std::size_t i = 0; i < w.data.size(); ++i) w.data[i] = static_cast<float>(i % 7) * 0.2F;
  const auto reference = tilt::rt::conv2d(x, w, 2, 1, 2);
  tilt::rt::Tensor result = tilt::rt::Tensor::zeros(reference.shape);
  if (!gpu.conv2d(x.data.data(), w.data.data(), result.data.data(), 2, 2, 4, 5,
                  3, 2, 3, 2, 2, 2, 1, 2)) return 19;
  for (std::size_t i = 0; i < result.data.size(); ++i) {
    if (std::abs(result.data[i] - reference.data[i]) > 0.0001F) return 20;
  }
  if (gpu.residency_available()) {
    tilt::rt::GpuBuffer da, db, dc;
    const float ra[] = {1, 2, 3, 4};
    const float rb[] = {5, 6, 7, 8};
    float rc[4] = {};
    if (!gpu.upload(ra, 4, da) || !gpu.upload(rb, 4, db) ||
        !gpu.gemm_resident(da, db, dc, 2, 2, 2) || !gpu.download(dc, rc, 4) ||
        std::abs(rc[0] - 19.0F) > 0.001F || std::abs(rc[3] - 50.0F) > 0.001F ||
        !gpu.release(da) || !gpu.release(db) || !gpu.release(dc)) return 22;
  }
  float ba[] = {1, 2, 3, 4, 5, 6, 7, 8};
  float bb[] = {1, 0, 0, 1, 2, 0, 0, 2};
  float bc[8] = {};
  if (!gpu.batch_gemm(ba, bb, bc, 2, 2, 2, 2) || bc[0] != 1 || bc[3] != 4 ||
      bc[4] != 10 || bc[7] != 16) return 23;
  const float mean[] = {2, 3};
  const float variance[] = {1, 4};
  const float gamma[] = {1, 2};
  const float beta[] = {0, 1};
  const float ni[] = {1, 2, 3, 5};
  float no[4] = {};
  if (!gpu.normalize(ni, mean, variance, gamma, beta, no, 1, 2, 2, 1e-5F) ||
      std::abs(no[0] + 1.0F) > 0.01F || std::abs(no[3] - 3.0F) > 0.01F) return 24;
  const float pi[] = {1, 3, 2, 4, 5, 0, 7, 6, 2, 8, 1, 9, 4, 3, 2, 1};
  float po[4] = {};
  if (!gpu.maxpool2d(pi, po, 1, 1, 4, 4, 2, 2) || po[0] != 5 || po[3] != 9) return 25;
  float sum = 0;
  if (!gpu.reduce_sum(ba, &sum, 8) || std::abs(sum - 36.0F) > 0.001F) return 26;
  const float dense_x[] = {1, 2, 3, 4};
  const float dense_w[] = {1, 2, 3, 4};
  const float dense_g[] = {1, 2, 1, 2};
  float dense_gx[4] = {}, dense_gw[4] = {}, dense_gb[2] = {};
  if (!gpu.dense_backward(dense_x, dense_w, dense_g, dense_gx, dense_gw, dense_gb, 2, 2, 2) ||
      std::abs(dense_gx[0] - 5.0F) > 0.001F || std::abs(dense_gx[3] - 11.0F) > 0.001F ||
      std::abs(dense_gb[0] - 2.0F) > 0.001F) return 27;
  if (gpu.backend() == tilt::rt::GpuBackend::Fake || gpu.backend() == tilt::rt::GpuBackend::Cuda) {
    tilt::rt::Tensor cbx = tilt::rt::Tensor::zeros({1, 1, 3, 3});
    tilt::rt::Tensor cbw = tilt::rt::Tensor::zeros({1, 1, 2, 2});
    for (std::size_t i = 0; i < cbx.data.size(); ++i) cbx.data[i] = static_cast<float>(i + 1);
    cbw.data = {1, 0, 0, -1};
    auto cby = tilt::rt::conv2d(cbx, cbw, 1, 0, 1);
    tilt::rt::Tensor cgy = tilt::rt::Tensor::zeros(cby.shape);
    for (float& v : cgy.data) v = 1.0F;
    tilt::rt::Tensor cgx = tilt::rt::Tensor::zeros(cbx.shape);
    tilt::rt::Tensor cgw = tilt::rt::Tensor::zeros(cbw.shape);
    tilt::rt::Tensor cgb = tilt::rt::Tensor::zeros({1});
    if (!gpu.conv2d_backward(cbx.data.data(), cbw.data.data(), cgy.data.data(), cgx.data.data(),
                             cgw.data.data(), cgb.data.data(), 1, 1, 3, 3, 1, 2, 2, 2, 2,
                             1, 0, 1) ||
        std::abs(cgb.data[0] - 4.0F) > 0.001F || std::abs(cgw.data[0] - 12.0F) > 0.001F ||
        std::abs(cgw.data[3] - 28.0F) > 0.001F)
      return 28;
    const float ids[] = {0, 1, 0};
    const float eg[] = {1, 2, 3, 4, 5, 6};
    float et[4] = {};
    if (!gpu.embedding_backward(ids, eg, et, 3, 2, 2) ||
        std::abs(et[0] - 6.0F) > 0.001F || std::abs(et[1] - 8.0F) > 0.001F ||
        std::abs(et[2] - 3.0F) > 0.001F || std::abs(et[3] - 4.0F) > 0.001F)
      return 29;
    tilt::rt::Tensor rx = tilt::rt::Tensor::zeros({2, 3, 2});
    tilt::rt::Tensor rw = tilt::rt::Tensor::zeros({2, 2});
    tilt::rt::Tensor ru = tilt::rt::Tensor::zeros({2, 2});
    tilt::rt::Tensor rb = tilt::rt::Tensor::zeros({2});
    for (std::size_t i = 0; i < rx.data.size(); ++i) rx.data[i] = 0.1F * static_cast<float>(i + 1);
    for (std::size_t i = 0; i < rw.data.size(); ++i) rw.data[i] = 0.05F * static_cast<float>(i + 1);
    for (std::size_t i = 0; i < ru.data.size(); ++i) ru.data[i] = 0.03F * static_cast<float>(i + 1);
    rb.data = {0.1F, -0.2F};
    tilt::rt::RecurrentCache rc;
    (void)tilt::rt::recorrente(rx, rw, ru, rb, tilt::rt::RecurrentKind::Rnn, &rc);
    tilt::rt::Tensor rgy = tilt::rt::Tensor::zeros({2, 2});
    for (float& v : rgy.data) v = 0.25F;
    tilt::rt::Tensor rgx, rgw, rgu, rgb;
    tilt::rt::recorrente_backward(rx, rw, ru, rb, tilt::rt::RecurrentKind::Rnn, rc, rgy,
                                  rgx, rgw, rgu, rgb);
    tilt::rt::Tensor rgx_gpu = tilt::rt::Tensor::zeros(rx.shape);
    tilt::rt::Tensor rgw_gpu = tilt::rt::Tensor::zeros(rw.shape);
    tilt::rt::Tensor rgu_gpu = tilt::rt::Tensor::zeros(ru.shape);
    tilt::rt::Tensor rgb_gpu = tilt::rt::Tensor::zeros(rb.shape);
    if (!gpu.recurrent_backward(rx.data.data(), rw.data.data(), ru.data.data(), rb.data.data(),
                                rc.estados_h.data(), nullptr, nullptr, rgy.data.data(),
                                rgx_gpu.data.data(), rgw_gpu.data.data(), rgu_gpu.data.data(),
                                rgb_gpu.data.data(), 0, 2, 3, 2, 2))
      return 30;
    for (std::size_t i = 0; i < rgx.data.size(); ++i)
      if (std::abs(rgx.data[i] - rgx_gpu.data[i]) > 0.001F) return 31;
    for (std::size_t i = 0; i < rgw.data.size(); ++i)
      if (std::abs(rgw.data[i] - rgw_gpu.data[i]) > 0.001F) return 32;
    for (std::size_t i = 0; i < rgu.data.size(); ++i)
      if (std::abs(rgu.data[i] - rgu_gpu.data[i]) > 0.001F) return 33;
    for (std::size_t i = 0; i < rgb.data.size(); ++i)
      if (std::abs(rgb.data[i] - rgb_gpu.data[i]) > 0.001F) return 34;
  }
  if (mode == "auto" && gpu.ensure("cuda:999")) return 10;
  if (gpu.ensure("cpu")) return 13;
  std::cout << gpu.info() << ": GEMM, conv2d, ReLU, GELU e soma ok\n";
  return 0;
}
