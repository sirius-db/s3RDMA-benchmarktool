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

// SDK-free JSONL helpers shared by the rig tools. json_escape maps every
// control or non-ASCII byte to \u00XX (a byte-to-U+00XX convention, not UTF-8
// decoding). Opaque header bytes may additionally travel as labelled hex
// (`*_hex`); loader paths are currently emitted as escaped text only.
#pragma once

#include <cstdio>
#include <mutex>
#include <string>

namespace s3rdma {

inline std::string json_escape(const std::string& s) {
  std::string out;
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') { out.push_back('\\'); out.push_back(static_cast<char>(c)); }
    else if (c < 0x20 || c >= 0x7f) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); out += b; }
    else out.push_back(static_cast<char>(c));
  }
  return out;
}

inline std::string hex_bytes(const std::string& bytes) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string out;
  for (unsigned char c : bytes) {
    out.push_back(digits[c >> 4]);
    out.push_back(digits[c & 15]);
  }
  return out;
}

inline std::mutex& jsonl_mutex() {
  static std::mutex m;
  return m;
}

// Latched when a record could not be written or flushed; a receipt with a
// gap can never carry a successful qualification.
inline bool& jsonl_output_failed() {
  static bool failed = false;
  return failed;
}

// One record per line, emitted atomically and flushed.
class JsonRec {
 public:
  explicit JsonRec(const std::string& event) { raw("event", "\"" + json_escape(event) + "\""); }
  JsonRec& str(const char* k, const std::string& v) { return raw(k, "\"" + json_escape(v) + "\""); }
  JsonRec& hex(const char* k, const std::string& bytes) { return raw(k, "\"" + hex_bytes(bytes) + "\""); }
  JsonRec& num(const char* k, long long v) { return raw(k, std::to_string(v)); }
  JsonRec& boolean(const char* k, bool v) { return raw(k, v ? "true" : "false"); }
  JsonRec& null(const char* k) { return raw(k, "null"); }
  void emit() {
    std::lock_guard<std::mutex> lk(jsonl_mutex());
    if (std::printf("{%s}\n", body_.c_str()) < 0 || std::fflush(stdout) != 0) jsonl_output_failed() = true;
  }

 private:
  JsonRec& raw(const char* k, const std::string& v) {
    if (!body_.empty()) body_ += ",";
    body_ += "\"" + std::string(k) + "\":" + v;
    return *this;
  }
  std::string body_;
};

}  // namespace s3rdma
