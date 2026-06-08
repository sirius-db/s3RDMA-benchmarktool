# Third-Party Notices

`s3RDMA-benchmarktool` is licensed under the Apache License 2.0 (see [`LICENSE`](LICENSE)).
It builds against and/or links the following third-party components, each under its
own license. This file is informational; the authoritative terms are those shipped
with each component.

## NVIDIA cuObject SDK — proprietary, **not redistributed**

The cuObject RDMA data path (`cuObjClient` / `cuObjServer`, headers `cuobjclient.h`,
`cuobjserver.h`, `cuobjrdma.h`, `cuobjtelem.h`, and `libcuobjclient` /
`libcuobjserver`) is part of NVIDIA's GPUDirect Storage / Magnum IO stack.

- **License:** NVIDIA Software License Agreement (proprietary).
- **Distribution:** This repository does **not** contain or redistribute any
  cuObject SDK binaries or headers. They are obtained at build time into the
  git-ignored `third_party/` cache — see [`third_party/README.md`](third_party/README.md)
  and `scripts/fetch_sdk.sh`.
- **Source:** <https://developer.nvidia.com/gpudirect-storage>

## NVIDIA cuFile / GPUDirect Storage — proprietary, **not redistributed**

`libcufile.so` and `libcufile_rdma.so` (cuFile, part of the CUDA Toolkit / GPUDirect
Storage). cuObject 1.0.0 is paired with cuFile 1.16.

- **License:** NVIDIA Software License Agreement / NVIDIA CUDA Toolkit EULA
  (proprietary).
- **Distribution:** Not redistributed here; fetched at build time (see above) or
  provided by the system CUDA install.
- **Source:** <https://developer.nvidia.com/gpudirect-storage>

## cpp-httplib — bundled via conda at build time

Header-only HTTP library used for the fake-S3 control plane (server endpoint and the
client HTTP call inside the cuObject GET callback). Resolved from conda-forge by
pixi; not vendored in this repository.

- **License:** MIT License.
- **Source:** <https://github.com/yhirose/cpp-httplib>

## OpenSSL — linked via conda at build time

`libcrypto` provides SHA-256 (`common/sha256.hpp`). Resolved from conda-forge by
pixi; not vendored in this repository.

- **License:** Apache License 2.0 (OpenSSL 3.x).
- **Source:** <https://www.openssl.org/>

## RDMA core (libibverbs / librdmacm) — linked via conda/system at build time

Userspace RDMA verbs and connection management used to link the cuObject libraries.

- **License:** Dual-licensed under GPL-2.0 OR BSD-2-Clause / MIT (per the rdma-core
  project); this project links against the BSD/MIT userspace libraries.
- **Source:** <https://github.com/linux-rdma/rdma-core>

---

The original source in this repository (`client/`, `server/`, `common/`, `scripts/`,
`CMakeLists.txt`) is Copyright 2025 Sirius Contributors, Apache-2.0.
`common/base64.hpp` and `common/sha256.hpp` are original implementations
(`sha256.hpp` calls OpenSSL's `SHA256`).
