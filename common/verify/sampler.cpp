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

#include "verify/sampler.hpp"

namespace s3rdma::verify {

namespace {

// splitmix64 finalizer: deterministic across platforms/compilers (pure uint64
// arithmetic), which keeps sampling decisions reproducible from the seed.
uint64_t mix64(uint64_t x) {
  x += 0x9e3779b97f4a7c15ULL;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  return x ^ (x >> 31);
}

}  // namespace

Sampler::Sampler(uint64_t seed, uint32_t every, uint32_t thread_id, uint32_t num_threads)
  : seed_(seed), every_(every), thread_id_(thread_id), num_threads_(num_threads) {}

bool Sampler::should_sample(uint32_t key_id, uint64_t occurrence) const {
  if (every_ == 0) return false;
  if (every_ == 1) return true;
  // One fixed residue per (thread, key): any N consecutive occurrences contain
  // exactly one sample, and keying the phase on key_id (not the global iter)
  // is what defeats the round-robin aliasing that starves keys under a naive
  // `iter % N`. Threads are de-phased by a stride derived from num_threads so
  // fewer threads than `every` spread across the window instead of clustering.
  const uint32_t threads = num_threads_ == 0 ? 1 : num_threads_;
  const uint32_t stride = every_ / threads == 0 ? 1 : every_ / threads;
  const uint64_t keyed = mix64(seed_ ^ (0x517cc1b727220a95ULL * (static_cast<uint64_t>(key_id) + 1)));
  // Reduce each term mod `every_` before combining so a 64-bit overflow in the
  // sum cannot shift the phase (2^64 % every_ is generally nonzero): both terms
  // are then < every_, the sum is < 2*every_, and the final residue is exact.
  const uint64_t keyed_phase = keyed % every_;
  const uint64_t thread_phase = (static_cast<uint64_t>(thread_id_) * stride) % every_;
  const uint64_t phase = (keyed_phase + thread_phase) % every_;
  return occurrence % every_ == phase;
}

}  // namespace s3rdma::verify
