# Turing GPU UNLOCK - Windows UEFI port (early research)

> **This is not a usable installer. There is no installer here yet, and nothing in this repository should be run
> against real hardware.** This is the early porting/scoping stage of taking `nlzy/turing-unlock` (a Linux kernel
> module, Turing/TU10x port of `Zagitta/gpu-unlock`) and porting it to a Windows UEFI boot program, the same way
> [sargentmajord/Windows-GPU-UNLOCK](https://github.com/sargentmajord/Windows-GPU-UNLOCK) already did for Ampere.

See [PORTING-PLAN.md](PORTING-PLAN.md) for the actual technical plan: what's confirmed to work, what's done, and
what's genuinely still ahead before this is anything you could point at real hardware.

## Known validation gap

The only Turing card `turing-unlock` itself has confirmed working is a Dell-made RTX 2080 Ti 22GB (an unusual OEM
workstation variant, PCI ID `10de:1e07`) - not the standard retail 2080 Ti. A correct Windows port of the mechanism
does not by itself make it safe on other Turing cards. Read [PORTING-PLAN.md](PORTING-PLAN.md) before assuming
otherwise.

## Credits

Built on the work of `nlzy` (`turing-unlock`), `Zagitta` (`gpu-unlock`), and `impacifist`/`sargentmajord`
(`Windows-GPU-UNLOCK`, the Windows UEFI porting pattern this follows). Full attribution:
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
