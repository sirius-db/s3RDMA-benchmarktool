# Runbook — perf baseline and probing a real S3-over-RDMA product

This runbook uses the benchmark's client to (1) establish a hardware perf
baseline with the shipped `client` + `server`, (2) probe a real vendor product
(any cuObject-gateway-compatible endpoint) with the stock client plus off-the-
shelf tools, covering most of the functional surface with no code changes and
naming the rest, and (3) run the `cuobj_scope_probe` binary and the planned
`client-verify` binary, then post-process their output.

All runs assume the **rig profile**: unsigned, plain HTTP, path-style,
single endpoint. TLS and SigV4 are a separate production-qualification phase and
are out of scope here. The RDMA data plane is NIC-to-NIC RoCE; the HTTP control
plane is plaintext, so it is externally observable (tcpdump/tshark) and
perturbable (mitmproxy) without touching any binary.

Build all binaries with `pixi run fetch-sdk && pixi run build` on an RDMA node
(Mellanox NIC + GPU, GPUDirect Storage). See [design.md](design.md) and
[test.md](test.md) for the environment.

| Binary | Role | Needs |
|---|---|---|
| `build/client/client` | stock N-thread GET benchmark | RDMA NIC + GPU |
| `build/server/server` | fake-S3 gateway (reference) | RDMA NIC + GPU |
| `build/probe/cuobj_scope_probe` | registration-scope × interior-landing probe (serial) | RDMA NIC + GPU |
| `build/probe/client-verify` | poison-backed online integrity (planned) | RDMA NIC + GPU |

Post-processing (rig-free Python, `probe/tools/`): `map_scope_outcome.py`,
`parse_reply_tags.py`, `mitm_inject.py`, `report_template.md`. Each `*.py`
supports `--selftest`.

Result vocabulary: **PASS / FAIL / INCONCLUSIVE / ENV_ERROR** per probe case;
`map_scope_outcome.py` adds product outcomes **1 / 2 / 3 / STOP / INCONCLUSIVE**.
The C++ never derives a product outcome — that mapping is done only by the
Python.

## 1. Hardware perf baseline (shipped client + server)

Establish the RDMA link's achievable line rate on this rig with the reference
implementation, before testing any vendor. Every later comparison is read
against this number.

On the server (RDMA) node:

```bash
pixi run gen-data                       # writes data/demo-bucket/* and prints sha256sums — SAVE them
./build/server/server --data-dir data --ip <rdma_ip> --rdma-port 18515 --port 8080
```

On the client (RDMA) node:

```bash
./build/client/client --server <server_host>:8080 --bucket demo-bucket \
    --objects obj-8m-0,obj-8m-1,obj-8m-2,obj-8m-3,obj-8m-4,obj-8m-5,obj-8m-6,obj-8m-7 \
    --mode gpu --threads 8 --duration 30
```

Read the aggregate `Gbps` line. Sweep `--threads 1,2,4,8` and `--range-size`
to sketch the size/thread curve. Record host, NIC/firmware, GPU/driver, date,
and cache posture next to the number — the baseline is per-rig, per-day; never
cross-compare rigs.

Two caveats that bound what the baseline proves:

- The stock client reports the requested byte count and does not read the RDMA
  reply tag, so the bandwidth assumes full delivery. Confirm the bytes are real
  at least once: `--out dump.bin` (single-thread) then compare `sha256sum
  dump.bin` against the saved gen-data digest.
- The callback builds a fresh HTTP client per GET (~2700 GET/s at 91 Gbps /
  4 MiB); watch TIME_WAIT / ephemeral ports on long runs.

## 2. Probing a vendor product — coverage and the gap

Point the same stock binaries at the vendor's gateway
(`PRODUCT_HTTP=<host:port>`, unsigned/plain-HTTP/path-style). Most of the
functional surface is reachable with **no code changes**; the rest needs the
probe binary and the planned `client-verify` — see §3 and the gap table.

- **P1 — control-plane matrix (curl, no client):** HEAD size, token-less GET
  rejection, malformed token, 416/edge ranges, path-style acceptance, verb
  rejection.
- **P2 — smoke + perf ladder (client):** `--threads 1..8` × sizes, host+gpu;
  compare against the §1 fake baseline re-run on the same node/day. This is the
  only perf number.
- **P3 — passive observation (byte-exact, zero touch):**

  ```bash
  sudo tshark -i <iface> -f "tcp port <product_http_port>" -T json -Y http.response \
      > product-http.json
  python3 probe/tools/parse_reply_tags.py product-http.json
  ```

  Emits the product WIRE observation: `accepted_status`, the exact
  `x-amz-rdma-reply` value(s)/casing/absence, and `server_tag_only_on_success`.

- **P4 — proxy reaction matrix (mitmproxy in front; RDMA bypasses it):**

  ```bash
  mitmdump --mode reverse:http://<product_http> -p <proxy_port> \
      -s probe/tools/mitm_inject.py --set inject=status_negative   # or drop_reply / wrong_reply / range_rewrite
  ./build/client/client --server <proxy_host>:<proxy_port> --bucket b --object k \
      --mode gpu --threads 1 --duration 5
  ```

  This measures the **stock-client reaction and the SDK return mapping**, not
  the product's byte-count semantics. `status_negative` rewrites a successful
  upstream response to 503 (a "successful upstream response rewritten to
  negative" — the proxy cannot know the RDMA write completed). The stock client
  ignores the reply tag, so `drop_reply` / `wrong_reply` are observable only on
  the wire (tshark), not in client behavior.

- **P5 — `cuobj_scope_probe`:** see §3.
- **P6 — online sustained integrity (`client-verify`, planned):** see §3.
- **P7 — report:** fill `probe/tools/report_template.md`, keeping the evidence
  layers separate (product-wire emission / stock-client reaction / SDK return /
  data landing / contract mapping = inference).

### What passes with no code, and the gap

| Reachable with zero code | Gap (probe / client-verify / hardware phase) |
|---|---|
| Interop + bandwidth position (P2) | Sustained-load online integrity (`client-verify`, planned) |
| Same-key concurrency at the HTTP/slot level (P2) | Concurrent interior-slot delivery decisiveness |
| Reply-tag value / status set / tag-only-on-success (P3) | These are OBSERVED values, not a frozen protocol constant set |
| missing / wrong-reply / status-negative / offset-range injection (P4) | Data-plane short-write injection — impossible against a real product |
| Single-shot integrity (`--out` + sha256) | N-way concurrent + lifecycle proof (deferred) |
| Registration scope × interior landing, serial (P5) | The interior GET is the one thing curl/proxy cannot fake — the probe fills it |

The gap is deliberate. `cuobj_scope_probe` is a **serial** measurement;
concurrency and lifecycle are not verified by it (see §3).

## 3. The probe binary and the improved client

### `cuobj_scope_probe` — registration scope × interior landing (serial)

The stock client always GETs into its registered base pointer, one session per
thread. A GPU-native consumer instead registers a large arena once and GETs into
**interior** slot pointers. Only a program linking the SDK can generate those
descriptors, so this cannot be tested with curl or a proxy. The probe answers,
decisively: **is a serial `base+offset` device read into a registered arena
physically viable on this product and SDK?**

```bash
P=./build/probe/cuobj_scope_probe
for exp in b0 i0 s0 si r p; do
  $P --exp $exp --bucket b --object obj-4m-0 --server <product_http> --mode gpu \
     --xfer 4194304 --expect-sha <sha256 of the first 4 MiB of obj-4m-0> \
     > scope-$exp.jsonl 2> scope-$exp.err
done
$P --exp m --bucket b --object obj-4m-0 --server <product_http> --mode gpu \
   --expect-sha <sha> > scope-m.jsonl          # alternate slice-token path (diagnostic only)

python3 probe/tools/map_scope_outcome.py scope-*.jsonl
```

Cases: `b0` base landing (health baseline) · `i0` interior landing · `s0`
cross-instance base · `si` cross-instance interior (the target shape) · `r` two
registrars · `p` disjoint slices · `m` explicit slice-token (diagnostic only).
A landing is **PASS** only with `--expect-sha` matching the full payload,
`rc == xfer`, exactly one callback, intact guard bands, and guaranteed GPU write
visibility; otherwise FAIL / INCONCLUSIVE / ENV_ERROR.

Scope boundary (the probe emits a `boundary` JSONL event stating this):
concurrency is deferred (`--concurrent` never yields a decisive PASS), lifecycle
is unverified, and the `cuMemObjPutDescriptor` return code is not trusted (it is
0 unconditionally on the pinned SDK). `map_scope_outcome.py` reads the per-case
verdicts and maps them to outcome 1 (target shape viable, serial) / 2 / 3 /
STOP / INCONCLUSIVE — always annotated "serial only".

### `client-verify` — poison-backed online integrity (planned)

The stock client reports the requested byte count and checksums nothing, so its
"success" during a long soak is blind. `client-verify` is a separate binary
(it recompiles the frozen `client` sources and adds a verify loop; the shipped
`client` is unchanged). Intended use:

```bash
# planned interface
./build/probe/client-verify --server <product_http> --bucket b \
    --objects obj-4m-0,obj-4m-1,... --mode gpu --threads 8 --duration 600 \
    --verify-manifest manifest.tsv --verify-every 64 --verify-abort-all
./build/probe/client-verify ... --iterations 1 --verify-once   # single-shot spot check
```

Each sampled GET poisons the destination with a nonce, writes guard canaries,
GETs, applies the GPUDirect visibility flush, and verifies the full-payload
digest and intact guards, so a product no-op or short write during load is
caught rather than hidden by the reported count. The manifest is strict (a
verified run refuses to start unless every requested span has a matching digest)
and sampling is aliasing-free. Status: the reusable core is in
`common/verify/{manifest,sampler}` (self-tested); the binary is a later
hardware-phase increment.

## Handling and safety

- The RDMA descriptor carries addresses, rkey, and GID. Redact captured pcaps
  before sharing.
- The proxy is a rig test aid only; never place it on the RDMA data path.
- Archive the exact commands, the environment fingerprint (NIC/GPU/driver and
  the loaded cuObject/cuFile library versions), and a redacted pcap with each
  report.

## Rig-preflight experiments

`cuobj_rig_probe` keeps concurrent and lifecycle measurements separate from
the serial `cuobj_scope_probe`. Archive the exact command, SDK/library
identity, fixture manifest, effective configuration, JSONL and process status.
Apply the approved memory/traffic limits and a bounded parent/wrapper to
every case. Watchdog termination is abnormal evidence.

| `--exp` | Measurement and qualification |
|---|---|
| `attr` | Record ordering/flush attributes and query failures. This is observational; it does not qualify the flush path. |
| `reg` | Measure base/last-slot callback capacity on the recorded GPU allocation. Refuse an insufficient bound; never clamp S. Qualify other allocation/interior-pointer profiles separately. |
| `diag` | Correlate request, callback, **raw-header**, SDK-return and authorized landing evidence. The rig probe speaks HTTP/1.1 through its own byte-preserving reader (`common/raw_http.hpp`): the exact response head is recorded (`response_head_hex`), fields are never decoded or merged, and completion authority requires a **complete, well-framed HTTP message** under the supported profile — one or more identical valid decimal `Content-Length` values satisfied, or a close-delimited body ended by EOF; chunked and every other transfer-coding combination are recorded but never qualify; within caps; no transport failure, status 200/206, exactly one plain-digit `x-amz-rdma-reply` matching the convention, exactly one plain-digit `x-amz-rdma-bytes-transferred` equal to the expected landing length and (standard contract) an empty HTTP body. Anything else keeps the GET **unknown** (retained, not read, not released). Keep request length separate from expected landing length (`--expected-bytes`), including both EOF cases. No digest means no verified-delivery PASS. |
| `rt-guard` | Inject only after a qualified complete transfer. Later invocations emit nothing. Primary PASS needs an **ordered re-entry of the same logical request** (same accepted size/offset, begun after the injected return), exactly one complete token-bearing request message written by the helper's own validated writer (no redirects, no retries; partial writes are unqualified), a negative SDK return and a landing that matches the supplied digest. An unchanged poison landing after the injected return is reported as its own outcome (`landing_unchanged_poison_after_injected_return`). A wire capture remains the independent emission witness. No re-invocation is INCONCLUSIVE. Run non-retryable and retry-disabled controls separately with an independently inspected configuration receipt. `rdma_io_retry_count_observed` is an unqualified substring observation (`retry_configuration_qualified=false`). |
| `c` | One registrar and one session per worker; do not share a session. Qualify the actual guarded layout, complete poison production, and validate every GET before reuse. Record distinct entered-GET overlap and all per-GET checks. No overlap is INCONCLUSIVE. The reference oracle is relative to verified serial reads from the same endpoint. |
| `exit` | Establish a known-complete GET, then retain the graph through normal return from main. Record the bounded parent result and a separate post-run health check. The labelled `_exit` variant is an abrupt observation, not normal-exit qualification. |

Unknown completion stops further submissions and reuse. Read neither payload
nor guards and perform no ordinary deregistration/free. Use only the approved
isolated-child lifetime and peer/process cleanup boundary; otherwise that arm
is NOT RUN. A timeout or later healthy probe is not a drain proof.

Run `./build/probe/cuobj_loader_harness --order cufile-first` and
`--order cuobj-first` in fresh processes. Preserve startup, load,
constructor (via `libcuobj_loader_fixture.so`), binding and mapped-identity
receipts. Both orders must show one cuFile object while the client is alive
and supply the required binding receipts. The harness currently reports
PARTIAL (exit 4) because symbol-origin evidence is unavailable; loader
qualification remains pending.

For a compatible gateway, use `--gateway` and the configured `--region`;
credentials come from the AWS environment variables. Keep count bases
explicit: callback, attempt, emission and capture are different evidence.
These diagnostics do not replace the stock benchmark's integrity, profile,
complete-log and zero-worker-failure eligibility rules.
