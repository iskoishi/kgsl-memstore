# Proposed patches

## Licensing

**Everything in this directory is GNU GPL-2.0-only** (see [`LICENSE`](LICENSE)), not
the MIT license that covers the rest of the repository.

These are diffs against the Linux kernel, and a patch file carries the license of
the text it quotes. The kernel is licensed GPL-2.0-only. Republishing them under
MIT would be incorrect, so this directory is licensed separately.

The rest of the repository — the report in `../README.md` and the original code in
`../poc/` — stays under MIT. The two do not intermix: nothing in `poc/` is kernel
code, and nothing here is.

---

## Verification status

Nothing here has been **compiled or run on a device**. What has been checked:

```
$ git apply --check -p1 patches/02-*.patch    # all three: OK
$ git apply --check -p1 patches/03-*.patch
$ git apply --check -p1 patches/04-*.patch
$ git diff --check                            # no whitespace errors
```

All three land cleanly on `msm-5.4.289` at the paths in the diff
(`a/drivers/gpu/kgsl/...`, so apply from the kernel root with `-p1`).

Base trees:

| Tree | Role |
|---|---|
| `msm-5.4.289` | the driver on the device under test |
| `gfx-kernel.lnx.1.0.r50-rel` | the upstream release this report also checked |

Both carry the same four conditions; they differ only in the constants the
patches are changing.

---

## 1. `01-enable-split-tables.dts.patch` — the right fix, config level

Enable `qcom,split-tables`. With split tables enabled, `kgsl_iommu_map_globals()`
returns early for every non-global page table, so the memstore is never present
in a per-process user page table at all. This is how every other platform in
Qualcomm's own tree behaves, and it removes the exposure without touching the
driver.

**Why this one first:** it is a single device-tree line, it is the design the
rest of the code already assumes, and it is the only one of the four that does
not risk changing driver behaviour.

**Caveat:** this is a platform decision, not a driver bug fix. It has to be
repeated per board device-tree.

---

## 2. `02-memstore-global-pt-only.patch` — the right fix, driver level

Make `kgsl_iommu_map_globals()` skip the memstore for every non-global page
table. This is unconditional and therefore also covers device-trees that never
turn split tables on.

Two implementation notes:

- The skip is implemented in **both** `kgsl_iommu_map_globals()` and
  `kgsl_iommu_unmap_globals()`. Skipping only the map would leave an unbalanced
  `kgsl_mmu_unmap()` on a descriptor that was never mapped.
- The memstore is identified by pointer identity,
  `&md->memdesc == device->memstore`, not by string comparison. That is exact —
  `kgsl_allocate_global()` returns `&md->memdesc` and the caller stores it in
  `device->memstore` — and it avoids adding a `string.h` dependency.

**Why this is not the first patch, and you should not apply it on its own.**
`map_globals()` is called from two sites:

- `iommu_pt_create()` line 1365, on the per-process page table
- `iommu_probe()` lines 2529-2530, on `defaultpagetable` and `lpac_pagetable`

When split tables are **disabled**, `iommu_pt_create()` sets `iommu->ppt_active`
and then calls `map_globals()` on the user page table (line 1363-1365). That is
PPT mode: the user page table is also the address space the GPU sees. Skipping
the memstore there removes the GPU's own write access to its timestamp storage.

So this patch is correct only where split tables are enabled — exactly the
situation where patch 1 already solves the problem. **Both patches require split
tables to be enabled.** Verify on hardware before either of them ships.

---

## 3. `03-memstore-unconditional-privileged.patch` — apply only after checking hardware

Stop gating `KGSL_MEMDESC_PRIVILEGED` on `ADRENO_APRIV`.

**This one is the least trustworthy, and the reason is a good one to check.**
The flag decides whether the memstore's page-table entry gets the privileged
access bit. On a target without APRIV the GPU's user-context access is not
privileged, so making the memstore privileged may stop the GPU from writing its
own timestamps at all. Whether the hardware still permits it depends on how the
global page table is configured on that part. I could not settle this without
hardware, so the patch is here as a candidate, not a recommendation.

---

## 4. `04-slot-range-build-assert.patch` — the one worth applying regardless

Align the slot arithmetic with the newer tree and add two build-time
assertions so this class of bug becomes a compile error.

The numbers, worked out from the source:

```
struct kgsl_devmemstore = 10 x u32 = 40 bytes
KGSL_MEMSTORE_SIZE      = 8 pages  = 32768
slots                   = 32768 / 40 = 819

shipped tree:   KGSL_MEMSTORE_MAX = 819 - 1 - 4 = 814
                context ids 1..814,  RB slots 814..818   -> overlap at 814

newer tree:     KGSL_MEMSTORE_MAX = 819 - 2 - 4 = 813
                KGSL_GLOBAL_CTXT_ID = 812
                context ids 1..812,  RB slots 813..817   -> disjoint
```

This is what the §4.4 overlap is, and the patch restores the `2` and the
`GLOBAL_CTXT_ID` bound that the shipped tree is missing. It is a two-line
arithmetic fix with two assertions attached, and it cannot change runtime
behaviour on any configuration that currently works.

**This is the only patch of the four whose correctness does not depend on
hardware.** The arithmetic above was evaluated in a standalone program with the
real `struct kgsl_devmemstore` (40 bytes), giving `TOTAL = 819`, and both
`BUILD_BUG_ON`s evaluate to pass. Nothing about it needs a device to confirm.

---

## What I am not fixing, and why

The interface contract behind `KGSL_CONTEXT_USER_GENERATED_TS` is the fourth
condition in the report, and it is the only one that does not depend on a
device tree. But the driver comments at `kgsl_events.c:247-254` say the
intention is explicit:

```c
	 * events in the future. Otherwise only allow timestamps that have been
	 * queued.
	if (timestamp_cmp(timestamp, queued) > 0)
		return -EINVAL;
```

This is how EGL and Vulkan request work for a timestamp that has not been queued
yet. Closing the gate unconditionally would break the whole timestamp-event API
for every existing client. Making the flag require a signature-level permission
would break the same clients silently.

That trade is an API decision and it belongs to the driver owners, so there is no
patch for it here. It is in the report because the cost of leaving it is small
(the fence event can be satisfied by a value the caller chose) and the cost of
fixing it is real, which is exactly the kind of trade worth writing down.
