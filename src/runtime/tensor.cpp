#include "runtime/tensor.hpp"

#include <cmath>
#include <numeric>
#include <stdexcept>
#include <utility>

namespace tilt::rt {

namespace {

std::int64_t product(const std::vector<std::int64_t>& v) {
  return std::accumulate(v.begin(), v.end(), static_cast<std::int64_t>(1),
                         std::multiplies<std::int64_t>());
}

[[noreturn]] void die(const std::string& m) { throw std::runtime_error("tensor: " + m); }

// xorshift64* — deterministic, portable.
struct Rng {
  std::uint64_t s;
  explicit Rng(std::uint64_t seed) : s(seed ? seed : 0x9E3779B97F4A7C15ULL) {}
  float unit() {
    s ^= s >> 12;
    s ^= s << 25;
    s ^= s >> 27;
    std::uint64_t x = s * 0x2545F4914F6CDD1DULL;
    return static_cast<float>((x >> 40) / static_cast<double>(1ULL << 24));  // [0,1)
  }
};

}  // namespace

std::int64_t Tensor::size() const { return product(shape); }

std::string Tensor::shape_str() const {
  std::string r = "f32";
  for (std::int64_t d : shape) r += ", " + std::to_string(d);
  return r;
}

Tensor Tensor::filled(std::vector<std::int64_t> shape, float value) {
  Tensor t;
  t.shape = std::move(shape);
  t.data.assign(static_cast<std::size_t>(t.size()), value);
  return t;
}

Tensor Tensor::xavier(std::vector<std::int64_t> shape, std::int64_t fan_in, std::int64_t fan_out,
                      std::uint64_t seed) {
  Tensor t;
  t.shape = std::move(shape);
  const auto n = static_cast<std::size_t>(t.size());
  t.data.resize(n);
  const float limit =
      std::sqrt(6.0F / static_cast<float>(std::max<std::int64_t>(1, fan_in + fan_out)));
  Rng rng(seed);
  for (std::size_t k = 0; k < n; ++k) t.data[k] = (rng.unit() * 2.0F - 1.0F) * limit;
  return t;
}

namespace {

Tensor elementwise(const Tensor& a, const Tensor& b, char op) {
  auto apply = [op](float x, float y) -> float {
    switch (op) {
      case '+': return x + y;
      case '-': return x - y;
      case '*': return x * y;
      default: return y == 0.0F ? 0.0F : x / y;
    }
  };

  if (a.shape == b.shape) {
    Tensor out = a;
    for (std::size_t k = 0; k < out.data.size(); ++k) out.data[k] = apply(a.data[k], b.data[k]);
    return out;
  }
  if (b.size() == 1) return scalar_op(a, b.data[0], op);
  if (a.size() == 1) {
    Tensor out = b;
    for (std::size_t k = 0; k < out.data.size(); ++k) out.data[k] = apply(a.data[0], b.data[k]);
    return out;
  }
  // last-dim broadcast: a[..., N] (+) b[N]
  if (b.rank() == 1 && !a.shape.empty() && b.shape[0] == a.shape.back()) {
    Tensor out = a;
    const auto n = static_cast<std::size_t>(b.shape[0]);
    for (std::size_t k = 0; k < out.data.size(); ++k) {
      out.data[k] = apply(a.data[k], b.data[k % n]);
    }
    return out;
  }
  die("formas incompativeis para operacao elementwise (" + a.shape_str() + " vs " + b.shape_str() +
      ")");
}

}  // namespace

Tensor add(const Tensor& a, const Tensor& b) { return elementwise(a, b, '+'); }
Tensor sub(const Tensor& a, const Tensor& b) { return elementwise(a, b, '-'); }
Tensor mul(const Tensor& a, const Tensor& b) { return elementwise(a, b, '*'); }
Tensor div(const Tensor& a, const Tensor& b) { return elementwise(a, b, '/'); }

Tensor scalar_op(const Tensor& a, float s, char op) {
  Tensor out = a;
  for (float& v : out.data) {
    switch (op) {
      case '+': v += s; break;
      case '-': v -= s; break;
      case '*': v *= s; break;
      default: v = s == 0.0F ? 0.0F : v / s;
    }
  }
  return out;
}

Tensor matmul(const Tensor& a, const Tensor& b) {
  // Accept [k]x[k,n], [m,k]x[k], [m,k]x[k,n].
  std::int64_t m = 1;
  std::int64_t k = 0;
  std::int64_t n = 1;
  bool drop_row = false;
  bool drop_col = false;

  if (a.rank() == 1 && b.rank() == 2) {
    k = a.shape[0];
    n = b.shape[1];
    if (b.shape[0] != k) die("matmul: dimensao interna incompativel");
    drop_row = true;
  } else if (a.rank() == 2 && b.rank() == 1) {
    m = a.shape[0];
    k = a.shape[1];
    if (b.shape[0] != k) die("matmul: dimensao interna incompativel");
    drop_col = true;
  } else if (a.rank() == 2 && b.rank() == 2) {
    m = a.shape[0];
    k = a.shape[1];
    n = b.shape[1];
    if (b.shape[0] != k) die("matmul: dimensao interna incompativel");
  } else {
    die("matmul espera tensores 1D/2D");
  }

  Tensor out;
  out.shape = {m, n};
  out.data.assign(static_cast<std::size_t>(m * n), 0.0F);
  for (std::int64_t i = 0; i < m; ++i) {
    for (std::int64_t p = 0; p < k; ++p) {
      const float av = a.data[static_cast<std::size_t>(i * k + p)];
      for (std::int64_t j = 0; j < n; ++j) {
        out.data[static_cast<std::size_t>(i * n + j)] +=
            av * b.data[static_cast<std::size_t>(p * n + j)];
      }
    }
  }
  if (drop_row) out.shape = {n};
  if (drop_col) out.shape = {m};
  return out;
}

Tensor transpose2d(const Tensor& a) {
  if (a.rank() != 2) die("transposta espera um tensor 2D");
  const std::int64_t r = a.shape[0];
  const std::int64_t c = a.shape[1];
  Tensor out;
  out.shape = {c, r};
  out.data.resize(a.data.size());
  for (std::int64_t i = 0; i < r; ++i) {
    for (std::int64_t j = 0; j < c; ++j) {
      out.data[static_cast<std::size_t>(j * r + i)] = a.data[static_cast<std::size_t>(i * c + j)];
    }
  }
  return out;
}

Tensor reshape(const Tensor& a, std::vector<std::int64_t> shape) {
  Tensor out = a;
  out.shape = std::move(shape);
  if (out.size() != a.size()) die("reformar: numero de elementos difere");
  return out;
}

Tensor apply_unary(const Tensor& a, const std::string& fn) {
  Tensor out = a;
  for (float& v : out.data) {
    if (fn == "relu") {
      v = v > 0.0F ? v : 0.0F;
    } else if (fn == "sigmoide") {
      v = 1.0F / (1.0F + std::exp(-v));
    } else if (fn == "tanh") {
      v = std::tanh(v);
    } else if (fn == "gelu") {
      v = 0.5F * v * (1.0F + std::tanh(0.7978845608F * (v + 0.044715F * v * v * v)));
    } else if (fn == "silu") {
      v = v / (1.0F + std::exp(-v));
    } else {
      die("ativacao desconhecida '" + fn + "'");
    }
  }
  return out;
}

Tensor softmax_last(const Tensor& a) {
  if (a.shape.empty()) die("softmax espera um tensor nao escalar");
  const auto n = static_cast<std::size_t>(a.shape.back());
  Tensor out = a;
  for (std::size_t base = 0; base < out.data.size(); base += n) {
    float mx = out.data[base];
    for (std::size_t k = 1; k < n; ++k) mx = std::max(mx, out.data[base + k]);
    float sum = 0.0F;
    for (std::size_t k = 0; k < n; ++k) {
      out.data[base + k] = std::exp(out.data[base + k] - mx);
      sum += out.data[base + k];
    }
    for (std::size_t k = 0; k < n; ++k) out.data[base + k] /= sum;
  }
  return out;
}

float sum_all(const Tensor& a) {
  float s = 0.0F;
  for (float v : a.data) s += v;
  return s;
}

float mean_all(const Tensor& a) {
  return a.data.empty() ? 0.0F : sum_all(a) / static_cast<float>(a.data.size());
}

std::int64_t argmax_last(const Tensor& a) {
  if (a.data.empty()) return -1;
  const auto n = static_cast<std::size_t>(a.shape.empty() ? a.data.size() : a.shape.back());
  std::size_t best = 0;
  for (std::size_t k = 1; k < n && k < a.data.size(); ++k) {
    if (a.data[k] > a.data[best]) best = k;
  }
  return static_cast<std::int64_t>(best);
}

}  // namespace tilt::rt
