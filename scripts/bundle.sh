#!/usr/bin/env bash
# Copyright 2025, Sirius Contributors.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Build self-contained deploy bundles for the client and server nodes.
#
# cuObject libs are taken from CUOBJ_SDK_ROOT if set (an unpacked SDK tree, e.g.
# the version-aligned 1.0.0), else from the system/pixi install.
#
# Version alignment (see doc/test.md):
#   - client: cuObject 1.0.0 pairs with cufile 1.16 (cuda-13.1). The node only
#     ships cufile 1.15, whose object API is incompatible with cuObject, so we
#     bundle the matched cufile 1.16 (+ libcufile_rdma) from third_party/ and
#     load the bundle libs FIRST. cudart comes from the node's CUDA install.
#   - the server node (no CUDA): bundle libcuobjserver + a cufile (for the NEEDED
#     soname) + libnuma/libstdc++/libgcc. RDMA libs come from the node's OFED.
# Requires on the client node: nvidia_peermem loaded + rdma_dev_addr_list set in
# cufile.json (see doc/test.md).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ENVLIB="${ROOT}/.pixi/envs/default/lib"
DIST="${ROOT}/dist"
CUFILE116="${ROOT}/third_party/cufile-1.16"  # matched cufile for the client
# Node CUDA lib dir (provides libcudart at runtime; appended after the bundle).
CLIENT_CUDA_LIB="${CLIENT_CUDA_LIB:-/usr/local/cuda/targets/x86_64-linux/lib}"

[ -x "${ROOT}/build/server/server" ] || { echo "build first: pixi run build" >&2; exit 1; }

# Resolve cuObject libs (prefer CUOBJ_SDK_ROOT).
find_cuobj() {  # $1 = cuobjclient|cuobjserver
  local name="lib$1.so.1"
  if [ -n "${CUOBJ_SDK_ROOT:-}" ]; then
    find "${CUOBJ_SDK_ROOT}" -name "${name}" 2>/dev/null | head -1 && return
  fi
  find /usr/local/cuda-*/targets/x86_64-linux/lib /usr/lib/x86_64-linux-gnu \
       "${ENVLIB}" -name "${name}" 2>/dev/null | head -1
}
CLIENT_SO="$(find_cuobj cuobjclient)"; SERVER_SO="$(find_cuobj cuobjserver)"
[ -n "${CLIENT_SO}" ] && [ -n "${SERVER_SO}" ] || { echo "cuObject libs not found" >&2; exit 1; }
echo "client cuObject lib: ${CLIENT_SO}"
echo "server cuObject lib: ${SERVER_SO}"

rm -rf "${DIST}"
copy_so() { cp -Lf "$2" "$1/$(basename "$2")"; }

# ---- server bundle (server node, no CUDA/GPU) ----------------------------
mkdir -p "${DIST}/server/bin" "${DIST}/server/lib"
cp -f "${ROOT}/build/server/server" "${DIST}/server/bin/"
copy_so "${DIST}/server/lib" "${SERVER_SO}"
for so in libcufile.so.0 libnuma.so.1 libstdc++.so.6 libgcc_s.so.1; do
  copy_so "${DIST}/server/lib" "${ENVLIB}/${so}"
done
cat > "${DIST}/server/run.sh" <<'EOF'
#!/usr/bin/env bash
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export LD_LIBRARY_PATH="${DIR}/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
exec "${DIR}/bin/server" "$@"
EOF
chmod +x "${DIST}/server/run.sh"
cp -a "${ROOT}/data" "${DIST}/server/data" 2>/dev/null || \
  echo "WARN: no data/ — run 'pixi run gen-data' first" >&2

# ---- client bundle (Ubuntu 24.04, A100; uses node's cufile/cudart) -------
mkdir -p "${DIST}/client/bin" "${DIST}/client/lib"
cp -f "${ROOT}/build/client/client" "${DIST}/client/bin/"
copy_so "${DIST}/client/lib" "${CLIENT_SO}"
for so in libcrypto.so.3 libstdc++.so.6 libgcc_s.so.1; do
  copy_so "${DIST}/client/lib" "${ENVLIB}/${so}"
done
# Matched cufile 1.16 (+ RDMA plugin) — must pair with cuObject 1.0.0.
[ -f "${CUFILE116}/libcufile.so.0" ] || { echo "missing ${CUFILE116}/libcufile.so.0" >&2; exit 1; }
copy_so "${DIST}/client/lib" "${CUFILE116}/libcufile.so.0"
copy_so "${DIST}/client/lib" "${CUFILE116}/libcufile_rdma.so.1"
# Launcher: bundle libs FIRST (so the matched cufile 1.16 wins over the node's
# 1.15), then the node CUDA dir for libcudart.
cat > "${DIST}/client/run.sh" <<EOF
#!/usr/bin/env bash
DIR="\$(cd "\$(dirname "\${BASH_SOURCE[0]}")" && pwd)"
export LD_LIBRARY_PATH="\${DIR}/lib:${CLIENT_CUDA_LIB}\${LD_LIBRARY_PATH:+:\${LD_LIBRARY_PATH}}"
exec "\${DIR}/bin/client" "\$@"
EOF
chmod +x "${DIST}/client/run.sh"

echo "bundles ready:"; du -sh "${DIST}/server" "${DIST}/client"
