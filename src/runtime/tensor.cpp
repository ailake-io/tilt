#include "runtime/tensor.hpp"

#include <cmath>
#include <numeric>
#include <stdexcept>
#include <thread>
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
  // Accept [k]x[k,n], [m,k]x[k,n], [m,k]x[k]. 2D x 2D splits rows across
  // threads above a work threshold.
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
  out.data.assign(static_cast<size_t>(m * n), 0.0F);

  auto block = [&](std::int64_t i0, std::int64_t i1) {
    for (std::int64_t i = i0; i < i1; ++i) {
      for (std::int64_t p = 0; p < k; ++p) {
        const float av = a.data[static_cast<size_t>(i * k + p)];
        for (std::int64_t j = 0; j < n; ++j) {
          out.data[static_cast<size_t>(i * n + j)] +=
              av * b.data[static_cast<size_t>(p * n + j)];
        }
      }
    }
  };

  // ikj acima de ~256k FLOPs: divide as linhas entre as threads disponiveis.
  const std::int64_t work = m * n * k;
  unsigned hw = std::thread::hardware_concurrency();
  if (hw == 0) hw = 1;
  const int nthreads = static_cast<int>(std::min<unsigned>(hw, 8));
  if (!drop_row && !drop_col && work > (1 << 18) && nthreads > 1) {
    std::vector<std::thread> pool;
    pool.reserve(static_cast<size_t>(nthreads));
    const std::int64_t chunk = (m + nthreads - 1) / nthreads;
    for (int t = 0; t < nthreads; ++t) {
      const std::int64_t i0 = t * chunk;
      const std::int64_t i1 = std::min(m, i0 + chunk);
      if (i0 >= i1) break;
      pool.emplace_back(block, i0, i1);
    }
    for (std::thread& th : pool) th.join();
  } else {
    block(0, m);
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

Tensor layer_norm_last(const Tensor& a) {
  if (a.shape.empty()) die("norma_camada espera um tensor nao escalar");
  const auto n = static_cast<std::size_t>(a.shape.back());
  Tensor out = a;
  for (std::size_t base = 0; base < out.data.size(); base += n) {
    float mean = 0.0F;
    for (std::size_t k = 0; k < n; ++k) mean += out.data[base + k];
    mean /= static_cast<float>(n);
    float var = 0.0F;
    for (std::size_t k = 0; k < n; ++k) {
      const float d = out.data[base + k] - mean;
      var += d * d;
    }
    var /= static_cast<float>(n);
    const float inv = 1.0F / std::sqrt(var + 1e-5F);
    for (std::size_t k = 0; k < n; ++k) out.data[base + k] = (out.data[base + k] - mean) * inv;
  }
  return out;
}

Tensor conv2d(const Tensor& x, const Tensor& k, std::int64_t passo) {
  if (x.rank() != 4) die("conv2d espera uma entrada [N, C_in, H, W] (" + x.shape_str() + ")");
  if (k.rank() != 4) die("conv2d espera um nucleo [C_out, C_in, KH, KW] (" + k.shape_str() + ")");
  if (passo < 1) die("conv2d: passo deve ser >= 1");
  const std::int64_t n = x.shape[0];
  const std::int64_t cin = x.shape[1];
  const std::int64_t h = x.shape[2];
  const std::int64_t w = x.shape[3];
  const std::int64_t cout = k.shape[0];
  if (k.shape[1] != cin) {
    die("conv2d: nucleo tem " + std::to_string(k.shape[1]) + " canais de entrada, mas a entrada tem " +
        std::to_string(cin));
  }
  const std::int64_t kh = k.shape[2];
  const std::int64_t kw = k.shape[3];
  if (kh > h || kw > w) {
    die("conv2d: nucleo " + std::to_string(kh) + "x" + std::to_string(kw) +
        " maior que a entrada " + std::to_string(h) + "x" + std::to_string(w));
  }
  const std::int64_t oh = (h - kh) / passo + 1;
  const std::int64_t ow = (w - kw) / passo + 1;

  Tensor out = Tensor::zeros({n, cout, oh, ow});

  // Um bloco de trabalho = par (amostra, canal de saida).
  const std::int64_t jobs = n * cout;
  auto block = [&](std::int64_t j0, std::int64_t j1) {
    for (std::int64_t j = j0; j < j1; ++j) {
      const std::int64_t nn = j / cout;
      const std::int64_t co = j % cout;
      const float* xbase = x.data.data() + static_cast<std::size_t>(nn * cin * h * w);
      const float* kbase = k.data.data() + static_cast<std::size_t>(co * cin * kh * kw);
      float* obase = out.data.data() + static_cast<std::size_t>(j * oh * ow);
      for (std::int64_t i = 0; i < oh; ++i) {
        for (std::int64_t jj = 0; jj < ow; ++jj) {
          float acc = 0.0F;
          for (std::int64_t c = 0; c < cin; ++c) {
            const float* xc = xbase + static_cast<std::size_t>(c * h * w);
            const float* kc = kbase + static_cast<std::size_t>(c * kh * kw);
            for (std::int64_t u = 0; u < kh; ++u) {
              for (std::int64_t v = 0; v < kw; ++v) {
                acc += xc[static_cast<std::size_t>((i * passo + u) * w + (jj * passo + v))] *
                       kc[static_cast<std::size_t>(u * kw + v)];
              }
            }
          }
          obase[static_cast<std::size_t>(i * ow + jj)] = acc;
        }
      }
    }
  };

  // Acima de ~256k FLOPs: divide os pares (amostra, canal de saida) entre threads.
  const std::int64_t work = jobs * oh * ow * cin * kh * kw;
  unsigned hw = std::thread::hardware_concurrency();
  if (hw == 0) hw = 1;
  const int nthreads = static_cast<int>(std::min<unsigned>(hw, 8));
  if (work > (1 << 18) && nthreads > 1) {
    std::vector<std::thread> pool;
    pool.reserve(static_cast<size_t>(nthreads));
    const std::int64_t chunk = (jobs + nthreads - 1) / nthreads;
    for (int t = 0; t < nthreads; ++t) {
      const std::int64_t j0 = t * chunk;
      const std::int64_t j1 = std::min(jobs, j0 + chunk);
      if (j0 >= j1) break;
      pool.emplace_back(block, j0, j1);
    }
    for (std::thread& th : pool) th.join();
  } else {
    block(0, jobs);
  }
  return out;
}

namespace {

// Parametro por canal: escalar (broadcast) ou rank-1 [C].
float param_por_canal(const Tensor& p, std::int64_t c, std::int64_t canais, const char* nome) {
  if (p.size() == 1) return p.data[0];
  if (p.rank() == 1 && p.shape[0] == canais) return p.data[static_cast<std::size_t>(c)];
  die(std::string("norma_lote: '") + nome + "' deve ser escalar ou [" + std::to_string(canais) +
      "] (" + p.shape_str() + ")");
}

}  // namespace

Tensor norma_lote(const Tensor& x, const Tensor& gama, const Tensor& beta, const Tensor& media,
                  const Tensor& var, float eps, bool em_treino) {
  if (x.rank() < 2) die("norma_lote espera um tensor [N, C, ...] (" + x.shape_str() + ")");
  const std::int64_t n = x.shape[0];
  const std::int64_t canais = x.shape[1];
  const std::int64_t rest = x.size() / (n * canais);  // elementos por (amostra, canal)

  std::vector<float> mean(static_cast<size_t>(canais), 0.0F);
  std::vector<float> varc(static_cast<size_t>(canais), 0.0F);
  if (em_treino) {
    // Media/variancia populacional por canal sobre N * rest.
    for (std::int64_t nn = 0; nn < n; ++nn) {
      for (std::int64_t c = 0; c < canais; ++c) {
        const float* base = x.data.data() + static_cast<std::size_t>((nn * canais + c) * rest);
        for (std::int64_t k = 0; k < rest; ++k) mean[static_cast<size_t>(c)] += base[k];
      }
    }
    const float cnt = static_cast<float>(n * rest);
    for (std::int64_t c = 0; c < canais; ++c) mean[static_cast<size_t>(c)] /= cnt;
    for (std::int64_t nn = 0; nn < n; ++nn) {
      for (std::int64_t c = 0; c < canais; ++c) {
        const float* base = x.data.data() + static_cast<std::size_t>((nn * canais + c) * rest);
        float acc = 0.0F;
        for (std::int64_t k = 0; k < rest; ++k) {
          const float d = base[k] - mean[static_cast<size_t>(c)];
          acc += d * d;
        }
        varc[static_cast<size_t>(c)] += acc;
      }
    }
    for (std::int64_t c = 0; c < canais; ++c) varc[static_cast<size_t>(c)] /= cnt;
  } else {
    for (std::int64_t c = 0; c < canais; ++c) {
      mean[static_cast<size_t>(c)] = param_por_canal(media, c, canais, "media");
      varc[static_cast<size_t>(c)] = param_por_canal(var, c, canais, "variancia");
    }
  }

  Tensor out = x;
  for (std::int64_t nn = 0; nn < n; ++nn) {
    for (std::int64_t c = 0; c < canais; ++c) {
      const float g = param_por_canal(gama, c, canais, "gama");
      const float b = param_por_canal(beta, c, canais, "beta");
      const float inv = 1.0F / std::sqrt(varc[static_cast<size_t>(c)] + eps);
      float* base = out.data.data() + static_cast<std::size_t>((nn * canais + c) * rest);
      for (std::int64_t k = 0; k < rest; ++k) {
        base[k] = g * (base[k] - mean[static_cast<size_t>(c)]) * inv + b;
      }
    }
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
