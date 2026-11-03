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

// SDK-linked fixture loaded by cuobj_loader_harness after it has chosen the
// library load order. A C facade constructs and destroys one cuObjClient so
// the harness can observe the resulting mappings without guessing the C++
// constructor ABI.
#include <cuobjclient.h>

#include <sys/types.h>

namespace {
ssize_t noop_get(const void*, char*, size_t, loff_t, const cufileRDMAInfo_t*) { return -1; }
}  // namespace

extern "C" void* s3rdma_cuobj_create() {
  CUObjOps_t ops{};
  ops.get = &noop_get;
  ops.put = nullptr;
  try {
    return new cuObjClient(ops, CUOBJ_PROTO_RDMA_DC_V1);
  } catch (...) {
    return nullptr;
  }
}

extern "C" void s3rdma_cuobj_destroy(void* client) { delete static_cast<cuObjClient*>(client); }
