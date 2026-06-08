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

# Deploy a bundle (built by scripts/bundle.sh) to a test node via rsync over ssh.
# Node coordinates must be provided via env vars (SERVER_HOST, CLIENT_HOST, etc.).
#
# Usage: scripts/deploy.sh <server|client>
set -euo pipefail

ROLE="${1:?usage: deploy.sh <server|client>}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# --- node coordinates (set via env) ----------------------------------------
SERVER_HOST="${SERVER_HOST:?set SERVER_HOST}"
SERVER_USER="${SERVER_USER:?set SERVER_USER}"
SERVER_PASS="${SERVER_PASS:?set SERVER_PASS}"
SERVER_DEST="${SERVER_DEST:-/opt/s3rdma}"

CLIENT_HOST="${CLIENT_HOST:?set CLIENT_HOST}"
CLIENT_USER="${CLIENT_USER:?set CLIENT_USER}"
CLIENT_PASS="${CLIENT_PASS:?set CLIENT_PASS}"
CLIENT_DEST="${CLIENT_DEST:-/opt/s3rdma}"

case "${ROLE}" in
  server) HOST=$SERVER_HOST; USER=$SERVER_USER; PASS=$SERVER_PASS; DEST=$SERVER_DEST ;;
  client) HOST=$CLIENT_HOST; USER=$CLIENT_USER; PASS=$CLIENT_PASS; DEST=$CLIENT_DEST ;;
  *) echo "role must be server|client" >&2; exit 2 ;;
esac

[ -d "${ROOT}/dist/${ROLE}" ] || { echo "run 'pixi run bundle' first" >&2; exit 1; }

SSH="ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null"
echo "== deploying ${ROLE} bundle -> ${USER}@${HOST}:${DEST} =="
sshpass -p "${PASS}" ${SSH} "${USER}@${HOST}" "mkdir -p '${DEST}'"
sshpass -p "${PASS}" rsync -az --delete -e "${SSH}" \
  "${ROOT}/dist/${ROLE}/" "${USER}@${HOST}:${DEST}/"
echo "== done; launch with: ${DEST}/run.sh =="
