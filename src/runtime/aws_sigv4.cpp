#include "runtime/aws_sigv4.hpp"

#include <algorithm>
#include <array>
#include <ctime>
#include <string>
#include <utility>
#include <vector>

#include "runtime/compat.hpp"
#include "runtime/sha256.hpp"

namespace tilt::rt {

namespace {

std::string hex_lower(const std::array<std::uint8_t, 32>& bytes) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out(64, '0');
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    out[2 * i] = kDigits[(bytes[i] >> 4) & 0x0F];
    out[2 * i + 1] = kDigits[bytes[i] & 0x0F];
  }
  return out;
}

}  // namespace

AwsSigV4Signature aws_sigv4_sign(
    const AwsSigV4Credentials& credentials, const std::string& service, const std::string& method,
    const std::string& canonical_uri, const std::string& canonical_query, const std::string& host,
    const std::string& payload_hash,
    const std::vector<std::pair<std::string, std::string>>& extra_headers) {
  const std::time_t agora = std::time(nullptr);
  const std::tm tm_utc = tilt_gmtime(agora);
  char data_buf[9];
  std::strftime(data_buf, sizeof data_buf, "%Y%m%d", &tm_utc);
  const std::string date_stamp = data_buf;
  char amz_buf[17];
  std::strftime(amz_buf, sizeof amz_buf, "%Y%m%dT%H%M%SZ", &tm_utc);
  const std::string amz_date = amz_buf;

  const std::string scope = date_stamp + "/" + credentials.region + "/" + service + "/aws4_request";
  std::vector<std::pair<std::string, std::string>> canon = {
      {"host", host},
      {"x-amz-content-sha256", payload_hash},
      {"x-amz-date", amz_date},
  };
  canon.insert(canon.end(), extra_headers.begin(), extra_headers.end());
  if (!credentials.session_token.empty()) {
    canon.emplace_back("x-amz-security-token", credentials.session_token);
  }
  std::sort(canon.begin(), canon.end());

  std::string canonical_headers;
  std::string signed_headers;
  for (std::size_t i = 0; i < canon.size(); ++i) {
    canonical_headers += canon[i].first + ":" + canon[i].second + "\n";
    if (i) signed_headers += ";";
    signed_headers += canon[i].first;
  }
  const std::string canonical_request = method + "\n" + canonical_uri + "\n" + canonical_query +
                                        "\n" + canonical_headers + "\n" + signed_headers + "\n" +
                                        payload_hash;
  const std::string string_to_sign =
      "AWS4-HMAC-SHA256\n" + amz_date + "\n" + scope + "\n" + sha256_hex(canonical_request);

  const auto k_date = hmac_sha256_raw("AWS4" + credentials.secret_key, date_stamp);
  const auto k_region = hmac_sha256_raw(k_date, credentials.region);
  const auto k_service = hmac_sha256_raw(k_region, service);
  const auto k_signing = hmac_sha256_raw(k_service, "aws4_request");
  const std::string signature = hex_lower(hmac_sha256_raw(k_signing, string_to_sign));

  AwsSigV4Signature result;
  result.amz_date = amz_date;
  result.authorization = "AWS4-HMAC-SHA256 Credential=" + credentials.access_key + "/" + scope +
                         ", SignedHeaders=" + signed_headers + ", Signature=" + signature;
  return result;
}

}  // namespace tilt::rt
