# Porting plan: Turing (TU10x) unlock, Linux kernel module -> Windows UEFI

Written 2026-10-31. Status: foundation laid, core transaction files not yet adapted or compiled. Nothing here has
been tested against real hardware, and nothing here should be run against real hardware yet.

## Why this looked more tractable once actually researched

The initial worry was that Turing support meant reverse-engineering unknown hardware behavior from scratch. That
turned out to be wrong: **nlzy/turing-unlock already did that work** and published it, GPL-2.0, as a working Linux
kernel module - a genuine Turing port of the same Zagitta/gpu-unlock project sargentmajord/Windows-GPU-UNLOCK is
already built on for Ampere. What's actually missing is a Windows/UEFI port of it, which is exactly the same shape
of work impacifist already did once, for Ampere, in the project this one is modeled on.

## The porting technique, confirmed to already work

sargentmajord/Windows-GPU-UNLOCK does not hand-translate the vendored Linux kernel module logic into native EFI
code. It takes Zagitta's original Linux C source nearly unchanged (`internal-boot/unlock/vendor/unlock_610.c` etc.)
and compiles it against a `compat/linux/*.h` shim that re-implements the small slice of the Linux kernel API the
code actually needs (`readl`/`writel`, `dma_alloc_coherent`, `request_firmware`, `devres_*`, `pci_*`, `ktime_*`,
...) in terms of EFI primitives underneath. The vendor file keeps its original structure and most of its original
code; a handful of `#ifdef GU_UEFI_PORT` blocks handle the genuinely platform-specific bits (target PCI ID
selection, BAR mapping/diagnostics, a couple of execution/cleanup hooks).

Checked directly, not assumed:

- `internal-boot/unlock/vendor/gfw_eval.c` (Ampere) and `kernel/turing_unlock/gfw_eval.c` (Turing) are
  near-identical - same logic, different name prefix (`gpu_unlock_` vs `turing_unlock_`), plus one small bad-value
  check Ampere's copy has that Turing's upstream doesn't yet (see "Also worth upstreaming" below).
- Turing's core transaction files (`unlock.c`, `boot.c`, `layout.c`, `prep.c`, `vbios.c`, `gfw_eval.c`, and its own
  `firmware.h`) need exactly the Linux headers already present in the copied `compat/linux/`: `aperture.h`,
  `delay.h`, `dma-mapping.h`, `errno.h`, `firmware.h`, `io.h`, `module.h`, `pci.h`, `printk.h`, `slab.h`,
  `string.h`, `timekeeping.h`, `types.h`. The four extra Linux headers Turing's source tree references as a whole
  (`async.h`, `device.h`, `init.h`, `mutex.h`) are needed only by `main.c` - the Linux PCI driver registration/probe
  file, which (same as Ampere) is not ported; this project's own entry point replaces it.
- Turing's own `firmware.h` helper (`turing_unlock_request_scoped`, `turing_unlock_check_devres`) calls
  `devres_open_group`/`devres_release_group`/`dev_info`/`request_firmware`/`struct device` - all of which the
  existing `compat/compat.h` already defines (confirmed by reading it directly: `struct device`, `devres_open_group`,
  `devres_release_group`, `dev_info` -> `GuLog`, `request_firmware`, the DMA alloc functions, a no-op
  `spin_lock_irqsave` since UEFI pre-boot is single-threaded). Ampere's `vendor/unlock_610.c` calls the very same
  pattern (`request_scoped`, `devres_open_group`) today, successfully, against this same shim.

So: the shim layer copied into this project should need **no changes** to support Turing's core files, and the
adaptation work is the same shape and (going by `gfw_eval.c`) a similar size to what Ampere's port already did.

## What's done so far

- New repo scaffolded, structured the same way as sargentmajord/Windows-GPU-UNLOCK: `internal-boot/unlock/vendor/`,
  `internal-boot/unlock/compat/`, `deps/edk2/`, `LICENSE`, `LICENSES/`, `THIRD_PARTY_NOTICES.md`.
- `compat/` copied unchanged from sargentmajord/Windows-GPU-UNLOCK (see confirmation above for why that's expected
  to be sufficient, not just convenient).
- `deps/edk2/MdePkg/Include/` copied unchanged (same EDK2 headers, same pinned commit, already BSD-2-Clause-Patent
  licensed and already used this way).
- Turing's vendor source copied in **pristine, unmodified** from `nlzy/turing-unlock` commit
  `417f333263ff169e8f449c931b70db9dfd7b8ef4`: `unlock.c`, `boot.c`/`.h`, `diagnostic.h`, `firmware.h`, `gfw_eval.c`/
  `.h`, `layout.c`/`.h`, `prep.c`/`.h`, `unlock.h`, `vbios.c`/`.h`, and `main.c` (kept for provenance, not ported -
  see THIRD_PARTY_NOTICES.md).

## What's genuinely still ahead

1. **Apply the UEFI adaptation to each vendor file**, following the exact pattern Ampere's `unlock_610.c` already
   shows (`#ifdef GU_UEFI_PORT` around target selection, BAR mapping and the one or two execution/cleanup hooks).
   `gfw_eval.c` is the simplest (near 1:1 with Ampere's already-ported copy) and should go first as the template;
   `unlock.c` is the largest (~975 lines) and does the real work (FWSEC/FRTS, GSP/SEC2 falcon staging, the SS0/SS1
   selector writes). `layout.c`, `prep.c`, `vbios.c` carry the TU102-specific register layout and need the
   constants below, not just mechanical adaptation.
2. **Swap in TU102-specific constants**, per `docs/Difference between Ampere and Turing.md` (copied from
   turing-unlock, reproduced below) and the register offsets already visible directly in `unlock.c`
   (`GSP_BASE=0x00110000`, `SEC_BASE=0x00840000`, `SEC_FALCON2=0x00841000`, `TU_FEAT_PLM=0x00409650`,
   `TU_FEAT_SS0=0x00409664`, `TU_FEAT_SS1=0x0040966c`, `TU_FEAT_READOUT=0x00409668`, unlock values
   `SS0=0x88888888`, `SS1=0x8`). Note `SEC_BASE` is identical to Ampere's `SEC2_BASE` (`0x00840000`) - not every
   constant differs.
3. **Get it compiling** against this project's host-side test harness, the same way
   sargentmajord/Windows-GPU-UNLOCK's `tests/test_transaction.c` + `tests/transaction_shim/` prove the Ampere
   vendor code is at least internally consistent without real hardware. This project doesn't have that harness
   built out yet - it needs porting/adapting too, which is useful work that needs zero hardware.
4. **A target-card entry in the project's own card-acceptance logic** (the equivalent of
   sargentmajord/Windows-GPU-UNLOCK's `installer/CardSelect.ps1` refusing anything outside an accepted ID list) -
   this project should refuse any card that isn't the one documented PCI ID turing-unlock itself has confirmed
   (`10de:1e07`, the Dell RTX 2080 Ti 22GB), the same "refuse rather than guess" posture the Ampere installer
   already takes, until more hardware is actually validated.
5. **Real hardware validation**, which is a separate, later, higher-stakes step - nothing above needs a Turing
   card to do correctly, and nothing past this point should happen without a plan for recovering a card that gets
   stuck, the same diligence the Ampere project applied before its first real hardware run.

## The register/layout differences (from turing-unlock's own docs, TU102 vs the two Ampere variants)

| Item | TU102 | GA100 | GA102 |
| --- | --- | --- | --- |
| WprMeta | `0x400` | `0x600` | `0x400` |
| Signature DMA destination | `0x600` | `0x800` | `0x600` |
| Payload size | `0xfa00` | `0xf800` | `0xfa00` |
| Fill dword | `0x4a7` | `0x4a7` | `0x19c` |
| Global canary | `0x6140` | `0x6340` | `0x6140` |
| f100 restore | DMEM `0x1700` | DMEM `0x1900` | Direct `OR 0x70` |
| Overflow SP | `0xff2c` | `0xff3c` | `0xff30` |
| Return slot | `0xff4c` | `0xff5c` | `0xff50` |

| Role | TU102 | GA100 | GA102 |
| --- | --- | --- | --- |
| `dma_copy_block` | `0x04d4` | `0x04d4` | `0x055b` |
| `reg_write_indirect` | `0x10aa` | `0x10aa` | `0x1101` |
| Write mid-entry | `0x10b9` | `0x10b9` | `0x1110` |
| `booterVerifyLsSignatures` | `0x3875` | `0x3747` | `0x3715` |
| `secure_teardown` | `0x7c43` | `0x7e76` | `0x8135` |
| Partial teardown | `0x7cfc` | `0x7f2f` | `0x81ee` |
| Stack-check failure | `0x7ba6` | `0x7dd9` | `0x811b` |
| `main` | `0x7d4f` | `0x7f82` | `0x82ef` |
| `csb_write` | `0x7ff1` | `0x8224` | `0x8592` |
| `csb_read` | `0x8031` | `0x8264` | `0x85d2` |

Behavioral notes from the same source: TU102 and GA102 both check FRTS/WPR2 readiness through `0x11821c` bit 17
(GA100 does not); all three use finalize state at `0x1180f8`, so a normal stock boot can make the next fire fail a
`0x29` check unless that state is handled; GA102 does extra startup-state preservation and f100 handling that
should not be assumed for TU102; full-teardown addresses are not interchangeable between chips even where they
look similar.

## Also worth upstreaming

Ampere's `vendor/gfw_eval.c` in sargentmajord/Windows-GPU-UNLOCK has a bad-value check Turing's upstream
`gfw_eval.c` doesn't yet have: rejecting `0xbadfxxxx` register-error values as "not a finished firmware boot"
(credited there to upstream Zagitta/gpu-unlock issue #42). Since this project's `gfw_eval.c` is practically the
same file, the same fix should be carried over here too when this file gets its UEFI pass - it's a correctness fix,
not Ampere-specific.

## Known validation gap (read this before testing anything on real hardware)

turing-unlock itself is confirmed on exactly one SKU: a Dell-made RTX 2080 Ti 22GB (TU102, PCI ID `10de:1e07`), an
unusual OEM workstation variant - not the standard retail 11GB 2080 Ti most people, including the person who
offered to test this, actually have. A UEFI port being correct does not establish that the underlying unlock
sequence is safe on other Turing SKUs; that's a separate, larger gap than "has this been ported to Windows yet."
