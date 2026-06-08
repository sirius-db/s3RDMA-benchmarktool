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

# Configure the CMake build. Builds against the version-aligned cuObject SDK if
# unpacked under third_party/ (or pointed to by CUOBJ_SDK_ROOT), else the system
# install. See doc/test.md.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SDK="${CUOBJ_SDK_ROOT:-${ROOT}/third_party/cuobj-1.0.0/extract}"

ARG=()
if [ -d "${SDK}" ]; then
  ARG=(-DCUOBJ_SDK_ROOT="$(realpath "${SDK}")")
  echo "configure: cuObject SDK root = ${SDK}"
else
  echo "configure: using system cuObject (no SDK root at ${SDK})"
fi

cmake -S "${ROOT}" -B "${ROOT}/build" -G Ninja "${ARG[@]}"
