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

# Start the fake-S3 server and run the client through full + range GET in both
# host and GPU modes. Requires the RDMA node (Mellanox NIC + GDS); the cuObject
# constructors fail without RDMA hardware, so this is for the cluster — see doc/test.md.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SERVER="${ROOT}/build/server/server"
CLIENT="${ROOT}/build/client/client"
DATA_DIR="${ROOT}/data"
BUCKET="demo-bucket"

HTTP_PORT="${HTTP_PORT:-8080}"
RDMA_PORT="${RDMA_PORT:-18515}"
SERVER_IP="${SERVER_IP:-127.0.0.1}"   # set to the RDMA NIC IP on the cluster

[ -x "${SERVER}" ] && [ -x "${CLIENT}" ] || { echo "build first: pixi run build" >&2; exit 1; }
[ -d "${DATA_DIR}/${BUCKET}" ] || { echo "gen data first: pixi run gen-data" >&2; exit 1; }

echo "== starting server =="
"${SERVER}" --data-dir "${DATA_DIR}" --ip "${SERVER_IP}" \
            --rdma-port "${RDMA_PORT}" --port "${HTTP_PORT}" &
SERVER_PID=$!
trap 'kill ${SERVER_PID} 2>/dev/null || true' EXIT
sleep 2

run() {
  echo "== client: $* =="
  "${CLIENT}" --server "${SERVER_IP}:${HTTP_PORT}" --bucket "${BUCKET}" "$@"
}

# Full GET (host + gpu)
run --object hello.txt --mode host
run --object blob-64m.bin --mode host
run --object blob-64m.bin --mode gpu

echo "== done =="
