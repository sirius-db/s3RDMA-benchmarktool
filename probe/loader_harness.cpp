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

// cuobj_loader_harness — which libcufile does a process end up with when the
// cuObject client and an explicit dlopen(libcufile.so.0, RTLD_NODELETE) are
// loaded in either order? Built without linking either SDK library so that the
// load order is really decided here. Receipts: the initial mappings (neither
// SDK loaded), each load step with its link-map name, cuFileDriverOpen's typed
// status, one client construction through an SDK-linked fixture, and the
// dev/inode identity of every libcufile*/libcuobj* mapping taken from
// /proc/self/maps itself. This covers only the loader sub-item.

#include <dlfcn.h>
#include <link.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <tuple>

#include <cufile.h>

#include "jsonl.hpp"

namespace {

using Rec = s3rdma::JsonRec;

struct Mapping {
  std::string path;
  unsigned long long major = 0, minor = 0, ino = 0;
  bool parsed = false;
};
using Identity = std::tuple<unsigned long long, unsigned long long, unsigned long long>;

std::string basename_of(const std::string& p) {
  const auto s = p.rfind('/');
  return s == std::string::npos ? p : p.substr(s + 1);
}

bool is_cufile_main(const std::string& base) {
  return base == "libcufile.so" || base.rfind("libcufile.so.", 0) == 0;
}
bool interesting(const std::string& base) {
  return base.rfind("libcufile", 0) == 0 || base.rfind("libcuobj", 0) == 0;
}

// /proc/self/maps: address perms offset dev inode path
std::map<std::string, Mapping> read_maps(bool& ok) {
  std::map<std::string, Mapping> out;
  std::ifstream maps("/proc/self/maps");
  ok = static_cast<bool>(maps);
  std::string line;
  while (std::getline(maps, line)) {
    std::istringstream ss(line);
    std::string addr, perms, offset, dev, inode, path;
    if (!(ss >> addr >> perms >> offset >> dev >> inode)) continue;
    std::getline(ss, path);
    const auto first = path.find('/');
    if (first == std::string::npos) continue;
    path = path.substr(first);
    if (!interesting(basename_of(path))) continue;
    Mapping m;
    m.path = path;
    const auto colon = dev.find(':');
    // Parse from owned, live strings; every field must be nonempty and fully consumed.
    auto parse_u64 = [](const std::string& s, int base, unsigned long long& out) {
      if (s.empty()) return false;
      char* end = nullptr;
      errno = 0;
      out = std::strtoull(s.c_str(), &end, base);
      return errno == 0 && end != nullptr && *end == '\0';
    };
    const std::string major_s = colon == std::string::npos ? "" : dev.substr(0, colon);
    const std::string minor_s = colon == std::string::npos ? "" : dev.substr(colon + 1);
    m.parsed = colon != std::string::npos && parse_u64(major_s, 16, m.major) &&
               parse_u64(minor_s, 16, m.minor) && parse_u64(inode, 10, m.ino);
    if (!m.parsed) ok = false;  // an unparsed candidate cannot qualify the snapshot
    // One entry per mapped file identity; several segments of one file collapse,
    // two files behind one pathname do not.
    const std::string key = path + "#" + std::to_string(m.major) + ":" + std::to_string(m.minor) + ":" + std::to_string(m.ino);
    out.emplace(key, m);
  }
  return out;
}

struct Snapshot {
  bool ok = false;
  std::map<std::string, Mapping> maps;
};

// Collects one snapshot, emits exactly that snapshot and returns it so the
// gate is applied to what was recorded.
Snapshot emit_maps(const char* phase) {
  Snapshot snap;
  snap.maps = read_maps(snap.ok);
  const auto& maps = snap.maps;
  const bool ok = snap.ok;
  Rec("mappings").str("phase", phase).boolean("maps_readable", ok).num("count", static_cast<long long>(maps.size())).emit();
  for (const auto& [key, m] : maps)
    Rec("mapping").str("phase", phase).str("path", m.path).str("basename", basename_of(m.path))
        .boolean("cufile_main", is_cufile_main(basename_of(m.path))).boolean("parsed", m.parsed)
        .num("dev_major", static_cast<long long>(m.major)).num("dev_minor", static_cast<long long>(m.minor))
        .num("ino", static_cast<long long>(m.ino)).emit();
  return snap;
}

// Associate a link-map pathname with a recorded mapping; symbol origin remains unqualified.
bool identity_for_linkmap(const Snapshot& snap, const std::string& linkmap_name, Identity& out) {
  for (const auto& [key, m] : snap.maps)
    if (m.parsed && m.path == linkmap_name) { out = Identity{m.major, m.minor, m.ino}; return true; }
  char buf[4096];
  if (::realpath(linkmap_name.c_str(), buf) == nullptr) return false;
  const std::string real = buf;
  for (const auto& [key, m] : snap.maps)
    if (m.parsed && m.path == real) { out = Identity{m.major, m.minor, m.ino}; return true; }
  return false;
}

void* open_or_report(const char* name, int flags, const char* label) {
  dlerror();
  void* h = dlopen(name, flags);
  const char* err = h ? nullptr : dlerror();
  const std::string error = err ? err : "";
  std::string linkmap_name;
  if (h) {
    struct link_map* lm = nullptr;
    if (dlinfo(h, RTLD_DI_LINKMAP, &lm) == 0 && lm && lm->l_name) linkmap_name = lm->l_name;
  }
  Rec("dlopen").str("label", label).str("name", name).boolean("ok", h != nullptr).str("error", error)
      .str("linkmap_name", linkmap_name).emit();
  return h;
}

}  // namespace

int main(int argc, char** argv) {
  std::string order = "cufile-first", fixture = "./build/probe/libcuobj_loader_fixture.so";
  bool open_driver = true;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--order" && i + 1 < argc) order = argv[++i];
    else if (a == "--fixture" && i + 1 < argc) fixture = argv[++i];
    else if (a == "--no-driver-open") open_driver = false;
    else {
      std::fprintf(stderr, "Usage: %s [--order cufile-first|cuobj-first] [--fixture <libcuobj_loader_fixture.so>] [--no-driver-open]\n", argv[0]);
      return 2;
    }
  }
  if (order != "cufile-first" && order != "cuobj-first") return 2;

  const Snapshot initial = emit_maps("initial");
  const bool initial_clean = initial.ok && initial.maps.empty();  // neither SDK may be loaded before the chosen order
  const int cufile_flags = RTLD_NOW | RTLD_GLOBAL | RTLD_NODELETE;
  void* cufile = nullptr;
  void* cuobj = nullptr;
  auto open_cuobj = [&]() {
    void* h = open_or_report("libcuobjclient.so.1", RTLD_NOW, "cuobjclient");
    return h ? h : open_or_report("libcuobjclient.so", RTLD_NOW, "cuobjclient-unversioned");
  };
  if (order == "cufile-first") {
    cufile = open_or_report("libcufile.so.0", cufile_flags, "cufile");
    emit_maps("after-first-load");
    cuobj = open_cuobj();
  } else {
    cuobj = open_cuobj();
    emit_maps("after-first-load");
    cufile = open_or_report("libcufile.so.0", cufile_flags, "cufile");
  }
  emit_maps("after-both-loads");

  bool driver_ok = !open_driver;
  if (open_driver && cufile) {
    using open_fn = decltype(&::cuFileDriverOpen);
    dlerror();
    auto fn = reinterpret_cast<open_fn>(dlsym(cufile, "cuFileDriverOpen"));
    const char* symbol_error = dlerror();
    if (symbol_error != nullptr || fn == nullptr) {
      Rec("driver_open").boolean("symbol_found", false).str("error", symbol_error ? symbol_error : "").emit();
    } else {
      Rec("driver_open_begin").emit();
      const CUfileError_t status = fn();
      driver_ok = status.err == CU_FILE_SUCCESS;
      Rec r("driver_open");
      r.boolean("symbol_found", true).num("err", static_cast<long long>(status.err));
      if (status.err == CU_FILE_CUDA_DRIVER_ERROR) r.num("cu_err", static_cast<long long>(status.cu_err)); else r.null("cu_err");
      r.emit();
    }
  }

  // One client through the SDK-linked fixture (no ABI guessing), observed
  // while it is alive, then destroyed through the same facade.
  bool construct_ok = false;
  Snapshot live;
  bool live_taken = false;
  if (cufile && cuobj) {
    void* fx = open_or_report(fixture.c_str(), RTLD_NOW, "fixture");
    if (fx) {
      dlerror();
      auto create = reinterpret_cast<void* (*)()>(dlsym(fx, "s3rdma_cuobj_create"));
      const char* d1 = dlerror();
      const std::string e1 = d1 ? d1 : "";  // copied before the next loader call
      auto destroy = reinterpret_cast<void (*)(void*)>(dlsym(fx, "s3rdma_cuobj_destroy"));
      const char* d2 = dlerror();
      const std::string e2 = d2 ? d2 : "";
      if (e1.empty() && e2.empty() && create && destroy) {
        void* client = create();
        construct_ok = client != nullptr;
        Rec("construct").boolean("symbol_found", true).boolean("ok", construct_ok).emit();
        live = emit_maps("with-client-alive");
        live_taken = true;
        destroy(client);
      } else {
        Rec("construct").boolean("symbol_found", false).str("error", !e1.empty() ? e1 : e2).emit();
      }
    }
  }
  emit_maps("after-destroy");

  // Correlate the handle actually bound as libcufile.so.0 with the mapped
  // identities: its link-map name must be one of the recorded cuFile paths.
  std::string cufile_linkmap;
  if (cufile) {
    struct link_map* lm = nullptr;
    if (dlinfo(cufile, RTLD_DI_LINKMAP, &lm) == 0 && lm && lm->l_name) cufile_linkmap = lm->l_name;
  }
  // The G10 gate is applied to the snapshot taken while the client was alive.
  std::set<Identity> cufile_ids;
  bool all_parsed = live_taken && live.ok;
  for (const auto& [key, m] : live.maps) {
    all_parsed &= m.parsed;
    if (is_cufile_main(basename_of(m.path))) cufile_ids.insert(Identity{m.major, m.minor, m.ino});
  }
  Identity handle_id{};
  const bool handle_resolved = live_taken && !cufile_linkmap.empty() && identity_for_linkmap(live, cufile_linkmap, handle_id);
  const bool handle_matches = handle_resolved && cufile_ids.count(handle_id) == 1;
  Rec("cufile_binding").str("linkmap_name", cufile_linkmap).boolean("handle_identity_resolved", handle_resolved)
      .boolean("handle_identity_in_live_set", handle_matches)
      .boolean("symbol_origin_available", false).emit();  // no symbol-origin receipt yet: evidence stays partial
  const bool env_ok = all_parsed && initial_clean && cufile && cuobj && driver_ok && construct_ok && handle_matches;
  // Without a symbol-origin receipt the single-identity result is PARTIAL, not PASS.
  const char* verdict = !env_ok ? "ENV_ERROR" : cufile_ids.size() == 1 ? "PARTIAL" : cufile_ids.empty() ? "ENV_ERROR" : "FAIL";
  Rec("result").str("order", order).num("libcufile_identities", static_cast<long long>(cufile_ids.size()))
      .boolean("initial_clean", initial_clean).boolean("driver_ok", driver_ok).boolean("construct_ok", construct_ok)
      .str("verdict", verdict)
      .str("scope", "loader sub-item only").emit();
  if (s3rdma::jsonl_output_failed()) return 3;  // an incomplete receipt never qualifies
  return !env_ok || cufile_ids.empty() ? 3 : cufile_ids.size() == 1 ? 4 : 1;  // 4 = partial (INCONCLUSIVE)
}
