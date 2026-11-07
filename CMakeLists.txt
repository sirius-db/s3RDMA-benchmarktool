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

cmake_minimum_required(VERSION 3.22)
project(s3_over_rdma LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# ---------------------------------------------------------------------------
# CUDA toolkit (from the pixi env): provides cudart + cuFile + cufile.h, which
# the cuObject headers #include. We only use the runtime/cuFile libs, not nvcc.
# ---------------------------------------------------------------------------
find_package(CUDAToolkit REQUIRED)
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)  # cpp-httplib needs pthreads
find_package(OpenSSL REQUIRED)  # libcrypto for SHA256 verification (client)

# ---------------------------------------------------------------------------
# cpp-httplib (header-only, from the pixi env) — fake-S3 control path.
# ---------------------------------------------------------------------------
find_path(HTTPLIB_INCLUDE_DIR httplib.h REQUIRED)
message(STATUS "cpp-httplib header: ${HTTPLIB_INCLUDE_DIR}/httplib.h")

# ---------------------------------------------------------------------------
# RDMA userspace stack (from the pixi env on non-RDMA build hosts): libibverbs / librdmacm.
# ---------------------------------------------------------------------------
find_library(IBVERBS_LIB ibverbs REQUIRED)
find_library(RDMACM_LIB rdmacm REQUIRED)
message(STATUS "ibverbs: ${IBVERBS_LIB}")
message(STATUS "rdmacm:  ${RDMACM_LIB}")

# ---------------------------------------------------------------------------
# cuObject SDK. Client lib + cuobjclient.h ship under the CUDA toolkit target
# dir (CUDA >= 13.1.1); the server lib + headers are a separate SDK under the
# system multiarch dir. See doc/design.md for the exact paths.
# ---------------------------------------------------------------------------
option(S3RDMA_BUILD_SERVER "Build the cuObjServer-based test server" ON)

set(CUOBJ_SDK_ROOT "" CACHE PATH "Optional cuObject SDK prefix (package, CUDA root or staged dir)")

set(CUOBJ_LIB_SUFFIXES lib lib/x86_64-linux-gnu)

if(CUOBJ_SDK_ROOT)
  # NO_DEFAULT_PATH so the conda env's CMAKE_PREFIX_PATH (which also ships a
  # libcuobjclient) cannot shadow the pinned tree.
  message(STATUS "Using cuObject SDK root: ${CUOBJ_SDK_ROOT}")
  file(GLOB _cuda_targets "${CUOBJ_SDK_ROOT}/usr/local/cuda-*/targets/x86_64-linux")
  set(_sdk_hints ${_cuda_targets}
      ${CUOBJ_SDK_ROOT}/targets/x86_64-linux
      ${CUOBJ_SDK_ROOT}
      ${CUOBJ_SDK_ROOT}/usr)
  set(_sdk_find_opts NO_DEFAULT_PATH)
else()
  file(GLOB _cuda_targets "/usr/local/cuda-*/targets/x86_64-linux")
  set(_sdk_hints
      ${CUDAToolkit_TARGET_DIR}
      /usr/local/cuda/targets/x86_64-linux
      ${_cuda_targets}
      /usr/lib/x86_64-linux-gnu
      /usr)
  set(_sdk_find_opts)
endif()

find_path(CUOBJCLIENT_INCLUDE_DIR cuobjclient.h
          HINTS ${_sdk_hints} PATH_SUFFIXES include ${_sdk_find_opts} REQUIRED)
find_library(CUOBJCLIENT_LIB cuobjclient HINTS ${_sdk_hints}
             PATH_SUFFIXES ${CUOBJ_LIB_SUFFIXES} ${_sdk_find_opts} REQUIRED)
get_filename_component(_cuobjclient_real "${CUOBJCLIENT_LIB}" REALPATH)
message(STATUS "cuobjclient: ${CUOBJCLIENT_LIB} -> ${_cuobjclient_real} (hdr ${CUOBJCLIENT_INCLUDE_DIR})")

if(S3RDMA_BUILD_SERVER)
  find_path(CUOBJSERVER_INCLUDE_DIR cuobjserver.h
            HINTS ${_sdk_hints} PATH_SUFFIXES include ${_sdk_find_opts} REQUIRED)
  find_library(CUOBJSERVER_LIB cuobjserver HINTS ${_sdk_hints}
               PATH_SUFFIXES ${CUOBJ_LIB_SUFFIXES} ${_sdk_find_opts} REQUIRED)
  message(STATUS "cuobjserver: ${CUOBJSERVER_LIB} (hdr ${CUOBJSERVER_INCLUDE_DIR})")

  # The server headers live in /usr/include. Adding that whole directory to the
  # include path poisons the conda sysroot (host glibc headers shadow the
  # toolchain's). Instead stage symlinks to only the cuObject server headers into
  # a private dir and point the server target at that.
  set(CUOBJ_SERVER_STAGE_DIR ${CMAKE_BINARY_DIR}/cuobj_server_include)
  file(MAKE_DIRECTORY ${CUOBJ_SERVER_STAGE_DIR})
  foreach(_hdr cuobjserver.h cuobjrdma.h cuobjtelem.h)
    file(CREATE_LINK ${CUOBJSERVER_INCLUDE_DIR}/${_hdr}
         ${CUOBJ_SERVER_STAGE_DIR}/${_hdr} SYMBOLIC)
  endforeach()
else()
  message(STATUS "cuobjserver: not built (S3RDMA_BUILD_SERVER=OFF)")
endif()

# ---------------------------------------------------------------------------
# Shared header-only helpers (logging + error-check macros).
# ---------------------------------------------------------------------------
add_library(s3rdma_common INTERFACE)
target_include_directories(s3rdma_common INTERFACE ${CMAKE_SOURCE_DIR}/common)
target_link_libraries(s3rdma_common INTERFACE CUDA::cudart)

add_subdirectory(client)
if(S3RDMA_BUILD_SERVER)
  add_subdirectory(server)
endif()
add_subdirectory(probe)
