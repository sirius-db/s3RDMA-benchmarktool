/*
 * Copyright 2025, Sirius Contributors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// AWS Signature Version 4 for the S3 control path (header authentication,
// single chunk). The RDMA endpoint contract sends an empty body, so callers
// sign with the UNSIGNED-PAYLOAD hash; the x-amz-rdma-token header is one of
// the signed headers and must therefore be final before signing.
#pragma once

#include <cstdlib>
#include <ctime>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>

#include "sha256.hpp"

namespace s3rdma {

inline constexpr const char* kUnsignedPayload = "UNSIGNED-PAYLOAD";

struct SigV4Credentials {
  std::string access_key;
  std::string secret_key;
  std::string session_token;  // optional
};

// Reads AWS_ACCESS_KEY_ID / AWS_SECRET_ACCESS_KEY / AWS_SESSION_TOKEN.
inline bool sigv4_credentials_from_env(SigV4Credentials& out) {
  const char* ak = std::getenv("AWS_ACCESS_KEY_ID");
  const char* sk = std::getenv("AWS_SECRET_ACCESS_KEY");
  const char* st = std::getenv("AWS_SESSION_TOKEN");
  if (!ak || !*ak || !sk || !*sk) return false;
  out.access_key = ak;
  out.secret_key = sk;
  out.session_token = st ? st : "";
  return true;
}

using HeaderList = std::vector<std::pair<std::string, std::string>>;

// `path` is raw; `query` is already canonical. Caller headers retain their
// wire order. Host, date, payload-hash, session-token and Authorization
// headers are managed here and must not also appear in `headers`.
struct SigV4Request {
  std::string method;
  std::string host;  // Host header value, "name" or "name:port"
  std::string path;
  std::string query;
  HeaderList headers;
  std::string payload_hash = kUnsignedPayload;
};

struct SigV4Result {
  HeaderList headers;  // Wire authentication headers; never log unredacted.
  std::string canonical_request;  // Contains session/RDMA tokens; sensitive.
  std::string string_to_sign;
  std::string signature;
};

inline std::string sigv4_amz_date(std::time_t t) {
  std::tm tm{};
  gmtime_r(&t, &tm);
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y%m%dT%H%M%SZ", &tm);
  return buf;
}

namespace sigv4_detail {

inline std::string lower(std::string s) {
  for (auto& c : s)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return s;
}

// Trim and collapse interior runs of spaces, as the canonical header rules require.
inline std::string canonical_value(const std::string& v) {
  std::string out;
  bool pending_space = false;
  for (char c : v) {
    if (c == ' ' || c == '\t') {
      pending_space = !out.empty();
      continue;
    }
    if (pending_space) out.push_back(' ');
    pending_space = false;
    out.push_back(c);
  }
  return out;
}

// RFC 3986 unreserved characters pass; '/' is kept for paths (S3 single encoding).
inline std::string uri_encode(const std::string& s, bool keep_slash) {
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : s) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' ||
        c == '.' || c == '~' || (keep_slash && c == '/')) {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(hex[c >> 4]);
      out.push_back(hex[c & 0xF]);
    }
  }
  return out;
}

inline std::string hmac_sha256(const std::string& key, const std::string& data) {
  if (key.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
    throw std::runtime_error("HMAC-SHA256 key is too long");
  unsigned char out[EVP_MAX_MD_SIZE];
  unsigned int len = 0;
  if (HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
           reinterpret_cast<const unsigned char*>(data.data()), data.size(),
           out, &len) == nullptr || len != SHA256_DIGEST_LENGTH)
    throw std::runtime_error("HMAC-SHA256 failed");
  return std::string(reinterpret_cast<const char*>(out), len);
}

inline std::string hex(const std::string& bin) {
  static const char* digits = "0123456789abcdef";
  std::string out;
  out.reserve(bin.size() * 2);
  for (unsigned char c : bin) {
    out.push_back(digits[c >> 4]);
    out.push_back(digits[c & 0xF]);
  }
  return out;
}

}  // namespace sigv4_detail

inline SigV4Result sigv4_sign(const SigV4Request& req, const SigV4Credentials& cred,
                              const std::string& region, const std::string& service,
                              const std::string& amz_date) {
  using namespace sigv4_detail;
  const std::string date = amz_date.substr(0, 8);

  std::map<std::string, std::string> hdrs{
      {"host", canonical_value(req.host)},
      {"x-amz-content-sha256", req.payload_hash},
      {"x-amz-date", amz_date}};
  if (!cred.session_token.empty())
    hdrs.emplace("x-amz-security-token", cred.session_token);
  for (const auto& h : req.headers) {
    const std::string name = lower(h.first);
    if (name == "host" || name == "x-amz-date" ||
        name == "x-amz-content-sha256" || name == "x-amz-security-token" ||
        name == "authorization")
      throw std::invalid_argument("SigV4 caller headers contain a managed header");
    const std::string value = canonical_value(h.second);
    auto [it, inserted] = hdrs.emplace(name, value);
    if (!inserted) it->second += "," + value;
  }

  std::string canonical_headers, signed_headers;
  for (const auto& h : hdrs) {
    canonical_headers += h.first + ":" + h.second + "\n";
    if (!signed_headers.empty()) signed_headers += ";";
    signed_headers += h.first;
  }

  SigV4Result r;
  r.canonical_request = req.method + "\n" + uri_encode(req.path, true) + "\n" + req.query + "\n" +
                        canonical_headers + "\n" + signed_headers + "\n" + req.payload_hash;
  const std::string scope = date + "/" + region + "/" + service + "/aws4_request";
  r.string_to_sign = "AWS4-HMAC-SHA256\n" + amz_date + "\n" + scope + "\n" +
                     sha256_hex(r.canonical_request.data(), r.canonical_request.size());

  std::string k = hmac_sha256("AWS4" + cred.secret_key, date);
  k = hmac_sha256(k, region);
  k = hmac_sha256(k, service);
  k = hmac_sha256(k, "aws4_request");
  r.signature = hex(hmac_sha256(k, r.string_to_sign));

  r.headers.emplace_back("x-amz-content-sha256", req.payload_hash);
  r.headers.emplace_back("x-amz-date", amz_date);
  if (!cred.session_token.empty()) r.headers.emplace_back("x-amz-security-token", cred.session_token);
  r.headers.emplace_back("Authorization", "AWS4-HMAC-SHA256 Credential=" + cred.access_key + "/" + scope +
                                              ", SignedHeaders=" + signed_headers +
                                              ", Signature=" + r.signature);
  return r;
}

}  // namespace s3rdma
