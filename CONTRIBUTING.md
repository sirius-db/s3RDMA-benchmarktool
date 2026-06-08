# Contributing to s3RDMA-benchmarktool

Thanks for your interest in contributing! This project is part of
[sirius-db](https://github.com/sirius-db). Please also follow the conventions of the
flagship [`sirius`](https://github.com/sirius-db/sirius) repo where they apply.

## Developer Certificate of Origin (DCO)

All commits must be signed off under the [Developer Certificate of
Origin](https://developercertificate.org/). Add a `Signed-off-by` trailer to every
commit:

```bash
git commit -s -m "your message"
```

This certifies that you wrote the patch or otherwise have the right to submit it
under the project's Apache-2.0 license.

## Building

This is a C++ project built with [pixi](https://pixi.sh) (conda-based). The RDMA data
path needs a Mellanox NIC + GPUDirect Storage; non-RDMA hosts can still
build and link.

```bash
export PATH="$HOME/.pixi/bin:$PATH"
pixi install
pixi run fetch-sdk     # pinned NVIDIA SDKs into third_party/ (see README)
pixi run build         # configure + compile
pixi run check-glibc   # verify the glibc baseline of the binaries
```

The NVIDIA cuObject SDK and cuFile are **not** vendored — see
[README](README.md#obtaining-the-nvidia-sdks) for how to obtain them.

## Code style & layout

- C++17, 2-space indent; match the surrounding style.
- New source files carry the Apache-2.0 header (see existing `.cpp`/`.hpp` files).
- Keep the two planes separated: HTTP control logic vs. the cuObject RDMA data path.
- Architecture is documented in [doc/design.md](doc/design.md) and
  [doc/internals.md](doc/internals.md) — update them when behavior changes.

## Pull requests

1. Fork and branch from the default branch.
2. Keep changes focused; write a clear description of *what* and *why*.
3. Ensure `pixi run build` succeeds (and `check-glibc` passes) on a build host.
4. Sign off every commit (`-s`).
5. Note any change that affects the wire protocol, the deploy bundle, or the pinned
   SDK versions.

## Reporting issues

Use the issue templates. For data-path bugs, include the environment details the
template asks for (NIC, MLNX_OFED, CUDA, cuObject/cuFile versions) — they are almost
always relevant.
