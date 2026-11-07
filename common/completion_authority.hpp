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

// The completion authority used by the rig probe, kept separate so the
// self-test exercises the same decision the callback makes.
#pragma once

#include <cstddef>
#include <string>

#include "raw_http.hpp"
#include "s3_protocol.hpp"

namespace s3rdma {

inline bool plain_digits(const std::string& s, size_t min_len, size_t max_len) {
  if (s.size() < min_len || s.size() > max_len) return false;
  for (char c : s) if (c < '0' || c > '9') return false;
  return true;
}

// Observations derived from one raw receipt; no decoding, no merging.
struct ReceiptView {
  bool present = false;        // head complete and well formed
  bool complete = false;       // whole message received, well framed, within caps, no transport failure
  int status = 0;
  size_t reply_count = 0, count_count = 0;
  std::string reply_raw, count_raw;
  bool reply_plain = false, count_plain = false;
  long declared = -1;
  size_t body_content = 0, body_octets = 0;
  bool body_truncated = false;
  std::string framing;
};

inline ReceiptView view_receipt(const RawResponse& r, size_t callback_size) {
  ReceiptView v;
  v.present = r.head_complete && !r.head_malformed;
  v.complete = v.present && r.message_complete && !r.transport_error && !r.body_truncated;
  v.status = r.status;
  v.body_content = r.body_content;
  v.body_octets = r.body_octets;
  v.body_truncated = r.body_truncated;
  v.framing = r.framing;
  v.reply_count = r.count(kHdrRdmaReply);
  v.count_count = r.count(kHdrRdmaBytesTransferred);
  if (const RawField* f = r.first(kHdrRdmaReply)) v.reply_raw = f->value;
  if (const RawField* f = r.first(kHdrRdmaBytesTransferred)) v.count_raw = f->value;
  v.reply_plain = v.reply_count == 1 && plain_digits(v.reply_raw, 3, 3);
  v.count_plain = v.count_count == 1 && plain_digits(v.count_raw, 1, 20);
  v.declared = parse_rdma_bytes_transferred(v.count_count > 0, v.count_raw, callback_size);
  return v;
}

// A GET is complete only when the whole message arrived well framed, the
// status is 200/206, exactly one plain three-digit reply matches the
// convention, exactly one plain count equals the expected landing length and,
// on the standard contract, the HTTP body is empty. Legacy fake-S3 answers
// reply "200" for 200/206 and may carry its small bounded body.
inline bool completion_authority(const ReceiptView& v, const ProtocolOptions& proto, size_t expected,
                                 size_t requested) {
  if (!v.complete) return false;
  if (v.status != 200 && v.status != 206) return false;
  if (!v.reply_plain || !v.count_plain) return false;
  const int reply = (v.reply_raw[0] - '0') * 100 + (v.reply_raw[1] - '0') * 10 + (v.reply_raw[2] - '0');
  if (v.declared < 0 || static_cast<size_t>(v.declared) != expected) return false;
  if (proto.token_encoding == TokenEncoding::Base64) return reply == 200 && expected == requested;
  return reply == v.status && v.body_content == 0;
}

}  // namespace s3rdma
