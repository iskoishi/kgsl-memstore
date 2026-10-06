# Security

This repository publishes a vulnerability research disclosure.

## What is disclosed here

A property of the Qualcomm KGSL kernel graphics driver in which kernel-trusted GPU
synchronization state (`memstore`) is readable and writable by any unprivileged GPU
submission, and the kernel accepts the forged value as authoritative.

## Explicitly out of scope of this writeup

- **No privilege escalation is claimed.** The write primitive is confined to the
  32 KiB `memstore` region, 4 bytes per write. There is no demonstration of arbitrary
  kernel memory read or write.
- **Cross-process fence forging is not dynamically verified.** The control-arm
  experiment is executed against the caller's own context only.
- No system crash, GPU fault, or denial of service was observed at any point.

## Responsible use

- Run the PoC only on hardware you own and have the right to test.
- The write target is an idle `memstore` slot with no kernel consumer, by design.
  Pointing it at an active context's slot produces cross-process synchronization
  timing effects on a live system and is deliberately not scripted.
- Do not use this code on shared, rented, or production devices.

## Reporting a finding

- Open an issue marked `security`, **not** a pull request. Do not attach
  exploit code to a public issue.
- A contact for coordinated reporting is `3849639991 at qq.com`.
  For Linux kernel findings, the kernel's own process is documented in
  `REPORTING-BUGS` at the root of the kernel tree. For Qualcomm parts, reports
  are routed through Qualcomm's product-security team. Use the channel that
  applies to the part you are reporting rather than this repository.

## Attribution

Please attribute findings from this repository as `kgsl-memstore`. The report in
`README.md` is published under the license in `LICENSE`.

