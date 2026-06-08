# Design — S3 over RDMA demo

S3-style object GET over RDMA using NVIDIA **cuObject** (Magnum IO GPUDirect
Storage). Data lands in host memory or directly in GPU memory.

## Architecture

cuObject provides only the RDMA data path (DC transport, Mellanox-only). The
S3/HTTP control path is ours: the client's cuObject `get` callback issues the
HTTP request, relaying an opaque RDMA descriptor; the server RDMA-writes the
object bytes into the client's registered buffer.

```
                 HTTP control path (cpp-httplib)
   client  ──── GET /bucket/object ───────────────►  server
 (cuObjClient)   x-amz-rdma-token: <RDMA desc>       (cuObjServer)
              ◄── 200/206  x-amz-rdma-bytes-transferred ──
       ▲                                                    │
       └──────────── RDMA_WRITE (data path) ◄───────────────┘
   host / GPU buffer                          ./data/<bucket>/<object>
```

**GET flow:** client registers a buffer (`cuMemObjGetDescriptor`) → `cuObjGet`
invokes callback → callback sends HTTP GET with RDMA token → server `pread`s
file into a pinned staging buffer → `handleGetObject()` RDMA-writes it to the
client → client verifies SHA256.

Supports range GET (`206`), HEAD (object size), rejects PUT/POST (`405`).

## Concurrency

The server pre-allocates a pool of **RDMA slots** (configurable via
`--concurrency`, default 32). Each slot owns a DCI channel and a small pinned
staging buffer (default **2 MiB**, `--chunk-size`). Large objects are
transferred in a loop: `pread` one chunk → `handleGetObject` RDMA-writes it to
`remote_addr + offset` → advance. This keeps per-slot memory small (32 slots ×
2 MiB = 64 MiB total) while supporting arbitrary object sizes.

File I/O uses `pread(2)` through the page cache by default. An FD cache avoids
repeated `open()` calls. The `httplib` thread pool is sized to match the slot
count.

The client supports `--threads N --duration S` for a sustained loop benchmark:
each thread pre-registers its buffer and loops GETs for S seconds.

## Scope

- GET + range GET only; no PUT/MPU.
- No SigV4 — the HTTP layer is a control channel, not a real S3 endpoint.
