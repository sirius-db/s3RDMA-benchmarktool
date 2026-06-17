# Internals — code-level walkthrough

A file-by-file, function-by-function tour of how a GET actually flows through the
code. `design.md` covers the *concept*; this doc follows the *implementation*.
Read `design.md` first for the high-level model.

## Two planes

The system splits cleanly into two independent planes:

- **Control plane** — plain HTTP over cpp-httplib. The client asks for an object
  and hands the server an opaque RDMA descriptor (base64-encoded in a header). No
  object bytes ever travel on this path.
- **Data plane** — NVIDIA cuObject (DC transport, RoCEv2). The server issues
  `RDMA_WRITE`s that land object bytes directly in the client's registered buffer,
  which is either pinned host memory or GPU device memory (GPUDirect).

A single full GET, with the real function and header names:

```
 client (cuObjClient)                              server (cuObjServer)
 ────────────────────                              ────────────────────
 get_registered(buf, size, bucket, object)
   └─ cuObjGet(&ctx, ptr, size)
        └─ get_callback(handle, ptr, size, …, rdma)   ← cuObject calls this ONCE
             base64(rdma->desc_str) ─┐
             HTTP GET /bucket/object │
               x-amz-rdma-token: <descriptor>  ──────►  run() httplib handler
               (Range: bytes=a-b)                         parse_object_path()
                                                          get_fd()  → fd + size
                                                          handle_get():
                                                            parse_rdma_remote_addr()
                                                            acquire_slot()
                                              ◄─ RDMA_WRITE ─  ping-pong loop:
        buf filled by RDMA  ◄───────────────────────────       pread → handleGetObject
             ◄── 200/206  x-amz-rdma-reply: 200       ──      release_slot()
        return size   ◄──┘
```

## The key insight: one callback, server-side chunking

`cuObjGet` invokes the client `get_callback` **exactly once per object**, not once
per chunk. The callback's `offset` argument is therefore always 0 and is explicitly
ignored — see `(void)offset;` at
[client/s3_client.cpp:64](../client/s3_client.cpp#L64). The callback's whole job is
to run the HTTP control request and report back how many bytes arrived.

All splitting of a large object into transfer units, and all pipelining of those
units, happens on the **server** inside `handle_get`. This is the non-obvious part
of the design: the client side is a single blocking HTTP round-trip per object,
while the server streams the object across many `RDMA_WRITE`s into
`remote_addr + offset`. Keep this in mind for the rest of the walkthrough.

## Client walkthrough

### [client/main.cpp](../client/main.cpp) — CLI + benchmark harness

The client binary is a sustained-throughput benchmark, not a one-shot fetch tool.
`main` (line 306) runs this pipeline:

1. **`discover_objects`** ([main.cpp:181](../client/main.cpp#L181)) — issues an HTTP
   `HEAD` per object (`head_size`, line 97) to learn sizes and the max size. Buffers
   are sized to the largest object so one buffer serves the whole round-robin set.
2. **`setup_buffers_clients`** ([main.cpp:201](../client/main.cpp#L201)) — allocates
   `N` buffers and `N` `S3Client`s, and **pre-registers** each buffer
   (`register_buffer`). This alloc+register step is timed and reported separately
   because it is the one-time, multi-hundred-millisecond cost (RDMA pinning).
3. **Warm-up** ([main.cpp:330](../client/main.cpp#L330)) — one GET per object on
   thread 0 to prime the server's page cache before measurement.
4. **`run_benchmark`** ([main.cpp:223](../client/main.cpp#L223)) — spawns `N` worker
   threads coordinated by three atomics: `ready` (workers count in), `go` (main
   releases them together with `memory_order_release`), `stop` (main sets it after
   `--duration` seconds). Each worker loops `get_registered` round-robin over the
   object list and accumulates bytes/iters. Any `n < 0` marks the thread failed.
5. **`report_results`** ([main.cpp:275](../client/main.cpp#L275)) — per-thread and
   aggregate MiB/s and Gbps.

The optional `--out <file>` dump ([main.cpp:343](../client/main.cpp#L343)) is
single-thread only; it copies the last fetched object to host via `Buffer::to_host`
and writes it out. There is **no checksum / integrity verification** anywhere in
the client (see [Doc-vs-code discrepancies](#doc-vs-code-discrepancies)).

### [client/s3_client.cpp](../client/s3_client.cpp) — the cuObject wrapper

- **Construction** ([s3_client.cpp:70](../client/s3_client.cpp#L70)) wires
  `ops_.get = &get_callback` (no PUT) and opens the RDMA data path with
  `new cuObjClient(ops_, CUOBJ_PROTO_RDMA_DC_V1)`. This is where GDS + a Mellanox
  NIC become hard requirements.
- **`register_buffer` / `deregister_buffer`** are thin wrappers over
  `cuMemObjGetDescriptor` / `cuMemObjPutDescriptor` — they pin the buffer and obtain
  its RDMA descriptor. `get` does register → `cuObjGet` → deregister in one call;
  `get_registered` skips the pinning (buffer already registered) and is what the
  benchmark loop uses.
- **`get_callback`** ([s3_client.cpp:33](../client/s3_client.cpp#L33)) is the heart
  of the control plane. It recovers the per-request `GetContext` via
  `cuObjClient::getCtx(handle)`, builds the headers (constants from
  [common/s3_util.hpp](../common/s3_util.hpp)) — base64-encoding `rdma->desc_str`
  into `x-amz-rdma-token`, plus an optional `Range` header — issues the
  HTTP GET, accepts only `200`/`206`, and returns `size`. Only standard
  cuObject headers are used; the server extracts the remote buffer address
  from the RDMA descriptor.

### [client/buffers.cpp](../client/buffers.cpp) — host vs GPU memory

`Buffer` ([buffers.cpp:16](../client/buffers.cpp#L16)) allocates with `cudaMalloc`
for `BufferMode::Gpu` (device memory, RDMA-written via GPUDirect) or `cudaMallocHost`
for `BufferMode::Host` (pinned host memory, required for RDMA registration). Both are
registerable by cuObject; the rest of the client is mode-agnostic. `to_host`
([buffers.cpp:31](../client/buffers.cpp#L31)) does a `cudaMemcpy` D2H for GPU buffers
and a plain `memcpy` for host buffers, used only by the `--out` dump.

## Server walkthrough

### [server/s3_server.cpp](../server/s3_server.cpp) `run` — HTTP routing

`run` ([s3_server.cpp:336](../server/s3_server.cpp#L336)) registers a single
`Get(".*")` handler and rejects `Put`/`Post`/`Delete` with `405`. The handler:

- parses the path with `parse_object_path`; bad paths → `400`.
- serves `HEAD` directly from `get_fd` (returns `Content-Length`, or `404`).
- for GET, validates the `x-amz-rdma-token` header (→ `400` if missing), calls
  `handle_get` (which parses the remote buffer address from the RDMA descriptor)
  and maps its return code to the HTTP status (`200`/`206`/`404`/`416`/`500`).
  On success, responds with `x-amz-rdma-reply: 200`.

The httplib thread pool is sized to `min(max_concurrency, 256)`
([s3_server.cpp:340](../server/s3_server.cpp#L340)); real back-pressure comes from
the slot pool, not the thread count.

### Slot pool — RDMA resource pooling

A "slot" (`RdmaSlot`) is one DCI channel plus **two** registered staging buffers
(the ping-pong pair). The pool grows on demand:

- **`acquire_slot`** ([s3_server.cpp:144](../server/s3_server.cpp#L144)) — fast path
  pops a free slot; otherwise, if under `max_concurrency`, builds a new one via
  `create_slot`; otherwise waits on `pool_cv_` until a slot is released. This wait is
  the system's back-pressure: at most `max_concurrency` GETs run concurrently.
- **`create_slot`** ([s3_server.cpp:126](../server/s3_server.cpp#L126)) — allocates a
  channel (`allocateChannelId`) and two `chunk_size` staging buffers via
  `allocHostBuffer` + `registerBuffer`.
- **`release_slot`** returns the slot and notifies one waiter.

### FD cache

`get_fd` ([s3_server.cpp:188](../server/s3_server.cpp#L188)) uses double-checked
locking over a `shared_mutex`: a shared lock for the common cache-hit path, an
exclusive lock only to `open` + `fstat` + insert on a miss. It caches the fd and the
file size, so repeated GETs of the same object skip the syscalls.

### `handle_get` — the ping-pong double-buffer loop

`handle_get` ([s3_server.cpp:221](../server/s3_server.cpp#L221)) is where bytes move:

1. Resolve the file via `get_fd` (`404` if absent). If a `Range` header is present,
   `parse_range` validates it against the file size (`416` if invalid) and sets the
   `[offset, length)` window; otherwise the whole file.
2. Base64-decode the descriptor, `acquire_slot`, and clamp the chunk size.
3. **Prefill** buf[0] with the first chunk (`plan_chunk` + `pread_full`).
4. **Loop** over chunks with a 0/1 ping-pong index `cur`:
   - if there is a next chunk, spawn a `std::thread` that runs
     `server_->handleGetObject(...)` (the `RDMA_WRITE` of buf[`cur`]) **while** the
     main thread `pread_full`s the next chunk into buf[`nxt`], then `join`s. This
     overlaps the file read with the network write.
   - the **last** chunk skips the pipeline and just RDMA-writes.
   - advance `file_off`, `remote_off` (`= remote_addr + bytes_done`), and `cur`.
5. `release_slot`, return `total_written` and `206` (range) or `200` (full).

`plan_chunk` ([s3_server.cpp:74](../server/s3_server.cpp#L74)) handles O_DIRECT: when
`--direct-io` is on, the read offset is aligned down to 4 KiB (`align_down`), the
length aligned up (`align_up`), and `local_off` records how far into the staging
buffer the real data starts so the RDMA write skips the alignment padding.
`pread_full` ([s3_server.cpp:51](../server/s3_server.cpp#L51)) loops over short reads
(retrying `EINTR`), which O_DIRECT with large buffers can produce.

## Wire protocol reference

All header constants live in [common/s3_util.hpp](../common/s3_util.hpp).
Only standard cuObject headers are used (see
[NVIDIA cuObject docs](https://docs.nvidia.com/gpudirect-storage/cuobject/index.html)).

| Header | Direction | Meaning |
|--------|-----------|---------|
| `x-amz-rdma-token` | client → server | base64 of `cufileRDMAInfo_t::desc_str` (opaque RDMA descriptor; contains remote buffer address, rkey, GID, etc.) |
| `x-amz-rdma-reply` | server → client | RDMA status: `"200"` on success |

Standard `Range: bytes=a-b` is also used for range GETs; the server answers `206`.
The server extracts the client's remote buffer base address from the RDMA
descriptor (DC_V1 format: colon-separated hex fields, first field = `buf_address`).

## common/ helpers

- **[base64.hpp](../common/base64.hpp)** — minimal RFC 4648 encode/decode, used only
  to carry the binary RDMA descriptor through a text HTTP header.
- **[sha256.hpp](../common/sha256.hpp)** — `sha256_hex` via OpenSSL. Present and
  included by the client, but **not called** (see below).
- **[s3_util.hpp](../common/s3_util.hpp)** — header constants plus
  `parse_object_path` (rejects empty parts and `.`/`..` traversal) and `parse_range`
  (requires `bytes=a-b`, validates `a <= b < file_size`).
- **[cuobj_check.hpp](../common/cuobj_check.hpp)** — `LOG_*`, `CUDA_CHECK`, and
  `CUOBJ_CHECK` (fail-fast `abort` macros). Note the comment at
  [cuobj_check.hpp:13-16](../common/cuobj_check.hpp#L13): `cuObjErr_t` is defined by
  **both** `cuobjclient.h` and `cuobjserver.h` with no cross-guard, so this header
  deliberately includes neither — each TU includes the correct cuObject header first.

## Doc-vs-code discrepancies

Noted here so `design.md` can be reconciled later:

1. **No SHA256 verification.** `design.md` says the client "verifies SHA256", but
   `sha256_hex` is never called. `sha256.hpp` is only `#include`d at
   [client/main.cpp:31](../client/main.cpp#L31); the benchmark client performs no
   integrity check on fetched data.
2. **Chunking is server-side, via one callback.** The data path is *not* "one
   callback per chunk". `cuObjGet` calls `get_callback` once per object; chunking and
   pipelining happen entirely inside the server's `handle_get` loop.
