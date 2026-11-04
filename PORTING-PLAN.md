# Porting plan: Turing (TU10x) unlock, Linux kernel module -> Windows UEFI

Written 2026-10-31. Status: the vendor transaction code, the safety-gate layer, and the console/asset-loading layer
are ported. The entry point (`app.c`) exists; as of 2026-11-01 three of its four machine-dependent checks
(`BarCheck`, `PromCheck`, `ColdCheck`) are real, generic, profile-driven implementations rather than stubs -
`DisplayCheck` remains the one genuine stub, since it needs this specific test machine's real console/display
topology, which nothing but a real capture can supply. See "The two-stage architecture" below for the full story.

The build itself has actually been exercised too: both `build.ps1` modes compile and link to a valid EFI binary
against a disposable scratch profile (including after the BarCheck/PromCheck/ColdCheck work, rebuilt and
reverified), and a synthetic-fixture register-level test (`tests/test_transaction.c`) proves `unlock.c` is
internally self-consistent - see item 3 under "What's genuinely still ahead" for everything that was found and
fixed to get there. A stage-1 read-only hardware-capture tool (`internal-boot/boot/`) also exists now and has
been boot-tested in an emulator (QEMU/OVMF, no real GPU present) - see "The two-stage architecture" below.
**That read-only capture tool is the only thing it's safe to ask anyone to actually run on real hardware right
now.** The unlock transaction itself has still never been tested against real hardware, and nothing in
`internal-boot/unlock/` should be run against real hardware yet.

## First real-hardware capture (2026-11-03, Cancretto, stock EVGA RTX 2080 Ti FTW3 11GB)

The first real capture against actual Turing hardware, via GitHub issue
[sargentmajord/Windows-GPU-UNLOCK#2](https://github.com/sargentmajord/Windows-GPU-UNLOCK/issues/2). Machine: ASUS
Z390-E (Intel 9900K, no bridge/switch between the GPU and the CPU root complex - the card sits directly under a
root port), BIOS modified with [NvStrapsReBar](https://github.com/terminatorul/NvStrapsReBar) for ReBAR, resizable
BAR enabled, Secure Boot disabled. Card: `10de:1e07`, subsystem `1b4c:a016` (EVGA), physically unmodified 11 GB -
does not match any `GuTargets` row and isn't being added to it on the strength of a clean capture alone; that
table is for cards with an actual confirmed unlock, not a successful read.

- **The capture itself is clean**: `capture-complete=1`, `diagnostic-result status=0x0`, no `incomplete reason=`
  lines anywhere in the log, exactly one PCI target found (`probe targets=0001`), exactly one GOP (`gops=0001`).
  First confirmation that `diagnostic.c`/`probe.c` actually work end to end on real firmware, not just in
  QEMU/OVMF.
- **Independently cross-checked**: the reporter computed `PROM.BIN`'s SHA-256 themselves before deciding not to
  publish the file (correctly - see `probe.c`'s own comment on why), and it matches the hash the probe computed
  from its own live MMIO read byte-for-byte (`281602497e3c...`). Two independent computations of the same 1 MB
  read agreeing is about as good a sanity check on `ProbeBar`'s PROM-dump path as this project could ask for
  without the file itself.
- **`gfw_eval.c`'s porting checks out against real registers**: `GU_GFW_PLM_OFFSET`/`BOOT_OFFSET`/`CPUCTL_OFFSET`
  (`0x118128`/`0x118234`/`0x110100`) read `0xffffff8f`/`0x000003ff`/`0x00000010` - none hit the `0xbadf`
  rejection, and `cpuctl & GU_GFW_CPUCTL_HALTED` (`0x10`) is set, so `turing_unlock_gfw_assess()` would correctly
  read this as a normal, fully-booted (halted) GFW state on a stock, never-unlocked card. First real evidence
  this part of the port's register semantics are right, not just internally consistent with itself.
- **New observation, not yet a bug to fix**: `app.c`'s `ColdCheck` reads a separate "memory size" register at
  `0x1183a4` directly (not through `gfw_eval.c`'s sample struct, so not covered by its `0xbadf` rejection) and
  compares it against a profile's `GU_DEVICE_MEMORY_SIZE`. On this real capture, at this point in a pre-driver
  EFI boot, that register actually reads `0xbadf5040` - the same "not a finished value" sentinel `gfw_eval.c`
  already knows to reject elsewhere, just not plumbed into this particular read. `GU_PREFLIGHT_ONLY` means
  nothing acts on this today, and the existing exact-equality check would already correctly refuse rather than
  misread it as a real size - so this isn't broken, just worth remembering precisely for whenever
  `GU_EXPERIMENTAL_UNLOCK` work actually starts: `ColdCheck` would benefit from the same `0xbadf`-prefix check
  `gfw_eval.c` already does, applied to this register too, rather than a bare equality compare.

## `tools/profile_gen.py` - a first, deliberately scoped-down port (2026-11-03)

Built against Cancretto's real capture (above) rather than guessed at. Deliberately smaller than
sargentmajord/Windows-GPU-UNLOCK's own `profile_gen.py`, which this is adapted from: that tool also
fingerprints a second ("console") adapter's firmware drivers, because the Ampere installer's architecture
needs one. Nothing here does that - `DisplayCheck` is still a stub and nothing should guess at a
two-adapter design for Turing that hasn't been built, let alone captured. This tool covers exactly what a
build that actually compiles today needs, confirmed by compiling against its output, not assumed:

- **A real, previously-unknown compile-time gap, found by actually building against generated output for
  the first time:** `checks.c` (ported unchanged from Ampere, per THIRD_PARTY_NOTICES.md) compiles
  *unconditionally* - only `app.c`'s *call* to `ConfigCheck` is gated behind `GU_EXPERIMENTAL_UNLOCK`, not
  the function bodies in `checks.c`/`app.c` that reference `GU_BOARD_ID`/`GU_BOARD_SUBSYSTEM`/
  `GU_CONFIG_CLASS_REV`/`GU_MSIX_CAP_*`/`GU_ROOT_PORT_*`/`GU_OTHER_GPU_*`. A profile that only supplied
  `BarCheck`/`PromCheck`/`ColdCheck`'s own fields (the original, too-narrow scope of this tool's first
  draft) failed to compile at all - `error: use of undeclared identifier 'GU_CONFIG_CLASS_REV'`. This was
  never caught before because nothing had ever compiled against anything but the TEMPLATE profile (which
  defines everything, just as placeholders) or no real profile at all.
- Fixed by having the tool also emit `GU_BOARD_ID`/`GU_BOARD_SUBSYSTEM`/`GU_TARGET_BUS`/
  `GU_CONFIG_CLASS_REV`/`GU_MSIX_CAP_OFFSET`/`GU_MSIX_CAP_DWORD`/`GU_ROOT_PORT_DEVICE`/
  `GU_ROOT_PORT_FUNCTION` - all genuinely derivable from the target card's own single-adapter capture, no
  second adapter needed. `GU_OTHER_GPU_ROOT_DEVICE`/`FUNCTION`/`ADR` *do* need a second adapter this tool
  has no capture for; they're emitted as explicit, clearly-commented placeholders (`0`) instead - safe
  because nothing reachable today ever evaluates them, only needs them to exist so `checks.c` compiles.
- **A second real finding, also only caught by actually building:** the target card's own plain PCI device
  path is `PciRoot(0)/Pci(a,b)/Pci(0,0)/END` - 4 nodes, **no `ADR` node**. This tool's first draft reused
  Ampere's `classify_path()` (which requires a 5th `ADR` node) for this, because Ampere's own
  `profile_gen.py` only ever uses that shape for a *GOP's* path, not a plain PCI handle's - a distinction
  this project's first draft missed by not reading closely enough, and which refused every real capture
  with a confusing `pci-path` finding until corrected. Added a separate `pci_device_path()` for the 4-node,
  no-ADR shape, used only for the target's own path; `classify_path()` is kept for if/when a GOP path
  (which does carry an `ADR` node) is ever needed again.
- Verified end to end, not just unit-tested: with a synthetic capture (Cancretto's real registers/PROM
  hash/device-path shape, with only the (device, subsystem) swapped to an actual `GuTargets` row and the
  one known-bad memory register cleared - see `tools/test_profile_gen.py`'s own comment for exactly what
  that does and doesn't change), the generated `profile.h` was used to build **both** `build.ps1` modes
  (default and `-ExperimentalUnlock`) via `$env:CPATH`, and both produced a valid EFI binary - the first
  time this project's unlock build has ever compiled against anything but the TEMPLATE profile.
- `tools/test_profile_gen.py` (16/16) checks the real capture directly (clean, PROM hash matches
  Cancretto's own independent computation, GFW registers check out, correctly refused for not being a
  `GuTargets` card) and the synthetic accepted-card path (produces a complete, correctly-shaped profile),
  plus one refusal test per check. Cancretto's capture (`tools/captures/run01-cancretto-2080ti-ftw3/`) is
  checked into this repo, unlike sargentmajord/Windows-GPU-UNLOCK's own private reference captures: he
  posted it publicly for this purpose and sanitized it himself first (see the GitHub issue).

## The two-stage architecture (found while writing app.c, not anticipated at first)

sargentmajord/Windows-GPU-UNLOCK is actually two separate programs, not one:

1. **A read-only diagnostic/capture program** (`internal-boot/boot/` - `diagnostic.c`, `handoff.c`,
   `prepare-console.c`) that reads and reports raw hardware state without assuming anything about the specific
   machine. This is what a brand new machine runs first.
2. **The actual unlock transaction** (`internal-boot/unlock/` - what this project has been porting) which needs a
   complete, per-machine `profile.h` - chip id, PROM hash, cold-boot register values, display topology, console
   driver fingerprints, even the exact PCI device path - before any of its checks mean anything. That profile is
   generated (by `tools/profile_gen.py`, a host-side Python tool, not firmware) from stage 1's capture of one real,
   specific machine. `profiles/z790-3060/profile.h` is the Ampere project maintainer's own real machine, captured
   this way - there is no universal "the right Ampere values" either, just one well-documented reference machine's
   real data.

This project had only ported stage 2 for most of its life. `nlzy/turing-unlock`, being a Linux kernel module
running on an already-booted system, never needed an equivalent to stage 1 at all: on Linux you can just read
`/sys/bus/pci/devices/.../config` interactively, no pre-compiled per-machine constants required. A UEFI pre-boot
program doesn't have that luxury.

Concretely, this means `app.c`'s `BarCheck`/`PromCheck`/`ColdCheck`/`DisplayCheck` *used to* need real captured
values before any of them could even be written, full stop. That turned out to be only half true: **checked
directly, 2026-11-01** - `BarCheck` and `PromCheck`'s own TEMPLATE-profile comments already said their logic is
generic (only `GU_BAR0_BASE`/`GU_BAR0_END`/`GU_PROM_SHA256` are machine-specific), and `ColdCheck`'s core question
("has the firmware finished booting") is already fully answered by `turing_unlock_gfw_assess()` without needing
any Turing-specific cold-register literal. All three are now real, ported from the Ampere `app.c` with the
register offsets and profile fields swapped to Turing's (`TU_FEAT_PLM/SS0/SS1/READOUT` restated locally in `app.c`
with a citation, since they're `vendor/unlock.c`'s own private `#define`s, not exposed through any header -
found in the process that `port.h`'s `GU_PLATFORM` struct was missing the `prom_verified` field Ampere's has,
fixed there too). Deliberately NOT ported: Ampere's `GU_STOCK_PROFILE_SS0`/`GU_ENTRY_LATCHED` machinery for
gating on an expected cold SS0/SS1 value - that's itself the product of real-hardware research this project has
none of yet, so `ColdCheck` only logs SS0/SS1 for visibility and gates on nothing until a real capture says what
they should read. `DisplayCheck` alone still needs:
1. **A stage-1 capture program for Turing - done, 2026-10-31, a first version.** `internal-boot/boot/` now exists:
   `diagnostic.c`/`diagnostic.h` ported from Ampere's almost unchanged (fully generic UEFI enumeration), `probe.c`/
   `probe.h` adapted to match any PCI device against `port.h`'s own `GuTargets` identities instead of a
   single already-known ID (since this tool has to run *before* any profile.h exists - see probe.c's own header
   comment), and `handoff.c` written from scratch, not a port: Ampere's `handoff.c` assumes an entire installer's
   worth of infrastructure (a dedicated boot partition, firmware NVRAM registration, a Windows-partition
   chainload) that doesn't exist for this project. Instead this one runs as a portable `\EFI\BOOT\BOOTX64.EFI` on
   an ordinary FAT32 USB stick - the one boot path nearly all firmware scans automatically with no NVRAM
   registration needed - writes its capture to that same USB stick, touches nothing else, and halts with no
   attempt to chain-load Windows itself (see `THIRD_PARTY_NOTICES.md` for the exact provenance of each file).

   Built with clang and actually boot-tested, not just compiled: installed QEMU + OVMF the same way LLVM was
   installed (`ClaudeElevatedRunner`, since a plain `winget install` can't complete the elevation prompt from
   this session), booted `BOOTX64.EFI` from a virtual FAT USB directory with no emulated GPU, and confirmed the
   full real flow end to end - console messages, `probe-pci-handles=5`, `probe targets=0000` (correctly finding
   no Turing GPU present), a complete capture log, firmware driver-image dumps, `capture-complete=1`, clean halt.
   This caught a real bug no amount of reading would have: the directory-creation call passed `EFI_FILE_DIRECTORY`
   (an *attribute* flag) as part of the *OpenMode* parameter instead of the Attributes parameter - an invalid
   mode combination that silently failed, so every later file-create inside that directory failed too. Fixed and
   reverified by rerunning the same boot test. (A subtler trap during this: a zombie QEMU process from the first,
   buggy run kept its file locks held and kept serving stale console output on a later rerun, which looked for a
   while like the fix hadn't worked at all - resolved by actually confirming the process list, not by guessing.)

   Still only tested in an emulator with no real Turing card present - "probe targets=0000" is the *correct*
   result there, not a confirmation that the probe logic works against a real card's actual register values.
2. A `profile_gen.py`-equivalent to turn that capture's output into a real `profile.h` - not started.
3. A real machine: one of the exact validated (device, subsystem) identities from `port.h`'s `GuTargets` table,
   in an actual PC, with whatever display/console adapter that PC actually has. This project's own maintainer
   does not have one.
4. Someone willing to run the stage-1 capture (read-only, makes no hardware changes) on that real machine - this
   is the ask now that item 1 exists: run the BOOTX64.EFI capture tool and share the resulting log, nothing more.
   Running the actual unlock transaction is a separate, later, far higher-stakes step (see "Known validation gap"
   below) and is explicitly not what's being asked for.

`machine-profile.h` and `profiles/TEMPLATE/profile.h` exist now specifically so the *shape* of what's needed is
clear and reviewable - every field has a comment saying what it is and where it would come from - without
pretending any of the placeholder values in it are real. It has a build-time `#error` specifically so it can never
be mistaken for a real profile and compiled into working firmware.

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

1. **Apply the UEFI adaptation to each vendor file that actually touches hardware or Linux APIs**, following the
   exact pattern Ampere's `unlock_610.c` already shows (`#ifdef GU_UEFI_PORT` around target selection, BAR
   mapping and the one or two execution/cleanup hooks). `gfw_eval.c` is the simplest (near 1:1 with Ampere's
   already-ported copy) and already done. `unlock.c` is the largest (~1000 lines) and does the real work
   (FWSEC/FRTS, GSP/SEC2 falcon staging, the SS0/SS1 selector writes); its register-access hardening (`rd`/`wr`
   fault injection) and its two safety-state hooks are now done too - see "unlock.c adaptation status" below.

   **Correction, 2026-10-31 (second pass):** `layout.c`, `prep.c` and `vbios.c` do *not* need this - checked
   directly, not assumed. All three are pure freestanding C: no `readl`/`writel`, no `dma_*`, no `pci_*`, no
   `request_firmware`, no kernel timing/allocation calls of any kind (confirmed by grepping each file for every
   Linux API `unlock.c` actually needs compat coverage for - zero matches in all three). They're memory-layout
   arithmetic (`layout.c`), a radix-table/WPR-metadata byte packer (`prep.c`), and an independent VBIOS-ROM byte
   parser (`vbios.c`) - nothing in any of them is platform-specific, only TU102-specific, and nlzy already wrote
   them correctly for TU102. These three need **zero** porting work beyond compiling them, which item 3 below
   covers.

   **Correction, 2026-10-31 (third pass): `boot.c` belongs on this same no-adaptation list, checked the same
   way.** It only includes `<linux/firmware.h>` (for `struct firmware`, already in `compat/`) and
   `<linux/string.h>`; zero `readl`/`writel`/`dma_*`/`pci_*`/`request_firmware` calls. It's `unlock.c`'s boot-
   descriptor parser (`turing_unlock_boot_desc_v4`, called from `unlock.c`'s `init_hw`-equivalent path) - pure
   byte parsing, same shape as `layout.c`/`prep.c`/`vbios.c`, not platform-specific at all.
2. ~~Swap in TU102-specific constants~~ - **done already, nothing to swap.** Checked directly: every constant in
   `docs/Difference between Ampere and Turing.md`'s TU102 column is already present and correct in the pristine
   vendor code - `generate_v67.py` (host-side, not firmware) has `PAYLOAD_SIZE=0xFA00`, `FILL_DWORD=0x4A7`, the
   canary `0x6140`, `put(0xFF4C, 0x10B9)` and the `0x7C43` teardown address, all matching the TU102 column exactly
   (not GA100's or GA102's). `unlock.c` already has `GSP_BASE=0x00110000`, `SEC_BASE=0x00840000`,
   `TU_FEAT_PLM/SS0/SS1/READOUT` and the `SS0=0x88888888`/`SS1=0x8` unlock values. The differences table only
   matters for porting *this* code to a *different* chip (GA100/GA102) - it was never something Turing's own,
   already-TU102-native code needed substituted in. The earlier wording here implied open work that didn't exist;
   corrected the same way the "Dell-made" error was corrected above, by checking the primary source again instead
   of trusting what this document already said.
3. **Get it compiling** against this project's host-side test harness, the same way
   sargentmajord/Windows-GPU-UNLOCK's `tests/test_transaction.c` proves the Ampere vendor code is at least
   internally consistent without real hardware.

   **`internal-boot/unlock/build.ps1` and `verify_pe.py` - written AND actually run, both build modes.**
   `build.ps1` is the real firmware-producing build (clang + lld-link, `-target x86_64-pc-windows-msvc
   -ffreestanding ...`, EFI-application subsystem), adapted from the Ampere project's own `build.ps1`,
   deliberately scoped down to the two modes `app.c`/`port.h` actually support (default preflight-only,
   and `-ExperimentalUnlock`) - no `-AutomaticBoot`/`-InternalBoot`/`-HeadlessResearch`/`-StockProfile`,
   since none of those Ampere research refinements have any Turing equivalent yet. `verify_pe.py` is
   copied unchanged - generic PE32+/EFI-application structure checks, nothing Ampere- or Turing-specific
   in it.

   This environment had no clang/lld-link toolchain at all (checked directly: not on PATH, not under
   `C:\Program Files\LLVM`, not in any winget link directory) - installed LLVM 23.1.4 via the
   `ClaudeElevatedRunner` task (the same elevated-task mechanism already used for the Ampere project's
   installer test suite; a plain `winget install` from this session's own shell can't complete the
   elevation prompt). With a real toolchain in hand, built **both** `build.ps1` modes (default and
   `-ExperimentalUnlock`) end to end against a disposable, uncommitted scratch `profile.h` (same shape as
   `profiles/TEMPLATE/profile.h`, just without its `#error` guard, built only to exercise the compiler/
   linker plumbing - never meant to look like a real machine and deleted after use). Both produced a
   valid PE32+/EFI-application binary that passed `verify_pe.py`'s own structural checks.

   Doing this surfaced four real bugs that no amount of reading could have caught, because nothing had
   ever actually tried to compile this project's `GU_UEFI_PORT` path before:
   - `vendor/unlock.c` calls `ioremap()`/`iounmap()`/`pci_resource_start()` directly (Ampere's own
     `unlock_610.c` uses `pci_iomap()`/`pci_iounmap()` instead, which is all `compat.h` had). Added the
     missing three to `compat/compat.h` and `compat.c`, same single-BAR0 mapping `pci_iomap` already used.
   - `vendor/unlock.c` also calls `udelay()` and `msleep()` directly - neither declared anywhere. Added
     both to the compat shim too.
   - `vendor/unlock.c`'s retained-resources warning calls `pci_err()` - also never declared. Added as a
     `GuLog`-backed macro, the same pattern `dev_err`/`dev_info`/`pr_err`/`pr_info` already use.
   - `vendor/firmware.h` `#include`s `<linux/device.h>` directly - the one PORTING-PLAN.md claim that
     turned out wrong on a second check: the "four extra headers only `main.c` needs" list (in the
     porting-technique section above) should not have included `device.h`, since `firmware.h` needs it
     too. Added `compat/linux/device.h` forwarding to `compat.h`, the exact same one-line pattern every
     other `compat/linux/*.h` file already uses.
   And two bugs in this project's *own* app-layer files, not the vendor code or the shim:
   - `app.c` and `checks.c` both reference profile macros (`GU_BOARD_ID`, `GU_CONFIG_CLASS_REV`,
     `GU_TARGET_BUS`, etc.) but neither one actually `#include`d `machine-profile.h` anywhere in its own
     include chain - `checks.c`'s own header comment even says this logic is "parameterized entirely by
     machine-profile.h" without the file doing it. Added the missing `#include` to both.
   - `app.c` declared two EFI_GUIDs (`GopGuid`, `PathGuid`) that are never referenced anywhere in the
     file - harmless under a plain compile, but fatal under `build.ps1`'s own `-Werror`. Removed both
     rather than silence the warning, since they were genuinely dead, not used by some `#ifdef` branch
     this build mode skips.

   **`tests/test_layout.c` - done.** `layout.c`'s `turing_unlock_layout()` is pure arithmetic (no `readl`/`writel`,
   no DMA, no PCI - see item 1's correction above), so it compiles and runs standalone, no mocks needed. Checked
   it actually works, not just written and assumed: built it with MSVC (`cl /std:c17`) and ran it - all four
   cases pass, including the hand-derived hex expected values for the synthetic 8 GiB layout (every one of
   those was independently worked out from the function's own align_down/align_up/sub_u64 chain before running
   it, then confirmed by actually running the binary, not the other way around).

   **The big one - `tests/test_transaction.c`'s equivalent for `unlock.c` - done, a first pass.** Unlike
   Ampere's 770-line harness, which checks its mock against a REAL captured card's register values (a
   genuine hardware oracle), Turing has no equivalent capture (same gap as `BarCheck`/`PromCheck`/
   `ColdCheck` in `app.c` - see "The two-stage architecture" above), so this harness has no external
   oracle at all - every register offset and side effect was derived directly from reading `unlock.c`,
   `boot.c`, `vbios.c` and `prep.c`'s own code (Turing's falcon reset/boot sequencing genuinely differs
   from Ampere's - e.g. "no BCR switch on Turing", `flcn_reset()`'s own comment), not fabricated and not
   copied from Ampere's differently-shaped mock. It proves `unlock.c` is internally self-consistent
   end-to-end against a synthetic, hand-built fixture (fake firmware blobs + a synthetic FWSEC/VBIOS
   image, no real board firmware contents, same posture as `tests/transaction_shim/vbios_fixture.h` in
   the Ampere project) - built with clang (same toolchain as the real build; MSVC's `cl.exe` cannot
   compile this file at all, since it rejects the GNU `void*` pointer arithmetic `unlock.c` relies on, a
   portability gap in `cl.exe` for this particular file, not a bug in the pristine vendor source) and run.
   Two scenarios pass: the full happy-path transaction (verified byte-for-byte: the published WPR
   metadata, the radix-walked GSP image, the boot image copy, the Booter signature patch) and SEC2 never
   reporting halted (correctly times out and the error-path PCIe FLR cleanup still frees every DMA buffer
   and restores the PCI command register). Deliberately not full parity with Ampere's ~40-scenario suite
   yet - see the file's own header comment for exactly what's not covered (the entry-time "reset
   required" FLR branch, FWSEC's "WPR2 already up" fast path, GSP failing to boot, DMA/firmware-request
   failure injection, the UEFI-port-only safety hooks, a non-page-aligned image size) - each would need
   its own careful pass, the same reasoning that applied to not attempting this file's full scope in one
   sitting.

   `gfw_eval.c` already has `tests/test_gfw_eval.c`'s Ampere equivalent ported too (small, pure logic, no
   mock needed, same shape as `test_layout.c`).
4. **Card-acceptance logic - done**, in `port.h`'s `GuTargets` table and `port.c`'s `GuTargetConfig`: an exact
   allow-list of every (device, subsystem) PCI identity with an actual community-confirmed report, sourced
   directly from nlzy/turing-unlock's own issue tracker (issues #1, #2, #22, #25 - see `THIRD_PARTY_NOTICES.md`
   and the comment above `GuTargets` for the exact citations). Every one of them is a third-party VRAM-modified
   "22GB" card, not a stock retail 2080 Ti - see "Known validation gap" below. This intentionally refuses a
   standard card even if it shares a device ID with one of the table entries, the same "refuse rather than guess"
   posture the Ampere installer already takes.
5. **Real hardware validation**, which is a separate, later, higher-stakes step - nothing above needs a Turing
   card to do correctly, and nothing past this point should happen without a plan for recovering a card that gets
   stuck, the same diligence the Ampere project applied before its first real hardware run.

## unlock.c adaptation status

Turing's `unlock.c` already had `Gu.fault` injection in `rd()`/`wr()` (done in an earlier commit). Added in this
pass, both checked directly against Ampere's already-ported `unlock_610.c` rather than guessed:

- `GuStarted()` called right where the vendor code itself sets `w->gsp_started = true` and `w->sec_started = true`
  (both immediately before the falcon's RUN bit is written) - these are the two points a transaction becomes
  irreversible enough that `GuSafeToReturn()` must start refusing a return to Windows until a matching
  `GuQuiesced()`.
- `GuQuiesced()` called at the one point `firmware_quiesced(w)` (both falcons either never started or are
  confirmed halted) is true and cleanup is about to proceed - the inverse of `w->retained = true`, which is the
  path taken when it's *not* true.
- A live PCI-identity reread added inside `sec_hs_fire`'s busy-wait loop (can run up to 5s), mirroring Ampere's
  `falcon_hs()` `wait_plm` loop: guards against the BAR no longer pointing at the same card mid-wait (FLR,
  hot-unplug, a driver reattaching). Compares against `Gu.target_device` (the runtime value `port.c`'s
  `GuTargetConfig` already resolved) rather than a compile-time constant, since Turing's card identity is a
  runtime allow-list (`GuTargets`), not Ampere's single compile-time `GU_BOARD_ID`.

**Closed, 2026-11-04:** the GSP side's equivalent long wait (`gsp_fwsec_frts`'s `poll(w, GSP_BASE + 0x100,
..., 5000000, true)`) did not get the same per-iteration identity recheck `sec_hs_fire`'s own hand-written loop
already has, because it goes through the shared generic `poll()` helper that's also used for many short,
hot-path waits elsewhere in the file (e.g. the 256-byte-chunk loop in `sec_dma`) - adding a PCI config-space
read to every `poll()` call would slow those down for no safety benefit and goes beyond what Ampere's own
pattern does (Ampere only added this to one specific hand-written loop, not to a shared primitive). Closed with
a second function, `poll_watched()` (`vendor/unlock.c`, `#ifdef GU_UEFI_PORT` only) - identical to `poll()`
except for the same live-identity reread `sec_hs_fire` already does per iteration - used only at
`gsp_fwsec_frts`'s one call site via a `#ifdef GU_UEFI_PORT` branch; the non-UEFI Linux build path is untouched
and still calls plain `poll()`. Compiled and linked clean in both build modes (preflight and
`-ExperimentalUnlock`) against a throwaway profile.

**Closed, 2026-11-04 (flagged by a third party, still not checked against real hardware - see the honesty note
below).** `GuDmaAllocate` (`port.c`) used to allocate staging buffers with `Gu.pci->AllocateBuffer(...,
AllocateAnyPages, ...)` only - no minimum-address constraint, so the firmware was free to (and routinely does)
hand back memory below 4 GiB. physicsdolphin, replying on github.com/nlzy/turing-unlock issue #26, reported that
their own UEFI unlocker ([nv-uefi-unlocker](https://github.com/physicsdolphin/nv-uefi-unlocker)) saw the booter
reject staging reads below 4 GiB outright (mailbox halt `0x91`), and that deterministic >4 GiB allocation
(`GetMemoryMap` + `AllocateAddress`) was the fix on their side.

Checked directly before implementing, not assumed: `EFI_PCI_IO_PROTOCOL.AllocateBuffer`'s own `Type` parameter is
documented in the vendored `deps/edk2/MdePkg/Include/Protocol/PciIo.h` as "not used and must be ignored" - so
there was never a way to request this through the call `GuDmaAllocate` already used, confirming physicsdolphin's
fix really did require bypassing it for raw `EFI_BOOT_SERVICES.AllocatePages`.

Added `GuDmaAllocateHigh()`: scans the UEFI memory map for a free `EfiConventionalMemory` region with enough room
above `0x100000000`, and if found, allocates there via `AllocatePages(AllocateAddress, ...)` instead. If no
region is found, or the specific address is refused, `GuDmaAllocate` falls back to exactly the original
`Gu.pci->AllocateBuffer(AllocateAnyPages, ...)` path - unchanged, still the one Ampere's own validated port uses.
Both paths funnel into the same `Gu.pci->Map()` call afterward (which, per spec, works on any host buffer, not
just ones from `AllocateBuffer`), and `GU_DMA` gained a `via_pages` flag so `GuDmaFree` calls the matching
`Gu.bs->FreePages`/`Gu.pci->FreeBuffer` rather than guessing - EDK2 does not guarantee these two allocators share
a free list. Compiled and linked clean (`-ExperimentalUnlock` build) against a throwaway profile.

**Honesty note, since this is exactly the kind of thing to not overstate:** this is a safe, strictly
try-first/fall-back-to-the-already-validated-path addition - it cannot make a working configuration stop
working, since the fallback is the unchanged Ampere-validated behavior. It is NOT a confirmation that Turing
needs this fix, or that it solves the `0x91` mailbox halt physicsdolphin saw - that remains unestablished for
nlzy/turing-unlock's own mechanism specifically, since this project still has no real Turing hardware to test
against. "Closed" here means the code path now exists and compiles, not that the underlying hardware question is
answered.

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

## Also worth upstreaming - done here

Ampere's `vendor/gfw_eval.c` in sargentmajord/Windows-GPU-UNLOCK has a bad-value check Turing's upstream
`gfw_eval.c` didn't have: rejecting `0xbadfxxxx` register-error values as "not a finished firmware boot" (credited
there to upstream Zagitta/gpu-unlock issue #42). Carried over into this project's `gfw_eval.c` - see the commit
history. Worth reporting upstream to nlzy/turing-unlock too, since it's a correctness fix, not Ampere-specific.

## Known validation gap (read this before testing anything on real hardware)

**Correction, 2026-10-31:** an earlier version of this document (and this project's README.md,
THIRD_PARTY_NOTICES.md, and a GitHub comment) said the one validated card was "Dell-made." That was wrong -
checked again directly against nlzy/turing-unlock's own issue tracker, and "Dell" appears nowhere in any report.
Corrected throughout.

What's actually there, quoted directly from github.com/nlzy/turing-unlock's issues (not a third-party database,
not a guess):

| Issue | Device | Subsystem | Described as |
|---|---|---|---|
| #1 | `10de:1e07` | not given | "modified-memory card" |
| #2 (iogotop) | `10de:1e07` | `10de:12fa` | "modified-memory cards" (dual-card/NVLink) |
| #2 (arbulin) | `10de:1e07` | `10de:12a4` | "modified-memory" |
| #2 (blackxwhite88) | `10de:1e04` | `10de:12ae` | reporter: "a different device variant, not 1e07" |
| #22 (both cards) | `10de:1e07` | `10de:12fa` | "modified-memory cards" (dual-card/NVLink) |
| #25 | `10de:1e04` | not given | stock/modified not stated |

Every report that says anything about it, in nlzy/turing-unlock's own issue tracker specifically, describes a
third-party VRAM-modified "22GB" card - never a stock retail 2080 Ti (which ships with 11 GB). The subsystem IDs
differ between individual reporters even at the same device ID (`1e07` shows both `12fa` and `12a4`), so there is
no single clean "the reference card" the way Ampere has one - these are a handful of different modded boards,
not one well-documented machine. `port.h`'s `GuTargets` table is an exact allow-list of the rows above that have
both a device and a subsystem id; a standard, unmodified 2080 Ti will not match any of them, on purpose. A UEFI
port being correct does not establish that the underlying unlock sequence is safe on a card outside this table;
that's a separate, larger gap than "has this been ported to Windows yet," and it is the reason this project does
not consider a standard 2080 Ti validated here, right now.

To be precise about what that does and doesn't mean: "never a stock retail 2080 Ti" above describes
nlzy/turing-unlock's own issue tracker, not a fact about Turing hardware in general. physicsdolphin pointed out
(github.com/nlzy/turing-unlock issue #26) that a different, more complete tool -
[physicsdolphin/nv-uefi-unlocker](https://github.com/physicsdolphin/nv-uefi-unlocker) - has closed issues
reporting successful unlocks on stock, unmodified cards: a factory 11 GB `10de:1e07` (issue #103), a stock RTX
2080 Super `10de:1e81` (issue #14), and a stock RTX 2080 `10de:1e87` (issue #62). Those three issues are detailed
and internally consistent (specific mailbox/register values, VBIOS checksums, hardware configs), so the claim
itself looks credible. This project has not independently verified that tool or run anything from it, and is not
adopting its (much broader, GTX16-RTX40) scope or its own `GuTargets`-equivalent list on the strength of someone
else's issue tracker alone - but the narrower point stands: a stock 2080 Ti being unvalidated here is a gap in
what *this port* (and the one upstream repo it's built from) has confirmed, not evidence that stock Turing cards
can't work at all.

## DisplayCheck: real now, built from six real-hardware captures (2026-11-04)

Cancretto (github.com/sargentmajord/Windows-GPU-UNLOCK#2) ran the stage-1 capture tool six times across three
display topologies on their two stock, unmodified 11GB EVGA 2080 Ti FTW3 cards: one card with the motherboard's
integrated graphics as the primary display, two cards with no iGPU, and two cards plus the iGPU as primary -
each topology captured once with Resizable BAR on and once off. All six captures finished cleanly
(`capture-complete=1`), so this is the first real multi-adapter data this project has had.

**What it showed, and why DisplayCheck didn't need Ampere's design:** Ampere's own `DisplayCheck` requires a
*second*, distinct "console" GPU, identified by a compile-time `GU_OTHER_GPU_ID`/`GU_OTHER_GPU_SUBSYSTEM` pair -
its reference machine has a dedicated console adapter separate from the target card. Turing's `FindTarget()`
already refuses unless exactly one PCI device matches `GuTargets` (`found != 1` is rejected), so there is only
ever one target card to validate, never a second adapter to identify by a hardcoded identity. Porting Ampere's
model as-is would have meant inventing a two-adapter design nothing here actually has.

The six captures gave a cleaner signal instead. Across all of them, whether the target card had its own bound GOP
(i.e. had actually been POSTed) was a perfect predictor of whether its live PROM hash matched the known-good
value:

| Configuration | Rebar | GOPs on the target card(s) | PROM hash |
|---|---|---|---|
| 1x 2080 Ti, iGPU primary | off/on | 0 of 1 | `f5fb04aa...` (wrong) |
| 2x 2080 Ti, no iGPU | off/on | 2 of 2 | `281602497e...` (matches known-good) |
| 2x 2080 Ti, iGPU primary | off/on | 0 of 2 | `f5fb04aa...` (wrong, both cards) |

Resizable BAR made no difference in any configuration. Whenever the iGPU was the boot-primary display, *every*
discrete card - whether one or two were installed - never got a GOP bound to it at all (`gops=1`, the iGPU's own,
system-wide), and every discrete card's live PROM read came back with the same wrong hash. Whenever no iGPU was
present, each discrete card got its own GOP and the correct hash, independent of card count or Rebar. This is a
real hardware effect, not a probe bug: an un-POSTed card's expansion-ROM-window read differs from a POSTed one's,
and whether a card gets POSTed on a multi-adapter system depends on which adapter the firmware picked as primary,
not on anything the capture tool controls.

That makes "does the target card have its own valid, bound GOP" a genuine, evidence-backed correctness signal,
independent of and earlier than `PromCheck` (which would also refuse in this situation, once it finally runs, but
only after `BarCheck` already has). Implemented as `TargetGopCheck()` in `app.c`: enumerate all GOP handles,
resolve each one's PCI parent via `LocateDevicePath`/`PciGuid` (not a compile-time identity - compared directly
against `Gu.pci`'s own `GetLocation()`, since the target is already known by the time `DisplayCheck` runs), and
require the match to have a sane `FrameBufferBase`/`FrameBufferSize`/mode. No `GU_OTHER_GPU_*` profile fields are
read by this logic at all; those remain untouched placeholders for a hypothetical future second-adapter design,
not because this check still needs them.

**Practical implication for testers and for this project's own guidance going forward, not a safety bug:**
someone whose target card is not the boot-primary display (an iGPU or a different card set as primary in BIOS)
will get a correct, safe refusal from `DisplayCheck` - the same "refuse rather than guess" behavior as everywhere
else in this project - even though their card and its firmware are completely fine. This should be called out
explicitly in any future testing instructions once `GU_EXPERIMENTAL_UNLOCK` testing actually starts: the target
card should be the boot-primary display (or the only display adapter), not a secondary one.

**Build status:** compiled and linked clean (both `build.ps1` modes) against a throwaway profile generated from
one of the six captures, following the same discipline as every other profile used for compile verification in
this project - built locally, never committed, discarded after the build. Not boot-tested; no profile exists yet
that `GuTargets` actually accepted at the time this was written (see the update directly below, which changed
that).

## GuTargets broadened from a per-board allow-list to a device-ID family (2026-11-04)

Everything above this section, and the "Known validation gap" table a few sections up, describes `GuTargets` as
it was before this change: an exact allow-list of specific (device, subsystem) pairs, each with its own
community-confirmed unlock report, refusing everything else including a standard stock 2080 Ti - correct and
deliberate with no real-world evidence to inform a broader decision.

That evidence now exists. `README.md`'s "What real-world reports actually show about the risk" section documents
reading every comment on every relevant public report (not just titles): no report anywhere of this unlock
mechanism permanently damaging a card, including a detailed factory-stock 11GB TU102 success report with
confirmed reboot-reapply stability (physicsdolphin/nv-uefi-unlocker#103). On the strength of that, `GuTargets`
(`port.h`) and `GuTargetConfig` (`port.c`) were changed to accept the Turing TU102 device-ID family
(`10de:1e07`, `10de:1e04`) regardless of subsystem, rather than only specific pre-approved boards - the same
scope the read-only capture tool's `probe.c` already used. The existing hardware checks
(`BarCheck`/`PromCheck`/`ColdCheck`/`DisplayCheck`) are unchanged and still refuse on a genuinely wrong or
unexpected card state; what changed is who gets to decide whether to try, not whether the code still checks its
own work.

Said plainly, since this is exactly the kind of thing to not blur: this is evidence the *mechanism* doesn't
damage cards, not evidence that *this project's own code* - which has never run against real hardware, still, as
of this change - correctly handles every card in that device family. That gap is now disclosed publicly
(README.md) rather than enforced by a maintainer allow-list; anyone running the actual unlock transaction is
doing so informed and at their own discretion, same as any other early real-hardware test of unreviewed firmware
code. Compiled and linked clean (both build modes) against a throwaway profile after this change.

## Post-broadening review (2026-11-04): one stale comment, two stale tests, one real validation gap

Checked every file touched by the GuTargets broadening for leftover inconsistencies, not just the two
already fixed in that commit - found four more, by actually testing rather than reading:

1. `port.h`'s comment directly above the `GuTargetConfig` *declaration* (separate from the definition in
   `port.c`, already fixed) still said "exact (device, subsystem) match" - stale, corrected.
2. `tools/test_profile_gen.py` had two tests asserting the old behavior would still hold:
   `test_refused_for_not_being_a_target_card` expected Cancretto's real capture to be refused for its
   device id (now accepted); `test_gu_targets_from_port_h_finds_the_real_table` expected 3 table rows
   (now 2). Both updated - the first now documents that the capture is still correctly refused, but for
   the real, pre-existing reason (`memory-size-not-ready`, already known since the 2026-11-03 entry), not
   a device-id mismatch. Full suite re-run: 16/16 before the fix below, 17/17 after.
3. **Real validation gap, found by actually running `profile_gen.py` against all seven of Cancretto's
   captures (the original plus the six from the DisplayCheck work), not assumed clean because the device
   broadening worked:** one of them (1x 2080 Ti + iGPU primary, Rebar on) has BAR0 not decoding at all -
   every single `mmio` register in it, not just the ones already checked, reads exactly `0xffffffff`. That
   value is NOT caught by `is_bad_sentinel()` (`0xffff0000 != 0xbadf0000`) and defeats the
   `CPUCTL_HALTED` bitmask check by construction (any mask ANDed with all-ones is true) - so this capture
   was being accepted (`ok: true`) and would have baked `0xffffffff` in as the expected chip id, a profile
   that could never pass `ColdCheck` on any real hardware. Fixed with an explicit, early check on the
   chip-id register specifically (a real chip id is never literally `0xffffffff`), with a message pointing
   at BIOS/ReBAR/boot-mode as the likely cause rather than guessing further. New regression test added
   (`test_mmio_unmapped_chip_id_all_ones`), built from the real capture's own values, not invented.
4. **The complete, honest result of testing all seven captures**: none of them currently produce a usable
   profile. The four dual-card captures all refuse on `target-count` (exactly one matching card is
   required; two were present). The two single-card-with-iGPU captures each fail for a different reason
   tied to the same underlying cause already documented in "DisplayCheck: what the captures actually
   showed" - the card was never POSTed because the iGPU was primary. **A usable profile needs one more
   real capture: a single card installed (not both), with no iGPU as the primary display.** Said plainly
   so nobody assumes otherwise from the device-id broadening alone: broadening `GuTargets` made the right
   *kind* of card eligible, it did not create a working profile from data that was never going to produce
   one.
