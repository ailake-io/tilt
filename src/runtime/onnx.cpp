#include "runtime/onnx.hpp"

#include <cstring>
#include <fstream>
#include <stdexcept>

namespace tilt::rt {

namespace {

// ---- Escritor protobuf minimo (varint + LEN + float) ----

struct Writer {
  std::string out;
  void varint(std::uint64_t v) {
    while (v >= 0x80) {
      out.push_back(static_cast<char>(0x80 | (v & 0x7F)));
      v >>= 7;
    }
    out.push_back(static_cast<char>(v));
  }
  void tag(std::uint32_t field, std::uint32_t wire) { varint((field << 3) | wire); }
  void varint_field(std::uint32_t field, std::uint64_t v) {
    tag(field, 0);
    varint(v);
  }
  void float_field(std::uint32_t field, float f) {
    tag(field, 5);
    std::uint32_t u = 0;
    std::memcpy(&u, &f, 4);
    out.push_back(static_cast<char>(u & 0xFF));
    out.push_back(static_cast<char>((u >> 8) & 0xFF));
    out.push_back(static_cast<char>((u >> 16) & 0xFF));
    out.push_back(static_cast<char>((u >> 24) & 0xFF));
  }
  void bytes_field(std::uint32_t field, const std::string& b) {
    tag(field, 2);
    varint(b.size());
    out.append(b);
  }
  void msg_field(std::uint32_t field, const std::string& m) { bytes_field(field, m); }
};

std::string encode_packed_int64(const std::vector<std::int64_t>& v) {
  Writer w;
  for (std::int64_t x : v) w.varint(static_cast<std::uint64_t>(x));
  return w.out;
}

std::string encode_raw_floats(const std::vector<float>& v) {
  std::string s;
  s.resize(v.size() * 4);
  if (!v.empty()) std::memcpy(s.data(), v.data(), s.size());
  return s;
}

// Como ONNX usa packed ints para dims, aqui um helper para attrs de ints
std::string encode_ints(const std::vector<std::int64_t>& v) {
  Writer w;
  for (std::int64_t x : v) w.varint(static_cast<std::uint64_t>(x));
  return w.out;
}

// AttributeProto: name (1) + type (20) + ints (7)
std::string attr_ints(const std::string& name, const std::vector<std::int64_t>& v) {
  Writer w;
  w.bytes_field(1, name);
  w.varint_field(20, 6);  // INTS
  w.bytes_field(7, encode_ints(v));
  return w.out;
}

// AttributeProto: name (1) + type (20) + i (3)
std::string attr_int(const std::string& name, std::int64_t v) {
  Writer w;
  w.bytes_field(1, name);
  w.varint_field(20, 2);  // INT
  w.varint_field(3, static_cast<std::uint64_t>(v));
  return w.out;
}

// AttributeProto: name (1) + type (20) + f (6)
std::string attr_float(const std::string& name, float v) {
  Writer w;
  w.bytes_field(1, name);
  w.varint_field(20, 4);  // FLOAT
  // field 6: 32-bit float
  std::uint32_t u = 0;
  std::memcpy(&u, &v, 4);
  w.tag(6, 5);
  w.out.push_back(static_cast<char>(u & 0xFF));
  w.out.push_back(static_cast<char>((u >> 8) & 0xFF));
  w.out.push_back(static_cast<char>((u >> 16) & 0xFF));
  w.out.push_back(static_cast<char>((u >> 24) & 0xFF));
  return w.out;
}

// Dim: dim_param (1, string) | dim_value (2, varint)
std::string dim_param(const std::string& p) {
  Writer w;
  w.bytes_field(1, p);
  return w.out;
}
std::string dim_value(std::int64_t v) {
  Writer w;
  w.varint_field(2, static_cast<std::uint64_t>(v));
  return w.out;
}

// TensorShapeProto: dim (1, rep msg); lote simbolico + dims literais.
std::string shape_proto_forma(const std::vector<std::int64_t>& forma_sem_lote) {
  Writer w;
  w.msg_field(1, dim_param("lote"));
  for (std::int64_t d : forma_sem_lote) w.msg_field(1, dim_value(d));
  return w.out;
}

// TypeProto.tensor_type: elem_type (1, varint FLOAT=1) + shape (2, msg)
std::string tensor_type_proto_forma(const std::vector<std::int64_t>& forma_sem_lote) {
  Writer inner;
  inner.varint_field(1, 1);  // FLOAT
  inner.msg_field(2, shape_proto_forma(forma_sem_lote));
  Writer w;
  w.msg_field(1, inner.out);
  return w.out;
}

// ValueInfoProto: name (1) + type (2)
std::string value_info_forma(const std::string& name, const std::vector<std::int64_t>& forma_sem_lote) {
  Writer w;
  w.bytes_field(1, name);
  w.msg_field(2, tensor_type_proto_forma(forma_sem_lote));
  return w.out;
}

// TensorProto (initializer): dims (1, packed) + data_type (2) + name (8) + raw_data (9)
std::string tensor_proto(const std::string& name, const std::vector<std::int64_t>& dims,
                         const std::vector<float>& data) {
  Writer w;
  w.bytes_field(1, encode_packed_int64(dims));
  w.varint_field(2, 1);  // FLOAT
  w.bytes_field(8, name);
  w.bytes_field(9, encode_raw_floats(data));
  // float_data (4) vazio: leitores aceitam raw_data.
  return w.out;
}

// NodeProto: input (1, rep) + output (2, rep) + name (3) + op_type (4) + attribute (5)
std::string node_proto(const std::string& op, const std::vector<std::string>& inputs,
                       const std::vector<std::string>& outputs, const std::string& name,
                       const std::vector<std::string>& attrs) {
  Writer w;
  for (const auto& i : inputs) w.bytes_field(1, i);
  for (const auto& o : outputs) w.bytes_field(2, o);
  w.bytes_field(3, name);
  w.bytes_field(4, op);
  for (const auto& a : attrs) w.msg_field(5, a);
  return w.out;
}

[[noreturn]] void die(const std::string& m) { throw std::runtime_error("onnx: " + m); }

}  // namespace

std::string onnx_export_bytes(const std::vector<OnnxLayer>& layers,
                              const std::vector<std::int64_t>& forma_entrada,
                              const std::string& model_name) {
  if (forma_entrada.empty()) die("dimensao de entrada invalida");
  for (std::int64_t d : forma_entrada) {
    if (d <= 0) die("dimensao de entrada invalida (" + std::to_string(d) + ")");
  }
  if (layers.empty()) die("modelo sem camadas");
  std::int64_t dense_count = 0;
  for (const auto& l : layers)
    if (l.kind == OnnxLayer::Dense) ++dense_count;
  if (dense_count == 0) die("modelo sem camada densa/linear");

  std::vector<std::string> nodes;
  std::vector<std::string> initializers;
  std::string cur = "entrada";
  std::int64_t dim = forma_entrada.back();
  int seq = 0;
  std::int64_t out_dim = forma_entrada.back();

  auto fresh = [&](const char* base) { return std::string(base) + std::to_string(seq++); };

  for (const auto& l : layers) {
    if (l.kind == OnnxLayer::Dense) {
      if (l.w.rank() != 2) die("peso denso precisa ser 2D");
      if (l.w.shape[0] != dim) {
        die("camada densa espera entrada " + std::to_string(l.w.shape[0]) + ", grafo tem " +
            std::to_string(dim));
      }
      const std::int64_t n = l.w.shape[1];
      if (static_cast<std::int64_t>(l.b.data.size()) != n) die("vies com tamanho errado");
      const std::string wn = "W" + std::to_string(seq);
      const std::string bn = "B" + std::to_string(seq);
      initializers.push_back(tensor_proto(wn, l.w.shape, l.w.data));
      initializers.push_back(tensor_proto(bn, l.b.shape, l.b.data));
      const std::string out = fresh("h");
      // Gemm: Y = A*B + C (transB=0 default; alpha/beta default 1).
      nodes.push_back(node_proto("Gemm", {cur, wn, bn}, {out}, "gemm" + std::to_string(seq), {}));
      cur = out;
      dim = n;
      out_dim = n;
    } else if (l.kind == OnnxLayer::Activation) {
      const std::string out = fresh("a");
      if (l.act == "relu") {
        nodes.push_back(node_proto("Relu", {cur}, {out}, "relu" + std::to_string(seq), {}));
      } else if (l.act == "sigmoide") {
        nodes.push_back(node_proto("Sigmoid", {cur}, {out}, "sigmoid" + std::to_string(seq), {}));
      } else if (l.act == "tanh") {
        nodes.push_back(node_proto("Tanh", {cur}, {out}, "tanh" + std::to_string(seq), {}));
      } else if (l.act == "gelu") {
        nodes.push_back(node_proto("Gelu", {cur}, {out}, "gelu" + std::to_string(seq), {}));
      } else if (l.act == "silu") {
        // silu(x) = x * sigmoid(x)
        const std::string sig = fresh("s");
        nodes.push_back(node_proto("Sigmoid", {cur}, {sig}, "sigmoid" + std::to_string(seq), {}));
        nodes.push_back(node_proto("Mul", {cur, sig}, {out}, "mul" + std::to_string(seq), {}));
      } else {
        die("ativacao '" + l.act + "' nao exportavel (use relu|gelu|silu|sigmoide|tanh)");
      }
      cur = out;
    } else if (l.kind == OnnxLayer::Softmax) {
      const std::string out = fresh("p");
      nodes.push_back(node_proto("Softmax", {cur}, {out}, "softmax" + std::to_string(seq),
                                 {attr_int("axis", 1)}));
      cur = out;
    } else if (l.kind == OnnxLayer::LayerNorm) {
      // LayerNormalization (opset 17+): Y = LN(X) com scale=1, bias=0.
      const std::string sc = "LN_scale" + std::to_string(seq);
      const std::string bi = "LN_bias" + std::to_string(seq);
      initializers.push_back(
          tensor_proto(sc, {dim}, std::vector<float>(static_cast<std::size_t>(dim), 1.0F)));
      initializers.push_back(
          tensor_proto(bi, {dim}, std::vector<float>(static_cast<std::size_t>(dim), 0.0F)));
      const std::string out = fresh("n");
      nodes.push_back(node_proto("LayerNormalization", {cur, sc, bi}, {out},
                                 "layernorm" + std::to_string(seq),
                                 {attr_int("axis", -1)}));
      cur = out;
    } else if (l.kind == OnnxLayer::Conv2d) {
      // Conv: Y = Conv(X, W, B) com kernel shape [C_out, C_in, KH, KW].
      // w: [C_out, C_in, KH, KW], b: [C_out]
      if (l.w.rank() != 4) die("peso conv2d precisa ser 4D [C_out, C_in, KH, KW]");
      const std::int64_t cout = l.w.shape[0];
      const std::int64_t kh = l.w.shape[2];
      const std::int64_t kw = l.w.shape[3];
      const std::string wn = "W_conv" + std::to_string(seq);
      const std::string bn = "B_conv" + std::to_string(seq);
      initializers.push_back(tensor_proto(wn, l.w.shape, l.w.data));
      initializers.push_back(tensor_proto(bn, l.b.shape, l.b.data));
      const std::string out = fresh("c");
      std::vector<std::string> attrs;
      attrs.push_back(attr_ints("kernel_shape", {kh, kw}));
      attrs.push_back(attr_ints("strides", {l.passo, l.passo}));
      attrs.push_back(attr_ints("pads", {0, 0, 0, 0}));
      nodes.push_back(node_proto("Conv", {cur, wn, bn}, {out}, "conv" + std::to_string(seq), attrs));
      cur = out;
      dim = cout;
      out_dim = cout;
    } else if (l.kind == OnnxLayer::NormaLote) {
      // BatchNormalization: Y = (X - mean) / sqrt(var + eps) * scale + B
      // w = scale (gama), b = bias (beta), media_running = mean, var_running = var
      if (l.w.size() != dim || l.b.size() != dim) {
        die("norma_lote: gama/beta com tamanho " + std::to_string(l.w.size()) +
            " != " + std::to_string(dim));
      }
      const std::string wn = "BN_scale" + std::to_string(seq);
      const std::string bn = "BN_bias" + std::to_string(seq);
      const std::string mn = "BN_mean" + std::to_string(seq);
      const std::string vn = "BN_var" + std::to_string(seq);
      initializers.push_back(tensor_proto(wn, {dim}, l.w.data));
      initializers.push_back(tensor_proto(bn, {dim}, l.b.data));
      initializers.push_back(tensor_proto(mn, {dim}, l.media_running.data));
      initializers.push_back(tensor_proto(vn, {dim}, l.var_running.data));
      const std::string out = fresh("bn");
      nodes.push_back(node_proto("BatchNormalization", {cur, wn, bn, mn, vn}, {out},
                                 "batchnorm" + std::to_string(seq),
                                 {attr_float("eps", 1e-5f)}));
      cur = out;
    } else if (l.kind == OnnxLayer::Flatten) {
      if (l.plano <= 0) die("camada achatar sem largura conhecida");
      const std::string out = fresh("f");
      nodes.push_back(node_proto("Flatten", {cur}, {out}, "flatten" + std::to_string(seq),
                                 {attr_int("axis", 1)}));
      cur = out;
      dim = l.plano;
      out_dim = l.plano;
    } else if (l.kind == OnnxLayer::MaxPool) {
      if (l.janela < 1 || l.passo < 1) die("agrupamento_max com janela/passo invalidos");
      const std::string out = fresh("p");
      nodes.push_back(node_proto("MaxPool", {cur}, {out}, "maxpool" + std::to_string(seq),
                                 {attr_ints("kernel_shape", {l.janela, l.janela}),
                                  attr_ints("strides", {l.passo, l.passo}),
                                  attr_ints("pads", {0, 0, 0, 0})}));
      cur = out;
    } else if (l.kind == OnnxLayer::Dropout) {
      continue;  // identidade na inferencia
    }
  }

  // Renomeia a saida final para "saida".
  // (Reescrever o ultimo node seria custoso; adiciona Identity.)
  nodes.push_back(node_proto("Identity", {cur}, {"saida"}, "saida_identity", {}));

  // GraphProto
  Writer g;
  for (const auto& n : nodes) g.msg_field(1, n);
  g.bytes_field(2, model_name.empty() ? "tilt" : model_name);
  for (const auto& t : initializers) g.msg_field(5, t);
  g.msg_field(11, value_info_forma("entrada", forma_entrada));
  g.msg_field(12, value_info_forma("saida", {out_dim}));
  const std::string graph = g.out;

  // OperatorSetIdProto: domain="" version=20 (Gelu + LayerNormalization).
  Writer opset;
  opset.bytes_field(1, "");
  opset.varint_field(2, 20);

  // ModelProto
  Writer m;
  m.varint_field(1, 10);  // ir_version (10 cobre opset 20)
  m.bytes_field(2, "tilt");
  m.msg_field(7, graph);
  m.msg_field(8, opset.out);
  m.bytes_field(6, "tilt onnx export");  // doc_string (campo 6)
  return m.out;
}

bool onnx_salvar(const std::string& path, const std::vector<OnnxLayer>& layers,
                 const std::vector<std::int64_t>& forma_entrada, const std::string& model_name,
                 std::string& err) {
  std::string bytes;
  try {
    bytes = onnx_export_bytes(layers, forma_entrada, model_name);
  } catch (const std::exception& e) {
    err = e.what();
    return false;
  }
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) {
    err = "nao foi possivel gravar '" + path + "'";
    return false;
  }
  f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (!f) {
    err = "falha ao gravar '" + path + "'";
    return false;
  }
  return true;
}

}  // namespace tilt::rt
