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

// cuObjClient demo client / benchmark.
//
// Fetches objects from the fake-S3 server over the cuObject RDMA data path.
// With --threads N and --duration S, each thread loops GETs for S seconds into
// pre-registered buffers, round-robin across the object list, reporting
// sustained aggregate BW.
//
// The cuObjClient RDMA path needs GDS + a Mellanox NIC, so functional runs
// happen on the RDMA node (build/link only on hosts without RDMA hardware). See doc/design.md.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <httplib.h>

#include "buffers.hpp"
#include "cuobj_check.hpp"
#include "s3_client.hpp"
#include "s3_util.hpp"
#include "sha256.hpp"

namespace {

struct Args {
  std::string host = "127.0.0.1";
  int port = 8080;
  std::string bucket, out, mode = "host";
  std::vector<std::string> objects;  // one or more object keys
  int threads = 1;
  int duration = 10;  // seconds
  size_t range_size = 0;  // 0 = full object; >0 = range read of this many bytes
};

void print_usage(const char* argv0) {
  std::printf(
      "Usage: %s [options]\n"
      "  --server <host:port>  Fake-S3 control endpoint (default: 127.0.0.1:8080)\n"
      "  --bucket <name>       Bucket name (required)\n"
      "  --object <key>        Object key (required; repeatable)\n"
      "  --objects <k1,k2,...>  Comma-separated object keys (round-robin)\n"
      "  --mode <host|gpu>     Destination buffer memory type (default: host)\n"
      "  --threads <N>         Concurrent GET threads (default: 1)\n"
      "  --duration <secs>     Sustained benchmark duration (default: 10)\n"
      "  --range-size <size>   Range-read size per GET, e.g. 1m, 16m (default: full object)\n"
      "  --out <path>          Dump last GET to file (single-thread only)\n"
      "  -h, --help            Show this help\n",
      argv0);
}

// Split a comma-separated string.
std::vector<std::string> split_csv(const std::string& s) {
  std::vector<std::string> out;
  std::istringstream ss(s);
  std::string tok;
  while (std::getline(ss, tok, ','))
    if (!tok.empty()) out.push_back(tok);
  return out;
}

// Parse a size string like "4m", "16M", "1024k", "1048576" into bytes.
bool parse_size(const std::string& s, size_t& out) {
  if (s.empty()) return false;
  try {
    size_t pos = 0;
    unsigned long long v = std::stoull(s, &pos);
    if (pos < s.size()) {
      char suffix = std::tolower(s[pos]);
      if (suffix == 'k') v *= 1024ULL;
      else if (suffix == 'm') v *= 1024ULL * 1024ULL;
      else if (suffix == 'g') v *= 1024ULL * 1024ULL * 1024ULL;
      else return false;
      ++pos;
    }
    if (pos != s.size() || v == 0) return false;
    out = static_cast<size_t>(v);
    return true;
  } catch (...) {
    return false;
  }
}

bool parse_int(const std::string& s, int& out) {
  try {
    size_t pos = 0;
    long v = std::stol(s, &pos);
    if (pos != s.size() || v < 0 || v > std::numeric_limits<int>::max()) return false;
    out = static_cast<int>(v);
    return true;
  } catch (...) {
    return false;
  }
}

// Query the object size via HEAD.
ssize_t head_size(const std::string& host, int port, const std::string& path) {
  httplib::Client cli(host, port);
  auto res = cli.Head(path);
  if (!res || res->status != 200) return -1;
  if (!res->has_header("Content-Length")) return -1;
  try {
    return std::stoll(res->get_header_value("Content-Length"));
  } catch (...) {
    return -1;
  }
}

// Object descriptor: key + size (from HEAD).
struct ObjDesc {
  std::string key;
  size_t size = 0;
};

// Per-thread result.
struct ThreadResult {
  int64_t total_bytes = 0;
  int64_t iters = 0;
  double secs = 0;
  bool ok = true;
};

}  // namespace

// Parse command-line arguments. Returns false on --help.
bool parse_args(int argc, char** argv, Args& out) {
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&](const char* name) -> const char* {
      if (i + 1 >= argc) { LOG_ERROR("missing value for %s", name); std::exit(2); }
      return argv[++i];
    };
    if (a == "--server") {
      std::string s = next("--server");
      auto c = s.find(':');
      if (c == std::string::npos) { LOG_ERROR("--server must be host:port"); return false; }
      out.host = s.substr(0, c);
      if (!parse_int(s.substr(c + 1), out.port) || out.port <= 0 || out.port > 65535) {
        LOG_ERROR("--server port must be 1..65535");
        return false;
      }
    } else if (a == "--bucket") out.bucket = next("--bucket");
    else if (a == "--object") out.objects.push_back(next("--object"));
    else if (a == "--objects") {
      auto v = split_csv(next("--objects"));
      out.objects.insert(out.objects.end(), v.begin(), v.end());
    }
    else if (a == "--mode") out.mode = next("--mode");
    else if (a == "--out") out.out = next("--out");
    else if (a == "--threads") {
      if (!parse_int(next("--threads"), out.threads) || out.threads < 1) {
        LOG_ERROR("--threads must be >= 1");
        return false;
      }
    }
    else if (a == "--duration") {
      if (!parse_int(next("--duration"), out.duration) || out.duration < 1) {
        LOG_ERROR("--duration must be >= 1");
        return false;
      }
    }
    else if (a == "--range-size") {
      if (!parse_size(next("--range-size"), out.range_size)) {
        LOG_ERROR("--range-size must be a positive size (e.g. 1m, 16m)");
        return false;
      }
    }
    else if (a == "-h" || a == "--help") { print_usage(argv[0]); return false; }
    else { LOG_ERROR("unknown argument: %s", a.c_str()); print_usage(argv[0]); return false; }
  }
  if (out.bucket.empty() || out.objects.empty()) {
    LOG_ERROR("--bucket and at least one --object/--objects required");
    print_usage(argv[0]);
    return false;
  }
  if (out.threads < 1) out.threads = 1;
  return true;
}

// HEAD each object to discover sizes. Returns {obj_descs, max_size}.
std::pair<std::vector<ObjDesc>, size_t> discover_objects(
    const std::string& host, int port, const std::string& bucket,
    const std::vector<std::string>& keys) {
  std::vector<ObjDesc> objs;
  size_t max_size = 0;
  for (auto& key : keys) {
    std::string p = "/" + bucket + "/" + key;
    ssize_t sz = head_size(host, port, p);
    if (sz <= 0) { LOG_ERROR("HEAD %s failed", p.c_str()); std::exit(1); }
    objs.push_back({key, static_cast<size_t>(sz)});
    max_size = std::max(max_size, static_cast<size_t>(sz));
  }
  return {objs, max_size};
}

// Allocate buffers, create clients, pre-register. Returns {results, bufs, clients, alloc_ms}.
std::tuple<std::vector<ThreadResult>,
           std::vector<std::unique_ptr<s3rdma::Buffer>>,
           std::vector<std::unique_ptr<s3rdma::S3Client>>,
           double>
setup_buffers_clients(s3rdma::BufferMode mode, size_t max_size, int N,
                      const std::string& host, int port,
                      const std::string& mode_str) {
  std::vector<ThreadResult> results(N);
  std::vector<std::unique_ptr<s3rdma::Buffer>> bufs(N);
  std::vector<std::unique_ptr<s3rdma::S3Client>> clients(N);

  auto alloc_t0 = std::chrono::steady_clock::now();
  for (int t = 0; t < N; ++t) {
    bufs[t] = std::make_unique<s3rdma::Buffer>(mode, max_size);
    clients[t] = std::make_unique<s3rdma::S3Client>(host, port);
    clients[t]->register_buffer(bufs[t]->data(), max_size);
  }
  auto alloc_t1 = std::chrono::steady_clock::now();
  double alloc_ms = std::chrono::duration<double, std::milli>(alloc_t1 - alloc_t0).count();
  LOG_INFO("alloc+register: %d x %zu MiB (%s) in %.1f ms",
           N, max_size / (1024 * 1024), mode_str.c_str(), alloc_ms);

  return {std::move(results), std::move(bufs), std::move(clients), alloc_ms};
}

// Run the sustained benchmark. Returns wall time in seconds.
double run_benchmark(std::vector<std::unique_ptr<s3rdma::S3Client>>& clients,
                     std::vector<std::unique_ptr<s3rdma::Buffer>>& bufs,
                     const std::vector<ObjDesc>& objs,
                     const std::string& bucket,
                     std::vector<ThreadResult>& results,
                     int N, int dur_secs, size_t range_size) {
  const size_t num_objs = objs.size();
  std::atomic<int> ready{0};
  std::atomic<bool> go{false};
  std::atomic<bool> stop{false};

  auto worker = [&](int tid) {
    ready.fetch_add(1);
    while (!go.load(std::memory_order_acquire)) {}

    ThreadResult& r = results[tid];
    auto t0 = std::chrono::steady_clock::now();
    int64_t iter = 0;

    while (!stop.load(std::memory_order_relaxed)) {
      auto& obj = objs[iter % num_objs];
      size_t xfer_size = (range_size > 0) ? std::min(range_size, obj.size) : obj.size;
      std::string range_hdr;
      if (range_size > 0)
        range_hdr = "bytes=0-" + std::to_string(xfer_size - 1);
      ssize_t n = clients[tid]->get_registered(bufs[tid]->data(), xfer_size,
                                                bucket, obj.key, range_hdr);
      if (n < 0) { r.ok = false; break; }
      r.total_bytes += n;
      r.iters = ++iter;
    }

    auto t1 = std::chrono::steady_clock::now();
    r.secs = std::chrono::duration<double>(t1 - t0).count();
  };

  std::vector<std::thread> threads;
  threads.reserve(N);
  for (int t = 0; t < N; ++t)
    threads.emplace_back(worker, t);

  while (ready.load() < N) {}
  LOG_INFO("all threads ready, starting %ds benchmark...", dur_secs);
  auto wall_t0 = std::chrono::steady_clock::now();
  go.store(true, std::memory_order_release);

  std::this_thread::sleep_for(std::chrono::seconds(dur_secs));
  stop.store(true, std::memory_order_relaxed);

  for (auto& t : threads) t.join();
  auto wall_t1 = std::chrono::steady_clock::now();

  return std::chrono::duration<double>(wall_t1 - wall_t0).count();
}

// Print per-thread and aggregate results.
void report_results(const std::vector<ThreadResult>& results, double wall_secs,
                   int N, const std::string& mode, size_t num_objs) {
  int64_t grand_total = 0;
  int64_t grand_iters = 0;
  for (int t = 0; t < N; ++t) {
    auto& r = results[t];
    if (!r.ok) continue;
    double mibps = (r.total_bytes / (1024.0 * 1024.0)) / (r.secs > 0 ? r.secs : 1e-9);
    LOG_INFO("  thread %2d: %ld iters, %.0f MiB in %.3f s (%.1f MiB/s)",
             t, (long)r.iters, r.total_bytes / (1024.0 * 1024.0), r.secs, mibps);
    grand_total += r.total_bytes;
    grand_iters += r.iters;
  }

  double agg_mibps = (grand_total / (1024.0 * 1024.0)) / (wall_secs > 0 ? wall_secs : 1e-9);
  double agg_gbps = agg_mibps * 8.0 / 1024.0;

  LOG_INFO("--- aggregate (threads=%d, mode=%s, duration=%.1fs, objects=%zu) ---",
           N, mode.c_str(), wall_secs, num_objs);
  LOG_INFO("  total iters: %ld", (long)grand_iters);
  LOG_INFO("  total data:  %.0f MiB", grand_total / (1024.0 * 1024.0));
  LOG_INFO("  aggregate:   %.1f MiB/s  (%.1f Gbps)", agg_mibps, agg_gbps);
}

// Deregister all buffers.
void cleanup(std::vector<std::unique_ptr<s3rdma::S3Client>>& clients,
             std::vector<std::unique_ptr<s3rdma::Buffer>>& bufs, int N) {
  for (int t = 0; t < N; ++t)
    clients[t]->deregister_buffer(bufs[t]->data());
}

int main(int argc, char** argv) {
  Args args;
  if (!parse_args(argc, argv, args)) return 0;

  auto [objs, max_size] = discover_objects(args.host, args.port, args.bucket, args.objects);

  s3rdma::BufferMode mode = s3rdma::parse_mode(args.mode);
  const int N = args.threads;
  const int dur_secs = args.duration;
  const size_t num_objs = objs.size();

  LOG_INFO("benchmark: %zu objects in /%s, buf=%zu MiB, mode=%s, threads=%d, "
           "duration=%ds",
           num_objs, args.bucket.c_str(), max_size / (1024 * 1024),
           args.mode.c_str(), N, dur_secs);
  for (size_t i = 0; i < num_objs; ++i)
    LOG_INFO("  obj[%zu]: %s (%zu MiB)", i, objs[i].key.c_str(),
             objs[i].size / (1024 * 1024));

  auto [results, bufs, clients, alloc_ms] =
      setup_buffers_clients(mode, max_size, N, args.host, args.port,
                            args.mode);

  const size_t range_size = args.range_size;
  if (range_size > 0)
    LOG_INFO("range-read mode: %zu KiB per GET", range_size / 1024);

  // Warm-up: one GET per object to prime server page cache.
  for (size_t i = 0; i < num_objs; ++i) {
    size_t wsz = (range_size > 0) ? std::min(range_size, objs[i].size) : objs[i].size;
    std::string whdr;
    if (range_size > 0) whdr = "bytes=0-" + std::to_string(wsz - 1);
    clients[0]->get_registered(bufs[0]->data(), wsz,
                                args.bucket, objs[i].key, whdr);
  }

  double wall_secs = run_benchmark(clients, bufs, objs, args.bucket, results, N, dur_secs, range_size);
  report_results(results, wall_secs, N, args.mode, num_objs);
  cleanup(clients, bufs, N);

  bool any_fail = false;
  for (auto& r : results) if (!r.ok) any_fail = true;
  if (any_fail) { LOG_ERROR("one or more threads failed"); return 1; }

  // Optional dump (single-thread only).
  if (!args.out.empty() && N == 1 && results[0].total_bytes > 0) {
    auto& last_obj = objs[(results[0].iters - 1) % num_objs];
    std::vector<char> host_copy = bufs[0]->to_host(last_obj.size);
    std::ofstream o(args.out, std::ios::binary);
    o.write(host_copy.data(), host_copy.size());
    LOG_INFO("wrote %zu bytes to %s", host_copy.size(), args.out.c_str());
  }
  return 0;
}
