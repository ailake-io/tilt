#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace tilt::rt {

// SHA-256 (FIPS 180-4) e HMAC-SHA256 (RFC 2104, bloco de 64 bytes),
// implementacao propria sem dependencias. Os *_hex devolvem hex lowercase;
// os *_raw expoem os 32 bytes para a cadeia de assinatura do AWS SigV4.
std::array<std::uint8_t, 32> sha256_raw(const std::string& data);
std::string sha256_hex(const std::string& data);

std::array<std::uint8_t, 32> hmac_sha256_raw(const std::string& key,
                                             const std::string& data);
std::array<std::uint8_t, 32> hmac_sha256_raw(const std::array<std::uint8_t, 32>& key,
                                             const std::string& data);
std::string hmac_sha256_hex(const std::string& key, const std::string& data);

}  // namespace tilt::rt
