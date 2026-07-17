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

set -eo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT}"

CXX="${CXX:-c++}"
BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/s3rdma-p0a.XXXXXX")"
cleanup() {
  local rc=$?
  rm -rf "${BUILD_DIR}" || true
  trap - EXIT
  exit "${rc}"
}
trap cleanup EXIT

BINARY="${BUILD_DIR}/s3rdma_verify_selftest"
PATTERN_FILE="${BUILD_DIR}/sha256-pattern.bin"
RUN_LOG="${BUILD_DIR}/selftest.log"
LINK_LOG="${BUILD_DIR}/link.log"

# The future green build is equivalent to:
#   c++ -std=c++17 -Wall -Wextra -I common tests/verify_selftest.cpp \
#       common/verify/manifest.cpp common/verify/sampler.cpp <crypto flags> \
#       -lcrypto -o <binary>
# During RED_FEATURE the two production .cpp files are intentionally absent.
# They are therefore appended only when present, allowing the clean test object
# to reach the linker and expose the planned undefined production symbols.

CRYPTO_CFLAGS=()
CRYPTO_LIBS=()
HAVE_LIBCRYPTO=0

probe_crypto() {
  local flags=("$@")
  printf '%s\n' \
    '#include <openssl/sha.h>' \
    'int main() {' \
    '  const unsigned char input[] = {97};' \
    '  unsigned char output[SHA256_DIGEST_LENGTH];' \
    '  return SHA256(input, 1, output) == nullptr;' \
    '}' | "${CXX}" -std=c++17 -x c++ - "${flags[@]}" -lcrypto \
      -o "${BUILD_DIR}/crypto_probe" >/dev/null 2>&1 || return 1
  "${BUILD_DIR}/crypto_probe" >/dev/null 2>&1
}

if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists libcrypto; then
  read -r -a candidate_cflags <<<"$(pkg-config --cflags libcrypto)"
  read -r -a candidate_libs <<<"$(pkg-config --libs libcrypto)"
  if probe_crypto "${candidate_cflags[@]}" "${candidate_libs[@]}"; then
    CRYPTO_CFLAGS=("${candidate_cflags[@]}")
    CRYPTO_LIBS=("${candidate_libs[@]}")
    HAVE_LIBCRYPTO=1
  fi
fi

if [[ "${HAVE_LIBCRYPTO}" -eq 0 ]] && command -v brew >/dev/null 2>&1; then
  openssl_prefix="$(brew --prefix openssl 2>/dev/null || true)"
  if [[ -n "${openssl_prefix}" ]]; then
    candidate_cflags=("-I${openssl_prefix}/include")
    candidate_libs=("-L${openssl_prefix}/lib" -lcrypto)
    if probe_crypto "${candidate_cflags[@]}" "${candidate_libs[@]}"; then
      CRYPTO_CFLAGS=("${candidate_cflags[@]}")
      CRYPTO_LIBS=("${candidate_libs[@]}")
      HAVE_LIBCRYPTO=1
    fi
  fi
fi

if [[ "${HAVE_LIBCRYPTO}" -eq 0 ]] && probe_crypto; then
  CRYPTO_LIBS=(-lcrypto)
  HAVE_LIBCRYPTO=1
fi

COMMON_FLAGS=(
  -std=c++17
  -Wall
  -Wextra
  -I common
  "-DS3RDMA_HAVE_LIBCRYPTO=${HAVE_LIBCRYPTO}"
)

objects=()
"${CXX}" "${COMMON_FLAGS[@]}" "${CRYPTO_CFLAGS[@]}" \
  -c tests/verify_selftest.cpp -o "${BUILD_DIR}/verify_selftest.o"
objects+=("${BUILD_DIR}/verify_selftest.o")

for source in common/verify/manifest.cpp common/verify/sampler.cpp; do
  if [[ -f "${source}" ]]; then
    object="${BUILD_DIR}/$(basename "${source}" .cpp).o"
    "${CXX}" "${COMMON_FLAGS[@]}" "${CRYPTO_CFLAGS[@]}" \
      -c "${source}" -o "${object}"
    objects+=("${object}")
  fi
done

set +e
if [[ "${HAVE_LIBCRYPTO}" -eq 1 ]]; then
  "${CXX}" "${objects[@]}" "${CRYPTO_LIBS[@]}" -o "${BINARY}" \
    2>"${LINK_LOG}"
else
  "${CXX}" "${objects[@]}" -o "${BINARY}" 2>"${LINK_LOG}"
fi
link_rc=$?
set -e
if [[ "${link_rc}" -ne 0 ]]; then
  cat "${LINK_LOG}" >&2
  echo "AC17 pending link: selftest binary was not produced" >&2
  exit "${link_rc}"
fi

# AC18 negative control: prove the binary's failure summary and nonzero status
# survive process execution before accepting the normal run.
set +e
"${BINARY}" --harness-fail-probe >"${BUILD_DIR}/failure-probe.log" 2>&1
failure_probe_rc=$?
set -e
failure_probe_last="$(tail -n 1 "${BUILD_DIR}/failure-probe.log")"
if [[ "${failure_probe_rc}" -eq 0 || "${failure_probe_last}" != "SELFTEST FAIL 1/1" ]]; then
  echo "AC18 harness failure-propagation probe did not fail as specified" >&2
  cat "${BUILD_DIR}/failure-probe.log" >&2
  exit 1
fi

run_args=()
if [[ "${HAVE_LIBCRYPTO}" -eq 1 ]]; then
  run_args=(--pattern-file "${PATTERN_FILE}")
fi

set +e
"${BINARY}" "${run_args[@]}" >"${RUN_LOG}" 2>&1
run_rc=$?
set -e
if [[ "${run_rc}" -ne 0 ]]; then
  cat "${RUN_LOG}" >&2
  exit "${run_rc}"
fi

last_line="$(tail -n 1 "${RUN_LOG}")"
if [[ ! "${last_line}" =~ ^SELFTEST\ PASS\ ([0-9]+)/([0-9]+)$ ]]; then
  echo "AC18 malformed final selftest line: ${last_line}" >&2
  exit 1
fi
if [[ "${HAVE_LIBCRYPTO}" -eq 1 ]]; then
  expected_checks=61
else
  expected_checks=57
fi
if [[ "${BASH_REMATCH[1]}" -ne "${BASH_REMATCH[2]}" ||
      "${BASH_REMATCH[2]}" -ne "${expected_checks}" ]]; then
  echo "AC18 invalid selftest check count: ${last_line}" >&2
  exit 1
fi

if [[ "${HAVE_LIBCRYPTO}" -eq 1 ]]; then
  pattern_size="$(wc -c <"${PATTERN_FILE}" | tr -d '[:space:]')"
  if [[ "${pattern_size}" != "1048576" ]]; then
    echo "AC17 pattern size mismatch: ${pattern_size}" >&2
    exit 1
  fi

  marker_count="$(grep -c '^PATTERN_SHA256 [0-9a-f][0-9a-f]*$' "${RUN_LOG}" || true)"
  internal_digest="$(awk '$1 == "PATTERN_SHA256" {print $2}' "${RUN_LOG}")"
  if [[ "${marker_count}" -ne 1 || ! "${internal_digest}" =~ ^[0-9a-f]{64}$ ]]; then
    echo "AC17 missing or malformed internal pattern digest" >&2
    exit 1
  fi

  if command -v shasum >/dev/null 2>&1; then
    system_digest="$(shasum -a 256 "${PATTERN_FILE}" | awk '{print $1}')"
  elif command -v sha256sum >/dev/null 2>&1; then
    system_digest="$(sha256sum "${PATTERN_FILE}" | awk '{print $1}')"
  elif command -v openssl >/dev/null 2>&1; then
    system_digest="$(openssl dgst -sha256 "${PATTERN_FILE}" | awk '{print $NF}')"
  else
    echo "AC17 no independent system SHA256 command is available" >&2
    exit 1
  fi

  if [[ ! "${system_digest}" =~ ^[0-9a-f]{64}$ ||
        "${internal_digest}" != "${system_digest}" ]]; then
    echo "AC17 system SHA256 cross-check mismatch" >&2
    echo "internal=${internal_digest} system=${system_digest}" >&2
    exit 1
  fi
else
  if ! grep -qx 'SKIP sha256-vectors' "${RUN_LOG}" ||
     ! grep -qx 'SKIP AC17: libcrypto unavailable' "${RUN_LOG}"; then
    echo "AC17 libcrypto-unavailable run did not report the permitted skip" >&2
    exit 1
  fi
fi

cat "${RUN_LOG}"
