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

// Fake-S3 / cuObjServer demo server.
//
// Serves GET / range-GET of files under --data-dir over the cuObject RDMA data
// path, fronted by an HTTP control endpoint. The cuObjServer RDMA session
// requires a Mellanox NIC, so this runs on the RDMA node (build/link only on
// a host without RDMA hardware). See doc/design.md.
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>

#include "cuobj_check.hpp"
#include "s3_server.hpp"

namespace {

s3rdma::S3Server* g_server = nullptr;

void on_signal(int sig) {
  LOG_INFO("received signal %d, shutting down", sig);
  if (g_server) g_server->stop();
}

void print_usage(const char* argv0) {
  std::printf(
      "Usage: %s [options]\n"
      "  --data-dir <path>   Directory served as the object store (default: ./data)\n"
      "  --ip <addr>         RDMA-capable local IP for cuObjServer (default: 0.0.0.0)\n"
      "  --rdma-port <port>  cuObjServer RDMA control port (default: 18515)\n"
      "  --port <port>       Fake-S3 HTTP control-path port (default: 8080)\n"
      "  --max-concurrency <N>  Max RDMA slots, allocated on demand (default: 1024)\n"
      "  --chunk-size <KiB>  Per-chunk RDMA write size in KiB (default: 2048)\n"
      "  --direct-io         Use O_DIRECT (bypass page cache; default: off)\n"
      "  -h, --help          Show this help\n",
      argv0);
}

bool parse_u16(const std::string& s, uint16_t& out) {
  try {
    size_t pos = 0;
    unsigned long v = std::stoul(s, &pos);
    if (pos != s.size() || v == 0 || v > std::numeric_limits<uint16_t>::max()) return false;
    out = static_cast<uint16_t>(v);
    return true;
  } catch (...) {
    return false;
  }
}

bool parse_u32(const std::string& s, uint32_t& out) {
  try {
    size_t pos = 0;
    unsigned long v = std::stoul(s, &pos);
    if (pos != s.size() || v == 0 || v > std::numeric_limits<uint32_t>::max()) return false;
    out = static_cast<uint32_t>(v);
    return true;
  } catch (...) {
    return false;
  }
}

bool parse_size_kib(const std::string& s, size_t& out) {
  try {
    size_t pos = 0;
    unsigned long long v = std::stoull(s, &pos);
    if (pos != s.size() || v == 0) return false;
    if (v > std::numeric_limits<size_t>::max() / 1024ULL) return false;
    out = static_cast<size_t>(v) * 1024ULL;
    return true;
  } catch (...) {
    return false;
  }
}

}  // namespace

int main(int argc, char** argv) {
  s3rdma::S3Server::Config cfg;

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&](const char* name) -> const char* {
      if (i + 1 >= argc) { LOG_ERROR("missing value for %s", name); std::exit(2); }
      return argv[++i];
    };
    if (a == "--data-dir") cfg.data_dir = next("--data-dir");
    else if (a == "--ip") cfg.ip = next("--ip");
    else if (a == "--rdma-port") {
      if (!parse_u16(next("--rdma-port"), cfg.rdma_port)) {
        LOG_ERROR("--rdma-port must be 1..65535");
        return 2;
      }
    }
    else if (a == "--port") {
      if (!parse_u16(next("--port"), cfg.http_port)) {
        LOG_ERROR("--port must be 1..65535");
        return 2;
      }
    }
    else if (a == "--max-concurrency") {
      if (!parse_u32(next("--max-concurrency"), cfg.max_concurrency)) {
        LOG_ERROR("--max-concurrency must be a positive integer");
        return 2;
      }
    }
    else if (a == "--chunk-size") {
      if (!parse_size_kib(next("--chunk-size"), cfg.chunk_size)) {
        LOG_ERROR("--chunk-size must be a positive integer KiB value");
        return 2;
      }
    }
    else if (a == "--direct-io") cfg.direct_io = true;
    else if (a == "-h" || a == "--help") { print_usage(argv[0]); return 0; }
    else { LOG_ERROR("unknown argument: %s", a.c_str()); print_usage(argv[0]); return 2; }
  }

  s3rdma::S3Server server(cfg);
  g_server = &server;
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  server.run();
  g_server = nullptr;
  return 0;
}
