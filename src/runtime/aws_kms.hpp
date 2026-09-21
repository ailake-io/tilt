#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace tilt::rt {

// GenerateDataKey AES_256; a chave em claro e retornada em memoria e o blob
// cifrado deve ser persistido no FileCryptoMetaData do Parquet.
void aws_kms_generate_data_key(const std::string& key_id,
                               std::array<std::uint8_t, 32>& plaintext,
                               std::string& ciphertext_blob,
                               std::string& resolved_key_id);

// Decrypt do blob KMS para reconstruir a chave AES de 256 bits.
void aws_kms_decrypt_data_key(const std::string& key_id,
                              const std::string& ciphertext_blob,
                              std::array<std::uint8_t, 32>& plaintext);

}  // namespace tilt::rt
