#!/usr/bin/env python3
# Copyright 2025, Sirius Contributors. Apache-2.0.
#
# Map cuobj_scope_probe per-case JSONL verdicts onto the registration-scope
# decision matrix (contract-family "section 8"). This mapping lives ONLY here,
# never in the probe binary: the C++ emits per-case measurement verdicts, the
# product outcome is derived by this rig-free script.
#
#   outcome 1  SI serial-verified  -> session-independent + interior-capable
#              (single arena registrar + per-worker sessions is viable)
#   outcome 2  R                   -> re-register per instance (record cost)
#   outcome 3  P                   -> per-instance disjoint slices only
#   STOP       only B0 works, every other candidate a clean failure
#   INCONCLUSIVE  B0 not clean, or the deciding case is not decisive
#
# Every outcome is annotated: this is a SERIAL measurement; concurrency and
# lifecycle are UNVERIFIED (design §3.2).

import json
import sys

CASES = ("b0", "i0", "s0", "si", "r", "p", "m")


def case_verdict(lines):
    """Last 'result' event's verdict for one case's JSONL lines, or None."""
    verdict = None
    for ln in lines:
        try:
            obj = json.loads(ln)
        except (ValueError, TypeError):
            continue
        if obj.get("event") == "result":
            verdict = obj.get("verdict")
    return verdict


def load(paths):
    """Group JSONL lines by their 'case' tag across all input files."""
    by_case = {}
    for p in paths:
        with open(p, "r", encoding="utf-8") as fh:
            for ln in fh:
                ln = ln.strip()
                if not ln:
                    continue
                try:
                    tag = json.loads(ln).get("case")
                except (ValueError, TypeError):
                    continue
                by_case.setdefault(tag, []).append(ln)
    return by_case


def decide(verdicts):
    """verdicts: {case -> 'PASS'|'FAIL'|'INCONCLUSIVE'|'ENV_ERROR'|None}.

    Returns (outcome, reason). Pure; unit-tested by --selftest.
    """
    b0 = verdicts.get("b0")
    si = verdicts.get("si")
    r = verdicts.get("r")
    p = verdicts.get("p")

    # The health baseline must be clean or nothing else is interpretable.
    if b0 != "PASS":
        return ("INCONCLUSIVE",
                "B0 health baseline is not PASS (%s); no scope conclusion" % b0)

    if si == "PASS":
        return ("1",
                "SI (cross-instance interior landing) PASS: session-independent "
                "+ interior-capable [serial only; concurrency+lifecycle UNVERIFIED]")
    if r == "PASS":
        return ("2",
                "SI not PASS but R (re-register per instance) PASS: register per "
                "instance, record startup cost [serial only]")
    if p == "PASS":
        return ("3",
                "SI/R not PASS but P (disjoint slices) PASS: per-instance disjoint "
                "registered slices [serial only]")

    # None of the interior/scope candidates are viable. STOP only if their
    # failures are decisive (FAIL), not merely INCONCLUSIVE/ENV.
    deciding = {k: verdicts.get(k) for k in ("i0", "s0", "si", "r", "p")}
    if any(v in ("INCONCLUSIVE", "ENV_ERROR", None) for v in deciding.values()):
        return ("INCONCLUSIVE",
                "B0 clean but a deciding case is non-decisive (%s); cannot map to "
                "STOP without clean failures" % deciding)
    return ("STOP",
            "B0 PASS but I0/S0/SI/R/P all cleanly FAIL: Sirius arena/interior shape "
            "not viable on this product/SDK [serial measurement]")


def main(argv):
    if "--selftest" in argv:
        return selftest()
    paths = [a for a in argv[1:] if not a.startswith("--")]
    if not paths:
        sys.stderr.write("usage: map_scope_outcome.py <scope-*.jsonl> [--selftest]\n")
        return 2
    by_case = load(paths)
    verdicts = {c: case_verdict(by_case.get(c, [])) for c in CASES}
    outcome, reason = decide(verdicts)
    print(json.dumps({
        "verdicts": verdicts,
        "outcome": outcome,
        "reason": reason,
        "boundary": "serial registration-scope x interior-landing; "
                    "concurrency deferred; lifecycle unverified",
    }, indent=2))
    return 0


def selftest():
    cases = [
        ({"b0": "PASS", "si": "PASS"}, "1"),
        ({"b0": "PASS", "si": "FAIL", "r": "PASS"}, "2"),
        ({"b0": "PASS", "si": "FAIL", "r": "FAIL", "p": "PASS"}, "3"),
        ({"b0": "PASS", "i0": "FAIL", "s0": "FAIL", "si": "FAIL",
          "r": "FAIL", "p": "FAIL"}, "STOP"),
        ({"b0": "FAIL", "si": "PASS"}, "INCONCLUSIVE"),   # baseline red dominates
        ({"b0": "PASS", "i0": "INCONCLUSIVE", "s0": "FAIL", "si": "FAIL",
          "r": "FAIL", "p": "FAIL"}, "INCONCLUSIVE"),     # non-decisive => not STOP
        ({"b0": "PASS", "si": "ENV_ERROR", "r": "FAIL", "p": "FAIL"}, "INCONCLUSIVE"),
    ]
    ok = 0
    for i, (verdicts, want) in enumerate(cases):
        got, reason = decide(verdicts)
        status = "PASS" if got == want else "FAIL"
        if got == want:
            ok += 1
        print("%s case %d: want=%s got=%s" % (status, i, want, got))
    total = len(cases)
    print("SELFTEST %s %d/%d" % ("PASS" if ok == total else "FAIL", ok, total))
    return 0 if ok == total else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
