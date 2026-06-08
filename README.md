# s3RDMA-benchmarktool

[![License](https://img.shields.io/badge/License-Apache_2.0-blue.svg)](LICENSE)

A minimal end-to-end benchmark of **S3-style object GET over RDMA** using NVIDIA's
**cuObject** SDK. A fake-S3 HTTP control endpoint carries an RDMA descriptor token,
and the server `RDMA_WRITE`s object bytes straight into the client's buffer — pinned
**host** memory or **GPU** device memory (GPUDirect Storage).

> A subproject of [**sirius-db**](https://github.com/sirius-db) — a GPU-native SQL
> engine. This benchmark explores feeding object data into GPU memory over RDMA.

## Obtaining the NVIDIA SDKs

This repository does **not** bundle any NVIDIA binaries. The cuObject SDK and the
matched cuFile (GPUDirect Storage) are proprietary and gated behind NVIDIA's
developer program, so — like the [`sirius`](https://github.com/sirius-db/sirius)
repo — they are version-pinned and fetched at build time into the git-ignored
`third_party/` cache.

Versions are pinned in [`third_party/versions.env`](third_party/versions.env)
(cuObject 1.0.0 + cuFile 1.16). Provide them one of two ways:

```bash
# 1. Fetch: set the gated download URLs, then pull into third_party/
export CUOBJ_SDK_URL=...   # from your NVIDIA developer access
export CUFILE_URL=...
pixi run fetch-sdk

# 2. Or use an existing unpacked SDK tree (e.g. a system CUDA install):
export CUOBJ_SDK_ROOT=/path/to/cuobject/sdk
```

See [third_party/README.md](third_party/README.md) for details.

## Quick start

```bash
export PATH="$HOME/.pixi/bin:$PATH"
pixi run fetch-sdk  # download the pinned cuObject/cuFile SDKs (see above)
pixi run build      # configure + compile client/server
pixi run gen-data   # create test objects under data/demo-bucket/
pixi run bundle     # self-contained dist/{server,client} bundles
pixi run deploy     # rsync bundles to the RDMA nodes (set *_HOST/*_USER/... env)
```

The RDMA data path requires a Mellanox NIC; hosts without one are build/link only.

## Docs

- **[doc/design.md](doc/design.md)** — architecture and cuObject API usage.
- **[doc/internals.md](doc/internals.md)** — code-level walkthrough: data flow, slot pool, ping-pong loop.
- **[doc/test.md](doc/test.md)** — test procedure, deploy/run steps, and results.

## Layout

```text
client/   cuObjClient wrapper, host/GPU buffers, CLI
server/   cuObjServer + cpp-httplib, slot pool, FD cache
common/   base64, sha256, S3 path/range helpers, logging macros
scripts/  fetch_sdk / configure / bundle / deploy / gen_testdata / run_demo
doc/      design.md, internals.md, test.md
```

## License

Apache-2.0 — see [LICENSE](LICENSE). Third-party components and the NVIDIA SDK terms
are listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Contributions are
welcome — see [CONTRIBUTING.md](CONTRIBUTING.md).
