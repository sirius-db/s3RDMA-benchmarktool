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

# Probe binaries are additive: the shipped client/server targets are not
# touched. buffers.cpp is recompiled from the frozen client sources.

add_executable(cuobj_scope_probe
    scope_probe_main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../client/buffers.cpp)

target_include_directories(cuobj_scope_probe PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../client
    ${CUOBJCLIENT_INCLUDE_DIR}
    ${HTTPLIB_INCLUDE_DIR})

target_link_libraries(cuobj_scope_probe PRIVATE
    s3rdma_common
    -Wl,--no-as-needed
    ${CUOBJCLIENT_LIB}
    CUDA::cuFile
    ${IBVERBS_LIB}
    ${RDMACM_LIB}
    -Wl,--as-needed
    CUDA::cudart
    OpenSSL::Crypto
    Threads::Threads)
