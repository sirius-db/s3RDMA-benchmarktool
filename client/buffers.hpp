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

// Destination buffer abstraction for the client: either pinned host memory
// (cudaMallocHost) or GPU device memory (cudaMalloc). Both are RDMA targets the
// server writes into; GPU mode exercises the GPUDirect Storage path.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace s3rdma {

enum class BufferMode { Host, Gpu };

BufferMode parse_mode(const std::string& s);  // "host" | "gpu"

class Buffer {
 public:
  Buffer(BufferMode mode, size_t size);
  ~Buffer();

  Buffer(const Buffer&) = delete;
  Buffer& operator=(const Buffer&) = delete;

  void* data() const { return ptr_; }
  size_t size() const { return size_; }
  BufferMode mode() const { return mode_; }

  // Copy [0, n) of the buffer into host memory (device->host copy for GPU mode).
  std::vector<char> to_host(size_t n) const;

 private:
  BufferMode mode_;
  size_t size_ = 0;
  void* ptr_ = nullptr;
};

}  // namespace s3rdma
