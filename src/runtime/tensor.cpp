#include "runtime/tensor.hpp"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <numeric>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>

namespace tilt::rt {

namespace detail {

namespace {

struct TensorBufferPool {
  std::mutex mu;
  std::unordered_map<std::size_t, std::vector<float*>> free;
  std::size_t cached_bytes = 0;
  static constexpr std::size_t kMaxBytes = 64U * 1024U * 1024U;
  static constexpr std::size_t kMaxPerSize = 8;
};

TensorBufferPool& tensor_buffer_pool() {
  // Leaked intentionally: Tensor destructors from other static objects cannot
  // race the pool's teardown at process exit. The cap bounds retained memory.
  static TensorBufferPool* pool = new TensorBufferPool();
  return *pool;
}

}  // namespace

float* tensor_buffer_allocate(std::size_t n) {
  if (n == 0) return static_cast<float*>(::operator new(0));
  TensorBufferPool& pool = tensor_buffer_pool();
  {
    std::lock_guard<std::mutex> lock(pool.mu);
    auto it = pool.free.find(n);
    if (it != pool.free.end() && !it->second.empty()) {
      float* p = it->second.back();
      it->second.pop_back();
      pool.cached_bytes -= n * sizeof(float);
      if (it->second.empty()) pool.free.erase(it);
      return p;
    }
  }
  return static_cast<float*>(::operator new(n * sizeof(float)));
}

void tensor_buffer_deallocate(float* p, std::size_t n) noexcept {
  if (!p) return;
  if (n == 0) {
    ::operator delete(p);
    return;
  }
  TensorBufferPool& pool = tensor_buffer_pool();
  const std::size_t bytes = n * sizeof(float);
  {
    std::lock_guard<std::mutex> lock(pool.mu);
    auto& bucket = pool.free[n];
    if (bytes <= TensorBufferPool::kMaxBytes &&
        bucket.size() < TensorBufferPool::kMaxPerSize &&
        pool.cached_bytes <= TensorBufferPool::kMaxBytes - bytes) {
      bucket.push_back(p);
      pool.cached_bytes += bytes;
      return;
    }
    if (bucket.empty()) pool.free.erase(n);
  }
  ::operator delete(p);
}

}  // namespace detail

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

Tensor embedding(const Tensor& indices, const Tensor& tabela) {
  if (indices.rank() < 1) die("embedding espera indices com pelo menos um eixo");
  if (tabela.rank() != 2 || tabela.shape[0] <= 0 || tabela.shape[1] <= 0) {
    die("embedding espera tabela [vocabulario, dimensao]");
  }
  const std::int64_t vocabulario = tabela.shape[0];
  const std::int64_t dimensao = tabela.shape[1];
  Tensor out;
  out.shape = indices.shape;
  out.shape.push_back(dimensao);
  out.data.resize(static_cast<std::size_t>(indices.size() * dimensao));
  for (std::int64_t i = 0; i < indices.size(); ++i) {
    const float valor = indices.data[static_cast<std::size_t>(i)];
    const auto id = static_cast<std::int64_t>(std::llround(valor));
    if (std::fabs(valor - static_cast<float>(id)) > 1e-5F || id < 0 || id >= vocabulario) {
      die("embedding: indice fora do intervalo [0," + std::to_string(vocabulario) + ")");
    }
    const std::size_t origem = static_cast<std::size_t>(id * dimensao);
    const std::size_t destino = static_cast<std::size_t>(i * dimensao);
    for (std::int64_t d = 0; d < dimensao; ++d) {
      out.data[destino + static_cast<std::size_t>(d)] =
          tabela.data[origem + static_cast<std::size_t>(d)];
    }
  }
  return out;
}

void embedding_backward(const Tensor& indices, const Tensor& grad_saida, Tensor& grad_tabela) {
  if (indices.rank() < 1 || grad_tabela.rank() != 2) {
    die("embedding_backward: formas incompativeis");
  }
  const std::int64_t dimensao = grad_tabela.shape[1];
  std::vector<std::int64_t> esperado = indices.shape;
  esperado.push_back(dimensao);
  if (grad_saida.shape != esperado) die("embedding_backward: gradiente com forma inesperada");
  const std::vector<std::int64_t> forma_tabela = grad_tabela.shape;
  grad_tabela = Tensor::zeros(forma_tabela);
  for (std::int64_t i = 0; i < indices.size(); ++i) {
    const float valor = indices.data[static_cast<std::size_t>(i)];
    const auto id = static_cast<std::int64_t>(std::llround(valor));
    if (std::fabs(valor - static_cast<float>(id)) > 1e-5F || id < 0 || id >= grad_tabela.shape[0]) {
      die("embedding_backward: indice fora do intervalo");
    }
    const std::size_t destino = static_cast<std::size_t>(id * dimensao);
    const std::size_t origem = static_cast<std::size_t>(i * dimensao);
    for (std::int64_t d = 0; d < dimensao; ++d) {
      grad_tabela.data[destino + static_cast<std::size_t>(d)] +=
          grad_saida.data[origem + static_cast<std::size_t>(d)];
    }
  }
}

namespace {
int recurrent_gate_count(RecurrentKind k) {
  return k == RecurrentKind::Rnn ? 1 : (k == RecurrentKind::Lstm ? 4 : 3);
}
void recurrent_check(const Tensor& x, const Tensor& w, const Tensor& u, const Tensor& b,
                     RecurrentKind k) {
  if (x.rank() != 2 && x.rank() != 3) die("recorrente espera entrada [T,F] ou [N,T,F]");
  const std::int64_t f = x.shape.back(), h = u.rank() == 2 ? u.shape[0] : 0;
  const std::int64_t g = static_cast<std::int64_t>(recurrent_gate_count(k)) * h;
  if (w.rank() != 2 || u.rank() != 2 || b.rank() != 1 || f <= 0 || h <= 0 || w.shape[0] != f ||
      w.shape[1] != g || u.shape[1] != g || b.shape[0] != g)
    die("recorrente espera pesos W [F,G*H], U [H,G*H] e b [G*H]");
}
float rsig(float x) {
  if (x >= 0.0F) {
    const float e = std::exp(-x);
    return 1.0F / (1.0F + e);
  }
  const float e = std::exp(x);
  return e / (1.0F + e);
}
}  // namespace
Tensor recorrente(const Tensor& x, const Tensor& w, const Tensor& u, const Tensor& b,
                  RecurrentKind kind, RecurrentCache* cache) {
  recurrent_check(x, w, u, b, kind);
  const bool batched = x.rank() == 3;
  const std::int64_t n = batched ? x.shape[0] : 1;
  const std::int64_t tmax = batched ? x.shape[1] : x.shape[0];
  const std::int64_t f = x.shape.back(), h = u.shape[0];
  const std::int64_t g = static_cast<std::int64_t>(recurrent_gate_count(kind)) * h;
  if (tmax <= 0) die("recorrente espera sequencia nao vazia");
  RecurrentCache local, *c = cache ? cache : &local;
  c->kind = kind;
  c->lote = n;
  c->tempo = tmax;
  c->entrada = f;
  c->oculta = h;
  c->estados_h.assign(static_cast<std::size_t>((tmax + 1) * n * h), 0.0F);
  c->estados_c.assign(
      kind == RecurrentKind::Lstm ? static_cast<std::size_t>((tmax + 1) * n * h) : 0, 0.0F);
  c->portas.assign(kind == RecurrentKind::Rnn ? 0 : static_cast<std::size_t>(tmax * n * g), 0.0F);
  auto xi = [&](std::int64_t bn, std::int64_t t, std::int64_t j) {
    return x.data[static_cast<std::size_t>(batched ? (bn * tmax + t) * f + j : t * f + j)];
  };
  auto hi = [&](std::int64_t t, std::int64_t bn, std::int64_t j) -> float& {
    return c->estados_h[static_cast<std::size_t>((t * n + bn) * h + j)];
  };
  auto ci = [&](std::int64_t t, std::int64_t bn, std::int64_t j) -> float& {
    return c->estados_c[static_cast<std::size_t>((t * n + bn) * h + j)];
  };
  auto pi = [&](std::int64_t t, std::int64_t bn, std::int64_t gate, std::int64_t j) -> float& {
    return c->portas[static_cast<std::size_t>((t * n + bn) * g + gate * h + j)];
  };
  for (std::int64_t t = 0; t < tmax; ++t)
    for (std::int64_t bn = 0; bn < n; ++bn) {
      if (kind == RecurrentKind::Rnn) {
        for (std::int64_t j = 0; j < h; ++j) {
          float z = b.data[static_cast<std::size_t>(j)];
          for (std::int64_t q = 0; q < f; ++q)
            z += xi(bn, t, q) * w.data[static_cast<std::size_t>(q * h + j)];
          for (std::int64_t q = 0; q < h; ++q)
            z += hi(t, bn, q) * u.data[static_cast<std::size_t>(q * h + j)];
          hi(t + 1, bn, j) = std::tanh(z);
        }
      } else if (kind == RecurrentKind::Lstm) {
        for (std::int64_t j = 0; j < h; ++j) {
          float z[4] = {};
          for (int gate = 0; gate < 4; ++gate) {
            z[gate] = b.data[static_cast<std::size_t>(gate * h + j)];
            for (std::int64_t q = 0; q < f; ++q)
              z[gate] += xi(bn, t, q) * w.data[static_cast<std::size_t>(q * g + gate * h + j)];
            for (std::int64_t q = 0; q < h; ++q)
              z[gate] += hi(t, bn, q) * u.data[static_cast<std::size_t>(q * g + gate * h + j)];
          }
          const float i = rsig(z[0]), ff = rsig(z[1]), gg = std::tanh(z[2]), o = rsig(z[3]);
          pi(t, bn, 0, j) = i;
          pi(t, bn, 1, j) = ff;
          pi(t, bn, 2, j) = gg;
          pi(t, bn, 3, j) = o;
          ci(t + 1, bn, j) = ff * ci(t, bn, j) + i * gg;
          hi(t + 1, bn, j) = o * std::tanh(ci(t + 1, bn, j));
        }
      } else {
        for (std::int64_t j = 0; j < h; ++j) {
          float z = b.data[static_cast<std::size_t>(j)],
                r = b.data[static_cast<std::size_t>(h + j)];
          for (std::int64_t q = 0; q < f; ++q) {
            z += xi(bn, t, q) * w.data[static_cast<std::size_t>(q * g + j)];
            r += xi(bn, t, q) * w.data[static_cast<std::size_t>(q * g + h + j)];
          }
          for (std::int64_t q = 0; q < h; ++q) {
            z += hi(t, bn, q) * u.data[static_cast<std::size_t>(q * g + j)];
            r += hi(t, bn, q) * u.data[static_cast<std::size_t>(q * g + h + j)];
          }
          const float zz = rsig(z), rr = rsig(r);
          float nn = b.data[static_cast<std::size_t>(2 * h + j)];
          for (std::int64_t q = 0; q < f; ++q)
            nn += xi(bn, t, q) * w.data[static_cast<std::size_t>(q * g + 2 * h + j)];
          for (std::int64_t q = 0; q < h; ++q)
            nn += rr * hi(t, bn, q) * u.data[static_cast<std::size_t>(q * g + 2 * h + j)];
          const float nh = std::tanh(nn);
          pi(t, bn, 0, j) = zz;
          pi(t, bn, 1, j) = rr;
          pi(t, bn, 2, j) = nh;
          hi(t + 1, bn, j) = (1.0F - zz) * nh + zz * hi(t, bn, j);
        }
      }
    }
  Tensor out;
  out.shape = batched ? std::vector<std::int64_t>{n, h} : std::vector<std::int64_t>{h};
  out.data.resize(static_cast<std::size_t>(n * h));
  for (std::int64_t bn = 0; bn < n; ++bn)
    for (std::int64_t j = 0; j < h; ++j)
      out.data[static_cast<std::size_t>(bn * h + j)] = hi(tmax, bn, j);
  return out;
}
void recorrente_backward(const Tensor& x, const Tensor& w, const Tensor& u, const Tensor& b,
                         RecurrentKind kind, const RecurrentCache& c, const Tensor& gy, Tensor& gx,
                         Tensor& gw, Tensor& gu, Tensor& gb) {
  recurrent_check(x, w, u, b, kind);
  const bool batched = x.rank() == 3;
  const std::int64_t n = batched ? x.shape[0] : 1, tmax = batched ? x.shape[1] : x.shape[0];
  const std::int64_t f = x.shape.back(), h = u.shape[0];
  const std::int64_t g = static_cast<std::int64_t>(recurrent_gate_count(kind)) * h;
  const std::vector<std::int64_t> yshape =
      batched ? std::vector<std::int64_t>{n, h} : std::vector<std::int64_t>{h};
  if (c.kind != kind || c.lote != n || c.tempo != tmax || c.entrada != f || c.oculta != h ||
      gy.shape != yshape)
    die("recorrente_backward: cache ou gradiente com forma inesperada");
  gx = Tensor::zeros(x.shape);
  gw = Tensor::zeros(w.shape);
  gu = Tensor::zeros(u.shape);
  gb = Tensor::zeros(b.shape);
  auto xi = [&](std::int64_t bn, std::int64_t t, std::int64_t j) {
    return x.data[static_cast<std::size_t>(batched ? (bn * tmax + t) * f + j : t * f + j)];
  };
  auto gxi = [&](std::int64_t bn, std::int64_t t, std::int64_t j) -> float& {
    return gx.data[static_cast<std::size_t>(batched ? (bn * tmax + t) * f + j : t * f + j)];
  };
  auto hi = [&](std::int64_t t, std::int64_t bn, std::int64_t j) {
    return c.estados_h[static_cast<std::size_t>((t * n + bn) * h + j)];
  };
  auto ci = [&](std::int64_t t, std::int64_t bn, std::int64_t j) {
    return c.estados_c[static_cast<std::size_t>((t * n + bn) * h + j)];
  };
  auto pi = [&](std::int64_t t, std::int64_t bn, std::int64_t gate, std::int64_t j) {
    return c.portas[static_cast<std::size_t>((t * n + bn) * g + gate * h + j)];
  };
  auto go = [&](std::int64_t bn, std::int64_t j) {
    return gy.data[static_cast<std::size_t>(bn * h + j)];
  };
  std::vector<float> dh(static_cast<std::size_t>(n * h), 0.0F),
      dc(static_cast<std::size_t>(n * h), 0.0F);
  for (std::int64_t t = tmax - 1; t >= 0; --t) {
    std::vector<float> dhp(static_cast<std::size_t>(n * h), 0.0F),
        dcp(static_cast<std::size_t>(n * h), 0.0F);
    for (std::int64_t bn = 0; bn < n; ++bn) {
      if (kind == RecurrentKind::Rnn) {
        for (std::int64_t j = 0; j < h; ++j) {
          const float d =
              (dh[static_cast<std::size_t>(bn * h + j)] + (t == tmax - 1 ? go(bn, j) : 0.0F)) *
              (1.0F - hi(t + 1, bn, j) * hi(t + 1, bn, j));
          gb.data[static_cast<std::size_t>(j)] += d;
          for (std::int64_t q = 0; q < f; ++q) {
            gw.data[static_cast<std::size_t>(q * h + j)] += xi(bn, t, q) * d;
            gxi(bn, t, q) += w.data[static_cast<std::size_t>(q * h + j)] * d;
          }
          for (std::int64_t q = 0; q < h; ++q) {
            gu.data[static_cast<std::size_t>(q * h + j)] += hi(t, bn, q) * d;
            dhp[static_cast<std::size_t>(bn * h + q)] +=
                u.data[static_cast<std::size_t>(q * h + j)] * d;
          }
        }
      } else if (kind == RecurrentKind::Lstm) {
        for (std::int64_t j = 0; j < h; ++j) {
          const float i = pi(t, bn, 0, j), ff = pi(t, bn, 1, j), gg = pi(t, bn, 2, j),
                      o = pi(t, bn, 3, j);
          const float dht =
              dh[static_cast<std::size_t>(bn * h + j)] + (t == tmax - 1 ? go(bn, j) : 0.0F);
          const float tc = std::tanh(ci(t + 1, bn, j));
          const float dct = dc[static_cast<std::size_t>(bn * h + j)] + dht * o * (1.0F - tc * tc);
          const float dz[4] = {dct * gg * i * (1.0F - i), dct * ci(t, bn, j) * ff * (1.0F - ff),
                               dct * i * (1.0F - gg * gg), dht * tc * o * (1.0F - o)};
          dcp[static_cast<std::size_t>(bn * h + j)] = dct * ff;
          for (int gate = 0; gate < 4; ++gate) {
            const std::int64_t col = static_cast<std::int64_t>(gate) * h + j;
            gb.data[static_cast<std::size_t>(col)] += dz[gate];
            for (std::int64_t q = 0; q < f; ++q) {
              gw.data[static_cast<std::size_t>(q * g + col)] += xi(bn, t, q) * dz[gate];
              gxi(bn, t, q) += w.data[static_cast<std::size_t>(q * g + col)] * dz[gate];
            }
            for (std::int64_t q = 0; q < h; ++q) {
              gu.data[static_cast<std::size_t>(q * g + col)] += hi(t, bn, q) * dz[gate];
              dhp[static_cast<std::size_t>(bn * h + q)] +=
                  u.data[static_cast<std::size_t>(q * g + col)] * dz[gate];
            }
          }
        }
      } else {
        for (std::int64_t j = 0; j < h; ++j) {
          const float z = pi(t, bn, 0, j), r = pi(t, bn, 1, j), nh = pi(t, bn, 2, j);
          const float dht =
              dh[static_cast<std::size_t>(bn * h + j)] + (t == tmax - 1 ? go(bn, j) : 0.0F);
          const float dz = dht * (hi(t, bn, j) - nh) * z * (1.0F - z);
          const float dn = dht * (1.0F - z) * (1.0F - nh * nh);
          float qn = 0.0F;
          for (std::int64_t q = 0; q < h; ++q)
            qn += hi(t, bn, q) * u.data[static_cast<std::size_t>(q * g + 2 * h + j)];
          const float dr = dn * qn * r * (1.0F - r), dnr = dn * r;
          gb.data[static_cast<std::size_t>(j)] += dz;
          gb.data[static_cast<std::size_t>(h + j)] += dr;
          gb.data[static_cast<std::size_t>(2 * h + j)] += dn;
          for (std::int64_t q = 0; q < f; ++q) {
            gxi(bn, t, q) += w.data[static_cast<std::size_t>(q * g + j)] * dz +
                             w.data[static_cast<std::size_t>(q * g + h + j)] * dr +
                             w.data[static_cast<std::size_t>(q * g + 2 * h + j)] * dn;
            gw.data[static_cast<std::size_t>(q * g + j)] += xi(bn, t, q) * dz;
            gw.data[static_cast<std::size_t>(q * g + h + j)] += xi(bn, t, q) * dr;
            gw.data[static_cast<std::size_t>(q * g + 2 * h + j)] += xi(bn, t, q) * dn;
          }
          for (std::int64_t q = 0; q < h; ++q) {
            const std::int64_t uz = q * g + j, ur = q * g + h + j, un = q * g + 2 * h + j;
            const float hp = hi(t, bn, q);
            gu.data[static_cast<std::size_t>(uz)] += hp * dz;
            gu.data[static_cast<std::size_t>(ur)] += hp * dr;
            gu.data[static_cast<std::size_t>(un)] += hp * dnr;
            dhp[static_cast<std::size_t>(bn * h + q)] += u.data[static_cast<std::size_t>(uz)] * dz +
                                                         u.data[static_cast<std::size_t>(ur)] * dr +
                                                         u.data[static_cast<std::size_t>(un)] * dnr;
          }
        }
      }
    }
    dh.swap(dhp);
    dc.swap(dcp);
  }
}

Tensor fatiar_lote(const Tensor& a, const std::vector<std::int64_t>& idx) {
  if (a.shape.empty()) die("fatiar_lote espera um tensor nao escalar");
  const std::int64_t linha = a.size() / a.shape[0];
  Tensor out;
  out.shape = a.shape;
  out.shape[0] = static_cast<std::int64_t>(idx.size());
  out.data.reserve(static_cast<std::size_t>(idx.size() * static_cast<std::size_t>(linha)));
  for (std::int64_t i : idx) {
    if (i < 0 || i >= a.shape[0]) die("fatiar_lote: indice fora do lote");
    const float* base = a.data.data() + static_cast<std::size_t>(i * linha);
    out.data.insert(out.data.end(), base, base + linha);
  }
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

Tensor conv2d(const Tensor& x, const Tensor& k, std::int64_t passo, std::int64_t padding,
              std::int64_t dilatacao) {
  if (x.rank() != 4) die("conv2d espera uma entrada [N, C_in, H, W] (" + x.shape_str() + ")");
  if (k.rank() != 4) die("conv2d espera um nucleo [C_out, C_in, KH, KW] (" + k.shape_str() + ")");
  if (passo < 1) die("conv2d: passo deve ser >= 1");
  if (padding < 0) die("conv2d: padding deve ser >= 0");
  if (dilatacao < 1) die("conv2d: dilatacao deve ser >= 1");
  const std::int64_t n = x.shape[0];
  const std::int64_t cin = x.shape[1];
  const std::int64_t h = x.shape[2];
  const std::int64_t w = x.shape[3];
  const std::int64_t cout = k.shape[0];
  if (k.shape[1] != cin) {
    die("conv2d: nucleo tem " + std::to_string(k.shape[1]) +
        " canais de entrada, mas a entrada tem " + std::to_string(cin));
  }
  const std::int64_t kh = k.shape[2];
  const std::int64_t kw = k.shape[3];
  const std::int64_t ekh = (kh - 1) * dilatacao + 1;
  const std::int64_t ekw = (kw - 1) * dilatacao + 1;
  const std::int64_t oh = (h + 2 * padding - ekh) / passo + 1;
  const std::int64_t ow = (w + 2 * padding - ekw) / passo + 1;
  if (oh < 1 || ow < 1) {
    if (padding == 0 && dilatacao == 1) {
      die("conv2d: nucleo " + std::to_string(kh) + "x" + std::to_string(kw) +
          " maior que a entrada " + std::to_string(h) + "x" + std::to_string(w));
    }
    die("conv2d: nucleo efetivo " + std::to_string(ekh) + "x" + std::to_string(ekw) +
        " nao cabe na entrada com padding " + std::to_string(padding));
  }

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
                const std::int64_t hh = i * passo + u * dilatacao - padding;
                const std::int64_t ww = jj * passo + v * dilatacao - padding;
                if (hh >= 0 && hh < h && ww >= 0 && ww < w) {
                  acc += xc[static_cast<std::size_t>(hh * w + ww)] *
                         kc[static_cast<std::size_t>(u * kw + v)];
                }
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

Tensor adicionar_vies_conv(const Tensor& y, const Tensor& vies) {
  if (y.rank() != 4) die("vies de conv2d espera uma saida [N, C, H, W] (" + y.shape_str() + ")");
  if (vies.rank() != 1 || vies.shape[0] != y.shape[1]) {
    die("vies de conv2d espera [" + std::to_string(y.shape[1]) + "] (" + vies.shape_str() + ")");
  }
  Tensor out = y;
  const std::int64_t n = y.shape[0];
  const std::int64_t canais = y.shape[1];
  const std::int64_t hw = y.shape[2] * y.shape[3];
  for (std::int64_t nn = 0; nn < n; ++nn) {
    for (std::int64_t c = 0; c < canais; ++c) {
      const float b = vies.data[static_cast<std::size_t>(c)];
      float* base = out.data.data() + static_cast<std::size_t>((nn * canais + c) * hw);
      for (std::int64_t k = 0; k < hw; ++k) base[static_cast<std::size_t>(k)] += b;
    }
  }
  return out;
}

void conv2d_backward(const Tensor& x, const Tensor& nucleo, const Tensor& grad_saida,
                     std::int64_t passo, std::int64_t padding, std::int64_t dilatacao,
                     Tensor& grad_x, Tensor& grad_nucleo, Tensor& grad_vies) {
  if (x.rank() != 4)
    die("conv2d_backward espera uma entrada [N, C_in, H, W] (" + x.shape_str() + ")");
  if (nucleo.rank() != 4) {
    die("conv2d_backward espera um nucleo [C_out, C_in, KH, KW] (" + nucleo.shape_str() + ")");
  }
  if (passo < 1) die("conv2d_backward: passo deve ser >= 1");
  if (padding < 0) die("conv2d_backward: padding deve ser >= 0");
  if (dilatacao < 1) die("conv2d_backward: dilatacao deve ser >= 1");
  const std::int64_t n = x.shape[0];
  const std::int64_t cin = x.shape[1];
  const std::int64_t h = x.shape[2];
  const std::int64_t w = x.shape[3];
  const std::int64_t cout = nucleo.shape[0];
  const std::int64_t kh = nucleo.shape[2];
  const std::int64_t kw = nucleo.shape[3];
  const std::int64_t ekh = (kh - 1) * dilatacao + 1;
  const std::int64_t ekw = (kw - 1) * dilatacao + 1;
  if (nucleo.shape[1] != cin) die("conv2d_backward: canais do nucleo incompativeis com a entrada");
  const std::int64_t oh = (h + 2 * padding - ekh) / passo + 1;
  const std::int64_t ow = (w + 2 * padding - ekw) / passo + 1;
  if (grad_saida.shape != std::vector<std::int64_t>({n, cout, oh, ow})) {
    die("conv2d_backward: gradiente com forma inesperada (" + grad_saida.shape_str() + ")");
  }
  grad_x = Tensor::zeros(x.shape);
  grad_nucleo = Tensor::zeros(nucleo.shape);
  grad_vies = Tensor::zeros({cout});
  for (std::int64_t nn = 0; nn < n; ++nn) {
    for (std::int64_t co = 0; co < cout; ++co) {
      for (std::int64_t i = 0; i < oh; ++i) {
        for (std::int64_t j = 0; j < ow; ++j) {
          const float gy =
              grad_saida.data[static_cast<std::size_t>(((nn * cout + co) * oh + i) * ow + j)];
          grad_vies.data[static_cast<std::size_t>(co)] += gy;
          for (std::int64_t ci = 0; ci < cin; ++ci) {
            for (std::int64_t u = 0; u < kh; ++u) {
              for (std::int64_t v = 0; v < kw; ++v) {
                const std::int64_t hh = i * passo + u * dilatacao - padding;
                const std::int64_t ww = j * passo + v * dilatacao - padding;
                const std::size_t ki =
                    static_cast<std::size_t>(((co * cin + ci) * kh + u) * kw + v);
                if (hh >= 0 && hh < h && ww >= 0 && ww < w) {
                  const std::size_t xi =
                      static_cast<std::size_t>(((nn * cin + ci) * h + hh) * w + ww);
                  grad_nucleo.data[ki] += x.data[xi] * gy;
                  grad_x.data[xi] += nucleo.data[ki] * gy;
                }
              }
            }
          }
        }
      }
    }
  }
}

Tensor maxpool2d(const Tensor& x, std::int64_t janela, std::int64_t passo) {
  if (x.rank() != 4) die("agrupamento_max espera uma entrada [N, C, H, W] (" + x.shape_str() + ")");
  if (janela < 1) die("agrupamento_max: janela deve ser >= 1");
  if (passo < 1) passo = janela;
  const std::int64_t n = x.shape[0];
  const std::int64_t canais = x.shape[1];
  const std::int64_t h = x.shape[2];
  const std::int64_t w = x.shape[3];
  if (janela > h || janela > w) {
    die("agrupamento_max: janela " + std::to_string(janela) + "x" + std::to_string(janela) +
        " maior que a entrada " + std::to_string(h) + "x" + std::to_string(w));
  }
  const std::int64_t oh = (h - janela) / passo + 1;
  const std::int64_t ow = (w - janela) / passo + 1;
  Tensor out = Tensor::zeros({n, canais, oh, ow});
  for (std::int64_t nn = 0; nn < n; ++nn) {
    for (std::int64_t c = 0; c < canais; ++c) {
      for (std::int64_t i = 0; i < oh; ++i) {
        for (std::int64_t j = 0; j < ow; ++j) {
          float melhor = x.data[static_cast<std::size_t>(((nn * canais + c) * h + i * passo) * w + j * passo)];
          for (std::int64_t u = 0; u < janela; ++u) {
            for (std::int64_t v = 0; v < janela; ++v) {
              if (u == 0 && v == 0) continue;
              const float atual = x.data[static_cast<std::size_t>(((nn * canais + c) * h + i * passo + u) * w +
                                                                  j * passo + v)];
              if (atual > melhor) melhor = atual;
            }
          }
          out.data[static_cast<std::size_t>(((nn * canais + c) * oh + i) * ow + j)] = melhor;
        }
      }
    }
  }
  return out;
}

Tensor maxpool2d_backward(const Tensor& x, const Tensor& grad_saida, std::int64_t janela,
                          std::int64_t passo) {
  if (x.rank() != 4) die("maxpool2d_backward espera uma entrada [N, C, H, W] (" + x.shape_str() + ")");
  if (janela < 1) die("maxpool2d_backward: janela deve ser >= 1");
  if (passo < 1) passo = janela;
  const std::int64_t n = x.shape[0];
  const std::int64_t canais = x.shape[1];
  const std::int64_t h = x.shape[2];
  const std::int64_t w = x.shape[3];
  const std::int64_t oh = (h - janela) / passo + 1;
  const std::int64_t ow = (w - janela) / passo + 1;
  if (grad_saida.shape != std::vector<std::int64_t>({n, canais, oh, ow})) {
    die("maxpool2d_backward: gradiente com forma inesperada (" + grad_saida.shape_str() + ")");
  }
  Tensor grad_x = Tensor::zeros(x.shape);
  for (std::int64_t nn = 0; nn < n; ++nn) {
    for (std::int64_t c = 0; c < canais; ++c) {
      for (std::int64_t i = 0; i < oh; ++i) {
        for (std::int64_t j = 0; j < ow; ++j) {
          std::int64_t melhor_u = 0;
          std::int64_t melhor_v = 0;
          float melhor = x.data[static_cast<std::size_t>(((nn * canais + c) * h + i * passo) * w + j * passo)];
          for (std::int64_t u = 0; u < janela; ++u) {
            for (std::int64_t v = 0; v < janela; ++v) {
              if (u == 0 && v == 0) continue;
              const float atual = x.data[static_cast<std::size_t>(((nn * canais + c) * h + i * passo + u) * w +
                                                                  j * passo + v)];
              if (atual > melhor) {
                melhor = atual;
                melhor_u = u;
                melhor_v = v;
              }
            }
          }
          const std::size_t xi = static_cast<std::size_t>(((nn * canais + c) * h + i * passo + melhor_u) * w +
                                                          j * passo + melhor_v);
          grad_x.data[xi] += grad_saida.data[static_cast<std::size_t>(((nn * canais + c) * oh + i) * ow + j)];
        }
      }
    }
  }
  return grad_x;
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

void norma_lote_estatisticas(const Tensor& x, Tensor& media, Tensor& var) {
  if (x.rank() < 2) die("norma_lote espera um tensor [N, C, ...] (" + x.shape_str() + ")");
  const std::int64_t n = x.shape[0];
  const std::int64_t canais = x.shape[1];
  const std::int64_t rest = x.size() / (n * canais);
  media = Tensor::zeros({canais});
  var = Tensor::zeros({canais});
  const float total = static_cast<float>(n * rest);
  for (std::int64_t nn = 0; nn < n; ++nn) {
    for (std::int64_t c = 0; c < canais; ++c) {
      const float* base = x.data.data() + static_cast<std::size_t>((nn * canais + c) * rest);
      for (std::int64_t k = 0; k < rest; ++k) media.data[static_cast<std::size_t>(c)] += base[k];
    }
  }
  for (std::int64_t c = 0; c < canais; ++c) media.data[static_cast<std::size_t>(c)] /= total;
  for (std::int64_t nn = 0; nn < n; ++nn) {
    for (std::int64_t c = 0; c < canais; ++c) {
      const float* base = x.data.data() + static_cast<std::size_t>((nn * canais + c) * rest);
      float acc = 0.0F;
      for (std::int64_t k = 0; k < rest; ++k) {
        const float d = base[k] - media.data[static_cast<std::size_t>(c)];
        acc += d * d;
      }
      var.data[static_cast<std::size_t>(c)] += acc;
    }
  }
  for (std::int64_t c = 0; c < canais; ++c) var.data[static_cast<std::size_t>(c)] /= total;
}

void norma_lote_backward(const Tensor& x, const Tensor& grad_saida, const Tensor& gama,
                         const Tensor& media, const Tensor& var, float eps, Tensor& grad_x,
                         Tensor& grad_gama, Tensor& grad_beta) {
  if (x.rank() < 2) die("norma_lote_backward espera um tensor [N, C, ...] (" + x.shape_str() + ")");
  if (grad_saida.shape != x.shape) {
    die("norma_lote_backward: gradiente com forma inesperada (" + grad_saida.shape_str() + ")");
  }
  const std::int64_t n = x.shape[0];
  const std::int64_t canais = x.shape[1];
  const std::int64_t rest = x.size() / (n * canais);
  if (media.shape != std::vector<std::int64_t>({canais}) ||
      var.shape != std::vector<std::int64_t>({canais})) {
    die("norma_lote_backward: media/var devem ser [" + std::to_string(canais) + "]");
  }
  grad_x = Tensor::zeros(x.shape);
  grad_gama = Tensor::zeros({canais});
  grad_beta = Tensor::zeros({canais});
  const float m = static_cast<float>(n * rest);
  for (std::int64_t c = 0; c < canais; ++c) {
    const float g = param_por_canal(gama, c, canais, "gama");
    const float mu = media.data[static_cast<std::size_t>(c)];
    const float inv = 1.0F / std::sqrt(var.data[static_cast<std::size_t>(c)] + eps);
    float soma_dy = 0.0F;
    float soma_dy_xchapeu = 0.0F;
    for (std::int64_t nn = 0; nn < n; ++nn) {
      const float* xb = x.data.data() + static_cast<std::size_t>((nn * canais + c) * rest);
      const float* gb = grad_saida.data.data() + static_cast<std::size_t>((nn * canais + c) * rest);
      for (std::int64_t k = 0; k < rest; ++k) {
        const float xchapeu = (xb[k] - mu) * inv;
        soma_dy += gb[k];
        soma_dy_xchapeu += gb[k] * xchapeu;
        grad_gama.data[static_cast<std::size_t>(c)] += gb[k] * xchapeu;
        grad_beta.data[static_cast<std::size_t>(c)] += gb[k];
      }
    }
    const float escala = g * inv / m;
    for (std::int64_t nn = 0; nn < n; ++nn) {
      const float* xb = x.data.data() + static_cast<std::size_t>((nn * canais + c) * rest);
      const float* gb = grad_saida.data.data() + static_cast<std::size_t>((nn * canais + c) * rest);
      float* ob = grad_x.data.data() + static_cast<std::size_t>((nn * canais + c) * rest);
      for (std::int64_t k = 0; k < rest; ++k) {
        const float xchapeu = (xb[k] - mu) * inv;
        ob[k] = escala * (m * gb[k] - soma_dy - xchapeu * soma_dy_xchapeu);
      }
    }
  }
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
