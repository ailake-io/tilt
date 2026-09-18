#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace tilt::rt {

// Dense row-major f32 tensor. matmul 2D x 2D divide linhas entre threads
// acima de um limiar; demais kernels sao escalares (SIMD: futuro).
// Shape-mismatched operations throw std::runtime_error.
struct Tensor {
  std::vector<std::int64_t> shape;
  std::vector<float> data;

  std::int64_t size() const;
  std::int64_t rank() const { return static_cast<std::int64_t>(shape.size()); }
  std::string shape_str() const;  // e.g. "f32, 2, 3"

  static Tensor filled(std::vector<std::int64_t> shape, float value);
  static Tensor zeros(std::vector<std::int64_t> shape) { return filled(std::move(shape), 0.0F); }
  static Tensor ones(std::vector<std::int64_t> shape) { return filled(std::move(shape), 1.0F); }
  // Deterministic Xavier-uniform fill seeded by `seed`.
  static Tensor xavier(std::vector<std::int64_t> shape, std::int64_t fan_in, std::int64_t fan_out,
                       std::uint64_t seed);
};

Tensor add(const Tensor& a, const Tensor& b);  // supports scalar and last-dim bias broadcast
Tensor sub(const Tensor& a, const Tensor& b);
Tensor mul(const Tensor& a, const Tensor& b);
Tensor div(const Tensor& a, const Tensor& b);
Tensor scalar_op(const Tensor& a, float s, char op);  // op in {+,-,*,/}

Tensor matmul(const Tensor& a, const Tensor& b);
Tensor transpose2d(const Tensor& a);
Tensor reshape(const Tensor& a, std::vector<std::int64_t> shape);

// Lookup de embeddings: indices [...], tabela [vocabulario, dimensao] -> [..., dimensao].
Tensor embedding(const Tensor& indices, const Tensor& tabela);
void embedding_backward(const Tensor& indices, const Tensor& grad_saida, Tensor& grad_tabela);

// Fatia linhas da dimensao 0 (mini-lote): saida [idx.size(), ...].
Tensor fatiar_lote(const Tensor& a, const std::vector<std::int64_t>& idx);

Tensor apply_unary(const Tensor& a, const std::string& fn);  // relu/gelu/silu/sigmoide/tanh
Tensor softmax_last(const Tensor& a);
Tensor layer_norm_last(const Tensor& a);  // normaliza sobre a ultima dimensao (sem affine)

// Convolucao 2D NCHW com padding e dilatacao simetricos.
// -> [N, C_out, floor((H + 2*padding - KH_eff)/passo)+1, ...],
// onde KH_eff = (KH - 1)*dilatacao + 1.
Tensor conv2d(const Tensor& x, const Tensor& nucleo, std::int64_t passo = 1,
              std::int64_t padding = 0, std::int64_t dilatacao = 1);
Tensor adicionar_vies_conv(const Tensor& y, const Tensor& vies);
void conv2d_backward(const Tensor& x, const Tensor& nucleo, const Tensor& grad_saida,
                     std::int64_t passo, std::int64_t padding, std::int64_t dilatacao,
                     Tensor& grad_x, Tensor& grad_nucleo, Tensor& grad_vies);

// Agrupamento maximo NCHW, padding valido: janela JxJ e passo S (padrao S = J).
Tensor maxpool2d(const Tensor& x, std::int64_t janela, std::int64_t passo = 0);
Tensor maxpool2d_backward(const Tensor& x, const Tensor& grad_saida, std::int64_t janela,
                          std::int64_t passo = 0);

// Batch norm por canal sobre [N, C, ...]: y = gama * (x - media) / sqrt(var + eps) + beta.
// gama/beta/media/var aceitos como [C] ou escalar; com em_treino, media/var sao
// calculadas do proprio lote (variancia populacional) e os tensores media/var ignorados.
Tensor norma_lote(const Tensor& x, const Tensor& gama, const Tensor& beta, const Tensor& media,
                  const Tensor& var, float eps, bool em_treino);
void norma_lote_estatisticas(const Tensor& x, Tensor& media, Tensor& var);
void norma_lote_backward(const Tensor& x, const Tensor& grad_saida, const Tensor& gama,
                         const Tensor& media, const Tensor& var, float eps, Tensor& grad_x,
                         Tensor& grad_gama, Tensor& grad_beta);

float sum_all(const Tensor& a);
float mean_all(const Tensor& a);
std::int64_t argmax_last(const Tensor& a);  // index of max in the last dim (rank-1 tensor)

}  // namespace tilt::rt
