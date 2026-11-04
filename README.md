# Turing GPU UNLOCK - Windows UEFI port (early research)

> **This project's own code has never been run against real Turing hardware, by anyone, including the people
> building it.** `internal-boot/unlock/` is where the actual unlock transaction lives. As of 2026-11-04 it will
> build and accept any card in the Turing TU102 device-ID family (`10de:1e07`, `10de:1e04`) rather than only a
> maintainer-curated allow-list of specific confirmed boards - see "What real-world reports actually show about
> the risk" below before running it. Running it on real hardware is now something anyone can choose to do at
> their own discretion, with that information in hand - not something this project is asking anyone to do, and
> not something confirmed safe for your specific card. The read-only hardware-capture tool, below, remains the
> lowest-risk way to help either way.

See [PORTING-PLAN.md](PORTING-PLAN.md) for the actual technical plan: what's confirmed to work, what's done, and
what's genuinely still ahead.

## Looking for testers

This project needs real data from actual Turing (TU10x) hardware to make further progress - there is no
substitute for real register values and real transaction logs. Two different things you can help with, at very
different risk levels - read both sections below before choosing either.

### Read-only hardware capture (lowest risk)

What the capture tool actually does: it boots as `\EFI\BOOT\BOOTX64.EFI` from an ordinary FAT32 USB stick, reads
PCI configuration space, memory-mapped registers and your card's own expansion ROM, and writes one log file back
to that same USB stick. It never touches your internal disks, boot order or firmware NVRAM, and it does not start
Windows itself - when it's done, it says so on screen and halts; you remove the USB stick and restart normally.
Source: [`internal-boot/boot/`](internal-boot/boot/) (built with `build-diagnostic.ps1`). It has been boot-tested
in an emulator (QEMU/OVMF) with no real GPU present, confirming the full flow works end to end, but **it has not
yet been run against a real Turing card** - if something looks wrong when you run it, please open an issue rather
than assuming it's your hardware.

If you have a Turing card (`10de:1e07` or `10de:1e04` device family) and are willing to run the capture tool and
share the resulting log, please open an issue. A standard retail card is just as useful as a modified one for
this step.

Along with the capture log, please also run [`tools/system-report.ps1`](tools/system-report.ps1) (read-only, no
elevation required, changes nothing) and paste its output too: `powershell -ExecutionPolicy Bypass -File
tools\system-report.ps1`. It prints your motherboard/BIOS, boot mode, and - most usefully - the exact PCI
device-tree chain for each graphics adapter, which is normally only discoverable one slow round-trip at a time
once something fails because of it.

### The actual unlock transaction (real, if low, risk - read this fully first)

This writes to the card's registers and runs real firmware on its secure co-processor. It is not something this
project is asking anyone to run - it's something you can choose to, informed, at your own discretion. Read "What
real-world reports actually show about the risk" below in full before deciding. If you do and are willing to
share the resulting `loader.log` regardless of outcome, please open an issue - a failed or unexpected run is at
least as useful as a clean one, since this project's own code has no real-world data behind it yet at all.

## What real-world reports actually show about the risk (2026-11-04)

If you're deciding whether to run the actual unlock transaction on real hardware, here's the honest picture from
reading every comment on every relevant public report, not just skimming titles:

- **No report, anywhere, of this mechanism damaging a card.** Across a detailed Turing success report
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

## Where "known validation gap" went

An earlier version of this README gated the unlock behind an exact per-board allow-list (only specific
VRAM-modified "22GB" cards with a community-confirmed report in nlzy/turing-unlock's own issue tracker - see git
history for that section if you want the original sourcing detail, issues #1/#2/#22/#25). As of 2026-11-04 that's
been replaced with the device-family acceptance described above, once the research in the previous section
existed to inform the decision. The existing hardware checks
([`BarCheck`/`PromCheck`/`ColdCheck`/`DisplayCheck`](internal-boot/unlock/app.c)) still refuse rather than guess
on a genuinely wrong or unexpected card state - that part hasn't changed.

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
