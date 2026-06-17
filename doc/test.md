# Test environment & results

End-to-end validation on real RDMA hardware (CX-6 RoCEv2). The data path
requires a Mellanox NIC; hosts without one are build/link only.

## Nodes

| Role | OS / HW | Requirements |
|------|---------|-------------|
| client | Ubuntu 24.04, A100 GPU, CUDA 13+ | GDS 1.15+, nvidia-fs, MLNX_OFED |
| server | Ubuntu 24.04, no GPU | MLNX_OFED |

Both nodes need CX-6 (or later) RoCEv2 NICs on the same L2 subnet.

## Version alignment

cuObject client lib must match cufile/GDS. The build uses **cuObject 1.0.0**
paired with **cufile 1.16** (`third_party/`). See `scripts/configure.sh` and
`scripts/bundle.sh`.

## Deploy (from the build host)

```bash
export PATH="$HOME/.pixi/bin:$PATH"
pixi run gen-data        # test objects under data/demo-bucket/
pixi run deploy          # build + bundle + rsync to both nodes
```

Set node coordinates via env vars: `SERVER_HOST`, `CLIENT_HOST`, `*_USER`,
`*_PASS`, `*_DEST` (see `scripts/deploy.sh`).

## Node prerequisites (one-time / per reboot)

- **Client (GPU node):**
  - `modprobe nvidia_peermem` — without it cufile disables userspace RDMA and the
    NIC shows `Down`, so buffer registration is "not RDMA capable".
  - set `properties.rdma_dev_addr_list` to the RoCE NIC IP in
    `/usr/local/cuda/gds/cufile.json`.
  - verify: `/usr/local/cuda/gds/tools/gdscheck -p` shows
    `Userspace RDMA: Supported` and `rdma_device_status Up: 1`.
- **Server:** open firewall for HTTP (8080/tcp), RDMA-CM (18515/tcp+udp),
  RoCEv2 (4791/udp).

## Run

Server — dynamic slot allocation (up to 1024), 2 MiB double-buffer ping-pong:
```bash
./run.sh --data-dir ./data --ip <SERVER_ROCE_IP> --rdma-port 18515 --port 8080
```

Client — `run.sh` sets `LD_LIBRARY_PATH` for the bundled libs.

## Results — large read (128 MiB object)

Single 128 MiB object, thread count varies. Determines how many threads are
needed to saturate the link. 10 s, page cache warm, 2 MiB server chunks,
double-buffer ping-pong.

Client NIC: CX-6 100G (single). Server NIC: bonded 2×100G.
Host = `cudaMallocHost` (pinned). GPU = `cudaMalloc` (GPUDirect RDMA).

```bash
./run.sh --server <SERVER>:8080 --bucket demo-bucket --object obj-128m-0.bin --mode host --threads N --duration 10
./run.sh --server <SERVER>:8080 --bucket demo-bucket --object obj-128m-0.bin --mode gpu  --threads N --duration 10
```

| Threads | Host Alloc+Reg (ms) | Host Gbps | GPU Alloc+Reg (ms) | GPU Gbps |
|---------|--------------------:|----------:|-------------------:|---------:|
| 1 | 678 | 64.4 | 653 | 63.4 |
| 2 | 742 | 85.2 | 672 | 85.2 |
| 4 | 913 | **90.4** | 746 | **90.6** |
| 8 | 1098 | **90.8** | 816 | **90.8** |
| 16 | 1279 | **90.7** | 857 | **90.8** |
| 32 | 2542 | **90.8** | 924 | **90.8** |
| 64 | 3135 | **90.8** | 1355 | **90.8** |
| 128 | 5905 | **90.8** | 1975 | **90.7** |

**Peak: ~91 Gbps at 4 threads** (~91% line rate on 100G). Host and GPU modes
match. Adding threads beyond 4 does not improve throughput for large objects.

## Results — range-read sweep (128 MiB object)

Single 128 MiB object, `--range-size` varies. High thread counts (32/64/128)
to eliminate control-plane serialization. Determines the minimum range size
that saturates the link.

```bash
./run.sh --server <SERVER>:8080 --bucket demo-bucket --object obj-128m-0.bin --mode host --threads N --duration 10 --range-size <SIZE>
```

### Host mode (Gbps)

| Range size | 4 threads | 8 threads | 16 threads | 32 threads | 64 threads | 128 threads |
|-----------|----------:|----------:|----------:|----------:|----------:|----------:|
| 256 KiB | 16.5 | 34.3 | 66.6 | 82.6 | 81.5 | 65.7 |
| 512 KiB | 27.9 | 55.6 | 85.3 | **90.7** | 87.8 | 83.5 |
| 1 MiB | 46.3 | 78.5 | **90.7** | 88.7 | 85.0 | **90.7** |
| 2 MiB | 66.9 | 89.5 | **90.8** | **90.8** | **90.8** | **90.8** |
| 4 MiB | 83.2 | **90.7** | **90.7** | **90.7** | **90.8** | **90.8** |
| 8 MiB | 88.6 | **90.7** | **90.8** | **90.8** | **90.8** | **90.8** |
| 16 MiB | 88.3 | **90.7** | **90.7** | **90.8** | **90.7** | **90.7** |
| 32 MiB | 90.2 | **90.8** | **90.7** | **90.8** | **90.8** | **90.7** |
| 64 MiB | 90.3 | **90.7** | **90.7** | **90.8** | **90.8** | **90.8** |
| 128 MiB (full) | 90.5 | **90.8** | 90.6 | **90.8** | **90.8** | **90.8** |

### GPU mode (Gbps)

| Range size | 4 threads | 8 threads | 16 threads | 32 threads | 64 threads | 128 threads |
|-----------|----------:|----------:|----------:|----------:|----------:|----------:|
| 256 KiB | 17.5 | 34.9 | 67.0 | 89.3 | **90.8** | 67.4 |
| 512 KiB | 27.9 | 50.9 | 84.6 | **90.6** | **90.8** | 71.3 |
| 1 MiB | 48.9 | 80.1 | **90.7** | **90.7** | **90.8** | **90.7** |
| 2 MiB | 70.2 | 89.9 | **90.8** | **90.8** | **90.8** | **90.7** |
| 4 MiB | 85.0 | **90.8** | **90.7** | **90.7** | **90.8** | **90.8** |
| 8 MiB | 89.7 | **90.8** | **90.8** | **90.7** | **90.8** | **90.8** |
| 16 MiB | 90.1 | **90.8** | **90.8** | **90.8** | **90.7** | **90.7** |
| 32 MiB | 90.3 | **90.8** | **90.8** | **90.8** | **90.8** | **90.8** |
| 64 MiB | 90.5 | **90.8** | **90.8** | **90.8** | **90.8** | **90.8** |
| 128 MiB (full) | 90.6 | **90.8** | **90.8** | **90.8** | **90.8** | **90.8** |

**Key findings:**

- **4 threads:** BW scales linearly with range size; 8 MiB reaches ~89 Gbps
  but doesn't fully saturate — HTTP round-trip is the bottleneck with only 4
  concurrent requests.
- **8 threads:** 4 MiB range hits line rate (~90.7 Gbps). Doubling concurrency
  halves the minimum range needed vs 4 threads.
- **16 threads:** 1 MiB range is enough to saturate the link.
- **32+ threads:** 512 KiB–2 MiB ranges reach line rate. At 128 threads,
  thread scheduling overhead pushes the minimum to 1 MiB.
- **Overall:** the minimum range size for max BW is inversely proportional to
  thread count. **8 threads × 4 MiB** or **16 threads × 1 MiB** are practical
  sweet spots for both host and GPU modes.

## Troubleshooting notes (root causes hit during bring-up)

- **cuObjGet fails / `cuFileHandleRegister` bad fd** → client cufile version
  mismatch (use 1.16 with cuObject 1.0.0; bundle libs load before the node's).
- **buffer "not RDMA capable" / device `Down`** → `nvidia_peermem` not loaded.
- **`No RDMA devices configured`** → `rdma_dev_addr_list` not set in `cufile.json`.
- **server `handleGetObject` returns `-5`** → must allocate a DCI channel
  (`allocateChannelId()`); channel 0 is invalid. (Fixed in `server/s3_server.cpp`.)
- **client can't reach server** → server firewall; open the ports above.
