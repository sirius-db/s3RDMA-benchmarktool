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

// Minimal base64 encode/decode used to carry the opaque cuObject RDMA
// descriptor (cufileRDMAInfo_t::desc_str, which is binary) inside an HTTP
// header on the fake-S3 control path. Header-only, no external deps.
#pragma once

#include <cstdint>
#include <string>

namespace s3rdma {

inline std::string base64_encode(const void* data, size_t len) {
  static const char tbl[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  const auto* p = static_cast<const unsigned char*>(data);
  std::string out;
  out.reserve(((len + 2) / 3) * 4);
  for (size_t i = 0; i < len; i += 3) {
    uint32_t n = p[i] << 16;
    if (i + 1 < len) n |= p[i + 1] << 8;
    if (i + 2 < len) n |= p[i + 2];
    out.push_back(tbl[(n >> 18) & 0x3F]);
    out.push_back(tbl[(n >> 12) & 0x3F]);
    out.push_back(i + 1 < len ? tbl[(n >> 6) & 0x3F] : '=');
    out.push_back(i + 2 < len ? tbl[n & 0x3F] : '=');
  }
  return out;
}

inline std::string base64_decode(const std::string& in) {
  auto val = [](unsigned char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;  // padding or invalid
  };
  std::string out;
  out.reserve((in.size() / 4) * 3);
  int buf = 0, bits = 0;
  for (unsigned char c : in) {
    int v = val(c);
    if (v < 0) continue;  // skip '=' / whitespace
    buf = (buf << 6) | v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<char>((buf >> bits) & 0xFF));
    }
  }
  return out;
}

}  // namespace s3rdma
