#include "runtime/onnx.hpp"

#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <utility>

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

// AttributeProto: name (1) + type (20) + s (4)
std::string attr_string(const std::string& name, const std::string& value) {
  Writer w;
  w.bytes_field(1, name);
  w.varint_field(20, 3);  // STRING
  w.bytes_field(4, value);
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
std::string value_info_forma(const std::string& name,
                             const std::vector<std::int64_t>& forma_sem_lote) {
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

std::string tensor_proto_int64(const std::string& name, const std::vector<std::int64_t>& dims,
                               const std::vector<std::int64_t>& data) {
  Writer w;
  w.bytes_field(1, encode_packed_int64(dims));
  w.varint_field(2, 7);  // INT64
  w.bytes_field(8, name);
  std::string raw;
  for (std::int64_t v : data) {
    for (int i = 0; i < 8; ++i) {
      raw.push_back(static_cast<char>(static_cast<std::uint64_t>(v) & 0xffU));
      v >>= 8;
    }
  }
  w.bytes_field(9, raw);
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

struct Reader {
  const std::string& bytes;
  std::size_t pos = 0;

  std::uint64_t varint() {
    std::uint64_t value = 0;
    for (int shift = 0; shift < 64; shift += 7) {
      if (pos >= bytes.size()) throw std::runtime_error("protobuf truncado");
      const unsigned char c = static_cast<unsigned char>(bytes[pos++]);
      value |= static_cast<std::uint64_t>(c & 0x7fU) << shift;
      if ((c & 0x80U) == 0) return value;
    }
    throw std::runtime_error("varint protobuf invalido");
  }

  std::string length_delimited() {
    const std::uint64_t n = varint();
    if (n > bytes.size() - pos) throw std::runtime_error("campo protobuf truncado");
    const std::string out = bytes.substr(pos, static_cast<std::size_t>(n));
    pos += static_cast<std::size_t>(n);
    return out;
  }

  void skip(std::uint32_t wire) {
    if (wire == 0) {
      (void)varint();
    } else if (wire == 1) {
      if (bytes.size() - pos < 8) throw std::runtime_error("campo protobuf truncado");
      pos += 8;
    } else if (wire == 2) {
      (void)length_delimited();
    } else if (wire == 5) {
      if (bytes.size() - pos < 4) throw std::runtime_error("campo protobuf truncado");
      pos += 4;
    } else {
      throw std::runtime_error("wire type protobuf nao suportado");
    }
  }
};

std::uint32_t field_number(std::uint64_t key) { return static_cast<std::uint32_t>(key >> 3); }
std::uint32_t wire_type(std::uint64_t key) { return static_cast<std::uint32_t>(key & 7U); }

void parse_packed_dims(const std::string& packed, std::vector<std::int64_t>& dims) {
  Reader r{packed};
  while (r.pos < packed.size()) dims.push_back(static_cast<std::int64_t>(r.varint()));
}

void parse_float_bytes(const std::string& raw, std::vector<float>& out) {
  if (raw.size() % 4 != 0) throw std::runtime_error("raw_data FLOAT32 com tamanho invalido");
  out.resize(raw.size() / 4);
  for (std::size_t i = 0; i < out.size(); ++i) {
    std::uint32_t bits = 0;
    for (int b = 0; b < 4; ++b) {
      bits |= static_cast<std::uint32_t>(static_cast<unsigned char>(raw[i * 4 + b])) << (8 * b);
    }
    std::memcpy(&out[i], &bits, sizeof(float));
  }
}

OnnxTensor parse_tensor_proto(const std::string& bytes) {
  Reader r{bytes};
  OnnxTensor tensor;
  std::int64_t data_type = 0;
  std::string raw_data;
  std::vector<float> float_data;
  while (r.pos < bytes.size()) {
    const std::uint64_t key = r.varint();
    const std::uint32_t field = field_number(key);
    const std::uint32_t wire = wire_type(key);
    if (field == 1 && wire == 2) {
      parse_packed_dims(r.length_delimited(), tensor.shape);
    } else if (field == 1 && wire == 0) {
      tensor.shape.push_back(static_cast<std::int64_t>(r.varint()));
    } else if (field == 2 && wire == 0) {
      data_type = static_cast<std::int64_t>(r.varint());
    } else if (field == 4 && wire == 2) {
      parse_float_bytes(r.length_delimited(), float_data);
    } else if (field == 4 && wire == 5) {
      std::uint32_t bits = 0;
      if (r.bytes.size() - r.pos < 4) throw std::runtime_error("campo FLOAT32 truncado");
      for (int b = 0; b < 4; ++b)
        bits |= static_cast<std::uint32_t>(static_cast<unsigned char>(r.bytes[r.pos++])) << (8 * b);
      float value = 0.0F;
      std::memcpy(&value, &bits, sizeof(value));
      float_data.push_back(value);
    } else if (field == 8 && wire == 2) {
      tensor.name = r.length_delimited();
    } else if (field == 9 && wire == 2) {
      raw_data = r.length_delimited();
    } else {
      r.skip(wire);
    }
  }
  tensor.data_type = data_type;
  if (data_type != 1) return tensor;
  std::size_t count = 1;
  for (std::int64_t dim : tensor.shape) {
    if (dim <= 0 || static_cast<std::uint64_t>(dim) >
                        static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()) / count)
      throw std::runtime_error("forma invalida no inicializador");
    count *= static_cast<std::size_t>(dim);
  }
  if (!raw_data.empty())
    parse_float_bytes(raw_data, tensor.data);
  else
    tensor.data = std::move(float_data);
  if (tensor.data.size() != count)
    throw std::runtime_error("dados com tamanho errado no inicializador");
  return tensor;
}

void parse_graph(const std::string& bytes, std::vector<OnnxTensor>& tensors) {
  Reader r{bytes};
  while (r.pos < bytes.size()) {
    const std::uint64_t key = r.varint();
    const std::uint32_t field = field_number(key);
    const std::uint32_t wire = wire_type(key);
    if (field == 5 && wire == 2) {
      OnnxTensor tensor = parse_tensor_proto(r.length_delimited());
      if (tensor.data_type == 1) tensors.push_back(std::move(tensor));
    } else
      r.skip(wire);
  }
}

void parse_model(const std::string& bytes, std::vector<OnnxTensor>& tensors) {
  Reader r{bytes};
  while (r.pos < bytes.size()) {
    const std::uint64_t key = r.varint();
    const std::uint32_t field = field_number(key);
    const std::uint32_t wire = wire_type(key);
    if (field == 7 && wire == 2)
      parse_graph(r.length_delimited(), tensors);
    else
      r.skip(wire);
  }
}

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
    if (l.kind == OnnxLayer::Recorrente) {
      if (l.w.rank() != 2 || l.u.rank() != 2 || l.b.rank() != 1 || l.w.shape[1] != l.b.shape[0] ||
          l.u.shape[1] != l.b.shape[0])
        die("peso recorrente com forma invalida");
      const std::int64_t input_size = l.w.shape[0];
      const std::int64_t gate_width = l.b.shape[0];
      const std::int64_t hidden = l.u.shape[0];
      const int gate_count = l.recorrente_tipo == "rnn" ? 1 : (l.recorrente_tipo == "lstm" ? 4 : 3);
      if (gate_width != static_cast<std::int64_t>(gate_count) * hidden || input_size != dim)
        die("camada recorrente com dimensoes incompativeis");
      const std::vector<int> order =
          l.recorrente_tipo == "lstm" ? std::vector<int>{0, 3, 1, 2} : std::vector<int>{0, 1, 2};
      std::vector<float> ow(static_cast<std::size_t>(input_size * gate_width));
      std::vector<float> orr(static_cast<std::size_t>(hidden * gate_width));
      std::vector<float> ob(static_cast<std::size_t>(2 * gate_width), 0.0F);
      for (int gate = 0; gate < gate_count; ++gate) {
        const int source = order[static_cast<std::size_t>(gate)];
        for (std::int64_t i = 0; i < input_size; ++i)
          for (std::int64_t j = 0; j < hidden; ++j)
            ow[static_cast<std::size_t>(i * gate_width + gate * hidden + j)] =
                l.w.data[static_cast<std::size_t>(i * gate_width + source * hidden + j)];
        for (std::int64_t i = 0; i < hidden; ++i)
          for (std::int64_t j = 0; j < hidden; ++j)
            orr[static_cast<std::size_t>(i * gate_width + gate * hidden + j)] =
                l.u.data[static_cast<std::size_t>(i * gate_width + source * hidden + j)];
        for (std::int64_t j = 0; j < hidden; ++j)
          ob[static_cast<std::size_t>(gate * hidden + j)] =
              l.b.data[static_cast<std::size_t>(source * hidden + j)];
      }
      const std::string wn = "W_rec" + std::to_string(seq);
      const std::string rn = "R_rec" + std::to_string(seq);
      const std::string bn = "B_rec" + std::to_string(seq);
      initializers.push_back(tensor_proto(wn, {1, input_size, gate_width}, ow));
      initializers.push_back(tensor_proto(rn, {1, hidden, gate_width}, orr));
      initializers.push_back(tensor_proto(bn, {1, 2 * gate_width}, ob));
      const std::string y = fresh("ry");
      const std::string yh = fresh("rh");
      std::vector<std::string> outputs = {y, yh};
      if (l.recorrente_tipo == "lstm") outputs.push_back(fresh("rc"));
      const std::string op =
          l.recorrente_tipo == "rnn" ? "RNN" : (l.recorrente_tipo == "lstm" ? "LSTM" : "GRU");
      nodes.push_back(
          node_proto(op, {cur, wn, rn, bn}, outputs, "recurrent" + std::to_string(seq),
                     {attr_int("hidden_size", hidden), attr_string("direction", "forward")}));
      const std::string axes = "rec_axes" + std::to_string(seq);
      initializers.push_back(tensor_proto_int64(axes, {1}, {0}));
      const std::string out = fresh("r");
      nodes.push_back(
          node_proto("Squeeze", {yh, axes}, {out}, "recurrent_squeeze" + std::to_string(seq), {}));
      cur = out;
      dim = hidden;
      out_dim = hidden;
    } else if (l.kind == OnnxLayer::Dense) {
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
                                 "layernorm" + std::to_string(seq), {attr_int("axis", -1)}));
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
      attrs.push_back(attr_ints("pads", {l.padding, l.padding, l.padding, l.padding}));
      attrs.push_back(attr_ints("dilations", {l.dilatacao, l.dilatacao}));
      nodes.push_back(
          node_proto("Conv", {cur, wn, bn}, {out}, "conv" + std::to_string(seq), attrs));
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
                                 "batchnorm" + std::to_string(seq), {attr_float("eps", 1e-5f)}));
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
      nodes.push_back(
          node_proto("MaxPool", {cur}, {out}, "maxpool" + std::to_string(seq),
                     {attr_ints("kernel_shape", {l.janela, l.janela}),
                      attr_ints("strides", {l.passo, l.passo}), attr_ints("pads", {0, 0, 0, 0})}));
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

bool onnx_carregar_tensores(const std::string& path, std::vector<OnnxTensor>& tensors,
                            std::string& err) {
  tensors.clear();
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    err = "nao foi possivel abrir o arquivo ONNX";
    return false;
  }
  const std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  try {
    parse_model(bytes, tensors);
    if (tensors.empty()) throw std::runtime_error("modelo sem inicializadores");
  } catch (const std::exception& e) {
    tensors.clear();
    err = e.what();
    return false;
  }
  return true;
}

}  // namespace tilt::rt
