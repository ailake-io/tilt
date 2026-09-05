#include "runtime/sha256.hpp"

#include <cstring>

namespace tilt::rt {

namespace {

constexpr std::uint32_t kK[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
    0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
    0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
    0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

constexpr std::uint32_t kH0[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                                  0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};

std::uint32_t rotr(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

std::array<std::uint8_t, 32> sha256_bytes(const std::uint8_t* data, std::size_t len) {
  std::uint32_t h[8];
  std::memcpy(h, kH0, sizeof h);

  // Mensagem com padding: 0x80, zeros ate len % 64 == 56, comprimento em bits
  // big-endian (64 bits). Processa em blocos de 64 bytes.
  const std::uint64_t bit_len = static_cast<std::uint64_t>(len) * 8u;
  std::size_t padded_len = len + 1 + 8;
  padded_len = (padded_len + 63u) & ~std::size_t(63u);
  std::string padded(padded_len, '\0');
  std::memcpy(padded.data(), data, len);
  padded[len] = static_cast<char>(0x80);
  for (int i = 0; i < 8; ++i) {
    padded[padded_len - 1 - static_cast<std::size_t>(i)] =
        static_cast<char>((bit_len >> (8 * i)) & 0xFFu);
  }

  for (std::size_t off = 0; off < padded_len; off += 64) {
    std::uint32_t w[64];
    for (int t = 0; t < 16; ++t) {
      const std::uint8_t* p =
          reinterpret_cast<const std::uint8_t*>(padded.data() + off) + 4 * t;
      w[t] = (static_cast<std::uint32_t>(p[0]) << 24) |
             (static_cast<std::uint32_t>(p[1]) << 16) |
             (static_cast<std::uint32_t>(p[2]) << 8) |
             static_cast<std::uint32_t>(p[3]);
    }
    for (int t = 16; t < 64; ++t) {
      const std::uint32_t s0 = rotr(w[t - 15], 7) ^ rotr(w[t - 15], 18) ^ (w[t - 15] >> 3);
      const std::uint32_t s1 = rotr(w[t - 2], 17) ^ rotr(w[t - 2], 19) ^ (w[t - 2] >> 10);
      w[t] = w[t - 16] + s0 + w[t - 7] + s1;
    }

    std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
    std::uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int t = 0; t < 64; ++t) {
      const std::uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
      const std::uint32_t ch = (e & f) ^ ((~e) & g);
      const std::uint32_t temp1 = hh + S1 + ch + kK[t] + w[t];
      const std::uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
      const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      const std::uint32_t temp2 = S0 + maj;
      hh = g;
      g = f;
      f = e;
      e = d + temp1;
      d = c;
      c = b;
      b = a;
      a = temp1 + temp2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += hh;
  }

  std::array<std::uint8_t, 32> out{};
  for (int i = 0; i < 8; ++i) {
    out[4 * i] = static_cast<std::uint8_t>((h[i] >> 24) & 0xFFu);
    out[4 * i + 1] = static_cast<std::uint8_t>((h[i] >> 16) & 0xFFu);
    out[4 * i + 2] = static_cast<std::uint8_t>((h[i] >> 8) & 0xFFu);
    out[4 * i + 3] = static_cast<std::uint8_t>(h[i] & 0xFFu);
  }
  return out;
}

std::string hex_encode(const std::uint8_t* bytes, std::size_t n) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out(2 * n, '0');
  for (std::size_t i = 0; i < n; ++i) {
    out[2 * i] = kDigits[(bytes[i] >> 4) & 0x0F];
    out[2 * i + 1] = kDigits[bytes[i] & 0x0F];
  }
  return out;
}

// HMAC-SHA256 com a chave ja ajustada ao bloco de 64 bytes (RFC 2104:
// chave > 64 bytes e' hasheada antes, senao zero-padded ate 64).
std::array<std::uint8_t, 32> hmac_sha256_block(const std::array<std::uint8_t, 64>& key,
                                               const std::string& data) {
  std::uint8_t ipad[64];
  std::uint8_t opad[64];
  for (int i = 0; i < 64; ++i) {
    ipad[i] = static_cast<std::uint8_t>(key[static_cast<std::size_t>(i)] ^ 0x36);
    opad[i] = static_cast<std::uint8_t>(key[static_cast<std::size_t>(i)] ^ 0x5c);
  }
  std::string inner;
  inner.reserve(64 + data.size());
  inner.append(reinterpret_cast<const char*>(ipad), 64);
  inner.append(data);
  const auto inner_hash = sha256_bytes(
      reinterpret_cast<const std::uint8_t*>(inner.data()), inner.size());
  std::string outer;
  outer.reserve(64 + inner_hash.size());
  outer.append(reinterpret_cast<const char*>(opad), 64);
  outer.append(reinterpret_cast<const char*>(inner_hash.data()), inner_hash.size());
  return sha256_bytes(reinterpret_cast<const std::uint8_t*>(outer.data()), outer.size());
}

}  // namespace

std::array<std::uint8_t, 32> sha256_raw(const std::string& data) {
  return sha256_bytes(reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
}

std::string sha256_hex(const std::string& data) {
  const auto digest = sha256_raw(data);
  return hex_encode(digest.data(), digest.size());
}

std::array<std::uint8_t, 32> hmac_sha256_raw(const std::array<std::uint8_t, 32>& key,
                                             const std::string& data) {
  std::array<std::uint8_t, 64> block{};
  std::memcpy(block.data(), key.data(), key.size());
  return hmac_sha256_block(block, data);
}

std::array<std::uint8_t, 32> hmac_sha256_raw(const std::string& key,
                                             const std::string& data) {
  std::array<std::uint8_t, 64> block{};
  if (key.size() > 64) {
    const auto digest = sha256_raw(key);
    std::memcpy(block.data(), digest.data(), digest.size());
  } else {
    std::memcpy(block.data(), key.data(), key.size());
  }
  return hmac_sha256_block(block, data);
}

std::string hmac_sha256_hex(const std::string& key, const std::string& data) {
  const auto mac = hmac_sha256_raw(key, data);
  return hex_encode(mac.data(), mac.size());
}

}  // namespace tilt::rt
