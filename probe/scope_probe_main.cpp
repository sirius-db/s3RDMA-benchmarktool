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

// cuobj_scope_probe: a SERIAL registration-scope x interior-landing measurement
// against a live gateway. It answers one question decisively: is Sirius's
// serial base+offset target shape physically viable on this product + SDK?
//
// SCOPE BOUNDARY (deliberate — this is not an all-purpose probe):
//   * Serial B0/I0/S0/SI/R/P only. Concurrency is NOT verified here (a
//     --concurrent request is recorded but never yields a decisive PASS).
//   * Lifecycle (re-register/re-GET ordering, teardown) is NOT verified here;
//     the report marks it UNVERIFIED.
//   * The binary emits per-case measurement verdicts only. It does NOT derive
//     product outcome/STOP conclusions — that mapping lives in the skill.
//
// Trustworthy-verdict rules:
//   * Disjoint guarded cells. Every landing site is [guard | payload | guard]
//     so a misdirected write cannot masquerade as a correct landing.
//   * Single-callback gate on EVERY cuObjGet path: the per-callback max must be
//     known (cap > 0) and >= xfer, and exactly one callback must occur; a split
//     request is not decisive and yields INCONCLUSIVE.
//   * Exact-completion PASS: a caller-supplied 64-hex --expect-sha must match
//     the full transferred payload, cuObjGet must return exactly xfer, guards
//     must be intact, and GPU write visibility must be guaranteed. Missing
//     digest or unguaranteed visibility -> INCONCLUSIVE, never PASS. The poison
//     pattern is checked to differ from the expected digest so an all-no-op
//     write over matching content cannot pass.
//   * Nothing hidden: each callback emits callback_begin BEFORE the HTTP call
//     and callback_end after, both flushed, so a hang leaves a trace. No SDK
//     error aborts; rc/errno are recorded verbatim.
//   * deregister rc is NOT trustworthy on the pinned 1.0.0 SDK
//     (cuMemObjPutDescriptor returns 0 unconditionally); it is never used as
//     cleanup evidence.
//
// One case per process (--exp); a wrapper owns timeouts and sequencing.

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <cuda_runtime.h>
#include <cuobjclient.h>
#include <httplib.h>

#include "base64.hpp"
#include "buffers.hpp"
#include "cuobj_check.hpp"
#include "s3_util.hpp"
#include "sha256.hpp"

namespace {

using s3rdma::Buffer;
using s3rdma::BufferMode;

constexpr unsigned char kGuardByte = 0xc3;
constexpr size_t kAlign = 4096;

std::mutex g_stdout_mu;

double now_ms() {
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// ---- Verdict vocabulary -----------------------------------------------------

enum class Verdict { Pass, Fail, Inconclusive, EnvError };

const char* to_string(Verdict v) {
  switch (v) {
    case Verdict::Pass: return "PASS";
    case Verdict::Fail: return "FAIL";
    case Verdict::Inconclusive: return "INCONCLUSIVE";
    case Verdict::EnvError: return "ENV_ERROR";
  }
  return "INCONCLUSIVE";
}

int exit_code(Verdict v) {
  switch (v) {
    case Verdict::Pass: return 0;
    case Verdict::Fail: return 1;
    case Verdict::EnvError: return 3;
    case Verdict::Inconclusive: return 4;
  }
  return 4;
}

// ---- Checked arithmetic -----------------------------------------------------

bool checked_add(size_t a, size_t b, size_t& out) {
  if (a > SIZE_MAX - b) return false;
  out = a + b;
  return true;
}
bool checked_mul(size_t a, size_t b, size_t& out) {
  if (a != 0 && b > SIZE_MAX / a) return false;
  out = a * b;
  return true;
}
bool align_up(size_t v, size_t& out) {
  size_t r = 0;
  if (!checked_add(v, kAlign - 1, r)) return false;
  out = (r / kAlign) * kAlign;
  return true;
}
size_t align_down(size_t v) { return (v / kAlign) * kAlign; }

bool is_lower_hex_64(const std::string& s) {
  if (s.size() != 64) return false;
  for (char c : s) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

// ---- JSONL emission ---------------------------------------------------------

std::string escape(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    if (c == '"' || c == '\\') {
      out.push_back('\\');
      out.push_back(c);
    } else if (static_cast<unsigned char>(c) < 0x20) {
      char buf[8];
      std::snprintf(buf, sizeof(buf), "\\u%04x", c);
      out += buf;
    } else {
      out.push_back(c);
    }
  }
  return out;
}

class Line {
 public:
  Line(const std::string& tag, const std::string& event) {
    buf_ = "{\"case\":\"" + tag + "\",\"event\":\"" + event + "\"";
  }
  Line& str(const char* k, const std::string& v) {
    buf_ += std::string(",\"") + k + "\":\"" + escape(v) + "\"";
    return *this;
  }
  Line& num(const char* k, long long v) {
    buf_ += std::string(",\"") + k + "\":" + std::to_string(v);
    return *this;
  }
  Line& real(const char* k, double v) {
    char t[48];
    std::snprintf(t, sizeof(t), "%.3f", v);
    buf_ += std::string(",\"") + k + "\":" + t;
    return *this;
  }
  Line& boolean(const char* k, bool v) {
    buf_ += std::string(",\"") + k + "\":" + (v ? "true" : "false");
    return *this;
  }
  void emit() {
    std::lock_guard<std::mutex> lk(g_stdout_mu);
    std::printf("%s}\n", buf_.c_str());
    std::fflush(stdout);
  }

 private:
  std::string buf_;
};

class Jsonl {
 public:
  explicit Jsonl(std::string tag) : tag_(std::move(tag)) {}
  Line event(const std::string& n) const { return Line(tag_, n); }

 private:
  std::string tag_;
};

// ---- Layout: disjoint guarded cells ----------------------------------------

struct Span {
  size_t offset = 0;
  size_t len = 0;
};
struct Slot {
  Span payload;
  Span lead_guard;
  Span trail_guard;
};
struct Layout {
  size_t xfer = 0;
  size_t guard = 0;
  size_t base_region = 0;
  size_t cell = 0;
  bool ok = false;
  std::string error;
};

bool build_layout(size_t xfer, size_t guard, Layout& out) {
  out.xfer = xfer;
  out.guard = guard;
  size_t tmp = 0;
  if (!checked_add(xfer, guard, tmp) || !align_up(tmp, out.base_region)) {
    out.error = "base region overflow";
    return false;
  }
  if (!checked_add(guard, xfer, tmp) || !checked_add(tmp, guard, tmp) ||
      !align_up(tmp, out.cell)) {
    out.error = "cell size overflow";
    return false;
  }
  out.ok = true;
  return true;
}

Slot base_slot(const Layout& L) {
  Slot s;
  s.payload = {0, L.xfer};
  s.lead_guard = {0, 0};
  s.trail_guard = {L.xfer, L.base_region - L.xfer};
  return s;
}

bool interior_slot(const Layout& L, size_t j, size_t arena, Slot& out) {
  size_t cell_start = 0, off = 0, end = 0, slot_end = 0;
  if (!checked_mul(j, L.cell, cell_start) ||
      !checked_add(L.base_region, cell_start, cell_start))
    return false;
  if (!checked_add(cell_start, L.guard, off)) return false;
  if (!checked_add(off, L.xfer, end)) return false;
  if (!checked_add(cell_start, L.cell, slot_end)) return false;
  if (slot_end > arena) return false;
  out.payload = {off, L.xfer};
  out.lead_guard = {cell_start, L.guard};
  out.trail_guard = {end, slot_end - end};
  return true;
}

size_t arena_for_slots(const Layout& L, size_t count) {
  size_t cells = 0, total = 0;
  if (!checked_mul(count, L.cell, cells) || !checked_add(L.base_region, cells, total))
    return SIZE_MAX;
  return total;
}

// ---- Buffer span helpers ----------------------------------------------------

void write_span(Buffer& buf, const Span& s, const void* src_host) {
  if (s.len == 0) return;
  char* base = static_cast<char*>(buf.data());
  if (buf.mode() == BufferMode::Gpu) {
    CUDA_CHECK(cudaMemcpy(base + s.offset, src_host, s.len, cudaMemcpyHostToDevice));
  } else {
    std::memcpy(base + s.offset, src_host, s.len);
  }
}

void fill_span(Buffer& buf, const Span& s, unsigned char byte) {
  if (s.len == 0) return;
  char* base = static_cast<char*>(buf.data());
  if (buf.mode() == BufferMode::Gpu) {
    CUDA_CHECK(cudaMemset(base + s.offset, byte, s.len));
  } else {
    std::memset(base + s.offset, byte, s.len);
  }
}

std::vector<char> read_span(const Buffer& buf, const Span& s) {
  std::vector<char> out(s.len);
  if (s.len == 0) return out;
  const char* base = static_cast<const char*>(buf.data());
  if (buf.mode() == BufferMode::Gpu) {
    CUDA_CHECK(cudaMemcpy(out.data(), base + s.offset, s.len, cudaMemcpyDeviceToHost));
  } else {
    std::memcpy(out.data(), base + s.offset, s.len);
  }
  return out;
}

bool all_bytes(const std::vector<char>& v, unsigned char byte) {
  for (char c : v)
    if (static_cast<unsigned char>(c) != byte) return false;
  return true;
}

// ---- GPU write-ordering visibility -----------------------------------------

struct Visibility {
  int raw = -1;
  std::string name = "unknown";
  bool trustworthy = false;
};

Visibility read_visibility(BufferMode mode) {
  Visibility v;
  if (mode == BufferMode::Host) {
    v.name = "host_na";
    v.trustworthy = true;
    return v;
  }
  int val = 0;
  if (cudaDeviceGetAttribute(&val, cudaDevAttrGPUDirectRDMAWritesOrdering, 0) != cudaSuccess) {
    (void)cudaGetLastError();
    v.name = "query_failed";
    return v;
  }
  v.raw = val;
  if (val >= 200) {
    v.name = "all_devices";
    v.trustworthy = true;
  } else if (val >= 100) {
    v.name = "owner";
    v.trustworthy = true;
  } else {
    v.name = "none";
  }
  return v;
}

// ---- Poison pattern (differs from real data; digest-checked) ----------------

std::vector<char> make_poison(size_t len) {
  std::vector<char> p(len);
  for (size_t i = 0; i < len; ++i) {
    p[i] = static_cast<char>((i * 167u + 13u) & 0xffu);
  }
  return p;
}

// ---- Probe callback: begin (pre-HTTP) + end (post-HTTP), both immediate -----

struct ProbeContext {
  const Jsonl* log = nullptr;
  std::string who;
  std::string host;
  int port = 8080;
  std::string path;
  std::string range;
  const void* arena_base = nullptr;
  std::atomic<int> callbacks{0};
};

ssize_t probe_get_callback(const void* handle, char* ptr, size_t size, loff_t offset,
                           const cufileRDMAInfo_t* rdma) {
  auto* ctx = static_cast<ProbeContext*>(cuObjClient::getCtx(handle));
  if (ctx == nullptr || rdma == nullptr) return -1;
  const int idx = ctx->callbacks.fetch_add(1);
  const long long delta = static_cast<long long>(reinterpret_cast<uintptr_t>(ptr) -
                                                 reinterpret_cast<uintptr_t>(ctx->arena_base));

  ctx->log->event("callback_begin")
      .str("who", ctx->who)
      .num("call_index", idx)
      .num("ptr_delta", delta)
      .num("cb_size", static_cast<long long>(size))
      .num("cb_offset", static_cast<long long>(offset))
      .emit();

  httplib::Headers headers = {
      {s3rdma::kHdrRdmaToken, s3rdma::base64_encode(rdma->desc_str, rdma->desc_len)},
  };
  if (!ctx->range.empty()) headers.emplace("Range", ctx->range);
  httplib::Client cli(ctx->host, ctx->port);
  auto res = cli.Get(ctx->path, headers);

  Line line = ctx->log->event("callback_end");
  line.str("who", ctx->who).num("call_index", idx);
  ssize_t rc = -1;
  if (!res) {
    line.boolean("http_ok", false).str("transport_error", httplib::to_string(res.error()));
  } else {
    line.boolean("http_ok", true).num("http_status", res->status);
    if (res->has_header(s3rdma::kHdrRdmaReply)) {
      const std::string raw = res->get_header_value(s3rdma::kHdrRdmaReply);
      line.boolean("reply_present", true)
          .str("reply_b64", s3rdma::base64_encode(raw.data(), raw.size()));
    } else {
      line.boolean("reply_present", false);
    }
    if (res->status == 200 || res->status == 206) rc = static_cast<ssize_t>(size);
  }
  line.num("returns", static_cast<long long>(rc)).emit();
  return rc;
}

// ---- Raw cuObjClient wrapper ------------------------------------------------

class ProbeClient {
 public:
  ProbeClient() {
    ops_.get = &probe_get_callback;
    ops_.put = nullptr;
    client_ = new cuObjClient(ops_, CUOBJ_PROTO_RDMA_DC_V1);
  }
  ~ProbeClient() { delete client_; }

  int register_memory(void* ptr, size_t size, int& err) {
    errno = 0;
    const int rc = static_cast<int>(client_->cuMemObjGetDescriptor(ptr, size));
    err = errno;
    return rc;
  }
  void deregister_memory(void* ptr) { client_->cuMemObjPutDescriptor(ptr); }
  ssize_t get(ProbeContext* ctx, void* ptr, size_t size, int& err) {
    errno = 0;
    try {
      const ssize_t n = client_->cuObjGet(ctx, ptr, size);
      err = errno;
      return n;
    } catch (const std::exception& e) {
      err = errno;
      std::fprintf(stderr, "[probe] cuObjGet threw: %s\n", e.what());
      return -1;
    } catch (...) {
      err = errno;
      std::fprintf(stderr, "[probe] cuObjGet threw a non-standard exception\n");
      return -1;
    }
  }
  int slice_token(void* base, size_t size, size_t off, char** out) {
    return static_cast<int>(client_->cuMemObjGetRDMAToken(base, size, off, CUOBJ_GET, out));
  }
  void put_token(char* d) { client_->cuMemObjPutRDMAToken(d); }
  ssize_t callback_max(void* ptr) { return client_->cuMemObjGetMaxRequestCallbackSize(ptr); }

 private:
  CUObjOps_t ops_{};
  cuObjClient* client_ = nullptr;
};

// ---- Options ----------------------------------------------------------------

struct Options {
  std::string host = "127.0.0.1";
  int port = 8080;
  std::string bucket, object, exp, mode = "gpu", expect_sha;
  size_t xfer = 4ULL * 1024 * 1024;
  size_t guard = 64ULL * 1024;
  size_t arena_bytes = 0;
  int concurrent = 0;
};

void usage(const char* a0) {
  std::fprintf(stderr,
               "Usage: %s --exp <b0|i0|s0|si|r|p|m> --bucket <b> --object <k> [options]\n"
               "  --server <host:port>   default 127.0.0.1:8080\n"
               "  --mode <host|gpu>      default gpu\n"
               "  --xfer <bytes>         GET size (default 4194304; clipped only w/o --expect-sha)\n"
               "  --guard <bytes>        guard band per cell (default 65536)\n"
               "  --arena-bytes <bytes>  override arena size\n"
               "  --concurrent <n>       si/r only: n>=2 (0=serial; 1 is rejected)\n"
               "  --expect-sha <hex64>   known digest of object[0,xfer); required for PASS\n"
               "Verdict PASS/FAIL/INCONCLUSIVE/ENV_ERROR; exit 0/1/4/3.\n",
               a0);
}

bool parse_size_arg(const char* s, size_t& out) {
  if (s == nullptr || *s == '\0' || *s == '-' || *s == '+') return false;
  errno = 0;
  char* end = nullptr;
  unsigned long long v = std::strtoull(s, &end, 10);
  if (end == s || *end != '\0' || errno == ERANGE || v == 0) return false;
  out = static_cast<size_t>(v);
  return true;
}

bool parse_options(int argc, char** argv, Options& o) {
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : nullptr; };
    const char* v = nullptr;
    if (a == "--server" && (v = next())) {
      const std::string s = v;
      const auto c = s.find(':');
      if (c == std::string::npos) return false;
      o.host = s.substr(0, c);
      size_t p = 0;
      if (!parse_size_arg(s.c_str() + c + 1, p) || p > 65535) return false;
      o.port = static_cast<int>(p);
    } else if (a == "--bucket" && (v = next())) {
      o.bucket = v;
    } else if (a == "--object" && (v = next())) {
      o.object = v;
    } else if (a == "--exp" && (v = next())) {
      o.exp = v;
    } else if (a == "--mode" && (v = next())) {
      o.mode = v;
      if (o.mode != "host" && o.mode != "gpu") return false;
    } else if (a == "--xfer" && (v = next())) {
      if (!parse_size_arg(v, o.xfer)) return false;
    } else if (a == "--guard" && (v = next())) {
      if (!parse_size_arg(v, o.guard)) return false;
    } else if (a == "--arena-bytes" && (v = next())) {
      if (!parse_size_arg(v, o.arena_bytes)) return false;
    } else if (a == "--concurrent" && (v = next())) {
      const std::string cs = v;
      if (cs == "0") {
        o.concurrent = 0;
      } else {
        size_t c = 0;
        if (!parse_size_arg(v, c) || c < 2 || c > 256) return false;  // 1 rejected
        o.concurrent = static_cast<int>(c);
      }
    } else if (a == "--expect-sha" && (v = next())) {
      o.expect_sha = v;
      if (!is_lower_hex_64(o.expect_sha)) return false;
    } else {
      return false;
    }
  }
  if (o.bucket.empty() || o.object.empty() || o.exp.empty()) return false;
  if (o.exp != "b0" && o.exp != "i0" && o.exp != "s0" && o.exp != "si" &&
      o.exp != "r" && o.exp != "p" && o.exp != "m")
    return false;
  // Concurrency stress is scoped to si (the target shape). r/p are fallback
  // registration-shape candidates for which a serial probe is sufficient.
  if (o.concurrent > 0 && o.exp != "si") return false;
  return true;
}

// ---- HEAD with classification ----------------------------------------------

struct HeadResult {
  ssize_t size = -1;
  bool ok = true;
  std::string detail;
};

HeadResult head_object(const Options& o, const std::string& path) {
  HeadResult r;
  httplib::Client cli(o.host, o.port);
  auto res = cli.Head(path);
  if (!res) {
    r.ok = false;
    r.detail = "HEAD transport failure: " + httplib::to_string(res.error());
    return r;
  }
  if (res->status != 200) {
    r.ok = false;
    r.detail = "HEAD status " + std::to_string(res->status);
    return r;
  }
  if (!res->has_header("Content-Length")) {
    r.ok = false;
    r.detail = "HEAD missing Content-Length";
    return r;
  }
  try {
    const long long n = std::stoll(res->get_header_value("Content-Length"));
    if (n <= 0) {
      r.ok = false;
      r.detail = "HEAD non-positive Content-Length";
    } else {
      r.size = static_cast<ssize_t>(n);
    }
  } catch (...) {
    r.ok = false;
    r.detail = "HEAD malformed Content-Length";
  }
  return r;
}

// ---- GET primitives (split for concurrency) --------------------------------

struct GetResult {
  ssize_t rc = -1;
  int errno_after = 0;
  int callbacks = 0;
  double start_ms = 0;
  double end_ms = 0;
};

void prepare_slot(Buffer& arena, const Slot& slot, const std::vector<char>& poison) {
  write_span(arena, slot.payload, poison.data());
  fill_span(arena, slot.lead_guard, kGuardByte);
  fill_span(arena, slot.trail_guard, kGuardByte);
}

GetResult issue_get(const Jsonl& log, const Options& o, ProbeClient& client,
                    const std::string& who, Buffer& arena, const Slot& slot) {
  ProbeContext ctx;
  ctx.log = &log;
  ctx.who = who;
  ctx.host = o.host;
  ctx.port = o.port;
  ctx.path = "/" + o.bucket + "/" + o.object;
  ctx.range = "bytes=0-" + std::to_string(o.xfer - 1);
  ctx.arena_base = arena.data();

  GetResult g;
  g.start_ms = now_ms();
  g.rc = client.get(&ctx, static_cast<char*>(arena.data()) + slot.payload.offset, o.xfer,
                    g.errno_after);
  g.end_ms = now_ms();
  g.callbacks = ctx.callbacks.load();
  return g;
}

struct SlotVerdict {
  Verdict verdict = Verdict::Inconclusive;
  std::string detail;
};

SlotVerdict verify_slot(const Jsonl& log, const Options& o, const Visibility& vis,
                        const std::string& who, Buffer& arena, const Slot& slot,
                        const GetResult& g) {
  const std::vector<char> payload = read_span(arena, slot.payload);
  const std::vector<char> lead = read_span(arena, slot.lead_guard);
  const std::vector<char> trail = read_span(arena, slot.trail_guard);
  const std::string digest = s3rdma::sha256_hex(payload.data(), payload.size());
  const bool digest_match = !o.expect_sha.empty() && digest == o.expect_sha;
  const bool guards_intact = all_bytes(lead, kGuardByte) && all_bytes(trail, kGuardByte);

  log.event("get")
      .str("who", who)
      .num("payload_offset", static_cast<long long>(slot.payload.offset))
      .num("rc", static_cast<long long>(g.rc))
      .num("errno", g.errno_after)
      .num("callbacks", g.callbacks)
      .real("start_ms", g.start_ms)
      .real("end_ms", g.end_ms)
      .emit();
  log.event("landed")
      .str("who", who)
      .str("sha256", digest)
      .boolean("digest_match", digest_match)
      .boolean("guards_intact", guards_intact)
      .str("visibility", vis.name)
      .emit();

  SlotVerdict sv;
  if (g.callbacks != 1) {
    sv.verdict = Verdict::Inconclusive;
    sv.detail = "callback count != 1 (split request, not decisive)";
  } else if (g.rc < 0) {
    sv.verdict = Verdict::Fail;
    sv.detail = "cuObjGet rc<0";
  } else if (!vis.trustworthy) {
    sv.verdict = Verdict::Inconclusive;
    sv.detail = "gpu rdma write visibility not guaranteed; no flush wired";
  } else if (!guards_intact) {
    sv.verdict = Verdict::Fail;
    sv.detail = "guard bytes overwritten";
  } else if (o.expect_sha.empty()) {
    sv.verdict = Verdict::Inconclusive;
    sv.detail = "no --expect-sha: landing not provable";
  } else if (g.rc != static_cast<ssize_t>(o.xfer)) {
    sv.verdict = Verdict::Fail;
    sv.detail = "rc != xfer";
  } else if (!digest_match) {
    sv.verdict = Verdict::Fail;
    sv.detail = "payload digest mismatch";
  } else {
    sv.verdict = Verdict::Pass;
    sv.detail = "ok";
  }
  return sv;
}

// Fold per-slot verdicts: FAIL dominates, then INCONCLUSIVE; carry the matching
// detail (a later FAIL must not keep an earlier INCONCLUSIVE detail).
Verdict fold(const std::vector<SlotVerdict>& svs, std::string& detail) {
  const SlotVerdict* fail = nullptr;
  const SlotVerdict* inc = nullptr;
  for (const auto& s : svs) {
    if (s.verdict == Verdict::Fail && fail == nullptr) fail = &s;
    if (s.verdict == Verdict::Inconclusive && inc == nullptr) inc = &s;
  }
  if (fail) {
    detail = fail->detail;
    return Verdict::Fail;
  }
  if (inc) {
    detail = inc->detail;
    return Verdict::Inconclusive;
  }
  detail = "ok";
  return Verdict::Pass;
}

// A serial guarded GET: prepare, gate is done by the caller, issue, verify.
SlotVerdict serial_get(const Jsonl& log, const Options& o, const Visibility& vis,
                       ProbeClient& client, const std::string& who, Buffer& arena,
                       const Slot& slot, const std::vector<char>& poison) {
  prepare_slot(arena, slot, poison);
  if (arena.mode() == BufferMode::Gpu) CUDA_CHECK(cudaDeviceSynchronize());
  const GetResult g = issue_get(log, o, client, who, arena, slot);
  if (arena.mode() == BufferMode::Gpu) CUDA_CHECK(cudaDeviceSynchronize());
  return verify_slot(log, o, vis, who, arena, slot, g);
}

// Require the SDK's per-callback max to be known and >= xfer (fail closed).
bool callback_gate(const Jsonl& log, ProbeClient& client, void* ptr, size_t xfer,
                   std::string& err) {
  const ssize_t cap = client.callback_max(ptr);
  log.event("callback_max").num("bytes", static_cast<long long>(cap)).emit();
  if (cap <= 0) {
    err = "per-callback max unknown (cap<=0); cannot guarantee single-callback GET";
    return false;
  }
  if (xfer > static_cast<size_t>(cap)) {
    err = "xfer exceeds per-callback max (would split into multiple callbacks)";
    return false;
  }
  return true;
}

int result(const Jsonl& log, Verdict v, const std::string& detail) {
  log.event("result").str("verdict", to_string(v)).str("detail", detail).emit();
  return exit_code(v);
}

}  // namespace

int main(int argc, char** argv) {
  Options o;
  if (!parse_options(argc, argv, o)) {
    usage(argv[0]);
    return 2;
  }

  const Jsonl log(o.exp);
  CUDA_CHECK(cudaSetDevice(0));

  const BufferMode mode = s3rdma::parse_mode(o.mode);
  const Visibility vis = read_visibility(mode);
  log.event("visibility").num("raw", vis.raw).str("name", vis.name)
      .boolean("trustworthy", vis.trustworthy).emit();
  // Evidence boundary, emitted early so it is present even on an ENV_ERROR exit.
  log.event("boundary")
      .str("scope", "serial registration-scope x interior-landing")
      .str("concurrency", "deferred")
      .str("lifecycle", "unverified")
      .str("deregister_rc", "not_trustworthy_sdk_1.0.0")
      .emit();

  const std::string path = "/" + o.bucket + "/" + o.object;
  const HeadResult head = head_object(o, path);
  log.event("head").num("content_length", head.size).str("detail", head.detail).emit();
  if (!head.ok) return result(log, Verdict::EnvError, head.detail);
  if (static_cast<size_t>(head.size) < o.xfer) {
    if (!o.expect_sha.empty())
      return result(log, Verdict::EnvError,
                    "object smaller than requested xfer; --expect-sha would not correspond");
    o.xfer = static_cast<size_t>(head.size);  // diagnostic (no digest) mode only
  }

  Layout L;
  if (!build_layout(o.xfer, o.guard, L)) return result(log, Verdict::EnvError, L.error);

  // Poison must differ from the expected payload, or an all-no-op write passes.
  const std::vector<char> poison = make_poison(o.xfer);
  if (!o.expect_sha.empty() &&
      s3rdma::sha256_hex(poison.data(), poison.size()) == o.expect_sha)
    return result(log, Verdict::EnvError, "poison pattern collides with --expect-sha");

  size_t slots_needed = (o.exp == "r") ? 2 : 1;
  const size_t derived = arena_for_slots(L, slots_needed);
  if (derived == SIZE_MAX) return result(log, Verdict::EnvError, "arena size overflow");
  size_t arena_bytes = o.arena_bytes ? o.arena_bytes : derived;

  // p: two disjoint 4K-aligned halves, each holding one guarded cell.
  size_t p_half = 0;
  if (o.exp == "p") {
    size_t two = 0;
    if (!checked_mul(L.cell, 2, two)) return result(log, Verdict::EnvError, "p overflow");
    if (arena_bytes < two) arena_bytes = two;
    p_half = align_down(arena_bytes / 2);
    if (p_half < L.cell) return result(log, Verdict::EnvError, "p half smaller than a cell");
  } else if (arena_bytes < derived) {
    return result(log, Verdict::EnvError, "arena too small for layout");
  }
  constexpr size_t kMaxReg = 4ULL * 1024 * 1024 * 1024 - 64 * 1024;
  if (arena_bytes > kMaxReg)
    return result(log, Verdict::EnvError, "arena exceeds CUOBJ_MAX_MEMORY_REG_SIZE");

  Buffer arena(mode, arena_bytes);
  log.event("env")
      .str("object", o.bucket + "/" + o.object)
      .str("mode", o.mode)
      .num("arena_bytes", static_cast<long long>(arena_bytes))
      .num("xfer", static_cast<long long>(o.xfer))
      .num("guard", static_cast<long long>(o.guard))
      .num("cell", static_cast<long long>(L.cell))
      .num("concurrent", o.concurrent)
      .boolean("expect_sha", !o.expect_sha.empty())
      .emit();

  // ---- b0 / i0 / s0 / si ----------------------------------------------------
  if (o.exp == "b0" || o.exp == "i0" || o.exp == "s0" || o.exp == "si") {
    ProbeClient a;
    int reg_err = 0;
    const int reg = a.register_memory(arena.data(), arena_bytes, reg_err);
    log.event("register").str("who", "A").num("rc", reg).num("errno", reg_err).emit();
    if (reg != 0) return result(log, Verdict::Fail, "arena registration failed");

    std::string gate_err;
    if (!callback_gate(log, a, arena.data(), o.xfer, gate_err)) {
      a.deregister_memory(arena.data());
      return result(log, Verdict::EnvError, gate_err);
    }

    const bool interior = (o.exp == "i0" || o.exp == "si");
    const bool cross = (o.exp == "s0" || o.exp == "si");

    Slot slot;
    if (!interior) {
      slot = base_slot(L);
    } else if (!interior_slot(L, 0, arena_bytes, slot)) {
      a.deregister_memory(arena.data());
      return result(log, Verdict::EnvError, "interior slot out of arena");
    }
    SlotVerdict sv;
    if (cross) {
      ProbeClient b;  // constructed ONLY for cross variants (s0/si)
      sv = serial_get(log, o, vis, b, "B", arena, slot, poison);
    } else {
      sv = serial_get(log, o, vis, a, "A", arena, slot, poison);
    }
    Verdict verdict = sv.verdict;
    std::string detail = sv.detail;
    // Concurrency is out of scope for this serial-boundary probe: a --concurrent
    // request is recorded but never yields a decisive PASS (deferred).
    if (o.concurrent > 0 && verdict == Verdict::Pass) {
      verdict = Verdict::Inconclusive;
      detail = "serial landing ok; concurrency requested but not verified here (deferred)";
    }
    a.deregister_memory(arena.data());
    return result(log, verdict, detail);
  }

  // ---- r --------------------------------------------------------------------
  if (o.exp == "r") {
    ProbeClient a, b;
    int ea = 0, eb = 0;
    const int ra = a.register_memory(arena.data(), arena_bytes, ea);
    const int rb = b.register_memory(arena.data(), arena_bytes, eb);
    log.event("register").str("who", "A").num("rc", ra).num("errno", ea).emit();
    log.event("register").str("who", "B").num("rc", rb).num("errno", eb).emit();
    if (ra != 0 || rb != 0) {
      if (ra == 0) a.deregister_memory(arena.data());
      if (rb == 0) b.deregister_memory(arena.data());
      return result(log, Verdict::Fail, "double registration failed");
    }
    std::string gate_err;
    if (!callback_gate(log, a, arena.data(), o.xfer, gate_err)) {
      a.deregister_memory(arena.data());
      b.deregister_memory(arena.data());
      return result(log, Verdict::EnvError, gate_err);
    }
    Slot sa, sb;
    if (!interior_slot(L, 0, arena_bytes, sa) || !interior_slot(L, 1, arena_bytes, sb)) {
      a.deregister_memory(arena.data());
      b.deregister_memory(arena.data());
      return result(log, Verdict::EnvError, "interior slot out of arena");
    }
    prepare_slot(arena, sa, poison);
    prepare_slot(arena, sb, poison);
    if (mode == BufferMode::Gpu) CUDA_CHECK(cudaDeviceSynchronize());

    std::vector<GetResult> gets(2);
    std::vector<SlotVerdict> svs(2);
    gets[0] = issue_get(log, o, a, "A", arena, sa);
    gets[1] = issue_get(log, o, b, "B", arena, sb);
    if (mode == BufferMode::Gpu) CUDA_CHECK(cudaDeviceSynchronize());
    svs[0] = verify_slot(log, o, vis, "A", arena, sa, gets[0]);
    svs[1] = verify_slot(log, o, vis, "B", arena, sb, gets[1]);
    std::string detail;
    const Verdict verdict = fold(svs, detail);
    a.deregister_memory(arena.data());
    b.deregister_memory(arena.data());
    return result(log, verdict, detail);
  }

  // ---- p: disjoint aligned halves, each a guarded cell ----------------------
  if (o.exp == "p") {
    char* base = static_cast<char*>(arena.data());
    ProbeClient a, b;
    int ea = 0, eb = 0;
    const int ra = a.register_memory(base, p_half, ea);
    const int rb = b.register_memory(base + p_half, arena_bytes - p_half, eb);
    log.event("register").str("who", "A").num("rc", ra).num("bytes", static_cast<long long>(p_half)).num("errno", ea).emit();
    log.event("register").str("who", "B").num("rc", rb).num("bytes", static_cast<long long>(arena_bytes - p_half)).num("errno", eb).emit();
    if (ra != 0 || rb != 0) {
      if (ra == 0) a.deregister_memory(base);
      if (rb == 0) b.deregister_memory(base + p_half);
      return result(log, Verdict::Fail, "slice registration failed");
    }
    std::string gate_err;
    if (!callback_gate(log, a, base, o.xfer, gate_err) ||
        !callback_gate(log, b, base + p_half, o.xfer, gate_err)) {
      a.deregister_memory(base);
      b.deregister_memory(base + p_half);
      return result(log, Verdict::EnvError, gate_err);
    }
    // A cell inside slice A (offset 0..) and slice B (offset p_half..).
    Slot sa{{L.guard, o.xfer}, {0, L.guard}, {L.guard + o.xfer, L.cell - L.guard - o.xfer}};
    Slot sb{{p_half + L.guard, o.xfer}, {p_half, L.guard},
            {p_half + L.guard + o.xfer, L.cell - L.guard - o.xfer}};
    prepare_slot(arena, sa, poison);
    prepare_slot(arena, sb, poison);
    if (mode == BufferMode::Gpu) CUDA_CHECK(cudaDeviceSynchronize());
    std::vector<GetResult> gets(2);
    gets[0] = issue_get(log, o, a, "A", arena, sa);
    gets[1] = issue_get(log, o, b, "B", arena, sb);
    if (mode == BufferMode::Gpu) CUDA_CHECK(cudaDeviceSynchronize());
    // Verify BOTH slots' guards after BOTH GETs (catch either overrun).
    std::vector<SlotVerdict> svs(2);
    svs[0] = verify_slot(log, o, vis, "A", arena, sa, gets[0]);
    svs[1] = verify_slot(log, o, vis, "B", arena, sb, gets[1]);
    std::string detail;
    const Verdict verdict = fold(svs, detail);
    a.deregister_memory(base);
    b.deregister_memory(base + p_half);
    return result(log, verdict, detail);
  }

  // ---- m: alternate slice-token path (diagnostic only) ----------------------
  if (o.exp == "m") {
    ProbeClient a;
    int ea = 0;
    const int reg = a.register_memory(arena.data(), arena_bytes, ea);
    log.event("register").str("who", "A").num("rc", reg).num("errno", ea).emit();
    if (reg != 0) return result(log, Verdict::Fail, "arena registration failed");
    Slot slot;
    if (!interior_slot(L, 0, arena_bytes, slot)) {
      a.deregister_memory(arena.data());
      return result(log, Verdict::EnvError, "interior slot out of arena");
    }
    char* desc = nullptr;
    const int tok = a.slice_token(arena.data(), o.xfer, slot.payload.offset, &desc);
    log.event("slice_token").num("rc", tok)
        .num("buffer_offset", static_cast<long long>(slot.payload.offset))
        .num("desc_len", desc ? static_cast<long long>(std::strlen(desc)) : 0).emit();
    Verdict verdict = Verdict::Inconclusive;
    std::string detail = "slice token unavailable";
    if (tok == 0 && desc != nullptr) {
      prepare_slot(arena, slot, poison);
      if (mode == BufferMode::Gpu) CUDA_CHECK(cudaDeviceSynchronize());
      httplib::Headers headers = {
          {s3rdma::kHdrRdmaToken, s3rdma::base64_encode(desc, std::strlen(desc))},
          {"Range", "bytes=0-" + std::to_string(o.xfer - 1)},
      };
      httplib::Client cli(o.host, o.port);
      auto res = cli.Get(path, headers);
      const int status = res ? res->status : 0;
      log.event("manual_get").num("http_status", status)
          .boolean("reply_present", res && res->has_header(s3rdma::kHdrRdmaReply)).emit();
      if (mode == BufferMode::Gpu) CUDA_CHECK(cudaDeviceSynchronize());
      const std::vector<char> payload = read_span(arena, slot.payload);
      const std::vector<char> lead = read_span(arena, slot.lead_guard);
      const std::vector<char> trail = read_span(arena, slot.trail_guard);
      const std::string digest = s3rdma::sha256_hex(payload.data(), payload.size());
      const bool guards = all_bytes(lead, kGuardByte) && all_bytes(trail, kGuardByte);
      const bool dmatch = !o.expect_sha.empty() && digest == o.expect_sha;
      log.event("landed").str("who", "manual").str("sha256", digest)
          .boolean("digest_match", dmatch).boolean("guards_intact", guards)
          .str("visibility", vis.name).emit();
      if (!(status == 200 || status == 206)) { verdict = Verdict::Fail; detail = "manual GET status"; }
      else if (!vis.trustworthy) { verdict = Verdict::Inconclusive; detail = "visibility not guaranteed"; }
      else if (!guards) { verdict = Verdict::Fail; detail = "guard overwritten"; }
      else if (o.expect_sha.empty()) { verdict = Verdict::Inconclusive; detail = "no --expect-sha"; }
      else if (!dmatch) { verdict = Verdict::Fail; detail = "digest mismatch"; }
      else { verdict = Verdict::Pass; detail = "ok"; }
      a.put_token(desc);
    }
    a.deregister_memory(arena.data());
    // M is an alternate-path diagnostic: never upgrade the cuObjGet-path verdict.
    log.event("result").str("verdict", to_string(verdict)).str("detail", detail)
        .boolean("diagnostic_only", true).emit();
    return exit_code(verdict);
  }

  usage(argv[0]);
  return 2;
}
