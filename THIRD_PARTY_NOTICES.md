# Third-party notices

## This project

An attempt at a Windows UEFI port of `nlzy/turing-unlock` (a Turing/TU10x port of `Zagitta/gpu-unlock`), following the same
porting approach already proven for Ampere in [sargentmajord/Windows-GPU-UNLOCK](https://github.com/sargentmajord/Windows-GPU-UNLOCK)
(itself a fork of [impacifist/Windows-GPU-UNLOCK](https://github.com/impacifist/Windows-GPU-UNLOCK)). It is licensed
**GPL-2.0-only**, like the work it builds on ([LICENSE](LICENSE)). Not affiliated with or endorsed by NVIDIA. No NVIDIA
firmware, video BIOS (PROM) image, driver or CUDA library is included.

**Status: early-stage porting work, not a usable installer yet.** See [PORTING-PLAN.md](PORTING-PLAN.md) for what is and
is not done, and why.

## Turing unlock core - nlzy/turing-unlock, GPL-2.0-only

- Source: [nlzy/turing-unlock](https://github.com/nlzy/turing-unlock), pinned commit `417f333263ff169e8f449c931b70db9dfd7b8ef4`.
- Used in: `internal-boot/unlock/vendor/` (`unlock.c`, `boot.c`, `layout.c`, `prep.c`, `vbios.c`, `gfw_eval.c`, and their
  headers), as a Turing port of Zagitta/gpu-unlock. Community-confirmed, per its own issue tracker (issues #1, #2, #22,
  #25), on several third-party VRAM-modified "22GB" RTX 2080 Ti cards (TU102, device IDs `10de:1e07` and `10de:1e04`) -
  never a stock retail card. `main.c` is kept for provenance but is not ported - its role (Linux PCI driver
  registration, async probe, module init) has no UEFI equivalent and is replaced by this project's own single-shot
  entry point, the same way the Ampere port replaces Zagitta's equivalent file.
- Changes made in this project so far: `gfw_eval.c` gained the same `0xbadf`-error-value rejection Ampere's port
  already carries (upstream Zagitta/gpu-unlock issue #42); `unlock.c`'s register access was hardened with bounds
  checking and a fault-injection hook (see PORTING-PLAN.md). The card-identity table in `port.h` (`GuTargets`) is
  this project's own addition, built from the exact (device, subsystem) pairs reported in the issues cited above -
  not part of nlzy/turing-unlock's own source.

## GPU unlock core - Zagitta/gpu-unlock, GPL-2.0-only

- Source: [Zagitta/gpu-unlock](https://github.com/Zagitta/gpu-unlock), the project turing-unlock itself is a port of. Not
  used directly here (see above), credited because turing-unlock's own notices credit it and this project inherits that
  chain.

## The compat (EFI) shim layer - sargentmajord/Windows-GPU-UNLOCK, GPL-2.0-only

- Source: [sargentmajord/Windows-GPU-UNLOCK](https://github.com/sargentmajord/Windows-GPU-UNLOCK)
  `internal-boot/unlock/compat/`, copied unchanged into `internal-boot/unlock/compat/` here.
- This is the layer that lets Zagitta/gpu-unlock-derived Linux kernel module C source (`<linux/io.h>`, `<linux/pci.h>`,
  `<linux/dma-mapping.h>`, `request_firmware`, `devres_*`, etc.) compile and run as a UEFI pre-boot program instead of a
  Linux kernel module, by providing EFI-backed implementations of the same function/struct surface. Confirmed sufficient
  for turing-unlock's own Linux header usage without modification - see PORTING-PLAN.md.

## EDK2 headers - BSD-2-Clause-Patent

- Source: [TianoCore EDK2](https://github.com/tianocore/edk2), copied from the pinned commit already used in
  sargentmajord/Windows-GPU-UNLOCK's `deps/edk2/MdePkg/Include/`. Licence and copyright notices:
  [LICENSES/EDK2-BSD-2-Clause-Patent.txt](LICENSES/EDK2-BSD-2-Clause-Patent.txt) and the header of each file.
- The programs use the header declarations; no EDK2 library binary is linked.

When you copy source from this project, keep these licences and copyright notices with it.
