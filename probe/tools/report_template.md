# Product-probe report — <vendor> / <date>

Rig: <host>  NIC: <model/fw>  GPU: <model/driver>  cuObject SDK: <version>
Gateway commit/profile: <unsigned dev profile>  PCIe: <same/diff root complex>
Loaded library hashes: cuobjclient=<sha> cuobjserver=<sha> cufile=<sha>

> Evidence boundary: this report is a SERIAL measurement. Concurrency and
> lifecycle are UNVERIFIED. Product completion-predicate values are OBSERVED
> intel, NOT the official step-2 freeze. A contradiction with the priors
> (reply "200", statuses {200,206}) escalates to the user, never silent.

## Evidence layers (never merge these columns)

| Fact | Layer | Value |
|---|---|---|
| reply-tag value / status set / tag-only-on-success | product WIRE emission (P3 tshark) | <from parse_reply_tags.py> |
| client reaction to dropped/wrong/negative reply | stock-client reaction (P4 mitm) | <...> |
| cuObjGet return under each anomaly | SDK return mapping (P4) | <...> |
| bytes actually landed | data landing (probe digest / --out sha256) | <...> |
| product outcome / STOP | contract mapping — INFERENCE (map_scope_outcome.py) | <...> |

## (1) Perf baseline

- Fake server+client baseline (same node/day): <Gbps> at <threads>×<size>, mode=<>
- Product perf ladder (P2): <Gbps> at <threads>×<size>
- Single-shot integrity (--out + sha256 vs manifest): <match/mismatch>
- Connection/TIME_WAIT watch: <clean / churn observed>

## (2) Control-plane matrix (P1 curl)

| Check | Expected | Observed |
|---|---|---|
| HEAD size | 200 + Content-Length | <> |
| token-less GET | 4xx | <> |
| malformed token | error, no wild write | <> |
| 416 / edge ranges | 416 | <> |
| path-style | accepted | <> |
| PUT/POST/DELETE | rejected | <> |

## (3) Completion-predicate observation (P3)

```
accepted_status = <>
reply_tag_values = <>   (byte-exact, incl. casing/whitespace)
server_tag_only_on_success = <true|false|CONTRADICTION>
```

## (4) Injection matrix (P4 mitm)

| Injection | Client outcome | tshark wire | Note |
|---|---|---|---|
| drop_reply | <unchanged — client ignores tag> | tag absent | wire-only observable |
| wrong_reply | <unchanged> | tag=500 | wire-only observable |
| status_negative | <client -1 / error> | 503, no tag | rewritten-to-negative |
| range_rewrite off | <slice sha256 match?> | Range rewritten | arbitrary-offset probe |

## (5) Registration scope × interior landing (P5 cuobj_scope_probe)

| Case | Verdict | Detail |
|---|---|---|
| b0 base | <> | health baseline |
| i0 interior | <> | |
| s0 cross-instance base | <> | |
| si cross-instance interior | <> | **Sirius target shape** |
| r two registrars | <> | |
| p disjoint slices | <> | |
| m slice-token | <> | diagnostic only |

Mapped outcome (map_scope_outcome.py): **<1|2|3|STOP|INCONCLUSIVE>** —
<reason>. [serial only; concurrency + lifecycle UNVERIFIED]

## (6) Online sustained integrity (P6 client-verify — PLANNED)

Status: <not run — client-verify not yet built / results if run>
- checked X/Y requests and bytes; sampling seed <>; max unsampled gap <>
- mismatches: <>

## Verdict summary

- Serial base+offset target shape viable on this product: **<yes/no/inconclusive>**
- ~80% functional coverage result: <pass/partial>
- Named gaps: concurrency (deferred), lifecycle (unverified), sustained online
  integrity (client-verify pending), swap-detection (Track B).
- Contradictions with priors requiring escalation: <none / list>
