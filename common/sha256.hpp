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

// SHA256 hex digest of a host buffer, via OpenSSL libcrypto. Used by the client
// to verify fetched objects against the `sha256sum` values printed by
// scripts/gen_testdata.sh.
#pragma once

#include <cstddef>
#include <cstdio>
#include <string>

#include <openssl/sha.h>

namespace s3rdma {

inline std::string sha256_hex(const void* data, size_t len) {
  unsigned char digest[SHA256_DIGEST_LENGTH];
  SHA256(static_cast<const unsigned char*>(data), len, digest);
  static const char* hex = "0123456789abcdef";
  std::string out;
  out.reserve(SHA256_DIGEST_LENGTH * 2);
  for (unsigned char b : digest) {
    out.push_back(hex[b >> 4]);
    out.push_back(hex[b & 0xF]);
  }
  return out;
}

}  // namespace s3rdma
