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

#include "s3_server.hpp"

#include <cerrno>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>
#include <thread>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <httplib.h>

#include "base64.hpp"
#include "cuobj_check.hpp"
#include "s3_util.hpp"

namespace s3rdma {

namespace {
constexpr size_t kAlign = 4096;  // O_DIRECT alignment (filesystem block size)

inline uint64_t align_down(uint64_t v, uint64_t a) { return v & ~(a - 1); }
inline uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

bool parse_u64_strict(const std::string& s, uint64_t& out) {
  try {
    size_t pos = 0;
    unsigned long long v = std::stoull(s, &pos);
    if (pos != s.size()) return false;
    out = static_cast<uint64_t>(v);
    return true;
  } catch (...) {
    return false;
  }
}

bool parse_size_strict(const std::string& s, size_t& out) {
  uint64_t v = 0;
  if (!parse_u64_strict(s, v) ||
      v > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
    return false;
  }
  out = static_cast<size_t>(v);
  return true;
}

// Full pread that retries on short reads (required for O_DIRECT with large sizes).
ssize_t pread_full(int fd, void* buf, size_t count, off_t offset) {
  size_t total = 0;
  auto* p = static_cast<char*>(buf);
  while (total < count) {
    ssize_t n = ::pread(fd, p + total, count - total, offset + total);
    if (n < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    if (n == 0) break;  // EOF
    total += static_cast<size_t>(n);
  }
  return static_cast<ssize_t>(total);
}

// Compute aligned pread parameters for a single chunk.
struct ChunkIO {
  uint64_t read_off;   // aligned file offset for pread
  size_t read_len;     // aligned length for pread
  uint64_t local_off;  // offset within staging buffer for RDMA
  size_t piece;        // actual data bytes in this chunk
};

ChunkIO plan_chunk(uint64_t file_off, uint64_t remaining, size_t chunk,
                   bool direct_io) {
  ChunkIO c;
  c.piece = (remaining > chunk) ? chunk : static_cast<size_t>(remaining);
  c.read_off = file_off;
  c.local_off = 0;
  c.read_len = c.piece;
  if (direct_io) {
    c.read_off = align_down(file_off, kAlign);
    c.local_off = file_off - c.read_off;
    c.read_len = align_up(c.piece + c.local_off, kAlign);
    if (c.read_len > chunk) c.read_len = chunk;
  }
  return c;
}
}  // namespace

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

S3Server::S3Server(Config cfg) : cfg_(std::move(cfg)) {
  LOG_INFO("starting cuObjServer RDMA data path on %s:%u "
           "(max_concurrency=%u, chunk=%zu KiB, direct_io=%s)",
           cfg_.ip.c_str(), cfg_.rdma_port, cfg_.max_concurrency,
           cfg_.chunk_size / 1024, cfg_.direct_io ? "on" : "off");
  cuObjServer::setupTelemetry(false, &std::cerr);
  cuObjServer::setTelemFlags(0x4);  // CUOBJ_LOG_PATH_ERROR only
  server_ = new cuObjServer(cfg_.ip.c_str(), cfg_.rdma_port,
                            CUOBJ_PROTO_RDMA_DC_V1);
  http_impl_ = new httplib::Server();
}

S3Server::~S3Server() {
  delete static_cast<httplib::Server*>(http_impl_);
  for (auto& [path, entry] : fd_cache_) {
    if (entry.fd_direct >= 0) ::close(entry.fd_direct);
  }
  if (server_) {
    for (auto& s : pool_) {
      for (int i = 0; i < 2; ++i)
        if (s.staging_reg[i]) server_->deRegisterBuffer(s.staging_reg[i]);
      server_->freeChannelId(s.channel);
    }
    delete server_;
  }
}

// ---------------------------------------------------------------------------
// Slot pool (dynamic: allocate on demand up to max_concurrency)
// ---------------------------------------------------------------------------

RdmaSlot S3Server::create_slot() {
  RdmaSlot s;
  s.channel = server_->allocateChannelId();
  for (int i = 0; i < 2; ++i) {
    s.staging[i] = server_->allocHostBuffer(cfg_.chunk_size);
    if (!s.staging[i]) {
      LOG_ERROR("allocHostBuffer(%zu) failed", cfg_.chunk_size);
      std::abort();
    }
    s.staging_reg[i] = server_->registerBuffer(s.staging[i], cfg_.chunk_size);
    if (!s.staging_reg[i]) {
      LOG_ERROR("registerBuffer failed");
      std::abort();
    }
  }
  return s;
}

RdmaSlot S3Server::acquire_slot() {
  std::unique_lock lk(pool_mu_);
  // Fast path: reuse a free slot.
  if (!pool_.empty()) {
    RdmaSlot s = pool_.back();
    pool_.pop_back();
    return s;
  }
  // Grow: allocate a new slot if under the limit.
  if (total_slots_ < cfg_.max_concurrency) {
    uint32_t id = total_slots_++;
    lk.unlock();
    RdmaSlot s;
    try {
      s = create_slot();
    } catch (...) {
      std::lock_guard guard(pool_mu_);
      --total_slots_;
      pool_cv_.notify_one();
      throw;
    }
    LOG_INFO("  new slot %u: channel=%u staging={%p,%p}",
             id, s.channel, s.staging[0], s.staging[1]);
    return s;
  }
  // At max: wait for a released slot.
  pool_cv_.wait(lk, [this] { return !pool_.empty(); });
  RdmaSlot s = pool_.back();
  pool_.pop_back();
  return s;
}

void S3Server::release_slot(RdmaSlot slot) {
  {
    std::lock_guard lk(pool_mu_);
    pool_.push_back(slot);
  }
  pool_cv_.notify_one();
}

// ---------------------------------------------------------------------------
// FD cache
// ---------------------------------------------------------------------------

int S3Server::get_fd(const std::string& path, uint64_t& file_size) {
  {
    std::shared_lock lk(fd_mu_);
    auto it = fd_cache_.find(path);
    if (it != fd_cache_.end()) {
      file_size = it->second.size;
      return it->second.fd_direct;
    }
  }
  std::unique_lock lk(fd_mu_);
  auto it = fd_cache_.find(path);
  if (it != fd_cache_.end()) {
    file_size = it->second.size;
    return it->second.fd_direct;
  }
  int flags = O_RDONLY;
  if (cfg_.direct_io) flags |= O_DIRECT;
  int fd = ::open(path.c_str(), flags);
  if (fd < 0) return -1;
  struct stat st{};
  if (::fstat(fd, &st) < 0) { ::close(fd); return -1; }
  FdEntry entry;
  entry.fd_direct = fd;
  entry.size = static_cast<uint64_t>(st.st_size);
  fd_cache_[path] = entry;
  file_size = entry.size;
  return fd;
}

// ---------------------------------------------------------------------------
// GET handler — double-buffer ping-pong
// ---------------------------------------------------------------------------

int S3Server::handle_get(const std::string& key, const std::string& range_hdr,
                         const std::string& rdma_token_b64, uint64_t remote_addr,
                         size_t req_chunk, ssize_t& bytes_out) {
  const std::string path = cfg_.data_dir + "/" + key;

  uint64_t file_size = 0;
  int fd = get_fd(path, file_size);
  if (fd < 0) {
    LOG_WARN("404 missing object: %s", path.c_str());
    return 404;
  }

  uint64_t offset = 0, length = file_size;
  bool is_range = false;
  if (!range_hdr.empty()) {
    ByteRange r = parse_range(range_hdr, file_size);
    if (!r.present || !r.valid) {
      LOG_WARN("416 invalid range '%s' (file_size=%lu)", range_hdr.c_str(),
               (unsigned long)file_size);
      return 416;
    }
    offset = r.start;
    length = r.end - r.start + 1;
    is_range = true;
  }

  const std::string rdma_descr = base64_decode(rdma_token_b64);
  RdmaSlot slot = acquire_slot();

  const size_t chunk = std::min(req_chunk, cfg_.chunk_size);
  if (chunk == 0) {
    LOG_ERROR("invalid zero chunk size");
    release_slot(slot);
    return 500;
  }
  uint64_t remaining = length;
  uint64_t file_off = offset;
  uint64_t remote_off = remote_addr;
  ssize_t total_written = 0;

  // Prefill buf[0] with first chunk.
  ChunkIO cur_c = plan_chunk(file_off, remaining, chunk, cfg_.direct_io);
  ssize_t nread = pread_full(fd, slot.staging[0], cur_c.read_len, cur_c.read_off);
  if (nread < 0 || static_cast<uint64_t>(nread) < cur_c.local_off + cur_c.piece) {
    LOG_ERROR("pread failed on %s (errno=%d: %s)", path.c_str(),
              errno, std::strerror(errno));
    release_slot(slot);
    return 500;
  }

  int cur = 0;  // current buffer index (ping-pong 0/1)
  while (remaining > 0) {
    int nxt = cur ^ 1;
    uint64_t next_file_off = file_off + cur_c.piece;
    uint64_t next_remaining = remaining - cur_c.piece;
    bool has_next = (next_remaining > 0);

    ssize_t rdma_n = -1;
    ibv_wc_status wc_status = IBV_WC_SUCCESS;
    bool pread_ok = true;
    ChunkIO next_c{};

    if (has_next) {
      next_c = plan_chunk(next_file_off, next_remaining, chunk, cfg_.direct_io);
      // Pipeline: RDMA write buf[cur] while pread into buf[nxt].
      std::thread rdma_t([&]() {
        rdma_n = server_->handleGetObject(key, slot.staging_reg[cur],
                                          remote_off, cur_c.piece, rdma_descr,
                                          slot.channel, cur_c.local_off,
                                          &wc_status);
      });
      ssize_t nr = pread_full(fd, slot.staging[nxt],
                               next_c.read_len, next_c.read_off);
      rdma_t.join();
      pread_ok = (nr >= 0 &&
                  static_cast<uint64_t>(nr) >= next_c.local_off + next_c.piece);
    } else {
      // Last chunk: RDMA only, no next pread.
      rdma_n = server_->handleGetObject(key, slot.staging_reg[cur],
                                        remote_off, cur_c.piece, rdma_descr,
                                        slot.channel, cur_c.local_off,
                                        &wc_status);
    }

    if (rdma_n < 0) {
      LOG_ERROR("handleGetObject failed for %s (rc=%zd, wc=%d:%s)",
                key.c_str(), rdma_n, (int)wc_status,
                ibv_wc_status_str(wc_status));
      release_slot(slot);
      return 500;
    }
    if (!pread_ok) {
      LOG_ERROR("pread failed on %s off=%lu", path.c_str(),
                (unsigned long)next_file_off);
      release_slot(slot);
      return 500;
    }

    total_written += rdma_n;
    remaining -= cur_c.piece;
    file_off += cur_c.piece;
    remote_off += cur_c.piece;
    cur = nxt;
    cur_c = next_c;
  }

  release_slot(slot);
  bytes_out = total_written;
  return is_range ? 206 : 200;
}

// ---------------------------------------------------------------------------
// HTTP server
// ---------------------------------------------------------------------------

void S3Server::run() {
  auto* svr = static_cast<httplib::Server*>(http_impl_);

  // Size the httplib thread pool generously; slots provide back-pressure.
  svr->new_task_queue = [this] {
    return new httplib::ThreadPool(
        std::min(cfg_.max_concurrency, (uint32_t)256));
  };

  svr->Get(".*", [this](const httplib::Request& req, httplib::Response& res) {
    ObjectPath p = parse_object_path(req.path);
    if (!p.valid) {
      res.status = 400;
      res.set_content("expected /<bucket>/<object>\n", "text/plain");
      return;
    }
    if (req.method == "HEAD") {
      const std::string fp = cfg_.data_dir + "/" + p.bucket + "/" + p.object;
      uint64_t sz = 0;
      if (get_fd(fp, sz) < 0) { res.status = 404; return; }
      res.status = 200;
      res.set_header("Content-Length", std::to_string(sz));
      return;
    }
    const std::string token = req.get_header_value(kHdrRdmaToken);
    if (token.empty()) {
      res.status = 400;
      res.set_content("missing RDMA token header\n", "text/plain");
      return;
    }
    uint64_t remote_addr = 0;
    if (!req.has_header(kHdrRemoteAddr) ||
        !parse_u64_strict(req.get_header_value(kHdrRemoteAddr), remote_addr)) {
      res.status = 400;
      res.set_content("invalid remote address header\n", "text/plain");
      return;
    }

    // Client-requested chunk size (capped by server's staging buffer).
    size_t req_chunk = cfg_.chunk_size;
    if (req.has_header(kHdrChunkSize)) {
      size_t parsed_chunk = 0;
      if (!parse_size_strict(req.get_header_value(kHdrChunkSize), parsed_chunk) ||
          parsed_chunk == 0) {
        res.status = 400;
        res.set_content("invalid chunk size header\n", "text/plain");
        return;
      }
      req_chunk = std::min(parsed_chunk, cfg_.chunk_size);
    }

    const std::string key = p.bucket + "/" + p.object;
    ssize_t bytes = 0;
    int status = handle_get(key, req.get_header_value("Range"), token,
                            remote_addr, req_chunk, bytes);
    res.status = status;
    if (status == 200 || status == 206) {
      res.set_header(kHdrBytes, std::to_string(bytes));
      res.set_content("OK\n", "text/plain");
    } else {
      res.set_content("error\n", "text/plain");
    }
  });

  auto reject = [](const httplib::Request&, httplib::Response& res) {
    res.status = 405;
    res.set_content("only GET is supported\n", "text/plain");
  };
  svr->Put(".*", reject);
  svr->Post(".*", reject);
  svr->Delete(".*", reject);

  LOG_INFO("fake-S3 HTTP control endpoint listening on 0.0.0.0:%u "
           "(data-dir=%s, max_concurrency=%u, chunk=%zu KiB)",
           cfg_.http_port, cfg_.data_dir.c_str(), cfg_.max_concurrency,
           cfg_.chunk_size / 1024);
  svr->listen("0.0.0.0", cfg_.http_port);
  LOG_INFO("HTTP server stopped");
}

void S3Server::stop() {
  if (http_impl_) static_cast<httplib::Server*>(http_impl_)->stop();
}

}  // namespace s3rdma
