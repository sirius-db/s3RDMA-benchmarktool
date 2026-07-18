#!/usr/bin/env python3
# Copyright 2025, Sirius Contributors. Apache-2.0.
#
# Parse `tshark -T json -Y http.response` output of the product control plane
# and emit the Probe-B OBSERVATION table (product WIRE emission only):
#   accepted_status set, exact x-amz-rdma-reply value(s)/casing/absence,
#   server_tag_only_on_success.
#
# Capture with, e.g.:
#   tshark -i <iface> -f "tcp port <port>" -T json -Y http.response > product-http.json
#
# The reply header is non-standard, so tshark carries it as a raw line under
# http.response.line ("x-amz-rdma-reply: 200\r\n") rather than a named field;
# this parser scans those lines case-insensitively.

import json
import sys

REPLY_HEADER = "x-amz-rdma-reply"


def _walk_http_layers(doc):
    """Yield each packet's http layer dict from tshark -T json output."""
    if isinstance(doc, dict):
        doc = [doc]
    for pkt in doc:
        layers = (pkt.get("_source", {}) or {}).get("layers", {}) or {}
        http = layers.get("http")
        if http is None:
            continue
        for h in (http if isinstance(http, list) else [http]):
            if isinstance(h, dict):
                yield h


def _response_code(http):
    code = http.get("http.response.code")
    if isinstance(code, list):
        code = code[0] if code else None
    try:
        return int(code)
    except (TypeError, ValueError):
        return None


def _reply_tag(http):
    """Return the reply header's value (str), or None if absent."""
    # tshark may surface the header as a named field OR inside response lines.
    for key, val in http.items():
        if key.lower() == REPLY_HEADER:
            return (val[0] if isinstance(val, list) else val)
    lines = http.get("http.response.line", [])
    if isinstance(lines, str):
        lines = [lines]
    for raw in lines:
        if not isinstance(raw, str):
            continue
        stripped = raw.strip()
        if ":" in stripped:
            name, _, value = stripped.partition(":")
            if name.strip().lower() == REPLY_HEADER:
                return value.strip()
    return None


def observe(doc):
    """Return the observation table from a parsed tshark-json document."""
    statuses = {}          # code -> count
    tag_values = {}        # exact reply value -> count
    tag_on_success = 0
    tag_on_failure = 0
    success_no_tag = 0
    responses = 0
    for http in _walk_http_layers(doc):
        code = _response_code(http)
        if code is None:
            continue
        responses += 1
        statuses[code] = statuses.get(code, 0) + 1
        tag = _reply_tag(http)
        success = code in (200, 206)
        if tag is not None:
            tag_values[tag] = tag_values.get(tag, 0) + 1
            if success:
                tag_on_success += 1
            else:
                tag_on_failure += 1
        elif success:
            success_no_tag += 1
    return {
        "responses": responses,
        "accepted_status": sorted(c for c, _ in statuses.items() if c in (200, 206)),
        "status_histogram": {str(k): v for k, v in sorted(statuses.items())},
        "reply_tag_values": tag_values,          # byte-exact, incl. casing/whitespace
        "reply_tag_present_on_success": tag_on_success,
        "reply_tag_present_on_failure": tag_on_failure,   # a non-2xx carrying a tag is a CONTRADICTION
        "success_without_tag": success_no_tag,
        "server_tag_only_on_success": (tag_on_failure == 0 and tag_on_success > 0),
    }


def main(argv):
    if "--selftest" in argv:
        return selftest()
    paths = [a for a in argv[1:] if not a.startswith("--")]
    if not paths:
        sys.stderr.write("usage: parse_reply_tags.py <tshark-json> [--selftest]\n")
        return 2
    with open(paths[0], "r", encoding="utf-8") as fh:
        doc = json.load(fh)
    print(json.dumps(observe(doc), indent=2))
    return 0


def selftest():
    sample = [
        {"_source": {"layers": {"http": {
            "http.response.code": "200",
            "http.response.line": ["x-amz-rdma-reply: 200\r\n", "Content-Type: text/plain\r\n"],
        }}}},
        {"_source": {"layers": {"http": {
            "http.response.code": ["206"],
            "http.response.line": ["X-Amz-Rdma-Reply: 200\r\n"],   # casing variant
        }}}},
        {"_source": {"layers": {"http": {
            "http.response.code": "500",       # failure carries NO tag
            "http.response.line": ["Content-Length: 6\r\n"],
        }}}},
    ]
    obs = observe(sample)
    checks = [
        ("accepted_status", obs["accepted_status"] == [200, 206]),
        ("tag_values", obs["reply_tag_values"] == {"200": 2}),
        ("tag_on_success", obs["reply_tag_present_on_success"] == 2),
        ("no_tag_on_failure", obs["reply_tag_present_on_failure"] == 0),
        ("tag_only_on_success", obs["server_tag_only_on_success"] is True),
        ("responses", obs["responses"] == 3),
    ]
    ok = sum(1 for _, c in checks if c)
    for name, c in checks:
        print("%s %s" % ("PASS" if c else "FAIL", name))
    total = len(checks)
    print("SELFTEST %s %d/%d" % ("PASS" if ok == total else "FAIL", ok, total))
    return 0 if ok == total else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
