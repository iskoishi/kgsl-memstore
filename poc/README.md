# PoC

Self-contained verification of the mechanism described in `../README.md` (§5.3).

## What it proves

One executable, three contexts, one control arm. The result is the exit code.

| Bit | Meaning |
|---|---|
| `0x80` | control arm's `0x33` call succeeded |
| `0x40` | experiment arm's `0x33` call succeeded |
| `0x20` | forged value observed from the CPU side |
| `0x10` | experiment arm's fence was signalled (POLLIN) |
| `0x08` | forged value still resident after the fence call |
| `0x01` | control arm's fence was signalled (should be 0) |

The exit code is this bit field as an integer. **`0xF8` (248) is a clean run.**
`0xF0` (240) is also acceptable: it means the post-fence readback raced the GPU and
the forged value had already been overwritten, which does not change the conclusion.
A set `0x01` would contradict the mechanism and has not been observed.

## Build

The binary uses raw syscalls and no libc, so it can be pushed to a device and run
without any runtime dependencies.

```sh
aarch64-alpine-linux-musl-gcc -static -O2 -o probe_fence probe_fence.c
```

## Run

Push it to a device with a working KGSL device node and execute it as an ordinary
third-party app uid (no `su`, no `adb shell` shell uid required — the shell path is
only used because it is convenient).

Results are written to `/data/local/tmp/pf.res`; the exit code carries the same
information as a bit field.

```sh
adb push probe_fence /data/local/tmp/
adb shell /data/local/tmp/probe_fence; echo "RC=$?"
adb shell cat /data/local/tmp/pf.res
```

## Interpretation

- Both arms return `-22` from the `0x33` call: `CONFIG_SYNC_FILE` was not compiled
  in on this kernel. The very first `0x33` call is a safe way to probe for this,
  because that path returns `-EINVAL` before dereferencing anything.
- Experiment arm `POLLIN`, control arm timeout: the mechanism in the report holds.
- Experiment arm timeout: this build's kernel does not accept the forged retire
  value. Compare `__adreno_readtimestamp()` — the `KGSL_TIMESTAMP_RETIRED` branch
  must read `memstore` directly rather than a software-maintained counter.

## Safety

The write target is `memstore[28008]`, a slot with no kernel consumer on the
reference device. Nothing here writes to an active context's slot, and the program
does not attempt any cross-process operation.
