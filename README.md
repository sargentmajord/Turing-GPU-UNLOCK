# Turing GPU UNLOCK - Windows UEFI port (early research)

> **This is not a usable installer, and the actual unlock transaction has never been run against real hardware -
> don't run anything in `internal-boot/unlock/` on a real machine.** This is the early porting/scoping stage of
> taking `nlzy/turing-unlock` (a Linux kernel module, Turing/TU10x port of `Zagitta/gpu-unlock`) and porting it to
> a Windows UEFI boot program, the same way
> [sargentmajord/Windows-GPU-UNLOCK](https://github.com/sargentmajord/Windows-GPU-UNLOCK) already did for Ampere.
> The one exception, below, is the read-only hardware-capture tool - that one is safe to run and is what this
> project could actually use help with right now.

See [PORTING-PLAN.md](PORTING-PLAN.md) for the actual technical plan: what's confirmed to work, what's done, and
what's genuinely still ahead before this is anything you could point at real hardware.

## Looking for testers - read-only hardware capture only

This project needs real captures from an actual Turing (TU10x) card to make any further progress - there is no
substitute for real register values from real hardware. **This is not a request to run the unlock itself** - see
"Known validation gap" below for exactly why that's not something to attempt yet, on any card.

What the capture tool actually does: it boots as `\EFI\BOOT\BOOTX64.EFI` from an ordinary FAT32 USB stick, reads
PCI configuration space, memory-mapped registers and your card's own expansion ROM, and writes one log file back
to that same USB stick. It never touches your internal disks, boot order or firmware NVRAM, and it does not start
Windows itself - when it's done, it says so on screen and halts; you remove the USB stick and restart normally.
Source: [`internal-boot/boot/`](internal-boot/boot/) (built with `build-diagnostic.ps1`). It has been boot-tested
in an emulator (QEMU/OVMF) with no real GPU present, confirming the full flow works end to end, but **it has not
yet been run against a real Turing card** - if something looks wrong when you run it, please open an issue rather
than assuming it's your hardware.

If you have a card matching one of the identities in [`internal-boot/unlock/port.h`](internal-boot/unlock/port.h)'s
`GuTargets` table (see "Known validation gap" - so far that means a third-party VRAM-modified "22GB" RTX 2080 Ti,
device ID `10de:1e07` or `10de:1e04`) and are willing to run the capture tool and share the resulting log, please
open an issue. A standard retail 2080 Ti is also useful information even though it won't match the validated list.

Along with the capture log, please also run [`tools/system-report.ps1`](tools/system-report.ps1) (read-only, no
elevation required, changes nothing) and paste its output too: `powershell -ExecutionPolicy Bypass -File
tools\system-report.ps1`. It prints your motherboard/BIOS, boot mode, and - most usefully - the exact PCI
device-tree chain for each graphics adapter, which is normally only discoverable one slow round-trip at a time
once something fails because of it.

## Known validation gap

Every card `turing-unlock` has a community-confirmed report for, in **its own issue tracker specifically**
(github.com/nlzy/turing-unlock issues #1, #2, #22, #25), is explicitly described by its own reporter as a
third-party **VRAM-modified "22GB" card** - never a stock retail 2080 Ti. (An earlier version of this file said
"Dell-made"; that was wrong and has been corrected - "Dell" appears nowhere in any of these reports.) A correct
Windows port of the mechanism does not by itself make it safe on a standard, unmodified card, even one sharing
the same PCI device ID. Read [PORTING-PLAN.md](PORTING-PLAN.md) before assuming otherwise.

To be clear about what that gap is and isn't: it's a statement about what *this project's one upstream source*
has confirmed, not a fact about Turing hardware in general. A different, more complete tool
([physicsdolphin/nv-uefi-unlocker](https://github.com/physicsdolphin/nv-uefi-unlocker)) has its own closed issues
reporting successful unlocks on stock, unmodified Turing cards - see PORTING-PLAN.md for the specifics. This
project hasn't independently verified that tool and isn't adopting its scope; the point is only that a stock
2080 Ti being unvalidated *here* isn't evidence it can't work at all.

### What real-world reports actually show about the risk (2026-11-04)

If you're reading this because you're considering being an early real-hardware tester of the actual unlock
transaction (not the read-only capture, which is already safe and described above), here's the honest picture
from reading every comment on every relevant public report, not just skimming titles:

- **No report, anywhere, of this mechanism damaging a card.** Across the Turing success report cited above
  ([physicsdolphin/nv-uefi-unlocker#103](https://github.com/physicsdolphin/nv-uefi-unlocker/issues/103) - a
  factory-stock, unmodified 11GB TU102 2080 Ti, unlocked, reapplied via an auto-replay loader on every boot, with
  the reporter explicitly confirming consistent results across multiple reboots) and several detailed Ampere
  reports on the same underlying mechanism
  ([#135](https://github.com/physicsdolphin/nv-uefi-unlocker/issues/135),
  [#126](https://github.com/physicsdolphin/nv-uefi-unlocker/issues/126),
  [#148](https://github.com/physicsdolphin/nv-uefi-unlocker/issues/148),
  [#164](https://github.com/physicsdolphin/nv-uefi-unlocker/issues/164)), the worst outcome ever reported is that
  the unlock **doesn't take, or quietly reverts to stock values** - never a hang, crash, or anything requiring
  more than a normal restart. One open issue that looked like a counter-example at first read
  ([#112](https://github.com/physicsdolphin/nv-uefi-unlocker/issues/112)) turned out, on reading the full thread
  rather than the title, to most likely never have run the actual unlock transaction at all - the reporter hadn't
  gotten past an earlier prerequisite step.
- **Whether it "takes" or relocks depends on platform power behavior, not luck.** Real, root-caused examples
  include GC6/D3cold wake transitions, root-port ACPI `_PR3`/`_OFF` power-rail cuts, and driver re-initialization
  on bind - all well-understood, all result in a clean revert to stock, never a bad state.
- **What none of this proves:** that *this specific project's code* works on *any* real Turing card. Every report
  above is from a different, separately-developed, far more iterated tool
  (physicsdolphin/nv-uefi-unlocker, eleven-plus bugfix releases from real-world testing). This project's own code
  has never been run against real hardware at all, by anyone. The realistic expectation for a first real test
  here is that it might just work, or it might need a round of log-based iteration first (a timing race, a
  staging-allocation quirk, etc. - exactly the kind of thing every report above shows happening to someone, and
  getting fixed) - not that it's likely to harm the card either way.

## Credits

Built on the work of `nlzy` (`turing-unlock`), `Zagitta` (`gpu-unlock`), and `impacifist`/`sargentmajord`
(`Windows-GPU-UNLOCK`, the Windows UEFI porting pattern this follows). Full attribution:
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
