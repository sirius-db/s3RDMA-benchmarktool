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

// Control-plane protocol options shared by the client and the probe: how the
// RDMA descriptor is carried, how the endpoint's reply is read, and whether
// requests are SigV4-signed. Defaults keep the shipped fake-S3 behaviour;
// a standard cuObject gateway needs verbatim tokens and, usually, SigV4.
// Contract: cuObject docs (x-amz-rdma-token / x-amz-rdma-reply) and NVIDIA's
// multi-storage-client s3_cuobject provider (x-amz-rdma-bytes-transferred,
// UNSIGNED-PAYLOAD signing, 501-or-absent = declined).
#pragma once

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <limits>
#include <string>

#include "base64.hpp"
#include "s3_util.hpp"
#include "sigv4.hpp"

namespace s3rdma {

inline constexpr const char* kHdrRdmaBytesTransferred = "x-amz-rdma-bytes-transferred";
inline constexpr int kRdmaReplyDeclined = 501;

enum class TokenEncoding { Verbatim, Base64 };
enum class AuthMode { None, SigV4 };

struct ProtocolOptions {
  TokenEncoding token_encoding = TokenEncoding::Base64;  // shipped fake-S3 convention
  AuthMode auth = AuthMode::None;
  std::string region = "us-east-1";
  SigV4Credentials credentials;
};

inline bool parse_token_encoding(const std::string& s, TokenEncoding& out) {
  if (s == "verbatim") { out = TokenEncoding::Verbatim; return true; }
  if (s == "base64") { out = TokenEncoding::Base64; return true; }
  return false;
}

inline bool parse_auth_mode(const std::string& s, AuthMode& out) {
  if (s == "none") { out = AuthMode::None; return true; }
  if (s == "sigv4") { out = AuthMode::SigV4; return true; }
  return false;
}

// Verbatim headers omit trailing NUL bytes; legacy Base64 preserves all len bytes.
inline std::string encode_rdma_token(const char* desc, size_t len, TokenEncoding enc) {
  if (enc == TokenEncoding::Base64) return base64_encode(desc, len);
  while (len > 0 && desc[len - 1] == '\0') --len;
  return std::string(desc, len);
}

// Headers for one control-plane request. `host_header` is the Host value the
// HTTP client will send; `token`/`range` are added only when non-empty.
inline HeaderList control_plane_headers(const ProtocolOptions& o, const std::string& method,
                                        const std::string& host_header, const std::string& path,
                                        const std::string& range, const std::string& token,
                                        std::time_t now = std::time(nullptr)) {
  HeaderList own;
  if (!token.empty()) own.emplace_back(kHdrRdmaToken, token);
  if (!range.empty()) own.emplace_back("Range", range);
  if (o.auth != AuthMode::SigV4) return own;
  SigV4Request req;
  req.method = method;
  req.host = host_header;
  req.path = path;
  req.headers = own;
  HeaderList all = own;
  for (auto& h : sigv4_sign(req, o.credentials, o.region, "s3", sigv4_amz_date(now)).headers)
    all.push_back(std::move(h));
  return all;
}

struct RdmaReply {
  enum class Kind { Declined, Code, Malformed };
  Kind kind = Kind::Declined;
  int code = 0;  // valid for Kind::Code
};

inline bool all_digits(const std::string& s) {
  if (s.empty()) return false;
  for (unsigned char c : s) if (c < '0' || c > '9') return false;
  return true;
}

// x-amz-rdma-reply: absent/empty or 501 = the endpoint did not use RDMA;
// an HTTP-style code otherwise; anything else is malformed.
inline RdmaReply parse_rdma_reply(bool present, const std::string& value) {
  if (!present || value.empty()) return {};
  if (!all_digits(value)) return {RdmaReply::Kind::Malformed, 0};
  int code = 0;
  for (char c : value) {
    const int digit = c - '0';
    if (code > (599 - digit) / 10)
      return {RdmaReply::Kind::Malformed, 0};
    code = code * 10 + digit;
  }
  if (code < 100) return {RdmaReply::Kind::Malformed, 0};
  if (code == kRdmaReplyDeclined) return {};
  return {RdmaReply::Kind::Code, code};
}

// Absent/empty means zero; otherwise accept ASCII decimal digits, including
// leading zeros, up to both the buffer size and LONG_MAX. Invalid input is -1.
inline long parse_rdma_bytes_transferred(bool present, const std::string& value,
                                         size_t size) {
  if (!present || value.empty()) return 0;
  if (!all_digits(value)) return -1;
  const size_t max_return = static_cast<size_t>(std::numeric_limits<long>::max());
  const size_t limit = size < max_return ? size : max_return;
  size_t n = 0;
  for (char c : value) {
    const size_t digit = static_cast<size_t>(c - '0');
    if (n > limit / 10 || (n == limit / 10 && digit > limit % 10)) return -1;
    n = n * 10 + digit;
  }
  return static_cast<long>(n);
}

}  // namespace s3rdma
