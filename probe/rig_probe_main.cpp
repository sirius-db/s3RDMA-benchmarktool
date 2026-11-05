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

// Concurrent and lifecycle diagnostics stay outside the serial scope probe.

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <functional>
#include <thread>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

#include <cuda_runtime.h>
#include <cuobjclient.h>

#include "buffers.hpp"
#include "jsonl.hpp"
#include "completion_authority.hpp"
#include "guard_witness.hpp"
#include "raw_http.hpp"
#include "s3_protocol.hpp"
#include "s3_util.hpp"
#include "sha256.hpp"

namespace {

using s3rdma::Buffer;
using s3rdma::BufferMode;
using Rec = s3rdma::JsonRec;

enum class Verdict { Pass, Fail, Inconclusive, EnvError };
const char* verdict_name(Verdict v) {
  switch (v) {
    case Verdict::Pass: return "PASS";
    case Verdict::Fail: return "FAIL";
    case Verdict::Inconclusive: return "INCONCLUSIVE";
    case Verdict::EnvError: return "ENV_ERROR";
  }
  return "?";
}
int verdict_exit(Verdict v) {
  switch (v) {
    case Verdict::Pass: return 0;
    case Verdict::Fail: return 1;
    case Verdict::Inconclusive: return 4;
    case Verdict::EnvError: return 3;
  }
  return 3;
}

long long now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Diagnostic emission that can never escape: an allocation or output failure
// while reporting latches the evidence/output failure instead of unwinding.
template <class F>
void report(F&& f) noexcept {
  try { f(); } catch (...) { s3rdma::jsonl_output_failed() = true; }
}

// Fallback after a reporting failure: preserve non-PASS readings; mark a
// candidate PASS as INCONCLUSIVE and return 3.
int terminal_fallback(Verdict v) noexcept {
  s3rdma::jsonl_output_failed() = true;
  if (v == Verdict::Pass) v = Verdict::Inconclusive;  // same incomplete-output ceiling as finish()
  std::fputs("{\"event\":\"result\",\"verdict\":\"", stdout);
  std::fputs(verdict_name(v), stdout);
  std::fputs("\",\"detail\":\"terminal reporting failed after the reading was selected\",\"evidence_output_failed\":true}\n", stdout);
  std::fflush(stdout);
  return 3;
}

int finish(Verdict v, const std::string& detail) {
  if (s3rdma::jsonl_output_failed() && v == Verdict::Pass) v = Verdict::Inconclusive;
  Rec("result").str("verdict", verdict_name(v)).str("detail", detail)
      .boolean("evidence_output_failed", s3rdma::jsonl_output_failed()).emit();
  return s3rdma::jsonl_output_failed() ? 3 : verdict_exit(v);
}

constexpr size_t kMaxReg = 4ULL * 1024 * 1024 * 1024 - 64 * 1024;  // CUOBJ_MAX_MEMORY_REG_SIZE
constexpr size_t kSsizeMax = static_cast<size_t>(std::numeric_limits<ssize_t>::max());
constexpr size_t kMaxBody = 64 * 1024;  // an unexpected HTTP body is evidence, not payload
constexpr size_t kMaxHead = 64 * 1024;

bool checked_add(size_t a, size_t b, size_t& out) {
  if (a > std::numeric_limits<size_t>::max() - b) return false;
  out = a + b;
  return true;
}
bool checked_mul(size_t a, size_t b, size_t& out) {
  if (a != 0 && b > std::numeric_limits<size_t>::max() / a) return false;
  out = a * b;
  return true;
}

// ---- Options ----------------------------------------------------------------

struct Options {
  std::string exp, host = "127.0.0.1", bucket, object, mode = "gpu", expect_sha, label = "diag";
  std::vector<std::string> objects;
  int port = 8080;
  size_t slot_bytes = 8ULL * 1024 * 1024;
  size_t guard = 64ULL * 1024;
  size_t slots = 16;
  size_t range_start = 0, range_end = 0;  // Inclusive endpoints when range_given is true.
  bool range_given = false;
  size_t expected_bytes = 0;
  bool expected_given = false;
  std::string neg_class = "known-complete";
  std::string guard_mode = "retryable";
  int iterations = 20;
  int deadline_s = 60;
  bool abrupt = false;
  bool child = false;
  s3rdma::ProtocolOptions proto;
};

void usage(const char* a0) {
  std::fprintf(stderr,
               "Usage: %s --exp <attr|reg|diag|rt-guard|c|exit> [--bucket <b> --object <k>] [options]\n"
               "  --server <host:port>     default 127.0.0.1:8080\n"
               "  --mode <host|gpu>        default gpu (reg always measures a cudaMalloc block)\n"
               "  --slot-bytes <n>         S (default 8388608)\n"
               "  --slots <n>              W (default 16)\n"
               "  --guard <n>              guard band per slot (default 65536; must be > 0)\n"
               "  --range <a-b>            diag/rt-guard: inclusive request range (default 0..S-1)\n"
               "  --expected-bytes <n>     diag/rt-guard: expected landing length (default: request length)\n"
               "  --case <label>           diag: case label for the records\n"
               "  --class <known-complete|unknown|no-emission>  diag: declared completion class\n"
               "  --expect-sha <hex64>     digest of the expected landing span (required for PASS)\n"
               "  --guard-mode <retryable|enotsup>  rt-guard: injected first-invocation return\n"
               "  --objects <k1,k2,...>    c: one object per slot (else slot k reads [k*S,(k+1)*S) of --object)\n"
               "  --iterations <n>         c: concurrent iterations (default 20)\n"
               "  --deadline-s <n>         exit: parent deadline (default 60)\n"
               "  --abrupt                 exit: child leaves via _exit (abrupt-exit observation)\n"
               "%s",
               a0, s3rdma::kProtocolUsage);
}

bool parse_size(const char* s, size_t& out) {
  if (s == nullptr || *s == '\0') return false;
  size_t value = 0;
  for (; *s != '\0'; ++s) {
    const unsigned char c = static_cast<unsigned char>(*s);
    if (c < '0' || c > '9') return false;
    const size_t digit = static_cast<size_t>(c - '0');
    if (value > (std::numeric_limits<size_t>::max() - digit) / 10) return false;
    value = value * 10 + digit;
  }
  out = value;
  return true;
}

bool is_lower_hex_64(const std::string& s) {
  if (s.size() != 64) return false;
  for (char c : s)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  return true;
}

bool parse_args(int argc, char** argv, Options& o) {
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : nullptr; };
    const char* v = nullptr;
    size_t n = 0;
    if (a == "--exp" && (v = next())) o.exp = v;
    else if (a == "--server" && (v = next())) {
      const std::string s = v;
      const auto c = s.find(':');
      if (c == std::string::npos || c == 0 || !parse_size(s.c_str() + c + 1, n) || n == 0 || n > 65535) return false;
      o.host = s.substr(0, c);
      o.port = static_cast<int>(n);
    } else if (a == "--bucket" && (v = next())) o.bucket = v;
    else if (a == "--object" && (v = next())) o.object = v;
    else if (a == "--objects" && (v = next())) {
      const std::string s = v;
      size_t p = 0;
      while (true) {
        const size_t q = s.find(',', p);
        const std::string item = s.substr(p, q == std::string::npos ? std::string::npos : q - p);
        if (item.empty()) return false;
        o.objects.push_back(item);
        if (q == std::string::npos) break;
        p = q + 1;
      }
    } else if (a == "--mode" && (v = next())) {
      o.mode = v;
      if (o.mode != "host" && o.mode != "gpu") return false;
    } else if (a == "--slot-bytes" && (v = next())) { if (!parse_size(v, o.slot_bytes)) return false; }
    else if (a == "--slots" && (v = next())) { if (!parse_size(v, o.slots)) return false; }
    else if (a == "--guard" && (v = next())) { if (!parse_size(v, o.guard)) return false; }
    else if (a == "--range" && (v = next())) {
      const std::string s = v;
      const auto d = s.find('-');
      if (d == std::string::npos) return false;
      size_t e = 0;
      if (!parse_size(s.substr(0, d).c_str(), n) || !parse_size(s.c_str() + d + 1, e) || e < n) return false;
      o.range_start = n;
      o.range_end = e;
      o.range_given = true;
    } else if (a == "--expected-bytes" && (v = next())) { if (!parse_size(v, o.expected_bytes)) return false; o.expected_given = true; }
    else if (a == "--case" && (v = next())) o.label = v;
    else if (a == "--class" && (v = next())) {
      o.neg_class = v;
      if (o.neg_class != "known-complete" && o.neg_class != "unknown" && o.neg_class != "no-emission") return false;
    } else if (a == "--expect-sha" && (v = next())) {
      o.expect_sha = v;
      if (!is_lower_hex_64(o.expect_sha)) return false;
    } else if (a == "--guard-mode" && (v = next())) {
      o.guard_mode = v;
      if (o.guard_mode != "retryable" && o.guard_mode != "enotsup") return false;
    } else if (a == "--iterations" && (v = next())) { if (!parse_size(v, n) || n == 0 || n > 100000) return false; o.iterations = static_cast<int>(n); }
    else if (a == "--deadline-s" && (v = next())) { if (!parse_size(v, n) || n == 0 || n > 3600) return false; o.deadline_s = static_cast<int>(n); }
    else if (a == "--abrupt") o.abrupt = true;
    else if (a == "--child") o.child = true;
    else if (auto f = s3rdma::parse_protocol_flag(a, (i + 1 < argc) ? argv[i + 1] : nullptr, o.proto); f.matched) {
      if (!f.ok) return false;
      if (f.used_value) ++i;
    } else return false;
  }
  if (o.exp.empty()) return false;
  const bool needs_object = o.exp == "diag" || o.exp == "rt-guard" || o.exp == "exit" ||
                            (o.exp == "c" && o.objects.empty());
  if (o.exp != "attr" && o.exp != "reg" && o.bucket.empty()) return false;
  if (needs_object && o.object.empty()) return false;
  return true;
}

// Geometry validated before any CUDA/SDK work. S must fit the SDK's signed
// sizes; every span must lie inside the allocation; guards are never zero.
struct Geometry {
  size_t cell = 0, arena = 0, expected = 0;
};

bool validate_geometry(const Options& o, size_t slots, Geometry& g, std::string& why) {
  size_t two_guard = 0;
  if (o.slot_bytes == 0 || o.slot_bytes > kSsizeMax) { why = "S must be 1..SSIZE_MAX"; return false; }
  if (o.guard == 0) { why = "guard must be > 0"; return false; }
  if (slots == 0 || slots > 256) { why = "W must be 1..256"; return false; }
  if (!checked_mul(o.guard, 2, two_guard) || !checked_add(o.slot_bytes, two_guard, g.cell) ||
      !checked_mul(g.cell, slots, g.arena) || g.arena > kMaxReg) {
    why = "W*(S+2G) overflows or exceeds CUOBJ_MAX_MEMORY_REG_SIZE";
    return false;
  }
  const size_t start = o.range_given ? o.range_start : 0;
  const size_t end = o.range_given ? o.range_end : o.slot_bytes - 1;
  size_t len = 0;
  if (end < start || !checked_add(end - start, 1, len) || len > o.slot_bytes) { why = "request range longer than S"; return false; }
  g.expected = o.expected_given ? o.expected_bytes : len;
  if (g.expected > len) { why = "--expected-bytes exceeds the request length"; return false; }
  if (o.exp == "c" && o.objects.empty()) {
    size_t last = 0;
    if (!checked_mul(o.slot_bytes, slots, last)) { why = "W*S overflows"; return false; }
  }
  return true;
}

// ---- GPU visibility (C§9 hold) ----------------------------------------------

struct Visibility {
  bool queried = false;
  int raw = -1;
  std::string name = "unknown";
  bool trustworthy = false;
};

Visibility read_visibility(BufferMode mode) {
  Visibility v;
  if (mode == BufferMode::Host) { v.name = "host_na"; v.trustworthy = true; return v; }
  int val = 0;
  if (cudaDeviceGetAttribute(&val, cudaDevAttrGPUDirectRDMAWritesOrdering, 0) != cudaSuccess) {
    (void)cudaGetLastError();
    v.name = "query_failed";
    return v;
  }
  v.queried = true;
  v.raw = val;
  if (val == cudaGPUDirectRDMAWritesOrderingAllDevices) { v.name = "all_devices"; v.trustworthy = true; }
  else if (val == cudaGPUDirectRDMAWritesOrderingOwner) { v.name = "owner"; v.trustworthy = true; }
  else if (val == cudaGPUDirectRDMAWritesOrderingNone) { v.name = "none"; }
  return v;
}

// ---- Per-attempt records ----------------------------------------------------

struct Attempt {
  int invocation = 0;
  long long t_begin = 0, t_end = 0;
  bool guarded = false, http_attempt = false, response = false, evidence_complete = true;
  int http_status = 0;
  std::string transport_error, request_error;
  bool reply_present = false, reply_unambiguous = false;
  std::string reply_raw, reply_kind;       // reply_raw = exact field bytes (OWS-trimmed), never decoded
  bool head_complete = false, head_malformed = false, request_partial = false, body_truncated = false;
  bool message_complete = false;   // whole HTTP response received and well framed
  bool entered_while_active = false;
  std::thread::id thread;            // identity used by the witness
  unsigned long long thread_id = 0;  // hash, diagnostic only
  long long body_octets = 0;
  bool probe_failure = false;      // request could not be built/sent by the probe itself (nothing token-bearing emitted)
  std::string framing;
  long long reply_count = 0, count_field_count = 0, request_bytes = 0;
  bool count_plain = false;                // count field present once with a plain decimal spelling
  std::string response_head;               // exact response head bytes
  int reply_code = 0;
  bool content_range_present = false;
  std::string content_range;
  bool count_present = false;
  long declared = 0;
  long long body_bytes = 0, cb_size = 0, cb_offset = 0;
  long long natural_return = 0, returned = 0;
  bool injection_applied = false;
  bool qualified_complete = false;   // authority from the validated raw receipt
  bool completion_candidate = false; // parsed/canonical candidate, diagnosis only
};

struct RigContext {
  const s3rdma::ProtocolOptions* proto = nullptr;
  std::string run_id, who, host, path, range;
  int port = 8080;
  long long get_id = 0, iteration = -1, slot = -1;
  size_t expected = 0;
  bool guard = false;
  long first_return = 0;
  std::atomic<int> invocations{0};
  std::atomic<int> http_attempts{0};   // complete, validated, token-bearing request messages written
  std::atomic<int> partial_messages{0};  // incomplete writes only
  std::atomic<int> tokenless_messages{0};
  std::atomic<int> active{0};            // callbacks of this context currently inside the probe
  std::mutex entry_mu;                   // makes invocation number and overlap observation one coherent step
  std::atomic<bool> local_failure{false};  // a probe-origin failure happened in this context
  std::atomic<int> submitted{0};   // set before the HTTP call: a request may be on the wire from here on
  std::atomic<int> completed{0};   // advanced only by a qualified completion receipt
  std::atomic<int> responses{0};
  std::atomic<bool> unknown{false};        // a possibly effective submission without completion proof
  std::atomic<bool> evidence_lost{false};  // a record could not be kept
  std::mutex mu;
  std::vector<Attempt> attempts;
  void push(const Attempt& a) {
    try {
      std::lock_guard<std::mutex> lk(mu);
      attempts.push_back(a);
    } catch (...) {
      evidence_lost.store(true);
    }
  }
};

// Parsed status/count candidate for diagnosis; it does not check receipt framing, provenance or field uniqueness.
bool completion_candidate(const Attempt& a, const s3rdma::ProtocolOptions& proto, size_t expected, size_t requested) {
  if (!a.response || (a.http_status != 200 && a.http_status != 206)) return false;
  if (a.reply_kind != "code" || !a.count_present || a.declared < 0 || static_cast<size_t>(a.declared) != expected) return false;
  return proto.token_encoding == s3rdma::TokenEncoding::Base64 ? (a.reply_code == 200 && expected == requested)
                                                                : a.reply_code == a.http_status;
}

ssize_t rig_callback_impl(const void* handle, char* ptr, size_t size, loff_t offset,
                          const cufileRDMAInfo_t* rdma) {
  (void)ptr;
  auto* ctx = static_cast<RigContext*>(cuObjClient::getCtx(handle));
  if (ctx == nullptr) return -ENOTSUP;
  Attempt at;
  {
    std::lock_guard<std::mutex> lk(ctx->entry_mu);  // coherent: ordinal and overlap flag from one critical section
    at.invocation = ctx->invocations.fetch_add(1);
    at.entered_while_active = ctx->active.fetch_add(1) > 0;  // released by the wrapper's Leave guard
  }
  at.thread = std::this_thread::get_id();
  at.thread_id = static_cast<unsigned long long>(std::hash<std::thread::id>{}(at.thread));
  if (rdma == nullptr || ctx->proto == nullptr) return -ENOTSUP;
  at.t_begin = now_ns();
  at.cb_size = static_cast<long long>(size);
  at.cb_offset = static_cast<long long>(offset);

  // Any later invocation — intentional guard or an unknown-completion latch —
  // emits nothing and returns a non-retryable value, whatever else happens.
  if (at.invocation >= 1 && (ctx->guard || ctx->unknown.load())) {
    at.guarded = true;
    at.returned = -ENOTSUP;
    at.t_end = now_ns();
    ctx->push(at);
    return -ENOTSUP;
  }

  const s3rdma::ProtocolOptions& proto = *ctx->proto;
  std::string target;
  s3rdma::HeaderList headers;  // sent in this order, byte for byte
  try {
    const std::string token =
        s3rdma::encode_rdma_token(rdma->desc_str, rdma->desc_len, proto.token_encoding);
    target = s3rdma::control_plane_target(proto, ctx->path);
    headers = s3rdma::control_plane_headers(proto, "GET", s3rdma::http_host_header(ctx->host, ctx->port),
                                            ctx->path, ctx->range, token);
  } catch (const std::exception& e) {
    at.request_error = e.what();
    at.probe_failure = true;
    ctx->local_failure.store(true);
    at.returned = -ENOTSUP;  // nothing was sent; do not let the library retry
    at.t_end = now_ns();
    ctx->push(at);
    return -ENOTSUP;
  }

  // Submission state is established before the network call: from here on an
  // exception means "possibly sent, completion unknown" until a qualified
  // receipt is recorded. A connect failure that wrote nothing is taken back.
  ctx->submitted.fetch_add(1);
  ctx->unknown.store(true);
  s3rdma::RawResponse res;
  try {
    res = s3rdma::raw_http_get(ctx->host, ctx->port, s3rdma::http_host_header(ctx->host, ctx->port), target,
                               headers, kMaxHead, kMaxBody, 30);
  } catch (const std::invalid_argument& e) {  // request validation: nothing was written
    at.request_error = e.what();
    at.probe_failure = true;
    ctx->local_failure.store(true);
    ctx->submitted.fetch_sub(1);
    if (ctx->submitted.load() == ctx->completed.load()) ctx->unknown.store(false);
    at.returned = -ENOTSUP;
    at.t_end = now_ns();
    ctx->push(at);
    return -ENOTSUP;
  }
  at.request_partial = res.request_partial;
  at.request_bytes = static_cast<long long>(res.request_bytes);
  if (!res.request_sent && !res.request_partial) {
    ctx->submitted.fetch_sub(1);  // no request bytes were accepted by the socket
    if (ctx->submitted.load() == ctx->completed.load()) ctx->unknown.store(false);
    if (res.transport_error) { at.probe_failure = true; ctx->local_failure.store(true); }
  } else if (res.request_sent && res.token_bearing) {
    at.http_attempt = true;
    ctx->http_attempts.fetch_add(1);  // one complete, validated, token-bearing message
  } else if (res.request_sent) {
    ctx->tokenless_messages.fetch_add(1);  // complete but not token-bearing
  } else {
    ctx->partial_messages.fetch_add(1);  // incomplete write: possibly emitted, never a counted message
  }
  at.body_bytes = static_cast<long long>(res.body_content);   // decoded content
  at.body_octets = static_cast<long long>(res.body_octets);   // retained wire octets after the head
  at.body_truncated = res.body_truncated;
  at.head_complete = res.head_complete;
  at.head_malformed = res.head_malformed;
  at.message_complete = res.message_complete;
  at.framing = res.framing;
  at.response_head = res.head_bytes;
  if (res.transport_error) at.transport_error = res.error;
  const s3rdma::ReceiptView view = s3rdma::view_receipt(res, size);
  ssize_t rc = -ENOTSUP;
  if (view.present) {
    at.response = true;
    ctx->responses.fetch_add(1);
    at.http_status = view.status;
    at.reply_count = static_cast<long long>(view.reply_count);
    at.reply_present = view.reply_count > 0;
    at.reply_raw = view.reply_raw;
    const s3rdma::RdmaReply reply = s3rdma::parse_rdma_reply(at.reply_present, at.reply_raw);
    at.reply_kind = reply.kind == s3rdma::RdmaReply::Kind::Code ? "code"
                    : reply.kind == s3rdma::RdmaReply::Kind::Declined ? "declined" : "malformed";
    at.reply_code = reply.code;
    at.reply_unambiguous = view.reply_plain;
    const s3rdma::RawField* cr = res.first("content-range");
    at.content_range_present = cr != nullptr;
    at.content_range = cr ? cr->value : "";
    at.count_field_count = static_cast<long long>(view.count_count);
    at.count_present = view.count_count > 0;
    at.count_plain = view.count_plain;
    at.declared = view.declared;
    // Legacy Base64 preserves the historical requested-size return.
    // Verbatim responses require matching status and use the declared count.
    if (res.status == 200 || res.status == 206) {
      if (proto.token_encoding == s3rdma::TokenEncoding::Base64) rc = static_cast<ssize_t>(size);
      else if (reply.kind == s3rdma::RdmaReply::Kind::Code && reply.code == res.status && at.declared >= 0)
        rc = static_cast<ssize_t>(at.declared);
    }
  }
  at.natural_return = rc;
  at.completion_candidate = completion_candidate(at, proto, ctx->expected, size);
  at.qualified_complete = s3rdma::completion_authority(view, proto, ctx->expected, size);
  if (at.qualified_complete && ctx->submitted.load() == 1) {
    ctx->completed.fetch_add(1);
    ctx->unknown.store(false);  // the single submission has a qualified receipt
  }
  if (ctx->guard && at.invocation == 0 && at.qualified_complete) {
    at.injection_applied = true;
    rc = static_cast<ssize_t>(ctx->first_return);
  }
  at.returned = rc;
  at.t_end = now_ns();
  ctx->push(at);
  if (ctx->evidence_lost.load()) return -ENOTSUP;  // never hand the library a retryable error without a record
  return rc;
}

// Nothing may leave the callback through the SDK's frames.
ssize_t rig_callback(const void* handle, char* ptr, size_t size, loff_t offset,
                     const cufileRDMAInfo_t* rdma) noexcept {
  struct Leave {  // the context's active-depth is released only when the probe has finished with this callback
    const void* h;
    ~Leave() noexcept {
      if (auto* c = static_cast<RigContext*>(cuObjClient::getCtx(h))) c->active.fetch_sub(1);
    }
  } leave{handle};
  try {
    return rig_callback_impl(handle, ptr, size, offset, rdma);
  } catch (...) {
    // A request may already have been sent: keep completion unknown and mark
    // the record incomplete without any allocating or throwing operation.
    if (auto* ctx = static_cast<RigContext*>(cuObjClient::getCtx(handle))) {
      if (ctx->submitted.load() > ctx->completed.load()) ctx->unknown.store(true);
      ctx->evidence_lost.store(true);
    }
    std::fputs("rig_callback: exception\n", stderr);
    return -ENOTSUP;
  }
}

void emit_attempts(const RigContext& ctx, const std::string& label) {
  for (const auto& a : ctx.attempts) {
    Rec("attempt").str("run_id", ctx.run_id).num("get_id", ctx.get_id).str("case", label).str("who", ctx.who)
        .num("iteration", ctx.iteration).num("slot", ctx.slot).num("invocation", a.invocation)
        .num("t_begin_ns", a.t_begin).num("t_end_ns", a.t_end)
        .boolean("guarded", a.guarded).str("count_basis", a.guarded ? "none" : "request_message")
        .boolean("http_attempt", a.http_attempt).boolean("response", a.response)
        .num("http_status", a.http_status).str("transport_error", a.transport_error)
        .str("request_error", a.request_error).num("body_bytes", a.body_bytes)
        .boolean("reply_present", a.reply_present).boolean("raw_reply_available", a.response)
        .boolean("head_complete", a.head_complete).boolean("head_malformed", a.head_malformed)
        .boolean("message_complete", a.message_complete).str("framing", a.framing).boolean("probe_failure", a.probe_failure)
        .num("request_bytes", a.request_bytes).boolean("request_partial", a.request_partial)
        .hex("response_head_hex", a.response_head)
        .str("reply_raw", a.reply_raw).hex("reply_raw_hex", a.reply_raw).num("reply_count", a.reply_count)
        .num("count_field_count", a.count_field_count).boolean("count_plain", a.count_plain)
        .boolean("body_truncated", a.body_truncated)
        .str("reply_kind", a.reply_kind).num("reply_code", a.reply_code).boolean("reply_unambiguous", a.reply_unambiguous)
        .boolean("content_range_present", a.content_range_present).str("content_range", a.content_range)
        .boolean("count_present", a.count_present).num("bytes_transferred_declared", a.declared)
        .num("cb_size", a.cb_size).num("cb_offset", a.cb_offset).num("body_content", a.body_bytes).num("body_octets", a.body_octets)
        .boolean("entered_while_active", a.entered_while_active).num("thread_id", static_cast<long long>(a.thread_id))
        .num("natural_return", a.natural_return).boolean("injection_applied", a.injection_applied)
        .num("callback_return", a.returned).boolean("qualified_complete", a.qualified_complete)
        .boolean("completion_candidate", a.completion_candidate)
        .boolean("evidence_complete", !ctx.evidence_lost.load())
        .emit();
  }
}

// ---- SDK wrapper ------------------------------------------------------------

struct SdkReturn {
  ssize_t value = -1;
  bool present = false;  // false when the SDK threw instead of returning
};

class Session {
 public:
  Session() {
    ops_.get = &rig_callback;
    ops_.put = nullptr;
    client_ = new cuObjClient(ops_, CUOBJ_PROTO_RDMA_DC_V1);
  }
  ~Session() { delete client_; }
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;
  // Suppresses the client destructor, not process-level finalizers.
  void leak() { client_ = nullptr; }

  int register_memory(void* ptr, size_t size) { return static_cast<int>(client_->cuMemObjGetDescriptor(ptr, size)); }
  int deregister_memory(void* ptr) {
    try { return static_cast<int>(client_->cuMemObjPutDescriptor(ptr)); } catch (...) { return -1; }
  }
  SdkReturn get(RigContext* ctx, void* ptr, size_t size) {
    SdkReturn r;
    try {
      r.value = client_->cuObjGet(ctx, ptr, size);
      r.present = true;
    } catch (...) {
      std::fputs("[rig] cuObjGet threw\n", stderr);
    }
    return r;
  }
  ssize_t callback_max(void* ptr) {
    try { return client_->cuMemObjGetMaxRequestCallbackSize(ptr); } catch (...) { return -1; }
  }

 private:
  CUObjOps_t ops_{};
  cuObjClient* client_ = nullptr;
};

// ---- Memory helpers ---------------------------------------------------------

std::vector<char> poison_pattern(size_t len, unsigned nonce) {
  std::vector<char> p(len);
  unsigned x = 0x9E3779B9u ^ nonce;
  for (size_t i = 0; i < len; ++i) {
    x = x * 1664525u + 1013904223u;
    p[i] = static_cast<char>((x >> 24) ^ 0xA5);
  }
  return p;
}

// Writes complete before the function returns: host memcpy, or an async H2D
// on a private stream followed by a CPU wait on its event (no device-wide sync).
void write_span(Buffer& b, size_t off, const std::vector<char>& bytes) {
  char* dst = static_cast<char*>(b.data()) + off;
  if (b.mode() == BufferMode::Host) { std::memcpy(dst, bytes.data(), bytes.size()); return; }
  cudaStream_t s = nullptr;
  cudaEvent_t e = nullptr;
  if (cudaStreamCreateWithFlags(&s, cudaStreamNonBlocking) != cudaSuccess) throw std::runtime_error("cudaStreamCreate failed");
  if (cudaEventCreateWithFlags(&e, cudaEventDisableTiming) != cudaSuccess) { cudaStreamDestroy(s); throw std::runtime_error("cudaEventCreate failed"); }
  const bool ok = cudaMemcpyAsync(dst, bytes.data(), bytes.size(), cudaMemcpyHostToDevice, s) == cudaSuccess &&
                  cudaEventRecord(e, s) == cudaSuccess && cudaEventSynchronize(e) == cudaSuccess;
  cudaEventDestroy(e);
  cudaStreamDestroy(s);
  if (!ok) throw std::runtime_error("poison H2D did not complete");
}

std::vector<char> read_span(const Buffer& b, size_t off, size_t len) {
  std::vector<char> out(len);
  const char* src = static_cast<const char*>(b.data()) + off;
  if (b.mode() == BufferMode::Host) std::memcpy(out.data(), src, len);
  else if (cudaMemcpy(out.data(), src, len, cudaMemcpyDeviceToHost) != cudaSuccess)
    throw std::runtime_error("cudaMemcpy D2H failed");
  return out;
}

struct Cell {  // [guard | payload | guard]
  size_t base = 0, payload = 0, guard = 0, size = 0;
};
Cell make_cell(size_t index, size_t guard, size_t payload) {
  Cell c;
  c.guard = guard;
  c.size = guard + payload + guard;
  c.base = index * c.size;
  c.payload = c.base + guard;
  return c;
}
void poison_cell(Buffer& b, const Cell& c, unsigned nonce) { write_span(b, c.base, poison_pattern(c.size, nonce)); }

// Everything outside the expected landing span must still be poison: both
// guards and the unused tail of the payload.
bool poison_intact_outside(const Buffer& b, const Cell& c, size_t landed, unsigned nonce) {
  const std::vector<char> expect = poison_pattern(c.size, nonce);
  const std::vector<char> whole = read_span(b, c.base, c.size);
  for (size_t i = 0; i < c.size; ++i) {
    const bool in_landing = i >= c.guard && i < c.guard + landed;
    if (!in_landing && whole[i] != expect[i]) return false;
  }
  return true;
}

std::string digest_of(const std::vector<char>& v) { return s3rdma::sha256_hex(v.data(), v.size()); }

std::string payload_poison_digest(const Cell& c, size_t n, unsigned nonce) {
  const auto p = poison_pattern(c.size, nonce);
  return s3rdma::sha256_hex(p.data() + c.guard, n);
}

std::string run_identity() { return std::to_string(static_cast<long long>(getpid())) + "-" + std::to_string(now_ns()); }

// Substring observation of the configured JSON (value only; the file may
// name lab addresses). Not a parsed or established effective setting.
std::string effective_retry_count() {
  const char* path = std::getenv("CUFILE_ENV_PATH_JSON");
  if (!path || !*path) return "env_unset";
  std::ifstream f(path);
  if (!f) return "unreadable";
  std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  const size_t k = text.find("rdma_io_retry_count");
  if (k == std::string::npos) return "absent";
  const size_t colon = text.find(':', k);
  if (colon == std::string::npos) return "malformed";
  size_t p = colon + 1;
  while (p < text.size() && (text[p] == ' ' || text[p] == '\t')) ++p;
  std::string digits;
  while (p < text.size() && text[p] >= '0' && text[p] <= '9') digits.push_back(text[p++]);
  return digits.empty() ? "malformed" : digits;
}

// ---- Experiments ------------------------------------------------------------

int exp_attr() {
  int ordering = -1, flush = -1;
  const bool ok_o = cudaDeviceGetAttribute(&ordering, cudaDevAttrGPUDirectRDMAWritesOrdering, 0) == cudaSuccess;
  const bool ok_f = cudaDeviceGetAttribute(&flush, cudaDevAttrGPUDirectRDMAFlushWritesOptions, 0) == cudaSuccess;
  (void)cudaGetLastError();
  const char* name = !ok_o ? "query_failed"
                     : ordering == cudaGPUDirectRDMAWritesOrderingAllDevices ? "all_devices"
                     : ordering == cudaGPUDirectRDMAWritesOrderingOwner ? "owner"
                     : ordering == cudaGPUDirectRDMAWritesOrderingNone ? "none" : "unknown";
  Rec("attr").boolean("ordering_queried", ok_o).num("ordering_raw", ordering).str("ordering", name)
      .boolean("flush_options_queried", ok_f).num("flush_options_raw", flush).emit();
  return finish(ok_o && ok_f ? Verdict::Pass : Verdict::EnvError,
                ok_o && ok_f ? "attributes recorded" : "attribute query incomplete");
}

int exp_reg(const Options& o) {
  size_t bytes = 0;
  if (o.slot_bytes == 0 || o.slot_bytes > kSsizeMax || o.slots == 0 || o.slots > 256 ||
      !checked_mul(o.slots, o.slot_bytes, bytes) || bytes > kMaxReg)
    return finish(Verdict::EnvError, "W*S invalid or exceeds CUOBJ_MAX_MEMORY_REG_SIZE");
  void* block = nullptr;
  if (cudaMalloc(&block, bytes) != cudaSuccess) return finish(Verdict::EnvError, "cudaMalloc failed");
  Session s;
  const int rc = s.register_memory(block, bytes);
  Rec("register").str("allocation", "cudaMalloc").num("bytes", static_cast<long long>(bytes)).num("rc", rc).emit();
  if (rc != 0) { cudaFree(block); return finish(Verdict::Fail, "registration of a dedicated cudaMalloc block refused"); }
  char* base = static_cast<char*>(block);
  const ssize_t at_base = s.callback_max(base);
  const ssize_t at_last = s.callback_max(base + (o.slots - 1) * o.slot_bytes);
  Rec("callback_max").num("at_base", at_base).num("at_last_slot", at_last)
      .num("slot_bytes", static_cast<long long>(o.slot_bytes)).num("max_reg_size", static_cast<long long>(kMaxReg)).emit();
  const int drc = s.deregister_memory(block);
  if (drc != 0) s.leak();  // retention decided before any fallible diagnostic
  Rec("deregister").num("rc", drc).emit();
  if (drc == 0) cudaFree(block);  // a failed release keeps the block (never free an unreleased registration)
  const ssize_t need = static_cast<ssize_t>(o.slot_bytes);
  const bool fits = at_base >= need && at_last >= need;
  if (drc != 0) return finish(Verdict::Fail, "deregistration failed; block and client retained");
  return finish(fits ? Verdict::Pass : Verdict::Fail,
                fits ? "capacity supports S without splitting; retry behaviour is measured separately"
                     : "callback size below S: construction refused for this S");
}

// The registered fixture has exactly one owner. Its terminal state is decided
// explicitly (release or retain); every other exit — early return, exception,
// unwind — retains: the native clients and the arena are never destroyed while
// a registration may still be written (§14.1).
class Fixture {
 public:
  Fixture(BufferMode mode, size_t bytes, size_t sessions) : arena_(new Buffer(mode, bytes)) {
    for (size_t k = 0; k < sessions; ++k) sessions_.emplace_back(new Session);
  }
  ~Fixture() {
    if (!released_) retain_now();
  }
  Fixture(const Fixture&) = delete;
  Fixture& operator=(const Fixture&) = delete;

  Buffer& arena() { return *arena_; }
  Session& session(size_t k) { return *sessions_[k]; }
  size_t sessions() const { return sessions_.size(); }

  int register_memory() {
    const int rc = sessions_[0]->register_memory(arena_->data(), arena_->size());
    registered_ = rc == 0;
    return rc;
  }

  // Checked release through the registrar. Deregistration failure retains the
  // graph; diagnostic failure only marks the receipt incomplete.
  bool release() {
    if (released_ || retained_) return false;
    int drc = 0;
    if (registered_) drc = sessions_[0]->deregister_memory(arena_->data());
    if (drc != 0) retain_now();  // decided before the report; a throwing report cannot undo it
    try { Rec("deregister").num("rc", drc).boolean("attempted", registered_).emit(); } catch (...) { s3rdma::jsonl_output_failed() = true; }
    if (drc != 0) return false;
    registered_ = false;
    released_ = true;
    sessions_.clear();
    arena_.reset();
    return true;
  }

  void mark_read() { read_ = true; }  // a read was attempted at some point in this fixture's life
  int retain(const std::string& label, Verdict v, const std::string& why) {
    retain_now();
    Rec("landing").str("case", label).str("state", read_ ? "read_attempted_before_retain" : "not_read")
        .str("retained", "arena,sessions,registration").emit();
    return finish(v, why);
  }

 private:
  void retain_now() {
    if (retained_) return;
    retained_ = true;
    for (auto& s : sessions_) if (s) { s->leak(); s.release(); }
    arena_.release();
  }
  std::unique_ptr<Buffer> arena_;
  std::vector<std::unique_ptr<Session>> sessions_;
  bool registered_ = false, released_ = false, retained_ = false, read_ = false;
};

// diag and rt-guard share one single-slot GET.
int exp_single_get(const Options& o, bool guard, const std::string& run_id) {
  Geometry g;
  std::string why;
  if (!validate_geometry(o, 1, g, why)) return finish(Verdict::EnvError, why);
  const BufferMode mode = s3rdma::parse_mode(o.mode);
  const Visibility vis = read_visibility(mode);
  Rec("visibility").str("ordering", vis.name).num("raw", vis.raw).boolean("trustworthy", vis.trustworthy).emit();
  if (!vis.trustworthy) return finish(Verdict::EnvError, "GPU visibility not qualified: " + vis.name);
  const size_t start = o.range_given ? o.range_start : 0;
  const size_t end = o.range_given ? o.range_end : o.slot_bytes - 1;
  const size_t requested = end - start + 1;
  const Cell cell = make_cell(0, o.guard, o.slot_bytes);
  const unsigned nonce = static_cast<unsigned>(now_ns() & 0xffffffffu);
  Fixture fx(mode, cell.size, 1);
  Buffer* arena = &fx.arena();
  Session* s = &fx.session(0);
  char* slot = static_cast<char*>(arena->data()) + cell.payload;
  const int reg = fx.register_memory();
  Rec("register").num("bytes", static_cast<long long>(cell.size)).num("rc", reg).emit();
  if (reg != 0) { fx.release(); return finish(Verdict::EnvError, "registration failed"); }
  const ssize_t cap = s->callback_max(slot);
  Rec("callback_max").num("at_payload", cap).num("requested", static_cast<long long>(requested)).emit();
  if (cap < static_cast<ssize_t>(requested)) {
    return fx.release() ? finish(Verdict::EnvError, "callback capacity below the request: one-callback profile not available")
                        : fx.retain(o.label, Verdict::EnvError, "callback capacity below the request; release failed");
  }
  try {
    poison_cell(*arena, cell, nonce);
  } catch (const std::exception& e) {
    return fx.release() ? finish(Verdict::EnvError, std::string("poison production failed: ") + e.what())
                        : fx.retain(o.label, Verdict::EnvError, "poison production failed; release failed");
  }
  if (g.expected > 0 && !o.expect_sha.empty() && payload_poison_digest(cell, g.expected, nonce) == o.expect_sha)
    return finish(Verdict::EnvError, "poison collides with --expect-sha");

  RigContext ctx;
  ctx.proto = &o.proto;
  ctx.run_id = run_id;
  ctx.get_id = 1;
  ctx.who = guard ? "rt-guard" : "diag";
  ctx.host = o.host;
  ctx.port = o.port;
  ctx.path = "/" + o.bucket + "/" + o.object;
  ctx.range = "bytes=" + std::to_string(start) + "-" + std::to_string(end);
  ctx.expected = g.expected;
  ctx.guard = guard;
  ctx.first_return = o.guard_mode == "enotsup" ? -ENOTSUP : -EPERM;

  Rec("get_begin").str("run_id", run_id).num("get_id", ctx.get_id).str("case", o.label).str("declared_class", o.neg_class)
      .str("range", ctx.range).num("requested", static_cast<long long>(requested))
      .num("expected", static_cast<long long>(g.expected)).str("expected_source", o.expected_given ? "explicit" : "request_length")
      .str("rdma_io_retry_count_observed", effective_retry_count()).boolean("retry_configuration_qualified", false)
      .boolean("guard", guard).num("first_return", ctx.first_return).emit();
  const SdkReturn r = s->get(&ctx, slot, requested);
  emit_attempts(ctx, o.label);
  const std::string observed_class = ctx.unknown.load() ? "unknown" : (ctx.http_attempts.load() == 0 ? "no-emission" : "known-complete");
  Rec("get_end").str("run_id", run_id).num("get_id", ctx.get_id).str("case", o.label).boolean("sdk_return_present", r.present)
      .num("cuobjget_return", r.value).num("invocations", ctx.invocations.load())
      .num("http_attempts", ctx.http_attempts.load()).num("responses", ctx.responses.load())
      .str("observed_class", observed_class).boolean("evidence_complete", !ctx.evidence_lost.load()).emit();

  // Gate: submission, completion, native return and evidence are evaluated
  // together before any landing read or release. No-emission cases may clean up.
  const bool submitted = ctx.submitted.load() > 0;
  const bool hold = submitted && (ctx.unknown.load() || ctx.evidence_lost.load() || !r.present ||
                                  ctx.completed.load() != ctx.submitted.load());
  if (o.neg_class == "unknown" || hold)
    return fx.retain(o.label, Verdict::Inconclusive,
                     "completion unknown (" + observed_class + "): landing not read, resources retained");

  bool outside_ok = false;
  std::string digest;
  try {
    fx.mark_read();
    digest = digest_of(read_span(*arena, cell.payload, g.expected));
    outside_ok = poison_intact_outside(*arena, cell, g.expected, nonce);
  } catch (const std::exception& e) {
    return fx.retain(o.label, Verdict::EnvError, std::string("landing read failed: ") + e.what());
  }
  const bool digest_checked = !o.expect_sha.empty();
  const bool digest_ok = digest_checked && digest == o.expect_sha;
  // Compare the observed landing with the fresh poison over the same span.
  const std::string poison_span = g.expected ? payload_poison_digest(cell, g.expected, nonce) : "";
  Rec("landing").str("run_id", run_id).num("get_id", ctx.get_id).str("case", o.label).str("sha256", digest).boolean("digest_checked", digest_checked)
      .boolean("digest_ok", digest_ok).boolean("poison_intact_outside", outside_ok)
      .str("poison_span_sha256", poison_span).boolean("landing_is_poison", g.expected > 0 && digest == poison_span).emit();
  if (!fx.release()) return fx.retain(o.label, Verdict::Fail, "deregistration failed; arena retained");

  if (ctx.evidence_lost.load())
    return finish(Verdict::Inconclusive, "evidence incomplete (a callback record was lost)");
  if (guard) {
    const bool injected = !ctx.attempts.empty() && ctx.attempts.front().injection_applied;
    const bool landing_poison = g.expected > 0 && digest == poison_span;
    if (!injected) return finish(Verdict::Inconclusive, "first transfer not qualified complete; guard not exercised");
    if (ctx.invocations.load() < 2)
      return finish(Verdict::Inconclusive, landing_poison ? "no re-entry; landing_unchanged_poison_after_injected_return"
                                                          : "guard not exercised: the library did not re-invoke the callback");
    std::vector<s3rdma::GuardAttempt> ga;
    for (auto& a : ctx.attempts)
      ga.push_back({a.invocation, a.t_begin, a.t_end, a.cb_size, a.cb_offset, a.injection_applied, a.guarded, a.http_attempt, a.request_partial, a.entered_while_active, a.thread});
    const s3rdma::GuardWitness w = s3rdma::guard_witness(ga);
    Rec("guard_witness").boolean("ok", w.ok).str("why", w.why).num("messages", ctx.http_attempts.load())
        .num("partial_messages", ctx.partial_messages.load()).num("tokenless_messages", ctx.tokenless_messages.load())
        .str("ordering_basis", "same-thread non-nested callback execution + strict return-marker order").emit();
    if (ctx.http_attempts.load() > 1) return finish(Verdict::Fail, "more than one request message for one GET");
    if (r.present && r.value >= 0) return finish(Verdict::Fail, "cuObjGet returned success after injected failure");
    if (!outside_ok) return finish(Verdict::Fail, "guard corruption after the completed transfer");
    if (!w.ok) return finish(Verdict::Inconclusive, "re-entry witness not established: " + w.why);
    if (landing_poison) return finish(Verdict::Inconclusive, "re-entry witnessed; landing_unchanged_poison_after_injected_return");
    if (!digest_checked) return finish(Verdict::Inconclusive, "re-entry witnessed; no integrity witness supplied");
    if (!digest_ok) return finish(Verdict::Inconclusive, "re-entry witnessed; landing did not match the independent object digest");
    return finish(Verdict::Pass, "ordered re-entry of the same request; one complete token-bearing message; negative SDK return; landing verified (capture is the independent cross-check)");
  }
  const bool one_cb = ctx.invocations.load() == 1;
  const bool rc_ok = r.present && r.value == static_cast<ssize_t>(g.expected);
  if (!digest_checked) return finish(Verdict::Inconclusive, "observed; no integrity witness supplied (--expect-sha)");
  const bool ok = one_cb && rc_ok && digest_ok && outside_ok;
  return finish(ok ? Verdict::Pass : Verdict::Fail,
                ok ? "delivered" : std::string(one_cb ? "" : "callbacks!=1 ") + (rc_ok ? "" : "rc!=expected ") +
                                       (digest_ok ? "" : "digest ") + (outside_ok ? "" : "poison-outside "));
}

int exp_concurrent(const Options& o, const std::string& run_id) {
  Geometry g;
  std::string why;
  if (!validate_geometry(o, o.slots, g, why)) return finish(Verdict::EnvError, why);
  if (!o.objects.empty() && o.objects.size() != o.slots) return finish(Verdict::EnvError, "--objects count must equal --slots");
  const BufferMode mode = s3rdma::parse_mode(o.mode);
  const Visibility vis = read_visibility(mode);
  Rec("visibility").str("ordering", vis.name).num("raw", vis.raw).boolean("trustworthy", vis.trustworthy).emit();
  if (!vis.trustworthy) return finish(Verdict::EnvError, "GPU visibility not qualified: " + vis.name);
  const size_t W = o.slots, S = o.slot_bytes;
  Fixture fx(mode, g.arena, W);
  Buffer* arena = &fx.arena();
  auto session = [&](size_t k) -> Session& { return fx.session(k); };
  const int reg = fx.register_memory();
  Rec("register").num("bytes", static_cast<long long>(arena->size())).num("rc", reg)
      .str("topology", "one registration (session 0); one session per worker; cross-instance GETs for workers >= 1").emit();
  if (reg != 0) { fx.release(); return finish(Verdict::EnvError, "registration failed"); }
  auto retain_all = [&](Verdict v, const std::string& d) { return fx.retain("c", v, d); };
  auto release_or_retain = [&](Verdict v, const std::string& d) {
    return fx.release() ? finish(v, d) : fx.retain("c", v, d + "; release failed; graph retained");
  };
  for (size_t k = 0; k < W; ++k) {
    const Cell c = make_cell(k, o.guard, S);
    const ssize_t cap = session(k).callback_max(static_cast<char*>(arena->data()) + c.payload);
    Rec("callback_max").num("slot", static_cast<long long>(k)).num("at_payload", cap).emit();
    if (cap < static_cast<ssize_t>(S)) {
      return release_or_retain(Verdict::EnvError, "callback capacity below S at a slot pointer");
    }
  }

  auto path_for = [&](size_t k) { return "/" + o.bucket + "/" + (o.objects.empty() ? o.object : o.objects[k]); };
  auto range_for = [&](size_t k) {
    const size_t a = o.objects.empty() ? k * S : 0;
    return "bytes=" + std::to_string(a) + "-" + std::to_string(a + S - 1);
  };
  long long next_get = 0;
  auto setup = [&](size_t k, long long iteration, RigContext& ctx) {
    ctx.proto = &o.proto;
    ctx.run_id = run_id;
    ctx.get_id = ++next_get;
    ctx.who = "w" + std::to_string(k);
    ctx.host = o.host;
    ctx.port = o.port;
    ctx.path = path_for(k);
    ctx.range = range_for(k);
    ctx.expected = S;
    ctx.iteration = iteration;
    ctx.slot = static_cast<long long>(k);
  };
  auto run_slot = [&](size_t k, RigContext& ctx) {
    const Cell c = make_cell(k, o.guard, S);
    return session(k).get(&ctx, static_cast<char*>(arena->data()) + c.payload, S);
  };
  auto slot_ok = [&](size_t k, const RigContext& ctx, const SdkReturn& r, unsigned nonce, std::string& d, bool& outside) {
    const Cell c = make_cell(k, o.guard, S);
    fx.mark_read();
    d = digest_of(read_span(*arena, c.payload, S));
    outside = poison_intact_outside(*arena, c, S, nonce);
    return r.present && r.value == static_cast<ssize_t>(S) && ctx.invocations.load() == 1 &&
           !ctx.attempts.empty() && ctx.attempts.front().qualified_complete && outside;
  };

  // Reference pass (serial): one verified digest per slot; payloads must differ
  // from each other and from their poison.
  std::vector<std::string> ref(W);
  try {
    for (size_t k = 0; k < W; ++k) {
      const Cell c = make_cell(k, o.guard, S);
      const unsigned nonce = 1u;
      poison_cell(*arena, c, nonce);
      const std::string pd = payload_poison_digest(c, S, nonce);
      RigContext ctx;
      setup(k, -1, ctx);
      const SdkReturn r = run_slot(k, ctx);
      emit_attempts(ctx, "c-ref");
      Rec("reference_return").str("run_id", run_id).num("get_id", ctx.get_id).num("slot", static_cast<long long>(k))
          .boolean("sdk_return_present", r.present).num("rc", r.value).num("callbacks", ctx.invocations.load())
          .boolean("unknown", ctx.unknown.load()).boolean("evidence_complete", !ctx.evidence_lost.load()).emit();
      if (ctx.unknown.load() || ctx.evidence_lost.load() || !r.present || ctx.completed.load() != ctx.submitted.load())
        return retain_all(Verdict::Inconclusive, "reference GET left completion unknown; graph retained");
      if (ctx.local_failure.load())  // joint hold is clear: nothing of this GET is outstanding
        return release_or_retain(Verdict::EnvError, "probe-origin failure during the reference pass (not a backend finding)");
      bool outside = false;
      std::string d;
      const bool ok = slot_ok(k, ctx, r, nonce, d, outside);
      Rec("reference").str("run_id", run_id).num("get_id", ctx.get_id).num("slot", static_cast<long long>(k)).str("sha256", d).boolean("ok", ok)
          .boolean("poison_intact_outside", outside).boolean("differs_from_poison", d != pd).emit();
      if (!ok || d == pd) {
        return release_or_retain(Verdict::EnvError, "reference GET for slot " + std::to_string(k) + " not verified");
      }
      ref[k] = d;
    }
  } catch (const std::exception& e) {
    return retain_all(Verdict::EnvError, std::string("reference pass failed: ") + e.what());
  }
  for (size_t a = 0; a < W; ++a)
    for (size_t b = a + 1; b < W; ++b)
      if (ref[a] == ref[b]) {
        return release_or_retain(Verdict::Inconclusive, "slot payloads are not distinguishable (equal reference digests)");
      }

  int max_overlap = 0, failing_round_overlap = 0;
  size_t failures = 0;
  for (int it = 0; it < o.iterations; ++it) {
    const unsigned nonce = 2u + static_cast<unsigned>(it);
    std::vector<RigContext> ctxs(W);
    std::vector<SdkReturn> rcs(W);
    try {
      for (size_t k = 0; k < W; ++k) {
        const Cell c = make_cell(k, o.guard, S);
        if (payload_poison_digest(c, S, nonce) == ref[k]) return retain_all(Verdict::Inconclusive, "round poison collides with a reference payload");
        poison_cell(*arena, c, nonce);
        setup(k, it, ctxs[k]);
      }
    } catch (const std::exception& e) {
      return retain_all(Verdict::EnvError, std::string("poison production failed: ") + e.what());
    }
    std::vector<std::thread> threads;
    try {
      for (size_t k = 0; k < W; ++k) threads.emplace_back([&, k] { rcs[k] = run_slot(k, ctxs[k]); });
    } catch (...) {
      for (auto& t : threads) if (t.joinable()) t.join();
      return retain_all(Verdict::EnvError, "worker thread creation failed; graph retained");
    }
    for (auto& t : threads) t.join();

    bool any_unknown = false;
    for (size_t k = 0; k < W; ++k)
      any_unknown |= ctxs[k].unknown.load() || ctxs[k].evidence_lost.load() || !rcs[k].present ||
                     ctxs[k].completed.load() != ctxs[k].submitted.load();
    // Overlap counts distinct entered GETs: one interval per context.
    std::vector<std::pair<long long, int>> ev;
    for (auto& c : ctxs) {
      if (c.attempts.empty()) continue;
      long long b = c.attempts.front().t_begin, e = c.attempts.front().t_end;
      for (auto& a : c.attempts) { b = std::min(b, a.t_begin); e = std::max(e, a.t_end); }
      ev.push_back({b, +1});
      ev.push_back({e, -1});
    }
    std::sort(ev.begin(), ev.end(), [](auto& x, auto& y) { return x.first != y.first ? x.first < y.first : x.second < y.second; });
    int cur = 0, round_max = 0;
    for (auto& e : ev) { cur += e.second; round_max = std::max(round_max, cur); }
    max_overlap = std::max(max_overlap, round_max);
    for (size_t k = 0; k < W; ++k) {
      emit_attempts(ctxs[k], "c");
      Rec("slot_return").str("run_id", run_id).num("get_id", ctxs[k].get_id).num("iteration", it).num("slot", static_cast<long long>(k))
          .boolean("sdk_return_present", rcs[k].present).num("rc", rcs[k].value).num("callbacks", ctxs[k].invocations.load())
          .boolean("unknown", ctxs[k].unknown.load()).boolean("evidence_complete", !ctxs[k].evidence_lost.load()).emit();
    }
    Rec("round").num("iteration", it).num("max_overlap", round_max).boolean("unknown", any_unknown).emit();
    if (any_unknown) {
      // G8 reading: a short or negative native return counts against the
      // concurrent shape only when qualified overlap was witnessed in this round.
      // Only independently observed failures count: a context whose own record
      // was lost (and therefore returned -ENOTSUP from the probe) is an
      // evidence failure, not a backend finding.
      bool observed_failure = false, evidence_failure = false;
      for (size_t k = 0; k < W; ++k) {
        if (ctxs[k].evidence_lost.load() || ctxs[k].local_failure.load()) { evidence_failure = true; continue; }
        observed_failure |= rcs[k].present && rcs[k].value != static_cast<ssize_t>(S);
      }
      const bool concurrent_failure = observed_failure && round_max >= 2;  // this round's overlap only
      const Verdict selected = concurrent_failure ? Verdict::Fail : Verdict::Inconclusive;  // selected before reporting
      const char* why = concurrent_failure ? "a GET returned short/negative under witnessed overlap; graph retained"
                        : observed_failure ? "observed short/negative return without witnessed overlap (G8 INCONCLUSIVE); graph retained"
                        : evidence_failure ? "probe evidence failure (record lost); not a backend finding; graph retained"
                                           : "a GET left completion unknown; no reuse; graph retained";
      try { return retain_all(selected, why); } catch (...) { return terminal_fallback(selected); }
    }

    bool local_in_round = false, observer_failed = false;
    std::vector<bool> native_fail(W, false);
    // Pass 1 (no reads): independently eligible native-return failures are
    // classified before any landing observation can fail.
    for (size_t k = 0; k < W; ++k) {
      if (ctxs[k].local_failure.load()) continue;
      const bool native_ok = rcs[k].present && rcs[k].value == static_cast<ssize_t>(S) && ctxs[k].invocations.load() == 1 &&
                             !ctxs[k].attempts.empty() && ctxs[k].attempts.front().qualified_complete;
      native_fail[k] = !native_ok;
      if (!native_ok) {
        ++failures;  // decided before any allocating report
        report([&] {
          Rec("slot_native_failure").str("run_id", run_id).num("get_id", ctxs[k].get_id).num("iteration", it)
              .num("slot", static_cast<long long>(k)).boolean("sdk_return_present", rcs[k].present).num("rc", rcs[k].value).emit();
        });
      }
    }
    try {
      for (size_t k = 0; k < W; ++k) {
        if (ctxs[k].local_failure.load()) {  // probe-origin: excluded from backend/payload candidates
          local_in_round = true;
          Rec("slot").str("run_id", run_id).num("get_id", ctxs[k].get_id).num("iteration", it).num("slot", static_cast<long long>(k))
              .boolean("probe_failure", true).boolean("ok", false).emit();
          continue;
        }
        bool outside = false;
        std::string d;
        const bool ok = slot_ok(k, ctxs[k], rcs[k], nonce, d, outside) && d == ref[k];
        if (!ok && !native_fail[k]) ++failures;  // payload/guard failure; pass 1 already counted this slot otherwise
        Rec("slot").str("run_id", run_id).num("get_id", ctxs[k].get_id).num("iteration", it).num("slot", static_cast<long long>(k))
            .boolean("sdk_return_present", rcs[k].present).num("rc", rcs[k].value)
            .str("sha256", d).str("expected_sha256", ref[k]).boolean("digest_ok", d == ref[k]).boolean("poison_intact_outside", outside)
            .num("callbacks", ctxs[k].invocations.load()).boolean("ok", ok).emit();
      }
    } catch (const std::exception& e) {
      observer_failed = true;  // decided first; the report below cannot unwind
      report([&] { Rec("observer_failure").num("iteration", it).str("what", e.what()).emit(); });
    } catch (...) {
      observer_failed = true;
      s3rdma::jsonl_output_failed() = true;
    }
    if (failures) { failing_round_overlap = round_max; break; }  // do not reuse slots after a correctness failure
    if (observer_failed) return retain_all(Verdict::EnvError, "landing observation failed; no independent failure established; graph retained");
    if (local_in_round) {  // no independent failure: a pure probe failure ends the arm without a backend claim
      Rec("round_probe_failure").num("iteration", it).emit();
      return release_or_retain(Verdict::EnvError, "probe-origin failure in a concurrent round (not a backend finding)");
    }
  }
  // The terminal reading is selected here, before any allocating report, so a
  // reporting failure can never replace it (see terminal_fallback).
  Verdict final = Verdict::Pass;
  const char* final_detail = "overlap witnessed; all slots correct against the serial references";
  bool keep_graph = false;
  if (failures) {  // retained either way; G8 is qualified only by the failing round's overlap
    keep_graph = true;
    final = failing_round_overlap >= 2 ? Verdict::Fail : Verdict::Inconclusive;
    final_detail = failing_round_overlap >= 2 ? "per-slot correctness failure under witnessed overlap; graph retained"
                                              : "per-slot correctness failure without witnessed overlap (G8 INCONCLUSIVE); graph retained";
  } else if (max_overlap < 2) {
    final = Verdict::Inconclusive;
    final_detail = "no overlap witnessed (max_overlap < 2)";
  }
  report([&] {
    Rec("summary").num("slots", static_cast<long long>(W)).num("iterations", o.iterations)
        .num("max_overlap", max_overlap).num("failures", static_cast<long long>(failures))
        .num("failing_round_overlap", failing_round_overlap).emit();
  });
  if (!keep_graph && !fx.release()) {  // cleanup failure becomes the reading before any further report
    keep_graph = true;
    final = Verdict::Fail;
    final_detail = "deregistration failed; graph retained";
  }
  try {
    if (keep_graph) return retain_all(final, final_detail);
    return finish(final, final_detail);
  } catch (...) {
    return terminal_fallback(final);
  }
}

int exp_exit_child(const Options& o, const std::string& run_id) {
  Geometry g;
  std::string why;
  if (!validate_geometry(o, 1, g, why)) { std::fprintf(stderr, "child: %s\n", why.c_str()); return 3; }
  const BufferMode mode = s3rdma::parse_mode(o.mode);
  const Visibility vis = read_visibility(mode);
  if (!vis.trustworthy) { std::fputs("child: GPU visibility not qualified\n", stderr); return 3; }
  const Cell cell = make_cell(0, o.guard, o.slot_bytes);
  auto* fx = new Fixture(mode, cell.size, 1);  // intentionally never destroyed: retained through the exit boundary
  Buffer* arena = &fx->arena();
  Session* s = &fx->session(0);
  if (fx->register_memory() != 0) { std::fputs("child: registration failed\n", stderr); return 3; }
  RigContext ctx;
  ctx.proto = &o.proto;
  ctx.run_id = run_id;
  ctx.get_id = 1;
  ctx.who = "exit-child";
  ctx.host = o.host;
  ctx.port = o.port;
  ctx.path = "/" + o.bucket + "/" + o.object;
  ctx.range = "bytes=0-" + std::to_string(o.slot_bytes - 1);
  ctx.expected = o.slot_bytes;
  const SdkReturn r = s->get(&ctx, static_cast<char*>(arena->data()) + cell.payload, o.slot_bytes);
  emit_attempts(ctx, "exit-child");
  const bool complete = r.present && r.value == static_cast<ssize_t>(o.slot_bytes) && !ctx.unknown.load() &&
                        !ctx.evidence_lost.load() && ctx.invocations.load() == 1 &&
                        ctx.attempts.size() == 1 && ctx.attempts.front().qualified_complete;
  Rec("child_get").str("run_id", run_id).num("get_id", ctx.get_id).boolean("sdk_return_present", r.present).num("cuobjget_return", r.value)
      .num("invocations", ctx.invocations.load()).boolean("known_complete", complete).emit();
  if (!complete) return s3rdma::jsonl_output_failed() ? 5 : 1;  // retained either way; no normal-exit claim
  Rec("child_exit").str("path", o.abrupt ? "_exit" : "return_from_main").str("retained", "arena,session,registration").emit();
  std::fflush(stdout);
  const int status = s3rdma::jsonl_output_failed() ? 5 : 0;  // 5 = child evidence output failed
  if (o.abrupt) _exit(status);
  return status;  // the client and registration stay alive through normal return; process finalizers still run
}

int exp_exit(const Options& o, int argc, char** argv) {
  char exe[4096];
  const ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
  if (n <= 0) return finish(Verdict::EnvError, "cannot resolve /proc/self/exe");
  const std::string self(exe, static_cast<size_t>(n));
  std::vector<char*> args(argv, argv + argc);
  std::string child_flag = "--child";
  args.push_back(child_flag.data());
  args.push_back(nullptr);
  const long long t0 = now_ns();
  const pid_t pid = fork();
  if (pid < 0) return finish(Verdict::EnvError, "fork failed");
  if (pid == 0) { execv(self.c_str(), args.data()); _exit(127); }

  int status = 0;
  bool reaped = false, wait_error = false;
  const long long deadline = t0 + static_cast<long long>(o.deadline_s) * 1000000000LL;
  while (now_ns() < deadline) {
    const pid_t r = waitpid(pid, &status, WNOHANG);
    if (r == pid) { reaped = true; break; }
    if (r < 0 && errno != EINTR) { wait_error = true; break; }
    usleep(20000);
  }
  if (wait_error) return finish(Verdict::EnvError, std::string("waitpid failed: ") + std::strerror(errno));
  if (!reaped) {
    const int krc = kill(pid, SIGKILL);
    const int kill_errno = krc == 0 ? 0 : errno;
    const long long until = now_ns() + 5000000000LL;
    int reap_errno = 0;
    while (!reaped && now_ns() < until) {
      const pid_t r = waitpid(pid, &status, WNOHANG);
      if (r == pid) reaped = true;
      else if (r < 0 && errno != EINTR) { reap_errno = errno; break; }
      else usleep(20000);
    }
    Rec("parent").boolean("deadline_hit", true).num("kill_rc", krc).num("kill_errno", kill_errno).boolean("reaped", reaped)
        .num("reap_errno", reap_errno).num("exit_status", reaped && WIFEXITED(status) ? WEXITSTATUS(status) : -1)
        .num("signal", reaped && WIFSIGNALED(status) ? WTERMSIG(status) : 0)
        .boolean("exited", reaped && WIFEXITED(status)).boolean("signaled", reaped && WIFSIGNALED(status))
        .num("elapsed_ms", (now_ns() - t0) / 1000000).num("pid", pid).emit();
    return finish(Verdict::Fail, reaped ? "child did not exit within the deadline (killed; abnormal evidence)"
                                        : "child did not exit and could not be reaped; unresolved (pid recorded)");
  }
  const bool exited = WIFEXITED(status), signaled = WIFSIGNALED(status);
  Rec("parent").boolean("deadline_hit", false).num("elapsed_ms", (now_ns() - t0) / 1000000)
      .boolean("exited", exited).num("exit_status", exited ? WEXITSTATUS(status) : -1)
      .boolean("signaled", signaled).num("signal", signaled ? WTERMSIG(status) : 0)
      .str("exit_path", o.abrupt ? "_exit" : "return_from_main").emit();
  if (exited && WEXITSTATUS(status) == 127) return finish(Verdict::EnvError, "child exec failed");
  if (exited && WEXITSTATUS(status) == 3) return finish(Verdict::EnvError, "child could not set up (see stderr)");
  if (exited && WEXITSTATUS(status) == 1) return finish(Verdict::Inconclusive, "child GET not known-complete; no exit claim");
  if (exited && WEXITSTATUS(status) == 5) return finish(Verdict::Inconclusive, "child evidence output failed; exit path not qualified");
  if (exited && WEXITSTATUS(status) == 0)
    return finish(o.abrupt ? Verdict::Inconclusive : Verdict::Pass,
                  o.abrupt ? "abrupt-exit observation (not normal-exit qualification)" : "normal return with retained graph, exit 0; run the post-run health check separately");
  return finish(Verdict::Fail, "child crashed or failed on the exit path");
}

}  // namespace

int main(int argc, char** argv) {
  Options o;
  if (!parse_args(argc, argv, o)) { usage(argv[0]); return 2; }
  std::string proto_error;
  if (!s3rdma::finish_protocol_options(o.proto, proto_error)) return finish(Verdict::EnvError, proto_error);
  const std::string run_id = run_identity();
  Rec("env").str("run_id", run_id).str("exp", o.exp).str("mode", o.mode).str("server", o.host + ":" + std::to_string(o.port))
      .str("token_encoding", o.proto.token_encoding == s3rdma::TokenEncoding::Verbatim ? "verbatim" : "base64")
      .str("auth", o.proto.auth == s3rdma::AuthMode::SigV4 ? "sigv4" : "none")
      .num("slot_bytes", static_cast<long long>(o.slot_bytes)).num("slots", static_cast<long long>(o.slots))
      .num("guard", static_cast<long long>(o.guard)).emit();
  if (o.exp == "exit" && !o.child) return exp_exit(o, argc, argv);  // no CUDA context in the parent
  if (o.exp == "attr") return exp_attr();
  if (o.exp != "reg" && o.exp != "diag" && o.exp != "rt-guard" && o.exp != "c" && o.exp != "exit") { usage(argv[0]); return 2; }
  if (o.exp == "reg") {
    size_t bytes = 0;
    if (o.slot_bytes == 0 || o.slot_bytes > kSsizeMax || o.slots == 0 || o.slots > 256 ||
        !checked_mul(o.slots, o.slot_bytes, bytes) || bytes > kMaxReg)
      return finish(Verdict::EnvError, "W*S invalid or exceeds CUOBJ_MAX_MEMORY_REG_SIZE");
  } else {
    Geometry g;
    std::string why;
    if (!validate_geometry(o, o.exp == "c" ? o.slots : 1, g, why)) return finish(Verdict::EnvError, why);
  }
  if (cudaSetDevice(0) != cudaSuccess) return finish(Verdict::EnvError, "cudaSetDevice(0) failed");
  try {
    if (o.exp == "reg") return exp_reg(o);
    if (o.exp == "diag") return exp_single_get(o, false, run_id);
    if (o.exp == "rt-guard") return exp_single_get(o, true, run_id);
    if (o.exp == "c") return exp_concurrent(o, run_id);
    if (o.exp == "exit") return exp_exit_child(o, run_id);
  } catch (...) {
    // Report unexpected experiment exceptions without constructing another JSON record.
    std::fputs("{\"event\":\"result\",\"verdict\":\"ENV_ERROR\",\"detail\":\"unhandled experiment exception; evidence incomplete\",\"evidence_output_failed\":true}\n", stdout);
    return 3;
  }
  usage(argv[0]);
  return 2;
}
