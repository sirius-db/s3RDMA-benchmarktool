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

// Integrity-manifest parsing and preflight closure for online GET verification.
// SDK-free (std only): compiles and runs on any dev host without cuObject / CUDA.
// The manifest binds each object key to its expected size and content digests so
// a sampled GET can be checked against a known-good value; closure refuses to
// start a verified run that could later report zero effective checks.
#pragma once

#include <cstdint>
#include <istream>
#include <map>
#include <string>
#include <vector>

namespace s3rdma::verify {

// One manifest record: full-object digest plus optional prefix digests keyed by
// prefix length (so a ranged GET of [0, N) can be checked without the full hash).
struct ManifestEntry {
  std::string key;
  uint64_t size = 0;                              // bytes; must match HEAD
  std::string full_sha256;                        // exactly 64 lowercase hex
  std::map<uint64_t, std::string> prefix_sha256;  // prefix_len -> 64-hex digest
};

struct Manifest {
  std::map<std::string, ManifestEntry> by_key;
};

struct ParseStatus {
  bool ok = false;
  std::string error;   // human-readable; empty iff ok
  size_t line = 0;     // 1-based offending line; 0 when not line-specific
};

// Strict, line-oriented TSV. Each record line is:
//   key <TAB> size <TAB> full_sha256 [ <TAB> prefix:<len>=<sha256> ]...
// Blank lines and lines whose first non-space char is '#' are skipped.
// Rejected (ParseStatus.ok=false, with the offending line): duplicate key,
// conflicting record for a key, size non-numeric or zero, empty key, a digest
// that is not exactly 64 lowercase hex chars, a malformed prefix token, or a
// duplicate prefix length within one record.
Manifest parse_manifest(std::istream& in, ParseStatus& status);

// A concrete GET the runbook intends to verify. size==0 means "full object"
// (its length comes from HEAD); size>0 with offset==0 is a prefix range [0,size).
struct RequestedSpan {
  std::string key;
  uint64_t offset = 0;
  uint64_t size = 0;
};

struct ClosureStatus {
  bool ok = false;
  std::string error;
};

// Preflight closure — the anti-fail-open gate. For every requested span:
//   * the key exists in the manifest,
//   * the manifest size equals head_sizes[key] (both present),
//   * a full-object span resolves to full_sha256,
//   * a prefix span [0, N) resolves to prefix_sha256[N].
// Any missing key, size mismatch, or absent digest -> ok=false with a specific
// error. Never silently skips: a run that would verify nothing must not start.
ClosureStatus validate_closure(const Manifest& manifest,
                               const std::vector<RequestedSpan>& spans,
                               const std::map<std::string, uint64_t>& head_sizes);

}  // namespace s3rdma::verify
