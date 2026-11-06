# Copyright 2025, Sirius Contributors. Apache-2.0.
#
# mitmproxy reverse-proxy addon: perturb the plaintext HTTP control plane of an
# S3-over-RDMA product WITHOUT touching either binary. The RDMA data plane is a
# separate NIC-to-NIC path and bypasses the proxy, so these injections exercise
# the CLIENT'S reaction to control-plane anomalies, not the product's data path.
#
# Run:
#   mitmdump --mode reverse:http://<product_host:port> -p <proxy_port> \
#            -s mitm_inject.py --set inject=<mode>
# then point the benchmark client at <proxy_host:proxy_port>.
#
# Modes:
#   drop_reply       remove x-amz-rdma-reply on an otherwise-successful response
#   wrong_reply      replace x-amz-rdma-reply with a wrong value ("500")
#   status_negative  rewrite a successful upstream response (2xx) to 503 AFTER
#                    it arrived — a "successful upstream response rewritten to
#                    negative". The proxy cannot know the RDMA write completed;
#                    do NOT call this "post-data-completion".
#   range_rewrite    rewrite the request Range to a non-zero offset of equal
#                    length (bytes=OFF-(OFF+len-1)) to probe arbitrary-offset
#                    handling; pair with client --out + dd + sha256 slice check.
#
# NOTE: the stock client ignores the reply tag entirely, so drop_reply /
# wrong_reply are observable only via tshark on the wire, not via client
# behavior. Only status_negative changes the stock client's outcome.

REPLY_HEADER = "x-amz-rdma-reply"

try:
    from mitmproxy import ctx, http
except ImportError:  # allow import for syntax checks off the rig
    ctx = None
    http = None


def _mode():
    return ctx.options.inject if ctx is not None else "off"


def load(loader):
    loader.add_option(
        name="inject", typespec=str, default="off",
        help="drop_reply | wrong_reply | status_negative | range_rewrite | off",
    )
    loader.add_option(
        name="range_offset", typespec=int, default=1048576,
        help="offset for range_rewrite (bytes); length is preserved",
    )


def request(flow):
    if _mode() != "range_rewrite":
        return
    rng = flow.request.headers.get("Range")
    if not rng or not rng.startswith("bytes="):
        return
    body = rng[len("bytes="):]
    if "-" not in body:
        return
    a, _, b = body.partition("-")
    try:
        start, end = int(a), int(b)
    except ValueError:
        return
    length = end - start + 1
    off = ctx.options.range_offset
    flow.request.headers["Range"] = "bytes=%d-%d" % (off, off + length - 1)
    ctx.log.info("range_rewrite: %s -> %s" % (rng, flow.request.headers["Range"]))


def response(flow):
    mode = _mode()
    if mode == "off":
        return
    status = flow.response.status_code
    success = status in (200, 206)
    if mode == "drop_reply" and success:
        if REPLY_HEADER in flow.response.headers:
            del flow.response.headers[REPLY_HEADER]
            ctx.log.info("drop_reply: removed %s" % REPLY_HEADER)
    elif mode == "wrong_reply" and success:
        flow.response.headers[REPLY_HEADER] = "500"
        ctx.log.info("wrong_reply: set %s=500" % REPLY_HEADER)
    elif mode == "status_negative" and success:
        # The upstream (incl. any RDMA write) already returned success; we
        # rewrite the HTTP status to a failure. The tag is dropped to match a
        # real failure response's shape.
        flow.response.status_code = 503
        flow.response.reason = "Service Unavailable"
        if REPLY_HEADER in flow.response.headers:
            del flow.response.headers[REPLY_HEADER]
        ctx.log.info("status_negative: 2xx rewritten to 503")
