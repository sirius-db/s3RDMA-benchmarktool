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

## Results — sustained benchmark

10 s, page cache warm, 2 MiB chunks, double-buffer ping-pong.

**Workload:** 11 objects round-robin (8×8 MiB + 2×64 MiB + 1×128 MiB).
Each thread registers one **128 MiB** buffer and loops GETs for 10 s.
Client NIC: CX-6 100G. Server NIC: bonded 2×100G.

```bash
OBJS=obj-8m-0.bin,obj-8m-1.bin,obj-8m-2.bin,obj-8m-3.bin,obj-8m-4.bin,obj-8m-5.bin,obj-8m-6.bin,obj-8m-7.bin,obj-64m-0.bin,obj-64m-1.bin,obj-128m-0.bin
```

Buffer per thread: **128 MiB** (= largest object).
Host = `cudaMallocHost` (pinned). GPU = `cudaMalloc` (GPUDirect RDMA).

```bash
./run.sh --server <SERVER>:8080 --bucket demo-bucket --objects $OBJS --mode host --threads N --duration 10
./run.sh --server <SERVER>:8080 --bucket demo-bucket --objects $OBJS --mode gpu  --threads N --duration 10
```

| Threads | Buf (N × MiB) | Host Alloc+Reg (ms) | Host BW (GiB/s) | Host Gbps | GPU Alloc+Reg (ms) | GPU BW (GiB/s) | GPU Gbps |
|---------|--------------|--------------------:|----------------:|----------:|-------------------:|---------------:|---------:|
| 1 | 1 × 128 | 724 | 5.9 | 47.6 | 646 | 6.8 | 54.6 |
| 2 | 2 × 128 | 707 | 9.9 | **79.4** | 651 | 10.0 | **80.2** |
| 8 | 8 × 128 | 1092 | 11.3 | **90.7** | 669 | 11.3 | **90.7** |
| 32 | 32 × 128 | 2576 | 11.3 | **90.8** | 895 | 11.3 | **90.8** |

### Error handling

| Test | Expected |
|------|----------|
| PUT / POST | `405` |
| HEAD/GET missing object | `404` |

**Peak: ~91 Gbps at 8+ threads** (~91% line rate on 100G). Host and GPU modes
match. Alloc+register is a one-time cost (~650 ms for 1×128 MiB).

## Troubleshooting notes (root causes hit during bring-up)

- **cuObjGet fails / `cuFileHandleRegister` bad fd** → client cufile version
  mismatch (use 1.16 with cuObject 1.0.0; bundle libs load before the node's).
- **buffer "not RDMA capable" / device `Down`** → `nvidia_peermem` not loaded.
- **`No RDMA devices configured`** → `rdma_dev_addr_list` not set in `cufile.json`.
- **server `handleGetObject` returns `-5`** → must allocate a DCI channel
  (`allocateChannelId()`); channel 0 is invalid. (Fixed in `server/s3_server.cpp`.)
- **client can't reach server** → server firewall; open the ports above.
