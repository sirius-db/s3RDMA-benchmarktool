#!/usr/bin/env bash
# Build and run the rig-free raw HTTP receipt reader self-test.
# Requires a C++17 compiler and OpenSSL 3.0+ headers and libcrypto.
# Pass extra include/library flags via CXXFLAGS / LDFLAGS when needed.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$(mktemp -d "${TMPDIR:-/tmp}/s3rdma-raw-http.XXXXXX")"
trap 'rm -rf "${OUT}"' EXIT

# CXXFLAGS/LDFLAGS are deliberately word-split into separate compiler arguments.
# shellcheck disable=SC2086
"${CXX:-c++}" -std=c++17 -Wall -Wextra ${CXXFLAGS:-} -I "${ROOT}/common" \
  "${ROOT}/tests/raw_http_selftest.cpp" -pthread ${LDFLAGS:-} -lcrypto -o "${OUT}/raw_http_selftest"
"${OUT}/raw_http_selftest"
