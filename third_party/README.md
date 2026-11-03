# third_party/ — vendored NVIDIA SDKs (not committed)

This directory is a **build-time cache** for NVIDIA-proprietary SDKs that the
project links against but **does not redistribute**. Everything here except this
README, `.gitkeep`, and `versions.env` is git-ignored.

Like the flagship [`sirius`](https://github.com/sirius-db/sirius) repo, NVIDIA
dependencies are pinned by version and obtained at build time rather than vendored
into the source tree.

## What lands here

`scripts/fetch_sdk.sh` populates the staging layout below. `scripts/configure.sh` uses the cuObject extract. The legacy deployment bundler still expects cuFile 1.16 and does not support the new 1.3.1 profile.

```
third_party/
  cuobj-<CUOBJ_VERSION>/extract/...     # cuObject SDK (headers + libcuobj{client,server}.so*)
  cufile-<CUFILE_VERSION>/              # libcufile.so.0, libcufile_rdma.so.1 (matched to cuObject)
```

Pinned versions live in [`versions.env`](versions.env).

## How to obtain

The cuObject SDK is gated behind NVIDIA's developer program, so the download URLs
are not baked in. Choose one:

1. **Fetch (recommended):** set `CUOBJ_SDK_URL` / `CUFILE_URL` (and optionally the
   `*_SHA256` checksums) in your environment or in `versions.env`, then:

   ```bash
   pixi run fetch-sdk
   ```

2. **Use an existing install:** point `CUOBJ_SDK_ROOT` at an SDK extract, CUDA toolkit root or staged `include`/`lib` prefix. This skips SDK downloads and selects cuObject for configuration. Select a matching CUDA/cuFile build environment separately.

If neither is set, the build falls back to the system cuObject install (see
`scripts/configure.sh`).

## License

The contents fetched here are subject to the **NVIDIA Software License Agreement**,
not this project's Apache-2.0 license. See [`../THIRD_PARTY_NOTICES.md`](../THIRD_PARTY_NOTICES.md).
