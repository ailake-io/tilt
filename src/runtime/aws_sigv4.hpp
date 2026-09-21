#pragma once

#include <string>
#include <utility>
#include <vector>

namespace tilt::rt {

struct AwsSigV4Credentials {
  std::string access_key;
  std::string secret_key;
  std::string session_token;
  std::string region;
};

struct AwsSigV4Signature {
  std::string authorization;
  std::string amz_date;
};

// Assina uma CanonicalRequest AWS SigV4. `extra_headers` precisa estar em
// minusculas e e incluido tanto na assinatura quanto na lista de headers.
AwsSigV4Signature aws_sigv4_sign(
    const AwsSigV4Credentials& credentials, const std::string& service,
    const std::string& method, const std::string& canonical_uri,
    const std::string& canonical_query, const std::string& host,
    const std::string& payload_hash,
    const std::vector<std::pair<std::string, std::string>>& extra_headers = {});

}  // namespace tilt::rt
