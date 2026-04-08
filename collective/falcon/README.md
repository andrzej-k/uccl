# Falcon Transport — UCCL Collective

Transport plugin for [OCP-NET-Falcon](https://github.com/opencomputeproject/OCP-NET-Falcon) network adapters.

## Status

**Stub** — interfaces are defined but not yet implemented.

## Files

| File | Purpose |
|------|---------|
| `transport_config.h` | Tunable parameters and constants |
| `transport.h` | Core classes: `Channel`, `FalconEngine`, `FalconEndpoint` |
| `transport.cc` | Transport implementation (TODO) |
| `nccl_plugin.cc` | NCCL v8 net plugin interface |
| `Makefile` | Build rules |

## Building

```bash
make build
```

This produces:
- `libnccl-net-falcon.so` — NCCL plugin shared library
- `libfalcon.a` — static library for linking

## Usage with NCCL

```bash
export NCCL_NET_PLUGIN=libnccl-net-falcon.so
# then run your NCCL workload
```
