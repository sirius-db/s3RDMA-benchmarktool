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

# Fetch the pinned NVIDIA SDKs (cuObject + matched cuFile) into third_party/.
#
# These SDKs are NVIDIA-proprietary and are NOT redistributed in this repo; they
# are downloaded at build time into the git-ignored third_party/ cache, in the
# layout that scripts/configure.sh and scripts/bundle.sh expect. Versions are
# pinned in third_party/versions.env. See third_party/README.md.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TP="${ROOT}/third_party"
VERSIONS="${TP}/versions.env"

[ -f "${VERSIONS}" ] || { echo "fetch-sdk: missing ${VERSIONS}" >&2; exit 1; }
# shellcheck source=/dev/null
. "${VERSIONS}"

CUOBJ_DIR="${TP}/cuobj-${CUOBJ_VERSION}"
CUFILE_DIR="${TP}/cufile-${CUFILE_VERSION}"

have_cmd() { command -v "$1" >/dev/null 2>&1; }

# download <url> <dest-file>
download() {
  local url="$1" dest="$2"
  if have_cmd curl; then
    curl -fSL --retry 3 -o "${dest}" "${url}"
  elif have_cmd wget; then
    wget -O "${dest}" "${url}"
  else
    echo "fetch-sdk: need curl or wget to download ${url}" >&2
    exit 1
  fi
}

# verify_sha256 <file> <expected-or-empty>
verify_sha256() {
  local file="$1" want="$2"
  [ -n "${want}" ] || return 0
  local got
  if have_cmd sha256sum; then
    got="$(sha256sum "${file}" | awk '{print $1}')"
  elif have_cmd shasum; then
    got="$(shasum -a 256 "${file}" | awk '{print $1}')"
  else
    echo "fetch-sdk: no sha256 tool; skipping checksum for ${file}" >&2
    return 0
  fi
  if [ "${got}" != "${want}" ]; then
    echo "fetch-sdk: checksum mismatch for ${file}" >&2
    echo "  expected ${want}" >&2
    echo "  got      ${got}" >&2
    exit 1
  fi
}

gated_error() {  # $1 = SDK name, $2 = env var name
  cat >&2 <<EOF
fetch-sdk: ${1} download URL not set (${2}).

The cuObject SDK and matched cuFile are gated behind NVIDIA's developer program,
so this repo cannot ship them or hard-code their URLs. To proceed, either:

  1. Set ${2} (and optionally the matching *_SHA256) in third_party/versions.env
     or in your environment, then re-run:  pixi run fetch-sdk
  2. Or point CUOBJ_SDK_ROOT at an already-unpacked SDK tree and skip this step;
     configure.sh / bundle.sh will use it directly.

NVIDIA downloads: https://developer.nvidia.com/gpudirect-storage
EOF
  exit 1
}

# ---------------------------------------------------------------------------
# cuObject SDK -> third_party/cuobj-<ver>/extract
# ---------------------------------------------------------------------------
fetch_cuobj() {
  if [ -d "${CUOBJ_DIR}/extract" ]; then
    echo "fetch-sdk: cuObject ${CUOBJ_VERSION} already present (${CUOBJ_DIR}/extract)"
    return 0
  fi
  [ -n "${CUOBJ_SDK_URL}" ] || gated_error "cuObject SDK" "CUOBJ_SDK_URL"
  echo "fetch-sdk: downloading cuObject ${CUOBJ_VERSION}"
  local tmp; tmp="$(mktemp -d)"
  local archive="${tmp}/cuobj.tar"
  download "${CUOBJ_SDK_URL}" "${archive}"
  verify_sha256 "${archive}" "${CUOBJ_SDK_SHA256:-}"
  mkdir -p "${CUOBJ_DIR}/extract"
  tar -xf "${archive}" -C "${CUOBJ_DIR}/extract"
  rm -rf "${tmp}"
  echo "fetch-sdk: cuObject unpacked -> ${CUOBJ_DIR}/extract"
}

# ---------------------------------------------------------------------------
# cuFile (matched) -> third_party/cufile-<ver>/{libcufile.so.0,libcufile_rdma.so.1}
# ---------------------------------------------------------------------------
fetch_cufile() {
  if [ -f "${CUFILE_DIR}/libcufile.so.0" ]; then
    echo "fetch-sdk: cuFile ${CUFILE_VERSION} already present (${CUFILE_DIR})"
    return 0
  fi
  [ -n "${CUFILE_URL}" ] || gated_error "cuFile" "CUFILE_URL"
  echo "fetch-sdk: downloading cuFile ${CUFILE_VERSION}"
  local tmp; tmp="$(mktemp -d)"
  local archive="${tmp}/cufile.tar"
  download "${CUFILE_URL}" "${archive}"
  verify_sha256 "${archive}" "${CUFILE_SHA256:-}"
  mkdir -p "${CUFILE_DIR}"
  # Extract just the runtime libs into a flat dir (bundle.sh expects them here).
  tar -xf "${archive}" -C "${tmp}"
  find "${tmp}" -name 'libcufile.so.0' -exec cp -Lf {} "${CUFILE_DIR}/" \; -quit
  find "${tmp}" -name 'libcufile_rdma.so.1' -exec cp -Lf {} "${CUFILE_DIR}/" \; -quit
  rm -rf "${tmp}"
  [ -f "${CUFILE_DIR}/libcufile.so.0" ] || {
    echo "fetch-sdk: libcufile.so.0 not found in cuFile archive" >&2; exit 1; }
  echo "fetch-sdk: cuFile libs -> ${CUFILE_DIR}"
}

# If the user already has an unpacked SDK, honor it and do nothing.
if [ -n "${CUOBJ_SDK_ROOT:-}" ]; then
  echo "fetch-sdk: CUOBJ_SDK_ROOT set (${CUOBJ_SDK_ROOT}); skipping download"
  exit 0
fi

fetch_cuobj
fetch_cufile
echo "fetch-sdk: done"
