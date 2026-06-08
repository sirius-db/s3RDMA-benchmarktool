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

// Shared helpers for the S3-over-RDMA demo: lightweight logging plus
// error-check macros for CUDA runtime and cuObject (cuObjErr_t) calls.
//
// These are intentionally tiny — they log a contextual message and abort on
// failure so the client/server code stays readable. See doc/design.md.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <cuda_runtime.h>
// NOTE: cuObjErr_t / CU_OBJ_SUCCESS are defined by BOTH cuobjclient.h and
// cuobjserver.h (with no cross-guard). Each translation unit must include the
// appropriate cuObject header *before* this one; we intentionally do not
// include either here to avoid duplicate-definition errors on the server.

// ---------------------------------------------------------------------------
// Logging (macros live at global scope; call them unqualified)
// ---------------------------------------------------------------------------
#define LOG_INFO(fmt, ...) \
  std::fprintf(stderr, "[INFO]  " fmt "\n", ##__VA_ARGS__)
#define LOG_WARN(fmt, ...) \
  std::fprintf(stderr, "[WARN]  " fmt "\n", ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) \
  std::fprintf(stderr, "[ERROR] %s:%d: " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__)

// ---------------------------------------------------------------------------
// CUDA runtime error checking
// ---------------------------------------------------------------------------
#define CUDA_CHECK(call)                                                    \
  do {                                                                      \
    cudaError_t err_ = (call);                                              \
    if (err_ != cudaSuccess) {                                             \
      LOG_ERROR("CUDA error in %s: %s", #call, cudaGetErrorString(err_));   \
      std::abort();                                                         \
    }                                                                       \
  } while (0)

// ---------------------------------------------------------------------------
// cuObject error checking (API returns cuObjErr_t; CU_OBJ_SUCCESS == 0)
// ---------------------------------------------------------------------------
#define CUOBJ_CHECK(call)                                                   \
  do {                                                                      \
    cuObjErr_t err_ = (call);                                               \
    if (err_ != CU_OBJ_SUCCESS) {                                           \
      LOG_ERROR("cuObject error in %s (code %d)", #call, (int)err_);        \
      std::abort();                                                         \
    }                                                                       \
  } while (0)
