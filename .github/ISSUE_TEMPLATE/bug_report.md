---
name: Bug report
about: Report a problem with the build, the control path, or the RDMA data path
title: "[bug] "
labels: bug
---

## Description

A clear description of what went wrong and what you expected instead.

## Reproduction

Steps / commands to reproduce (server command line, client command line, object set):

```bash
# server
# client
```

## Environment

> The RDMA data path is highly environment-sensitive — please fill this in.

- Role(s) affected: client / server / both
- NIC (e.g. CX-6) and link rate:
- MLNX_OFED version:
- CUDA version / GPU (client):
- cuObject version:
- cuFile / GDS version:
- OS / distro + kernel:
- `nvidia_peermem` loaded (client)? `rdma_dev_addr_list` set in `cufile.json`?

## Logs

Relevant server/client output, `gdscheck -p` output, and any `cufile.log`.
