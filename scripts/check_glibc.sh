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

# Report the max required GLIBC symbol version of the built binaries. Must be
# <= the target baseline (e.g. 2.31 for older glibc distros) for the binaries to run on
# the deploy nodes. See doc/test.md and the sysroot pin in pixi.toml.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

for bin in "${ROOT}/build/client/client" "${ROOT}/build/server/server"; do
  [ -f "${bin}" ] || continue
  echo "== ${bin} =="
  objdump -T "${bin}" | grep -o 'GLIBC_[0-9.]*' | sort -V -u | tail -n1
done
