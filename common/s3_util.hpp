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

// Small shared helpers for the fake-S3 control path: HTTP header names used to
// carry RDMA metadata, plus parsing of object paths and Range headers.
#pragma once

#include <cstdint>
#include <string>

namespace s3rdma {

// HTTP header names exchanged on the control path (see doc/design.md).
// Only standard cuObject headers are used — see
// https://docs.nvidia.com/gpudirect-storage/cuobject/index.html
inline constexpr const char* kHdrRdmaToken = "x-amz-rdma-token";   // base64 desc_str
inline constexpr const char* kHdrRdmaReply = "x-amz-rdma-reply";   // RDMA status ("200" = OK)

// Parse the remote buffer base address from a decoded RDMA descriptor.
// DC_V1 format: "buf_address:buf_size:rkey:lid:dctn:rocev2_flag:gid"
// (all fields hex except rocev2_flag which is decimal).
// Returns 0 on parse failure.
inline uint64_t parse_rdma_remote_addr(const std::string& rdma_descr) {
  auto colon = rdma_descr.find(':');
  if (colon == std::string::npos || colon == 0) return 0;
  try {
    return std::stoull(rdma_descr.substr(0, colon), nullptr, 16);
  } catch (...) {
    return 0;
  }
}

// Parsed "/<bucket>/<object>" request path.
struct ObjectPath {
  std::string bucket;
  std::string object;
  bool valid = false;
};

inline ObjectPath parse_object_path(const std::string& path) {
  ObjectPath p;
  if (path.empty() || path[0] != '/') return p;
  auto slash = path.find('/', 1);
  if (slash == std::string::npos || slash + 1 >= path.size()) return p;
  p.bucket = path.substr(1, slash - 1);
  p.object = path.substr(slash + 1);
  if (p.bucket == "." || p.bucket == ".." || p.bucket.find('/') != std::string::npos)
    return p;
  if (p.object == "." || p.object == ".." ||
      p.object.find("/../") != std::string::npos ||
      p.object.rfind("../", 0) == 0 ||
      (p.object.size() >= 3 && p.object.compare(p.object.size() - 3, 3, "/..") == 0))
    return p;
  p.valid = !p.bucket.empty() && !p.object.empty();
  return p;
}

// Parsed inclusive byte range from a "bytes=a-b" Range header.
struct ByteRange {
  uint64_t start = 0;
  uint64_t end = 0;   // inclusive
  bool present = false;
  bool valid = false;
};

// Parse "bytes=a-b" (a and b required for this demo). file_size is used to
// clamp / validate. Returns present=false when no range string is given.
inline ByteRange parse_range(const std::string& spec, uint64_t file_size) {
  ByteRange r;
  if (spec.empty()) return r;
  r.present = true;
  const std::string prefix = "bytes=";
  if (spec.rfind(prefix, 0) != 0) return r;  // not "bytes="
  std::string body = spec.substr(prefix.size());
  auto dash = body.find('-');
  if (dash == std::string::npos) return r;
  try {
    r.start = std::stoull(body.substr(0, dash));
    r.end = std::stoull(body.substr(dash + 1));
  } catch (...) {
    return r;
  }
  if (r.start > r.end || r.end >= file_size) return r;
  r.valid = true;
  return r;
}

}  // namespace s3rdma
