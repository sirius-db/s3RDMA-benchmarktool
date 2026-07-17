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

#include "verify/manifest.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace s3rdma::verify {

namespace {

bool is_lower_hex_64(const std::string& s) {
  if (s.size() != 64) return false;
  for (char c : s) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

// Strict decimal: nonempty, digits only, no sign/whitespace, overflow-checked.
bool parse_u64_strict(const std::string& s, uint64_t& out) {
  if (s.empty()) return false;
  uint64_t value = 0;
  for (char c : s) {
    if (c < '0' || c > '9') return false;
    const uint64_t digit = static_cast<uint64_t>(c - '0');
    if (value > (UINT64_MAX - digit) / 10) return false;
    value = value * 10 + digit;
  }
  out = value;
  return true;
}

std::vector<std::string> split_tabs(const std::string& line) {
  std::vector<std::string> fields;
  size_t start = 0;
  while (true) {
    const size_t tab = line.find('\t', start);
    if (tab == std::string::npos) {
      fields.push_back(line.substr(start));
      return fields;
    }
    fields.push_back(line.substr(start, tab - start));
    start = tab + 1;
  }
}

}  // namespace

Manifest parse_manifest(std::istream& in, ParseStatus& status) {
  Manifest manifest;
  status = ParseStatus{};

  auto fail = [&status](size_t line_no, std::string message) {
    status.ok = false;
    status.line = line_no;
    status.error = std::move(message);
  };

  std::string line;
  size_t line_no = 0;  // physical lines, including skipped ones
  while (std::getline(in, line)) {
    ++line_no;
    const size_t first_ns = line.find_first_not_of(' ');
    if (first_ns == std::string::npos) continue;  // blank
    if (line[first_ns] == '#') continue;          // comment

    const std::vector<std::string> fields = split_tabs(line);
    if (fields.size() < 3) {
      fail(line_no, "expected key<TAB>size<TAB>full_sha256");
      return manifest;
    }

    ManifestEntry entry;
    entry.key = fields[0];
    if (entry.key.empty()) {
      fail(line_no, "empty object key");
      return manifest;
    }
    if (!parse_u64_strict(fields[1], entry.size)) {
      fail(line_no, "invalid size '" + fields[1] + "' for key '" + entry.key + "'");
      return manifest;
    }
    if (entry.size == 0) {
      fail(line_no, "zero size for key '" + entry.key + "'");
      return manifest;
    }
    entry.full_sha256 = fields[2];
    if (!is_lower_hex_64(entry.full_sha256)) {
      fail(line_no, "full digest for key '" + entry.key +
                        "' is not exactly 64 lowercase hex chars");
      return manifest;
    }

    for (size_t i = 3; i < fields.size(); ++i) {
      const std::string& token = fields[i];
      const std::string prefix_tag = "prefix:";
      if (token.compare(0, prefix_tag.size(), prefix_tag) != 0) {
        fail(line_no, "malformed prefix token '" + token + "'");
        return manifest;
      }
      const size_t eq = token.find('=', prefix_tag.size());
      if (eq == std::string::npos) {
        fail(line_no, "prefix token '" + token + "' has no '='");
        return manifest;
      }
      const std::string len_str = token.substr(prefix_tag.size(), eq - prefix_tag.size());
      uint64_t prefix_len = 0;
      if (!parse_u64_strict(len_str, prefix_len)) {
        fail(line_no, "invalid prefix length '" + len_str + "'");
        return manifest;
      }
      if (prefix_len == 0) {
        fail(line_no, "zero prefix length");
        return manifest;
      }
      const std::string digest = token.substr(eq + 1);
      if (!is_lower_hex_64(digest)) {
        fail(line_no, "prefix digest at length " + len_str +
                          " is not exactly 64 lowercase hex chars");
        return manifest;
      }
      if (!entry.prefix_sha256.emplace(prefix_len, digest).second) {
        fail(line_no, "duplicate prefix length " + len_str + " for key '" +
                          entry.key + "'");
        return manifest;
      }
    }

    if (!manifest.by_key.emplace(entry.key, entry).second) {
      fail(line_no, "duplicate record for key '" + entry.key + "'");
      return manifest;
    }
  }

  status.ok = true;
  return manifest;
}

ClosureStatus validate_closure(const Manifest& manifest,
                               const std::vector<RequestedSpan>& spans,
                               const std::map<std::string, uint64_t>& head_sizes) {
  ClosureStatus status;

  auto fail = [&status](std::string message) {
    status.ok = false;
    status.error = std::move(message);
    return status;
  };

  // Anti-fail-open: a verified run that would check nothing must not start.
  if (spans.empty()) {
    return fail("no requested spans: a verified run must check at least one span");
  }

  for (const RequestedSpan& span : spans) {
    const auto entry_it = manifest.by_key.find(span.key);
    if (entry_it == manifest.by_key.end()) {
      return fail("key '" + span.key + "' absent from the manifest");
    }
    const auto head_it = head_sizes.find(span.key);
    if (head_it == head_sizes.end()) {
      return fail("key '" + span.key + "' absent from the HEAD results");
    }
    const ManifestEntry& entry = entry_it->second;
    if (entry.size != head_it->second) {
      return fail("size mismatch for key '" + span.key + "': manifest " +
                  std::to_string(entry.size) + " vs HEAD " +
                  std::to_string(head_it->second));
    }
    if (span.offset != 0) {
      return fail("unsupported span for key '" + span.key +
                  "': only full-object and prefix spans are verifiable");
    }
    if (span.size == 0) continue;  // full object: full_sha256 is always present
    if (entry.prefix_sha256.find(span.size) == entry.prefix_sha256.end()) {
      return fail("no prefix digest for key '" + span.key + "' at length " +
                  std::to_string(span.size));
    }
  }

  status.ok = true;
  return status;
}

}  // namespace s3rdma::verify
