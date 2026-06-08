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

// Fake-S3 server: an HTTP control endpoint (cpp-httplib) fronting the cuObject
// RDMA data path (cuObjServer). It serves GET / range-GET of files under a data
// directory, performing an RDMA_WRITE into the client's registered buffer.
//
// Concurrent requests are supported via a dynamically-growing pool of RdmaSlots
// (up to max_concurrency). Each slot has two staging buffers for ping-pong:
// pread into buf[A] while RDMA-writing buf[B], then swap.
#pragma once

#include <cstdint>
#include <condition_variable>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <cuobjserver.h>

namespace s3rdma {

// A pooled RDMA resource: one DCI channel + two ping-pong staging buffers.
struct RdmaSlot {
  uint16_t channel = 0;
  void* staging[2] = {nullptr, nullptr};
  struct rdma_buffer* staging_reg[2] = {nullptr, nullptr};
};

class S3Server {
 public:
  struct Config {
    std::string data_dir = "./data";
    std::string ip = "0.0.0.0";   // RDMA-capable local IP (cuObjServer)
    uint16_t rdma_port = 18515;   // RDMA control port for cuObjServer
    uint16_t http_port = 8080;    // fake-S3 HTTP control-path port
    uint32_t max_concurrency = 1024;  // max slots (allocated on demand)
    size_t chunk_size = 2ULL * 1024 * 1024;  // per-chunk RDMA write size
    bool direct_io = false;  // O_DIRECT: bypass page cache (better for cold data)
  };

  explicit S3Server(Config cfg);
  ~S3Server();

  // Blocks serving HTTP until stop() is called (e.g. from a signal handler).
  void run();
  void stop();

 private:
  // Handle one GET (full or range). Acquires a slot, transfers data in
  // chunk-sized ping-pong steps (pread + RDMA_WRITE overlapped), releases slot.
  int handle_get(const std::string& key, const std::string& range_hdr,
                 const std::string& rdma_token_b64, uint64_t remote_addr,
                 size_t req_chunk, ssize_t& bytes_out);

  // --- Slot pool (dynamic) ---
  RdmaSlot create_slot();              // allocate channel + 2 staging buffers
  RdmaSlot acquire_slot();             // get from pool or create on demand
  void release_slot(RdmaSlot slot);

  std::mutex pool_mu_;
  std::condition_variable pool_cv_;
  std::vector<RdmaSlot> pool_;         // free slots
  uint32_t total_slots_ = 0;           // total ever allocated

  // --- FD cache ---
  int get_fd(const std::string& path, uint64_t& file_size);

  std::shared_mutex fd_mu_;
  struct FdEntry { int fd_direct = -1; uint64_t size = 0; };
  std::unordered_map<std::string, FdEntry> fd_cache_;

  Config cfg_;
  cuObjServer* server_ = nullptr;     // RDMA data path
  void* http_impl_ = nullptr;         // httplib::Server (opaque to keep header light)
};

}  // namespace s3rdma
