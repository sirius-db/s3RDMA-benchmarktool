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

// Aliasing-free sampling decision for online GET verification. SDK-free (std
// only). A naive `iter % N` beats against the benchmark's `objs[iter % num_objs]`
// round-robin: when gcd(N, num_objs) > 1 some keys are never sampled and every
// thread samples iteration 0 in phase. This keys the decision on the PER-KEY
// occurrence counter plus a deterministic per-thread phase offset, so each key
// is sampled ~1-in-N per thread and threads are de-phased.
#pragma once

#include <cstdint>

namespace s3rdma::verify {

class Sampler {
 public:
  // every == 0 disables sampling (should_sample always returns false).
  // seed makes the per-thread/per-key phase reproducible across runs.
  Sampler(uint64_t seed, uint32_t every, uint32_t thread_id, uint32_t num_threads);

  // occurrence = 0-based count of GETs THIS thread has issued for THIS key.
  // Contract: every==1 -> always true; every==N>1 -> exactly one of every N
  // consecutive occurrences per (thread, key); no key is starved regardless of
  // num_objs; two threads do not sample the same occurrences of a shared key in
  // phase. The decision depends only on the constructor inputs plus (key_id,
  // occurrence), so it is pure and reproducible.
  bool should_sample(uint32_t key_id, uint64_t occurrence) const;

 private:
  uint64_t seed_;
  uint32_t every_;
  uint32_t thread_id_;
  uint32_t num_threads_;
};

}  // namespace s3rdma::verify
