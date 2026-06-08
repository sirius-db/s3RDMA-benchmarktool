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

#include "buffers.hpp"

#include <cstring>

#include <cuda_runtime.h>

#include "cuobj_check.hpp"

namespace s3rdma {

BufferMode parse_mode(const std::string& s) {
  if (s == "gpu") return BufferMode::Gpu;
  return BufferMode::Host;  // default
}

Buffer::Buffer(BufferMode mode, size_t size) : mode_(mode), size_(size) {
  if (mode_ == BufferMode::Gpu) {
    CUDA_CHECK(cudaMalloc(&ptr_, size_));
  } else {
    // Pinned host memory: required for RDMA registration and faster transfers.
    CUDA_CHECK(cudaMallocHost(&ptr_, size_));
  }
}

Buffer::~Buffer() {
  if (!ptr_) return;
  if (mode_ == BufferMode::Gpu) cudaFree(ptr_);
  else cudaFreeHost(ptr_);
}

std::vector<char> Buffer::to_host(size_t n) const {
  std::vector<char> out(n);
  if (mode_ == BufferMode::Gpu) {
    CUDA_CHECK(cudaMemcpy(out.data(), ptr_, n, cudaMemcpyDeviceToHost));
  } else {
    std::memcpy(out.data(), ptr_, n);
  }
  return out;
}

}  // namespace s3rdma
