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

// Client wrapper around cuObjClient. The cuObject get callback issues the HTTP
// request to the fake-S3 server (relaying the RDMA descriptor token); the
// server RDMA_WRITEs the object bytes into our registered buffer.
//
// The cuObjClient constructor sets up the cuFile RDMA path and requires GDS +
// a Mellanox NIC, so functional GET runs happen on the RDMA node (build/link
// only on hosts without RDMA hardware). See doc/design.md.
#pragma once

#include <cstdint>
#include <string>

#include <cuobjclient.h>

namespace s3rdma {

// Per-request context recovered inside the get callback via cuObjClient::getCtx.
struct GetContext {
  std::string host;
  int port = 8080;
  std::string path;    // "/<bucket>/<object>"
  std::string range;   // "bytes=a-b" or empty
};

class S3Client {
 public:
  S3Client(std::string host, int port);
  ~S3Client();

  // Register buffer, perform GET (full if range empty), deregister.
  // Returns bytes transferred (negative on failure).
  ssize_t get(void* ptr, size_t size, const std::string& bucket,
              const std::string& object, const std::string& range);

  // Pre-register / deregister a buffer for repeated GETs.
  void register_buffer(void* ptr, size_t size);
  void deregister_buffer(void* ptr);

  // GET into an already-registered buffer (skips register/deregister).
  ssize_t get_registered(void* ptr, size_t size, const std::string& bucket,
                         const std::string& object, const std::string& range);

 private:
  std::string host_;
  int port_;
  CUObjOps_t ops_{};
  cuObjClient* client_ = nullptr;
};

}  // namespace s3rdma
