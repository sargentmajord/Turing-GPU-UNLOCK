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

## Credits

Built on the work of `nlzy` (`turing-unlock`), `Zagitta` (`gpu-unlock`), and `impacifist`/`sargentmajord`
(`Windows-GPU-UNLOCK`, the Windows UEFI porting pattern this follows). Full attribution:
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
