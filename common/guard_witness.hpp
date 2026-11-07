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

// The G7 re-entry witness, as a pure decision over recorded callback
// attempts so the self-test can exercise it: the guarded second entry must be
// the library re-issuing the same logical request (same accepted size and
// offset) entered only after the injected first entry had left the probe
// (non-nested execution on the same OS thread, recorded coherently at entry)
// and strictly after its return-decision marker, with exactly one complete
// token-bearing message and no partial/unknown emission.
#pragma once

#include <string>
#include <thread>
#include <vector>

namespace s3rdma {

struct GuardAttempt {
  int invocation = 0;
  long long t_begin = 0, t_end = 0;  // t_end = pre-record return-decision marker, not the SDK's receipt of the return
  long long cb_size = 0, cb_offset = 0;
  bool injected = false;   // the natural result was replaced by the injected return
  bool guarded = false;    // returned -ENOTSUP without emission
  bool http_attempt = false;
  bool partial = false;
  bool entered_while_active = false;  // another callback of this context had not yet left the probe when this one entered
  std::thread::id thread;             // identity of the OS thread that ran the callback (compared for equality)
};

struct GuardWitness {
  bool ok = false;
  std::string why;
};

inline GuardWitness guard_witness(const std::vector<GuardAttempt>& a) {
  GuardWitness w;
  if (a.size() < 2) { w.why = "no re-entry"; return w; }
  const GuardAttempt& first = a[0];
  for (auto& x : a) if (x.entered_while_active) { w.why = "overlapping callback entries (including the first)"; return w; }
  if (!first.injected || first.guarded || !first.http_attempt) { w.why = "first entry was not an emitted, injected transfer"; return w; }
  int messages = 0;
  for (auto& x : a) { messages += x.http_attempt ? 1 : 0; if (x.partial) { w.why = "partial write present"; return w; } }
  if (messages != 1) { w.why = "message count != 1"; return w; }
  for (size_t i = 1; i < a.size(); ++i) {
    const GuardAttempt& later = a[i];
    if (!later.guarded) { w.why = "a later entry was not guarded"; return w; }
    // Return-before-re-entry: the earlier callback must have left the probe
    // (non-nested execution) and the later entry must be strictly after the
    // earlier return-decision marker; timestamps alone are not accepted.
    if (later.t_begin <= first.t_end) { w.why = "re-entry not strictly after the injected return marker"; return w; }
    // Return-before-re-entry proof: a single OS thread cannot enter the next
    // callback without having returned from the previous one; non-nested
    // entries on the same thread therefore establish that the SDK received
    // the injected return first. Different threads leave only tracked probe
    // intervals, which are not accepted as the primary witness.
    if (later.thread != first.thread) { w.why = "re-entry on a different thread: return order not provable from probe intervals"; return w; }
    if (later.cb_size != first.cb_size || later.cb_offset != first.cb_offset) { w.why = "re-entry shape differs from the original request"; return w; }
  }
  w.ok = true;
  return w;
}

}  // namespace s3rdma
