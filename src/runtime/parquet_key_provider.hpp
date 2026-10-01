#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace tilt::rt {

// Integrações HTTP sem SDK para wrapping de uma data key AES-256 usada pelo
// Modular Encryption do Parquet. Aceita tokens estáticos, arquivos de segredo,
// Azure OAuth/IMDS, GCP metadata e renovação Vault; tokens nunca entram no
// metadata do arquivo.
void parquet_cloud_generate(const std::string& provider, const std::string& key_id,
                            std::array<std::uint8_t, 32>& plaintext,
                            std::string& ciphertext, std::string& resolved_key_id);
void parquet_cloud_decrypt(const std::string& provider, const std::string& key_id,
                           const std::string& ciphertext,
                           std::array<std::uint8_t, 32>& plaintext);

}  // namespace tilt::rt
