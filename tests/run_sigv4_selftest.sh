#!/usr/bin/env bash
# Build and run the rig-free SigV4 / control-plane protocol self-test.
# Requires a C++17 compiler and OpenSSL 3.0+ headers and libcrypto.
# Pass extra include/library flags via CXXFLAGS / LDFLAGS when needed.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$(mktemp -d "${TMPDIR:-/tmp}/s3rdma-sigv4.XXXXXX")"
trap 'rm -rf "${OUT}"' EXIT

# CXXFLAGS/LDFLAGS are deliberately word-split into separate compiler arguments.
# shellcheck disable=SC2086
"${CXX:-c++}" -std=c++17 -Wall -Wextra ${CXXFLAGS:-} -I "${ROOT}/common" \
  "${ROOT}/tests/sigv4_selftest.cpp" ${LDFLAGS:-} -lcrypto -o "${OUT}/sigv4_selftest"
"${OUT}/sigv4_selftest"
