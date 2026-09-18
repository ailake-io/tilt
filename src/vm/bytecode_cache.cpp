#include "vm/bytecode_cache.hpp"

#include <cstring>
#include <fstream>

#include "runtime/sha256.hpp"

namespace tilt::vm {

namespace {

constexpr char kMagic[] = "TILTC1";

void put_u8(std::string& o, std::uint8_t v) { o.push_back(static_cast<char>(v)); }

void put_u32(std::string& o, std::uint32_t v) {
  for (int k = 0; k < 4; ++k) o.push_back(static_cast<char>((v >> (8 * k)) & 0xFF));
}

void put_i32(std::string& o, std::int32_t v) { put_u32(o, static_cast<std::uint32_t>(v)); }

void put_u64(std::string& o, std::uint64_t v) {
  for (int k = 0; k < 8; ++k) o.push_back(static_cast<char>((v >> (8 * k)) & 0xFF));
}

void put_str(std::string& o, const std::string& s) {
  put_u32(o, static_cast<std::uint32_t>(s.size()));
  o.append(s);
}

struct Reader {
  const char* p = nullptr;
  std::size_t n = 0;
  bool ok = true;

  bool take_u8(std::uint8_t& v) {
    if (n < 1) return ok = false;
    v = static_cast<std::uint8_t>(*p);
    ++p;
    --n;
    return true;
  }
  bool take_u32(std::uint32_t& v) {
    if (n < 4) return ok = false;
    v = 0;
    for (int k = 0; k < 4; ++k) v |= static_cast<std::uint32_t>(static_cast<unsigned char>(p[k])) << (8 * k);
    p += 4;
    n -= 4;
    return true;
  }
  bool take_bytes(std::string& s, std::size_t len) {
    if (n < len) return ok = false;
    s.assign(p, len);
    p += len;
    n -= len;
    return true;
  }
  bool take_str(std::string& s) {
    std::uint32_t len = 0;
    if (!take_u32(len)) return false;
    if (len > n || len > (1u << 24)) return ok = false;  // sanidade: 16 MiB
    return take_bytes(s, len);
  }
};

// true se o const serializa (escreve o payload); falso = pular o chunk.
bool put_const(std::string& o, const rt::Value& v) {
  switch (v.kind) {
    case rt::ValueKind::Nulo: put_u8(o, 0); return true;
    case rt::ValueKind::Logico:
      put_u8(o, 1);
      put_u8(o, v.b ? 1 : 0);
      return true;
    case rt::ValueKind::Inteiro:
      put_u8(o, 2);
      put_u64(o, static_cast<std::uint64_t>(v.i));
      return true;
    case rt::ValueKind::Decimal: {
      put_u8(o, 3);
      std::uint64_t bits = 0;
      std::memcpy(&bits, &v.d, sizeof bits);
      put_u64(o, bits);
      return true;
    }
    case rt::ValueKind::Texto:
      put_u8(o, 4);
      put_str(o, v.s);
      return true;
    default:
      return false;  // lista/mapa/tabela/tensor: fora do cache
  }
}

bool get_const(Reader& r, rt::Value& v) {
  std::uint8_t tag = 0;
  if (!r.take_u8(tag)) return false;
  switch (tag) {
    case 0:
      v = rt::Value::nulo();
      return true;
    case 1: {
      std::uint8_t b = 0;
      if (!r.take_u8(b)) return false;
      v = rt::Value::logico(b != 0);
      return true;
    }
    case 2: {
      if (r.n < 8) return r.ok = false;
      std::uint64_t w = 0;
      for (int k = 0; k < 8; ++k)
        w |= static_cast<std::uint64_t>(static_cast<unsigned char>(r.p[k])) << (8 * k);
      r.p += 8;
      r.n -= 8;
      v = rt::Value::inteiro(static_cast<std::int64_t>(w));
      return true;
    }
    case 3: {
      if (r.n < 8) return r.ok = false;
      std::uint64_t bits = 0;
      for (int k = 0; k < 8; ++k)
        bits |= static_cast<std::uint64_t>(static_cast<unsigned char>(r.p[k])) << (8 * k);
      r.p += 8;
      r.n -= 8;
      double d = 0.0;
      std::memcpy(&d, &bits, sizeof d);
      v = rt::Value::decimal(d);
      return true;
    }
    case 4: {
      std::string s;
      if (!r.take_str(s)) return false;
      v = rt::Value::texto(std::move(s));
      return true;
    }
    default:
      r.ok = false;
      return false;
  }
}

}  // namespace

std::array<std::uint8_t, 32> tiltc_sha(const std::string& bytes) { return rt::sha256_raw(bytes); }

std::string tiltc_path_for(const std::string& source_path) { return source_path + "c"; }

bool tiltc_save(const std::string& path, const std::string& source_bytes,
                const CachedProgram& prog) {
  std::string o;
  o.append(kMagic, 6);
  put_u32(o, kTiltcVersion);
  const auto sha = tiltc_sha(source_bytes);
  o.append(reinterpret_cast<const char*>(sha.data()), sha.size());
  // Filtra entries nao serializaveis antes de contar.
  std::vector<std::pair<std::string, CachedChunk>> kept;
  for (const auto& [name, cc] : prog.entries) {
    bool ok = true;
    for (const auto& c : cc.chunk.consts) {
      std::string probe;
      if (!put_const(probe, c)) {
        ok = false;
        break;
      }
    }
    // Valida ops conhecidos (fail-closed contra ops futuros sem suporte).
    for (const auto& in : cc.chunk.code) {
      if (static_cast<std::uint8_t>(in.op) > static_cast<std::uint8_t>(Op::ReturnNil)) ok = false;
    }
    if (ok) kept.emplace_back(name, cc);
  }
  put_u32(o, static_cast<std::uint32_t>(kept.size()));
  for (const auto& [name, cc] : kept) {
    put_u8(o, cc.is_pipeline ? 0 : 1);
    put_str(o, name);
    put_i32(o, cc.nparams);
    put_i32(o, cc.chunk.num_locals);
    put_u32(o, static_cast<std::uint32_t>(cc.chunk.code.size()));
    for (const auto& in : cc.chunk.code) {
      put_u8(o, static_cast<std::uint8_t>(in.op));
      put_i32(o, in.a);
      put_i32(o, in.b);
    }
    put_u32(o, static_cast<std::uint32_t>(cc.chunk.consts.size()));
    for (const auto& c : cc.chunk.consts) put_const(o, c);
    put_u32(o, static_cast<std::uint32_t>(cc.chunk.op_names.size()));
    for (const auto& s : cc.chunk.op_names) put_str(o, s);
    put_u32(o, static_cast<std::uint32_t>(cc.chunk.names.size()));
    for (const auto& s : cc.chunk.names) put_str(o, s);
  }
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) return false;
  f.write(o.data(), static_cast<std::streamsize>(o.size()));
  f.close();
  return static_cast<bool>(f);
}

bool tiltc_load(const std::string& path, const std::string& source_bytes, CachedProgram& out) {
  out = CachedProgram{};
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  Reader r{data.data(), data.size()};
  if (r.n < 6 || std::string(r.p, 6) != kMagic) return false;
  r.p += 6;
  r.n -= 6;
  std::uint32_t ver = 0;
  if (!r.take_u32(ver) || ver != kTiltcVersion) return false;
  if (r.n < 32) return false;
  const auto want = tiltc_sha(source_bytes);
  if (std::string(r.p, 32) != std::string(reinterpret_cast<const char*>(want.data()), 32)) {
    return false;  // fonte mudou: ignora
  }
  r.p += 32;
  r.n -= 32;
  std::uint32_t nentries = 0;
  if (!r.take_u32(nentries) || nentries > 100000) return false;
  for (std::uint32_t k = 0; k < nentries; ++k) {
    std::uint8_t kind = 0;
    std::string name;
    std::uint32_t ncode = 0, nconsts = 0, nopnames = 0, nnames = 0;
    std::int32_t nparams = 0, num_locals = 0;
    auto take_i32 = [&](std::int32_t& v) {
      std::uint32_t w = 0;
      if (!r.take_u32(w)) return false;
      v = static_cast<std::int32_t>(w);
      return true;
    };
    if (!r.take_u8(kind) || (kind != 0 && kind != 1) || !r.take_str(name) || !take_i32(nparams) ||
        !take_i32(num_locals) || !r.take_u32(ncode) || ncode > 1000000) {
      return false;
    }
    CachedChunk cc;
    cc.is_pipeline = (kind == 0);
    cc.nparams = nparams;
    cc.chunk.num_locals = num_locals;
    for (std::uint32_t i = 0; i < ncode; ++i) {
      std::uint8_t op = 0;
      std::int32_t a = 0, b = 0;
      if (!r.take_u8(op) || !take_i32(a) || !take_i32(b)) return false;
      if (op > static_cast<std::uint8_t>(Op::ReturnNil)) return false;
      cc.chunk.code.push_back({static_cast<Op>(op), a, b});
    }
    if (!r.take_u32(nconsts) || nconsts > 1000000) return false;
    for (std::uint32_t i = 0; i < nconsts; ++i) {
      rt::Value v;
      if (!get_const(r, v)) return false;
      cc.chunk.consts.push_back(std::move(v));
    }
    if (!r.take_u32(nopnames) || nopnames > 100000) return false;
    for (std::uint32_t i = 0; i < nopnames; ++i) {
      std::string s;
      if (!r.take_str(s)) return false;
      cc.chunk.op_names.push_back(std::move(s));
    }
    if (!r.take_u32(nnames) || nnames > 100000) return false;
    for (std::uint32_t i = 0; i < nnames; ++i) {
      std::string s;
      if (!r.take_str(s)) return false;
      cc.chunk.names.push_back(std::move(s));
    }
    out.entries[name] = std::move(cc);
  }
  out.source_sha = want;
  return r.ok;
}

}  // namespace tilt::vm
