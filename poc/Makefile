# probe_fence - KGSL memstore trust-boundary PoC
#
# Statically linked, raw syscalls, no libc. Build once, push anywhere.
#
# Prerequisites:
#   an aarch64 musl cross compiler. Any of these works:
#     aarch64-alpine-linux-musl-gcc        (alpine cross, used for the run on record)
#     aarch64-linux-musl-gcc
#     aarch64-none-linux-musl-gcc
#   If your default aarch64 toolchain is glibc, set TARGET= and build with
#   -static only if a static libc is available; otherwise the binary will not
#   run on a stripped device image.

TARGET ?= aarch64-alpine-linux-musl-gcc

probe_fence: probe_fence.c
	$(TARGET) -static -O2 -o $@ probe_fence.c

clean:
	rm -f probe_fence

.PHONY: clean
