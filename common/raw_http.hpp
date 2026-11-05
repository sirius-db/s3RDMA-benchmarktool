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

// Byte-preserving HTTP/1.1 GET for the rig probe's control plane. One request
// message per call (no redirects, no retries, no keep-alive); the request is
// validated before anything is written; the response head is kept exactly as
// received (no trimming beyond edge OWS of parsed values, no percent decoding,
// no merging of repeated fields); the message framing is checked so that a
// receipt is only "complete" when the whole HTTP message arrived. Not used by
// the stock client or the scope probe.
#pragma once

#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <thread>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "sigv4.hpp"

namespace s3rdma {

struct RawField {
  std::string name, lower_name, value, line;
};

struct RawResponse {
  // Emission (one validated, token-bearing request message).
  bool request_sent = false;      // all request bytes accepted by the socket
  bool request_partial = false;   // some bytes accepted, then failure (possibly emitted)
  bool token_bearing = false;     // the validated request carries a non-empty x-amz-rdma-token
  size_t request_bytes = 0;
  // Receipt.
  bool head_complete = false;     // CRLFCRLF within max_head
  bool head_malformed = false;    // outside the strict HTTP/1.1 grammar
  std::string head_bytes;         // exact bytes (bounded prefix when incomplete)
  int status = 0;
  std::string http_version, reason;
  std::vector<RawField> fields;   // wire order
  // Framing of the final message (RFC 9112 §6.3). Supported profile: one or
  // more identical valid decimal Content-Length values, or a close-delimited
  // body. Chunked and every other transfer-coding combination are recorded
  // but never qualify (message_complete stays false).
  std::string framing;            // "content-length" | "chunked-unqualified" | "close-delimited" | "invalid"
  long long content_length = -1;  // declared, when framing == content-length
  size_t body_octets = 0;         // raw octets received after the head, bounded by max_body
  size_t body_content = 0;        // retained content octets for supported framing; unavailable for other framing (stored as zero)
  bool body_truncated = false;    // reading stopped at max_body before the message ended
  bool message_complete = false;  // the whole response message was received and well framed
  bool transport_error = false;   // any send/recv/connect failure or timeout
  std::string error;

  size_t count(const char* lower_name) const {
    size_t n = 0;
    for (auto& f : fields) n += f.lower_name == lower_name;
    return n;
  }
  const RawField* first(const char* lower_name) const {
    for (auto& f : fields) if (f.lower_name == lower_name) return &f;
    return nullptr;
  }
};

namespace raw_http_detail {

struct Fd {
  int fd = -1;
  ~Fd() { if (fd >= 0) close(fd); }
};
// Address lists are shared owners with freeaddrinfo as deleter: a resolver
// result produced after the caller abandoned its wait is freed by the task's
// own last reference, never leaked.
using AddrList = std::shared_ptr<addrinfo>;
inline AddrList own_addrs(addrinfo* ai) { return AddrList(ai, [](addrinfo* p) { if (p) freeaddrinfo(p); }); }

using Clock = std::chrono::steady_clock;

enum class BudgetState { Ready, Expired, OptionError };
struct Budget {
  BudgetState state = BudgetState::Ready;
  int err = 0;  // errno captured at the failed option call
};
// Arms the socket with the remaining budget; distinguishes an expired
// deadline from a failed socket option and keeps that call's errno.
inline Budget arm_remaining_timeout(int fd, Clock::time_point deadline) {
  auto left = std::chrono::duration_cast<std::chrono::microseconds>(deadline - Clock::now());
  if (left.count() <= 0) return {BudgetState::Expired, 0};
  timeval tv{};
  tv.tv_sec = static_cast<time_t>(left.count() / 1000000);
  tv.tv_usec = static_cast<suseconds_t>(left.count() % 1000000);
  if (tv.tv_sec == 0 && tv.tv_usec == 0) tv.tv_usec = 1;
  if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) != 0 ||
      setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv) != 0)
    return {BudgetState::OptionError, errno};
  return {BudgetState::Ready, 0};
}
// Report timeout-class I/O errors observed after the overall deadline as budget expiry.
inline std::string io_error(const char* op, int err, Clock::time_point deadline, const char* phase) {
  if ((err == EAGAIN || err == EWOULDBLOCK || err == ETIMEDOUT) && Clock::now() >= deadline)
    return std::string("deadline exceeded during ") + phase + " (socket timeout)";
  return std::string(op) + ": " + std::strerror(err);
}
inline std::string budget_error(const Budget& b, const char* phase) {
  return b.state == BudgetState::Expired ? std::string("deadline exceeded during ") + phase
                                         : std::string("socket timeout option failed during ") + phase + ": " + std::strerror(b.err);
}

// Test-only seam: when set, replaces send() so partial writes and write
// errors can be produced deterministically. Never set by the probe.
inline std::function<ssize_t(int, const void*, size_t, int)>& send_seam() {
  static std::function<ssize_t(int, const void*, size_t, int)> f;
  return f;
}

inline bool is_tchar(unsigned char c) {
  if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) return true;
  return c != 0 && std::strchr("!#$%&'*+-.^_`|~", static_cast<int>(c)) != nullptr;
}
inline bool is_pchar_or_delim(unsigned char c) {  // origin-form request target bytes
  if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) return true;
  return c != 0 && std::strchr("-._~%!$&'()*+,;=:@/?", static_cast<int>(c)) != nullptr;
}
inline std::string lower_ascii(std::string s) {
  for (auto& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return s;
}
inline bool valid_field_value(const std::string& v) {
  for (unsigned char c : v) if ((c < 0x20 && c != '\t') || c == 0x7f) return false;
  return true;
}

// Builds the request message or throws std::invalid_argument. Exactly one
// Host (ours), caller fields validated, framing headers reserved.
inline std::string build_request(const std::string& host_header, const std::string& target,
                                 const HeaderList& headers, bool& token_bearing) {
  if (target.empty() || target[0] != '/') throw std::invalid_argument("request target must be origin-form");
  for (unsigned char c : target)
    if (!is_pchar_or_delim(c)) throw std::invalid_argument("request target contains bytes that require encoding");
  if (host_header.empty() || !valid_field_value(host_header) || host_header.find(' ') != std::string::npos)
    throw std::invalid_argument("invalid Host value");
  token_bearing = false;
  std::string req = "GET " + target + " HTTP/1.1\r\nHost: " + host_header + "\r\n";
  for (auto& h : headers) {
    if (h.first.empty()) throw std::invalid_argument("empty field name");
    for (unsigned char c : h.first) if (!is_tchar(c)) throw std::invalid_argument("invalid field name");
    const std::string lname = lower_ascii(h.first);
    if (lname == "host" || lname == "connection" || lname == "content-length" || lname == "transfer-encoding")
      throw std::invalid_argument("reserved header supplied by the caller: " + lname);
    if (!valid_field_value(h.second)) throw std::invalid_argument("invalid field value for " + lname);
    if (lname == "x-amz-rdma-token" && !h.second.empty()) token_bearing = true;
    req += h.first + ": " + h.second + "\r\n";
  }
  req += "Connection: close\r\n\r\n";
  return req;
}

// Strict status line + fields. Any deviation marks the head malformed.
inline void parse_head(RawResponse& r) {
  const std::string& h = r.head_bytes;
  size_t pos = 0;
  auto next_line = [&](std::string& out) {
    const size_t e = h.find("\r\n", pos);
    if (e == std::string::npos) return false;
    out = h.substr(pos, e - pos);
    pos = e + 2;
    return true;
  };
  std::string line;
  if (!next_line(line) || line.size() < 13 || line.compare(0, 7, "HTTP/1.") != 0 ||
      line[7] < '0' || line[7] > '9' || line[8] != ' ' || line[12] != ' ' ||
      line[9] < '0' || line[9] > '9' || line[10] < '0' || line[10] > '9' || line[11] < '0' || line[11] > '9') {
    r.head_malformed = true;
    return;
  }
  r.http_version = line.substr(0, 8);
  r.status = (line[9] - '0') * 100 + (line[10] - '0') * 10 + (line[11] - '0');
  r.reason = line.substr(13);
  if (!valid_field_value(r.reason)) { r.head_malformed = true; return; }
  while (next_line(line)) {
    if (line.empty()) return;
    if (line[0] == ' ' || line[0] == '\t') { r.head_malformed = true; return; }  // obs-fold
    const size_t colon = line.find(':');
    if (colon == std::string::npos || colon == 0) { r.head_malformed = true; return; }
    RawField f;
    f.line = line;
    f.name = line.substr(0, colon);
    for (unsigned char c : f.name) if (!is_tchar(c)) { r.head_malformed = true; return; }
    f.lower_name = lower_ascii(f.name);
    size_t b = colon + 1, e = line.size();
    while (b < e && (line[b] == ' ' || line[b] == '\t')) ++b;
    while (e > b && (line[e - 1] == ' ' || line[e - 1] == '\t')) --e;
    f.value = line.substr(b, e - b);
    if (!valid_field_value(f.value)) { r.head_malformed = true; return; }
    r.fields.push_back(std::move(f));
  }
  r.head_malformed = true;
}

inline bool parse_decimal(const std::string& s, long long& out) {
  if (s.empty() || s.size() > 18) return false;
  long long v = 0;
  for (char c : s) { if (c < '0' || c > '9') return false; v = v * 10 + (c - '0'); }
  out = v;
  return true;
}

// Classifies the supported profile: Transfer-Encoding never qualifies, TE+CL is
// invalid, and repeated Content-Length values must be valid and equal.
inline void classify_framing(RawResponse& r) {
  const size_t te = r.count("transfer-encoding");
  const size_t cl = r.count("content-length");
  if (te > 0) {
    r.framing = (te == 1 && lower_ascii(r.first("transfer-encoding")->value) == "chunked" && cl == 0)
                    ? "chunked-unqualified" : "invalid";
    return;
  }
  if (cl == 0) { r.framing = "close-delimited"; return; }
  long long v = -1;
  for (auto& f : r.fields)
    if (f.lower_name == "content-length") {
      long long x = 0;
      if (!parse_decimal(f.value, x) || (v >= 0 && x != v)) { r.framing = "invalid"; return; }
      v = x;
    }
  r.framing = "content-length";
  r.content_length = v;
}

}  // namespace raw_http_detail

// Sends exactly one validated GET and returns the raw receipt. `host_header`
// must be the value that was signed; `headers` are the caller's (already
// signed) fields. `deadline_s` bounds the whole exchange: numeric hosts skip
// the resolver, names are resolved on an abandonable helper thread, and every
// blocking socket call gets the remaining budget. A connect() interrupted by
// a signal abandons that address candidate. The target allowlist is a byte
// filter, not a URI parser (percent escapes are not validated).
inline RawResponse raw_http_get(const std::string& host, int port, const std::string& host_header,
                                const std::string& target, const HeaderList& headers,
                                size_t max_head = 64 * 1024, size_t max_body = 64 * 1024,
                                int deadline_s = 30) {
  using namespace raw_http_detail;
  RawResponse r;
  const std::string req = build_request(host_header, target, headers, r.token_bearing);  // throws before any I/O
  const auto deadline = Clock::now() + std::chrono::seconds(deadline_s);

  // Resolution under the same budget: numeric hosts never block; a name is
  // resolved on a helper thread that is abandoned (detached) if the deadline
  // passes, so the caller's bound holds even though getaddrinfo itself cannot
  // be interrupted.
  AddrList addrs;
  {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICHOST;
    const std::string service = std::to_string(port);
    addrinfo* numeric = nullptr;
    if (getaddrinfo(host.c_str(), service.c_str(), &hints, &numeric) == 0) {
      addrs = own_addrs(numeric);
    } else {
      // The task owns its result; if the caller gives up, the shared state's
      // last reference frees the list. The thread itself may outlive the call.
      auto task = std::make_shared<std::packaged_task<std::pair<int, AddrList>()>>([host, service] {
        addrinfo h{};
        h.ai_family = AF_UNSPEC;
        h.ai_socktype = SOCK_STREAM;
        addrinfo* out = nullptr;
        const int rc = getaddrinfo(host.c_str(), service.c_str(), &h, &out);
        return std::make_pair(rc, rc == 0 ? own_addrs(out) : AddrList());
      });
      auto fut = task->get_future();
      std::thread([task] { (*task)(); }).detach();
      if (fut.wait_until(deadline) != std::future_status::ready) {
        r.transport_error = true;
        r.error = "deadline exceeded during resolution (resolver task abandoned; it frees its own result)";
        return r;
      }
      auto res = fut.get();
      if (res.first != 0) { r.transport_error = true; r.error = std::string("getaddrinfo: ") + gai_strerror(res.first); return r; }
      addrs = std::move(res.second);
    }
  }
  Fd sock;
  for (addrinfo* a = addrs.get(); a != nullptr; a = a->ai_next) {
    Fd cand;
    cand.fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
    if (cand.fd < 0) continue;
#ifdef SO_NOSIGPIPE
    { int one = 1; if (setsockopt(cand.fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one) != 0) { r.error = "SO_NOSIGPIPE unavailable"; break; } }
#endif
    if (const Budget b = arm_remaining_timeout(cand.fd, deadline); b.state != BudgetState::Ready) { r.error = budget_error(b, "connect"); break; }
    if (connect(cand.fd, a->ai_addr, a->ai_addrlen) != 0) { r.error = std::string("connect: ") + std::strerror(errno); continue; }
    r.error.clear();
    sock.fd = cand.fd;
    cand.fd = -1;
    break;
  }
  if (sock.fd < 0) { r.transport_error = true; if (r.error.empty()) r.error = "no usable address"; return r; }

  int flags = 0;
#ifdef MSG_NOSIGNAL
  flags = MSG_NOSIGNAL;
#endif
  size_t off = 0;
  while (off < req.size()) {
    if (const Budget b = arm_remaining_timeout(sock.fd, deadline); b.state != BudgetState::Ready) { r.error = budget_error(b, "send"); break; }
    const ssize_t n = send_seam() ? send_seam()(sock.fd, req.data() + off, req.size() - off, flags)
                                  : send(sock.fd, req.data() + off, req.size() - off, flags);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) { r.error = io_error("send", errno, deadline, "send"); break; }
    off += static_cast<size_t>(n);
  }
  r.request_bytes = off;
  if (off < req.size()) {
    r.transport_error = true;
    r.request_partial = off > 0;
    return r;
  }
  r.request_sent = true;

  std::string buf;
  char tmp[8192];
  size_t head_end = std::string::npos;
  bool eof = false;
  while (head_end == std::string::npos && buf.size() < max_head && !eof) {
    if (const Budget b = arm_remaining_timeout(sock.fd, deadline); b.state != BudgetState::Ready) { r.error = budget_error(b, "receive"); r.transport_error = true; break; }
    const ssize_t n = recv(sock.fd, tmp, sizeof tmp, 0);
    if (n < 0 && errno == EINTR) continue;
    if (n < 0) { r.error = io_error("recv", errno, deadline, "receive"); r.transport_error = true; break; }
    if (n == 0) { eof = true; break; }
    buf.append(tmp, static_cast<size_t>(n));
    head_end = buf.find("\r\n\r\n");
  }
  if (head_end == std::string::npos || head_end + 4 > max_head) {
    r.head_bytes = buf.substr(0, max_head);  // bounded partial evidence
    if (!r.transport_error) {
      r.transport_error = true;
      r.error = head_end != std::string::npos || buf.size() >= max_head ? "response head exceeds cap"
                                                                        : "connection closed before end of head";
    }
    return r;
  }
  r.head_complete = true;
  r.head_bytes = buf.substr(0, head_end + 4);
  parse_head(r);
  if (r.head_malformed) return r;
  classify_framing(r);

  // Body octets (bounded) and message completion per framing.
  std::string body = buf.substr(head_end + 4);
  if (body.size() > max_body) { body.resize(max_body); r.body_truncated = true; }
  bool recv_error = false;
  while (!eof && !r.body_truncated) {
    if (r.framing == "content-length" && r.content_length >= 0 &&
        body.size() >= static_cast<size_t>(r.content_length)) break;
    if (const Budget b = arm_remaining_timeout(sock.fd, deadline); b.state != BudgetState::Ready) { r.error = budget_error(b, "body"); recv_error = true; break; }
    const ssize_t n = recv(sock.fd, tmp, sizeof tmp, 0);
    if (n < 0 && errno == EINTR) continue;
    if (n < 0) { r.error = io_error("recv", errno, deadline, "body"); recv_error = true; break; }
    if (n == 0) { eof = true; break; }
    body.append(tmp, static_cast<size_t>(n));
    if (body.size() > max_body) { body.resize(max_body); r.body_truncated = true; }
  }
  r.body_octets = body.size();
  if (recv_error) r.transport_error = true;
  if (r.framing == "content-length") {
    r.body_content = body.size();
    r.message_complete = !recv_error && !r.body_truncated && r.content_length >= 0 &&
                         body.size() == static_cast<size_t>(r.content_length);
  } else if (r.framing == "close-delimited") {
    r.body_content = body.size();
    r.message_complete = eof && !recv_error && !r.body_truncated;
  } else {
    r.message_complete = false;  // chunked (unqualified in this profile) or invalid framing
  }
  return r;
}

}  // namespace s3rdma
