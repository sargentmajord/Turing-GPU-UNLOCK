// Turing GPU UNLOCK - Windows UEFI port. TEMPLATE profile - every value below is
// a placeholder, not a real machine. See machine-profile.h and PORTING-PLAN.md.
/*
 * This is a schema, not data: it shows exactly which fields a real machine
 * profile needs and what each one means, mirroring the structure of
 * sargentmajord/Windows-GPU-UNLOCK's real profiles/z790-3060/profile.h
 * (GPL-2.0-only) - but with no real values, because none exist yet for Turing.
 * Every one of these has to come from an actual diagnostic capture of one
 * specific real machine with a real validated card installed (see the
 * "Known validation gap" table in PORTING-PLAN.md for which PCI identities that
 * could even be). Producing a real profiles/<name>/profile.h needs:
 *
 *   1. A read-only diagnostic/capture program (internal-boot/boot/-equivalent,
 *      mirroring the Ampere project's own diagnostic.c/handoff.c) that reads and
 *      reports raw hardware state WITHOUT needing any of these values filled in
 *      first - this project does not have that program yet either. The Ampere
 *      project bootstrapped its own profile the same way: build the
 *      profile-less diagnostic first, run it on real hardware, then convert
 *      what it reports into a profile.h.
 *   2. A profile-generator tool (tools/profile_gen.py-equivalent) that turns
 *      that capture's raw output into the constants below.
 *   3. A real machine: one of the exact validated (device, subsystem) card
 *      identities from port.h's GuTargets table, in some real PC, with whatever
 *      console/display adapter that PC actually has.
 *
 * None of that exists in this project yet. This file exists so the shape of
 * what's needed is clear and reviewable now, not so it can be filled in from
 * guesswork - see the individual field comments for what each one actually
 * requires, and the #error below, which exists specifically so this placeholder
 * can never be mistaken for a real profile and built into working firmware.
 */
#pragma once

#error This is the TEMPLATE profile - a schema, not a real machine. It cannot be \
  built. Generate a real profiles/<name>/profile.h from an actual diagnostic \
  capture (see the comment at the top of this file and PORTING-PLAN.md), then \
  pass -I to that folder instead of letting machine-profile.h fall back to this \
  one.

#define GU_PROFILE_TEMPLATE 1

/* Build-mode gates for this profile - same meaning as the Ampere project's.
 * Until a manual unlock has actually been verified once on this real machine,
 * leave GU_HEADLESS_RESEARCH blocked here too (remove only after that, the same
 * order the Ampere project did it in). */
#if !__STDC_HOSTED__ && defined(GU_HEADLESS_RESEARCH)
#error Headless firmware builds are blocked for this profile
#endif
#if !__STDC_HOSTED__ && defined(GU_RELEASE_ONLY) && defined(GU_EXPERIMENTAL_UNLOCK)
#error The release-only test must not be combined with an unlock build
#endif

/* 1 if a combo console-splitter firmware component owns the console GOP on top
 * of the target GPU (seen on some AMI-based boards); 0 if the firmware's own
 * console driver(s) own each GPU's GOP directly. Determined from the real
 * capture, not guessed. */
#define GU_CONSOLE_AGGREGATE 0 /* placeholder */

#define GU_PROFILE_NAME "TEMPLATE-NOT-A-REAL-MACHINE"

/* ---- Values the checks pin that differ between cards and boards - read from
 * the card's own capture by the (not yet built) profile generator. ---- */
#define GU_TARGET_BUS 0U /* placeholder, informational only as of 2026-11-10 - checks.c's GuCheckConfig
   no longer pins Bus to this value (bus numbering is per-motherboard, not a fact about the card; see
   GuCheckConfig's own comment and port.c's matching 2026-11-04 change). Still generated/logged, not
   gated on. */
#define GU_CONFIG_CLASS_REV 0x00000000U     /* placeholder - PCI class/revision dword (config dword 2) */
#define GU_PCIE_CAP_OFFSET 0x00U            /* placeholder - PCI Express capability offset */
#define GU_PCIE_CAP_DWORD 0x00000000U       /* placeholder - its first dword */
#define GU_MSIX_CAP_OFFSET 0x00U            /* placeholder - MSI-X capability offset */
#define GU_MSIX_CAP_DWORD 0x00000000U       /* placeholder - its first dword */
#define GU_NVGOP_NAME L"nvgop-placeholder"  /* placeholder - file name of the card's GOP driver in the option ROM */
#define GU_CONSOLE_TEXT_FILE_GUID {0,0,0,{0,0,0,0,0,0,0,0}}  /* placeholder */
#define GU_CONSOLE_GFX_FILE_GUID {0,0,0,{0,0,0,0,0,0,0,0}}   /* placeholder */
#define GU_TARGET_SHORT "RTX 2080 Ti"
#define GU_TARGET_SHORT_NOSPACE "RTX2080Ti"
#define GU_TARGET_LONG "RTX 2080 Ti 22GB (VRAM-modified)"
#define GU_CONSOLE_SHORT "unknown"          /* placeholder - whatever console/display GPU the real test machine has */
#define GU_TARGET_DISPLAY_NUMBER "2080Ti"

/* ---- Target card. Must be one exact row from port.h's GuTargets table - see
 * PORTING-PLAN.md's "Known validation gap" before picking one. ---- */
#define GU_BOARD_ID 0x00000000U       /* placeholder - must match a GuTargets[].Device */
#define GU_BOARD_SUBSYSTEM 0x00000000U /* placeholder - must match a GuTargets[].Subsystem */
#define GU_BAR0_BASE 0x00000000ULL     /* placeholder, informational only as of 2026-11-10 - BarCheck/
   ConfigCheck no longer pin BAR0's address to this value, only its shape (16 MiB, correct ACPI
   descriptor/config-space encoding) - the real base address is live-detected via GetBarAttributes()
   every run, the same way GuRead32/GuWrite32 already resolved it through EFI_PCI_IO_PROTOCOL's own
   BarIndex parameter without ever needing to know it in advance. See BarCheck's own comment. */
#define GU_BAR0_END 0x00000000ULL      /* placeholder - BAR0 is a 16 MiB aperture on both GA10x and TU10x, but its base address is assigned per-system */
#define GU_CHIP_ID 0x00000000U         /* placeholder, informational only as of 2026-11-10 - ColdCheck no
   longer requires an exact match: this project has real data for exactly one TU102 SKU (10de:1e07,
   0x162000a1 on both of Cancretto's cards) and none for the other GuTargets-accepted id (10de:1e04,
   documented as a different variant) - pinning either as "the" correct value would be a guess. Rejected
   live instead: unmapped (0xffffffff), the 0xbadfxxxx "not ready" sentinel, or zero. See ColdCheck's
   own comment. */
#define GU_DEVICE_MEMORY_SIZE 0x0000U  /* placeholder - usable framebuffer size in MiB, decoded from
   NV_PFB_PRI_MMU_LOCAL_MEMORY_RANGE (register offset 0x100ce0, see app.c's TuringUsableFbMib() and its
   own comment) - NOT a direct register value and NOT register offset 0x1183a4, which this project used
   until 2026-11-06: that offset is populated on GA10x (Ampere) but reads a "not finished" sentinel on
   every real Turing capture seen so far, POSTed or not. */
#define GU_PROM_SHA256 "0000000000000000000000000000000000000000000000000000000000000000" /* placeholder - SHA-256 of this exact card's own 1 MiB expansion ROM image */
#define GU_ROOT_PORT_DEVICE 0U /* placeholder */
#define GU_ROOT_PORT_FUNCTION 0U /* placeholder */
#define GU_RTX_PATH_BYTES 0 /* placeholder - the target card's full UEFI device path, byte for byte, from the real capture */

/* ---- Console adapter (whatever GPU actually drives the display on the real
 * test machine - may not exist at all if the target card itself drives the
 * display; the Ampere reference machine has a separate console GPU, but that is
 * not a given for every machine). ---- */
#define GU_OTHER_GPU_BUS 0 /* placeholder */
#define GU_OTHER_GPU_ID 0x00000000U /* placeholder */
#define GU_OTHER_GPU_SUBSYSTEM 0x00000000U /* placeholder */
#define GU_OTHER_GPU_ROOT_DEVICE 0U /* placeholder */
#define GU_OTHER_GPU_ROOT_FUNCTION 0U /* placeholder */
#define GU_OTHER_GPU_ADR 0x00000000U /* placeholder */
#define GU_OTHER_GPU_PATH_BYTES 0 /* placeholder */

/* ---- Firmware console driver fingerprints - board/firmware specific, read
 * from the real capture the same way the Ampere profile's are. ---- */
#define GU_CONSOLE_TEXT_SIZE 0x0000U /* placeholder */
#define GU_CONSOLE_TEXT_STOP 0x0000U /* placeholder */
#define GU_CONSOLE_TEXT_VERSION 0x00U /* placeholder */
#define GU_CONSOLE_GFX_SIZE 0x0000U /* placeholder */
#define GU_CONSOLE_GFX_STOP 0x0000U /* placeholder */
#define GU_CONSOLE_GFX_VERSION 0x00U /* placeholder */

/* ---- NVIDIA GOP driver fingerprint for the target card itself. ---- */
#define GU_NVGOP_VERSION 0x00000U /* placeholder */
#define GU_NVGOP_IMAGE_SIZE 0x00000U /* placeholder */
#define GU_NVGOP_STOP 0x000U /* placeholder */
#define GU_NVGOP_TEXT_RVA 0x000U /* placeholder */
#define GU_NVGOP_TEXT_BYTES 0x00000U /* placeholder */
#define GU_NVGOP_TEXT_SHA256 "0000000000000000000000000000000000000000000000000000000000000000" /* placeholder */

/* ---- EDID/mouse driver - only fill these in if the real capture actually
 * shows one bound to the display handle; the Ampere reference machine has
 * none, and if Turing's test machine doesn't either, copy its GU_NO_EDID_DRIVER
 * block instead of guessing values here. ---- */
#define GU_NO_EDID_DRIVER 1 /* placeholder - confirm against the real capture, don't assume */
#define GU_EDID_IMAGE_SIZE 0U
#define GU_EDID_STOP 0U
#define GU_EDID_TEXT_BYTES 0U
#define GU_EDID_TEXT_SHA256 ""
#define GU_COMBO_SIZE 0U
#define GU_COMBO_STOP 0U
#define GU_COMBO_VERSION 0U
