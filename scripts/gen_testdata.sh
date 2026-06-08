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

# Generate sample objects served by the demo server under data/demo-bucket/.
#   - hello.txt         : tiny text object (smoke test)
#   - obj-8m-{0..7}.bin : 8 × 8 MiB   (mixed-size benchmark)
#   - obj-64m-{0,1}.bin : 2 × 64 MiB  (mixed-size benchmark)
#   - obj-128m-0.bin    : 1 × 128 MiB  (mixed-size benchmark)
#   - blob-64m.bin      : legacy 64 MiB (single-object benchmark)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUCKET_DIR="${ROOT}/data/demo-bucket"
mkdir -p "${BUCKET_DIR}"

printf 'hello from s3-over-rdma\n' > "${BUCKET_DIR}/hello.txt"

# Legacy single-object test.
dd if=/dev/urandom of="${BUCKET_DIR}/blob-64m.bin" bs=1M count=64 status=none

# Mixed-size benchmark objects: 8×8M + 2×64M + 1×128M = 320 MiB total.
for i in $(seq 0 7); do
  dd if=/dev/urandom of="${BUCKET_DIR}/obj-8m-${i}.bin" bs=1M count=8 status=none
done
for i in 0 1; do
  dd if=/dev/urandom of="${BUCKET_DIR}/obj-64m-${i}.bin" bs=1M count=64 status=none
done
dd if=/dev/urandom of="${BUCKET_DIR}/obj-128m-0.bin" bs=1M count=128 status=none

echo "Generated test data in ${BUCKET_DIR}:"
ls -lh "${BUCKET_DIR}"
( cd "${BUCKET_DIR}" && sha256sum *.bin *.txt )
