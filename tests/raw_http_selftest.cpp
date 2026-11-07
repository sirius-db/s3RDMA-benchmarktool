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

// Rig-free self-test for the raw receipt reader, the completion authority and
// the G7 re-entry witness. A loopback listener returns canned bytes; every
// check runs on the main thread after the server thread has been joined.
#include <arpa/inet.h>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "completion_authority.hpp"
#include "guard_witness.hpp"
#include "raw_http.hpp"

namespace {
int g_pass = 0, g_fail = 0;
void check(bool ok, const char* id, const char* what) {
  if (ok) ++g_pass; else { ++g_fail; std::printf("FAIL %s: %s\n", id, what); }
}

// Serves exactly one connection with the given bytes. Every phase is bounded
// (non-blocking listener polled against a budget, read/write timeouts on the
// accepted socket) so teardown never depends on the client under test. The
// worker's observations are published by join(); `served_all` means the
// intended response bytes were written completely. `rst_close` ends the
// connection with a reset instead of an orderly close.
struct OneShot {
  int lfd = -1, port = 0;
  bool setup_ok = false, rst_close = false;
  bool accept_ok = false, accept_timed_out = false, poll_error = false, request_ok = false, served_all = false;
  bool setup_failed = false, linger_ok = false;
  size_t served_bytes = 0;  // partial service witness for negative cases
  std::string response, request_seen;
  std::thread th;
  int trickle_ms = 0;
  explicit OneShot(std::string resp, bool rst = false, int trickle = 0) : rst_close(rst), response(std::move(resp)), trickle_ms(trickle) {
    lfd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (lfd < 0 || bind(lfd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || listen(lfd, 1) != 0) return;
    socklen_t len = sizeof a;
    if (getsockname(lfd, reinterpret_cast<sockaddr*>(&a), &len) != 0) return;
    const int lflags = fcntl(lfd, F_GETFL, 0);
    if (lflags < 0 || fcntl(lfd, F_SETFL, lflags | O_NONBLOCK) != 0) return;
    port = ntohs(a.sin_port);
    setup_ok = true;
    th = std::thread([this] {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
      int c = -1;
      while (c < 0) {  // readiness is not a reservation: retry until a connection is really accepted
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0) { accept_timed_out = true; return; }
        pollfd pfd{lfd, POLLIN, 0};
        const int pr = poll(&pfd, 1, static_cast<int>(left));
        if (pr < 0) { if (errno == EINTR) continue; poll_error = true; return; }
        if (pr == 0) { accept_timed_out = true; return; }
        c = accept(lfd, nullptr, nullptr);
        if (c < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR && errno != ECONNABORTED) { poll_error = true; return; }
      }
      accept_ok = true;
      // Every later phase runs against the same absolute deadline: a peer that
      // trickles bytes cannot keep the worker alive past it.
      auto arm = [&](int fd) {
        const auto left = std::chrono::duration_cast<std::chrono::microseconds>(deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) return false;
        timeval tv{static_cast<time_t>(left.count() / 1000000), static_cast<suseconds_t>(left.count() % 1000000)};
        return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) == 0 && setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv) == 0;
      };
      const int cflags = fcntl(c, F_GETFL, 0);
      if (cflags < 0 || fcntl(c, F_SETFL, cflags & ~O_NONBLOCK) != 0 || !arm(c)) { setup_failed = true; close(c); return; }
      char buf[8192];
      ssize_t n = 0;
      while (request_seen.size() < 65536 && arm(c) &&
             (n = recv(c, buf, std::min(sizeof buf, 65536 - request_seen.size()), 0)) > 0) {  // prefix bound is exact
        request_seen.append(buf, static_cast<size_t>(n));
        if (request_seen.find("\r\n\r\n") != std::string::npos) break;
      }
      request_ok = request_seen.find("\r\n\r\n") != std::string::npos;
      size_t off = 0;
      while (off < response.size()) {
        if (trickle_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(trickle_ms));  // controlled slow peer
        if (!arm(c)) break;  // the budget gate follows the delay: every send starts inside the absolute budget
        const ssize_t w = send(c, response.data() + off, trickle_ms > 0 ? 1 : response.size() - off, 0);
        if (w <= 0) break;
        off += static_cast<size_t>(w);
      }
      served_bytes = off;
      served_all = off == response.size();
      if (rst_close) { linger lg{1, 0}; linger_ok = setsockopt(c, SOL_SOCKET, SO_LINGER, &lg, sizeof lg) == 0; }
      close(c);
    });
  }
  const std::string& join() { if (th.joinable()) th.join(); return request_seen; }
  bool delivered() { join(); return setup_ok && accept_ok && !setup_failed && request_ok && served_all; }
  ~OneShot() { join(); if (lfd >= 0) close(lfd); }
};

const s3rdma::HeaderList kHeaders{{"x-amz-rdma-token", "7f:1:2"}, {"Range", "bytes=0-7"}};

s3rdma::RawResponse fetch(OneShot& s, size_t max_head = 65536, size_t max_body = 65536,
                          const s3rdma::HeaderList& h = kHeaders, const std::string& target = "/b/k") {
  return s3rdma::raw_http_get("127.0.0.1", s.port, "example:8080", target, h, max_head, max_body, 5);
}
// Fetch, then require that the fixture really delivered its bytes before any
// negative assertion is read as protocol evidence.
s3rdma::RawResponse fetch_delivered(OneShot& s, const char* id, size_t max_head = 65536, size_t max_body = 65536) {
  auto r = fetch(s, max_head, max_body);
  check(s.delivered(), id, "fixture accepted, read the request and served the whole response");
  return r;
}

s3rdma::ProtocolOptions standard() {
  s3rdma::ProtocolOptions o;
  o.token_encoding = s3rdma::TokenEncoding::Verbatim;
  return o;
}
bool authority(const s3rdma::RawResponse& r, size_t expected = 8, size_t requested = 8, bool legacy = false) {
  s3rdma::ProtocolOptions o = legacy ? s3rdma::ProtocolOptions{} : standard();
  return s3rdma::completion_authority(s3rdma::view_receipt(r, requested), o, expected, requested);
}
std::string head(const char* status, const char* extra = "") {
  return std::string("HTTP/1.1 ") + status + "\r\nx-amz-rdma-reply: 206\r\nx-amz-rdma-bytes-transferred: 8\r\n" + extra + "\r\n";
}
}  // namespace

int main() {
  std::signal(SIGPIPE, SIG_IGN);
  {  // R1 exact request bytes and exact head preservation
    const std::string h = head("206 Partial Content", "Content-Range: bytes 0-7/16\r\nContent-Length: 0\r\n");
    OneShot s(h);
    check(s.setup_ok, "R1", "fixture listening");
    auto r = fetch(s);
    const std::string& req = s.join();
    check(s.delivered(), "R1", "fixture accepted, read the request and served the whole response");
    check(req == "GET /b/k HTTP/1.1\r\nHost: example:8080\r\nx-amz-rdma-token: 7f:1:2\r\nRange: bytes=0-7\r\nConnection: close\r\n\r\n",
          "R1", "the whole request matches the supplied target/Host/headers plus Connection: close");
    check(r.request_sent && !r.request_partial && r.token_bearing, "R1", "one complete token-bearing message");
    check(r.head_complete && !r.head_malformed && r.status == 206 && r.head_bytes == h, "R1", "head preserved exactly");
    check(r.framing == "content-length" && r.message_complete && r.body_content == 0, "R1", "empty content-length body is a complete message");
    check(authority(r), "R1", "authority accepts the proper standard receipt");
    check(!authority(r, 8, 8, true), "R1", "legacy convention rejects reply 206");
  }
  {  // R2 percent escapes are not decoded and cannot qualify
    OneShot s("HTTP/1.1 206 OK\r\nx-amz-rdma-reply: %32%30%36\r\nx-amz-rdma-bytes-transferred: 8\r\nContent-Length: 0\r\n\r\n");
    auto r = fetch_delivered(s, "fixture");
    const auto* f = r.first("x-amz-rdma-reply");
    check(f != nullptr && f->value == "%32%30%36" && !authority(r), "R2", "encoded reply stays encoded and is refused");
  }
  {  // R3 duplicate reply / duplicate count fields are visible and refused
    OneShot s("HTTP/1.1 206 OK\r\nx-amz-rdma-reply: 206\r\nX-AMZ-RDMA-REPLY: 501\r\nx-amz-rdma-bytes-transferred: 8\r\nContent-Length: 0\r\n\r\n");
    auto r = fetch_delivered(s, "fixture");
    check(r.count("x-amz-rdma-reply") == 2 && !authority(r), "R3", "duplicate reply refused");
    OneShot s2("HTTP/1.1 206 OK\r\nx-amz-rdma-reply: 206\r\nx-amz-rdma-bytes-transferred: 8\r\nx-amz-rdma-bytes-transferred: 0\r\nContent-Length: 0\r\n\r\n");
    auto r2 = fetch_delivered(s2, "fixture");
    check(r2.count("x-amz-rdma-bytes-transferred") == 2 && !authority(r2), "R3", "duplicate count refused");
  }
  {  // R4 count spelling / bounds
    for (const char* v : {"+8", "08x", "", "99999999999999999999999", "9"}) {
      OneShot s(std::string("HTTP/1.1 206 OK\r\nx-amz-rdma-reply: 206\r\nx-amz-rdma-bytes-transferred: ") + v + "\r\nContent-Length: 0\r\n\r\n");
      auto r = fetch_delivered(s, "fixture");
      check(!authority(r), "R4", "bad count spelling or value refused");
    }
  }
  {  // R5 grammar: obs-fold, LF-only, space in name, HTTP/1.x, DEL in reason
    OneShot a("HTTP/1.1 206 OK\r\nx-amz-rdma-reply: 2\r\n 06\r\n\r\n"); auto ra = fetch_delivered(a, "fixture");
    check(ra.head_complete && ra.head_malformed && !authority(ra), "R5", "obs-fold malformed");
    OneShot b("HTTP/1.1 206 OK\nx-amz-rdma-reply: 206\n\n"); auto rb = fetch_delivered(b, "fixture");
    check(!rb.head_complete && rb.transport_error && !authority(rb), "R5", "LF-only never completes");
    OneShot c("HTTP/1.1 206 OK\r\nBad Name: x\r\n\r\n"); auto rc = fetch_delivered(c, "fixture");
    check(rc.head_malformed, "R5", "space in field name malformed");
    std::string vx = head("206 OK", "Content-Length: 0\r\n");
    vx[7] = 'x';  // only the minor-version byte differs from a qualifying response
    OneShot d(vx); auto rd = fetch_delivered(d, "fixture");
    check(rd.head_malformed && !authority(rd), "R5", "HTTP/1.x refused (isolated version byte)");
    OneShot e(std::string("HTTP/1.1 206 O\x7fK\r\nx-amz-rdma-reply: 206\r\nx-amz-rdma-bytes-transferred: 8\r\nContent-Length: 0\r\n\r\n")); auto re = fetch_delivered(e, "fixture");
    check(re.head_malformed && !authority(re), "R5", "DEL in reason refused");
    OneShot f2("HTTP/1.1 206 \r\nx-amz-rdma-reply: 206\r\nx-amz-rdma-bytes-transferred: 8\r\nContent-Length: 0\r\n\r\n"); auto rf = fetch_delivered(f2, "fixture");
    check(!rf.head_malformed && rf.reason.empty() && authority(rf), "R5", "empty reason phrase is valid");
  }
  {  // R6 framing: declared length not satisfied; chunked complete/incomplete; close-delimited EOF; invalid framing
    OneShot a(head("206 OK", "Content-Length: 8\r\n")); auto ra = fetch_delivered(a, "fixture");
    check(ra.head_complete && !ra.message_complete && !authority(ra), "R6", "missing declared body is incomplete, not empty");
    OneShot b(head("206 OK", "Transfer-Encoding: chunked\r\n") + "0\r\n\r\n"); auto rb = fetch_delivered(b, "fixture");
    check(rb.framing == "chunked-unqualified" && !rb.message_complete && !authority(rb), "R6", "chunked is recorded but never qualifies");
    OneShot b2(head("206 OK", "Transfer-Encoding: chunked\r\n") + "0;\r\n\r\n"); auto rb2 = fetch_delivered(b2, "fixture");
    check(!rb2.message_complete && !authority(rb2), "R6", "malformed chunk extension cannot qualify");
    OneShot b3(head("206 OK", "Transfer-Encoding: chunked\r\n") + "0\r\nnot-a-field\r\n\r\n"); auto rb3 = fetch_delivered(b3, "fixture");
    check(!rb3.message_complete && !authority(rb3), "R6", "malformed trailer cannot qualify");
    OneShot c(head("206 OK", "Transfer-Encoding: chunked\r\nContent-Length: 0\r\n")); auto rc = fetch_delivered(c, "fixture");
    check(rc.framing == "invalid" && !authority(rc), "R6", "TE plus CL is invalid framing");
    OneShot c2(head("206 OK", "Content-Length: 0\r\nContent-Length: 0\r\n")); auto rc2 = fetch_delivered(c2, "fixture");
    check(rc2.framing == "content-length" && rc2.message_complete && authority(rc2), "R6", "identical repeated lengths are accepted");
    OneShot d(head("206 OK")); auto rd = fetch_delivered(d, "fixture");
    check(rd.framing == "close-delimited" && rd.message_complete && rd.body_content == 0 && authority(rd), "R6", "close-delimited empty body completes at EOF");
    OneShot e(head("206 OK", "Content-Length: 0\r\nContent-Length: 3\r\n")); auto re = fetch_delivered(e, "fixture");
    check(re.framing == "invalid" && !re.message_complete && !authority(re), "R6", "conflicting lengths are invalid framing");
    OneShot f(head("206 OK", "Content-Length: 3\r\n") + "abc"); auto rf = fetch_delivered(f, "fixture");
    check(rf.message_complete && rf.body_content == 3 && !authority(rf), "R6", "standard contract refuses a nonempty body");
    OneShot g("HTTP/1.1 206 OK\r\nx-amz-rdma-reply: 200\r\nx-amz-rdma-bytes-transferred: 8\r\nContent-Length: 3\r\n\r\nOK\n"); auto rg = fetch_delivered(g, "fixture");
    check(authority(rg, 8, 8, true), "R6", "legacy convention accepts its bounded body");
  }
  {  // R7 caps: head terminator inside/at/beyond max_head; body cap-1/cap/cap+1
    const std::string h = head("206 OK", "Content-Length: 0\r\n");
    OneShot a(h); auto ra = fetch(a, h.size(), 65536); a.join();
    check(ra.head_complete && authority(ra), "R7", "head exactly at cap is complete");
    OneShot b(h); auto rb = fetch(b, h.size() - 1, 65536); b.join();  // client stops early by design; delivery is not required here
    check(!rb.head_complete && rb.error == "response head exceeds cap" && rb.head_bytes.size() <= h.size() - 1 && !authority(rb), "R7", "head beyond cap refused, prefix bounded");
    for (size_t n : {size_t(1023), size_t(1024), size_t(1025)}) {
      OneShot c(head("206 OK", ("Content-Length: " + std::to_string(n) + "\r\n").c_str()) + std::string(n, 'x'));
      auto rc = fetch(c, 65536, 1024); c.join();
      const bool expect_complete = n <= 1024;
      check(rc.message_complete == expect_complete && rc.body_truncated == !expect_complete && rc.body_octets <= 1024,
            "R7", "body cap boundary");
    }
  }
  {  // R8 request validation: nothing is written for invalid inputs
    OneShot s(head("206 OK", "Content-Length: 0\r\n"));
    bool threw = false;
    try { fetch(s, 65536, 65536, kHeaders, "/b/a key"); } catch (const std::invalid_argument&) { threw = true; }
    check(threw, "R8", "space in the request target is refused before I/O");
    threw = false;
    try { fetch(s, 65536, 65536, {{"x-amz-rdma-token", "7f\r\nInjected: 1"}}); } catch (const std::invalid_argument&) { threw = true; }
    check(threw, "R8", "CRLF in a field value is refused");
    threw = false;
    try { fetch(s, 65536, 65536, {{"Host", "evil"}}); } catch (const std::invalid_argument&) { threw = true; }
    check(threw, "R8", "a second Host is refused");
    auto r = fetch(s, 65536, 65536, {{"Range", "bytes=0-7"}}); s.join();
    check(r.request_sent && !r.token_bearing, "R8", "a token-less request is sent but not token-bearing");
  }
  {  // R9 connect refusal while a bound, non-listening socket reserves the port
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const bool bound = fd >= 0 && bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0;
    socklen_t len = sizeof a;
    const bool named = bound && getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len) == 0;
    check(named, "R9", "reserved a bound, non-listening port");
    const int port = ntohs(a.sin_port);
    auto r = s3rdma::raw_http_get("127.0.0.1", port, "h", "/", kHeaders, 4096, 4096, 2);  // the port stays reserved
    if (fd >= 0) close(fd);
    check(r.transport_error && !r.request_sent && !r.request_partial && r.request_bytes == 0, "R9", "refused connection emits nothing");
  }
  {  // R10 G7 re-entry witness
    using s3rdma::GuardAttempt;
    const std::thread::id me = std::this_thread::get_id();
    std::thread::id other;
    std::thread probe_thread([&] { other = std::this_thread::get_id(); });
    probe_thread.join();
    std::vector<GuardAttempt> good{{0, 100, 200, 8, 0, true, false, true, false, false, me}, {1, 300, 310, 8, 0, false, true, false, false, false, me}};
    check(s3rdma::guard_witness(good).ok, "R10", "ordered same-shape guarded re-entry is a witness");
    auto early = good; early[1].t_begin = 150;
    check(!s3rdma::guard_witness(early).ok, "R10", "re-entry before the injected return marker is refused");
    auto equal = good; equal[1].t_begin = 200;
    check(!s3rdma::guard_witness(equal).ok, "R10", "equal markers are refused");
    auto nested = good; nested[1].entered_while_active = true;
    check(!s3rdma::guard_witness(nested).ok, "R10", "entry while the first callback was still active is refused even with a later timestamp");
    auto first_active = good; first_active[0].entered_while_active = true;  // the injected entry itself overlapped
    check(!s3rdma::guard_witness(first_active).ok, "R10", "overlap recorded on the first (injected) entry is refused");
    auto other_thread = good; other_thread[1].thread = other;  // a real distinct identity, not a different hash
    check(!s3rdma::guard_witness(other_thread).ok, "R10", "re-entry on another thread is not a return-order proof");
    auto shape = good; shape[1].cb_size = 4;
    check(!s3rdma::guard_witness(shape).ok, "R10", "a different shape is refused");
    auto two = good; two[1].guarded = false; two[1].http_attempt = true;
    check(!s3rdma::guard_witness(two).ok, "R10", "a second message is refused");
    auto partial = good; partial[0].partial = true;
    check(!s3rdma::guard_witness(partial).ok, "R10", "a partial write is refused");
    check(!s3rdma::guard_witness({good[0]}).ok, "R10", "no re-entry is not a witness");
  }
  {  // R11 a reset is a receive error, not the EOF that completes a close-delimited message
    OneShot s(head("206 OK"), true);
    auto r = fetch_delivered(s, "R11");
    check(s.linger_ok, "R11", "fixture installed the reset linger");
    check(r.head_complete && r.framing == "close-delimited" && r.transport_error && !r.message_complete && !authority(r),
          "R11", "RST after the head is a receive error and never completes a close-delimited receipt");
  }
  {  // R12 writer seam: partial write then error; slow one-byte progress
    OneShot s(head("206 OK", "Content-Length: 0\r\n"));
    size_t written = 0;
    s3rdma::raw_http_detail::send_seam() = [&](int fd, const void* b, size_t n, int fl) -> ssize_t {
      if (written >= 10) { errno = EPIPE; return -1; }
      const size_t take = std::min<size_t>(10 - written, n);
      const ssize_t w = send(fd, b, take, fl);
      if (w > 0) written += static_cast<size_t>(w);
      return w;
    };
    auto r = fetch(s);
    s3rdma::raw_http_detail::send_seam() = nullptr;
    s.join();
    check(r.request_partial && !r.request_sent && r.request_bytes == 10 && r.transport_error && !r.head_complete,
          "R12", "a write that stops after 10 bytes is a partial (possibly emitted) message, never a complete one");
    OneShot s2(head("206 OK", "Content-Length: 0\r\n"));
    s3rdma::raw_http_detail::send_seam() = [](int fd, const void* b, size_t, int fl) -> ssize_t { return send(fd, b, 1, fl); };
    auto r2 = fetch_delivered(s2, "R12");
    s3rdma::raw_http_detail::send_seam() = nullptr;
    check(r2.request_sent && !r2.request_partial && authority(r2), "R12", "one-byte successful writes still yield one complete message and a qualified receipt");
    {  // the total budget is not renewed by progress: 1 byte per call, each call sleeping toward a 1 s deadline
      OneShot s3(head("206 OK", "Content-Length: 0\r\n"));
      s3rdma::raw_http_detail::send_seam() = [](int fd, const void* b, size_t, int fl) -> ssize_t {
        const ssize_t w = send(fd, b, 1, fl);
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        return w;
      };
      auto r3 = s3rdma::raw_http_get("127.0.0.1", s3.port, "example:8080", "/b/k", kHeaders, 65536, 65536, 1);
      s3rdma::raw_http_detail::send_seam() = nullptr;
      s3.join();
      check(r3.transport_error && r3.request_partial && !r3.request_sent && r3.request_bytes >= 1 &&
                r3.error.rfind("deadline exceeded during send", 0) == 0,
            "R12", "send progress does not renew the total budget; the write ends partial with a deadline error");
    }
    {  // EINTR once, then progress: the writer retries at the same offset and the fixture sees the whole request
      OneShot s4(head("206 OK", "Content-Length: 0\r\n"));
      bool interrupted = false;
      s3rdma::raw_http_detail::send_seam() = [&](int fd, const void* b, size_t n, int fl) -> ssize_t {
        if (!interrupted) { interrupted = true; errno = EINTR; return -1; }
        return send(fd, b, n, fl);
      };
      auto r4 = fetch_delivered(s4, "R12");
      s3rdma::raw_http_detail::send_seam() = nullptr;
      check(interrupted && r4.request_sent && !r4.request_partial && authority(r4) &&
                s4.request_seen == "GET /b/k HTTP/1.1\r\nHost: example:8080\r\nx-amz-rdma-token: 7f:1:2\r\nRange: bytes=0-7\r\nConnection: close\r\n\r\n",
            "R12", "an interrupted write is retried at the same offset; the whole request arrives once, one complete message");
    }
    {  // receive-side slow peer: the head trickles one byte per 100 ms against a 1 s budget;
       // the oracle requires real progress (several received bytes) and exhaustion at the helper's return
      const std::string h5 = head("206 OK", "Content-Length: 0\r\n");
      OneShot s5(h5, false, 100);
      const auto t0 = std::chrono::steady_clock::now();
      auto r5 = s3rdma::raw_http_get("127.0.0.1", s5.port, "example:8080", "/b/k", kHeaders, 65536, 65536, 1);
      const auto elapsed = std::chrono::steady_clock::now() - t0;
      s5.join();
      check(s5.accept_ok && s5.request_ok && s5.served_bytes >= 3 && !s5.served_all, "R12", "fixture served a partial head (progress happened)");
      check(r5.request_sent && r5.transport_error && !r5.head_complete && !authority(r5) &&
                r5.head_bytes.size() >= 3 && h5.compare(0, r5.head_bytes.size(), r5.head_bytes) == 0 &&
                elapsed >= std::chrono::milliseconds(900) && elapsed < std::chrono::seconds(4) &&
                r5.error.find("deadline exceeded") != std::string::npos,
            "R12", "repeated receive progress does not renew the budget; the retained prefix is the expected head prefix");
    }
  }
  std::printf("RAW_HTTP SELFTEST %s %d/%d\n", g_fail ? "FAIL" : "PASS", g_pass, g_pass + g_fail);
  return g_fail ? 1 : 0;
}
