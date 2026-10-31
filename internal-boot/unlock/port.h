/* Turing GPU UNLOCK - Windows UEFI port. Adapted from
   sargentmajord/Windows-GPU-UNLOCK's internal-boot/unlock/port.h (GPL-2.0-only);
   see THIRD_PARTY_NOTICES.md. Not compiled or tested yet - see PORTING-PLAN.md. */
#pragma once
#include <Uefi.h>
#include <Protocol/PciIo.h>
#include <Protocol/DevicePath.h>
#include <stddef.h>

/* Every (device, subsystem) identity with an actual community-confirmed report of
 * this unlock working, quoted directly from nlzy/turing-unlock's own issue tracker
 * - not a guess, not a third-party database cross-reference. Every one of them is
 * explicitly reported as a VRAM-modified "22GB" card, never a stock retail board -
 * see PORTING-PLAN.md and README.md for why that matters. An earlier version of
 * this file (and this project's own docs and a GitHub comment) incorrectly said
 * "Dell-made" - that was wrong; "Dell" appears nowhere in any of these reports and
 * has been corrected throughout.
 *
 * Sources (github.com/nlzy/turing-unlock):
 *  - issue #1: device 10de:1e07, no subsystem given ("modified-memory card")
 *  - issue #2 (iogotop): 10de:1e07 / 10de:12fa
 *  - issue #2 (arbulin): 10de:1e07 / 10de:12a4 ("modified-memory")
 *  - issue #2 (blackxwhite88): 10de:1e04 / 10de:12ae - note: the reporter
 *    themselves calls this "a different device variant, not 1e07"
 *  - issue #22 (both cards): 10de:1e07 / 10de:12fa ("modified-memory cards")
 *  - issue #25: device 10de:1e04, no subsystem given, stock/modified not stated
 *
 * Only entries with BOTH a device id and a subsystem id actually reported are
 * included below - issue #1 and #25 are cited above for context but contribute no
 * table entry, since matching on device id alone would also match an unvalidated
 * stock card that happens to share it. Refuse rather than guess: GuTargetConfig
 * (port.c) accepts only an exact match against one of these rows, nothing else -
 * including a standard, unmodified 2080 Ti, even if someone can get one to boot
 * this far. If that is the card you actually have, this project does not (yet)
 * consider it validated, on purpose. */
#define GU_TARGET_COUNT 3
typedef struct { UINT32 Device, Subsystem; const char *Source; } GU_TARGET;
static const GU_TARGET GuTargets[GU_TARGET_COUNT] = {
  {0x1e0710deu, 0x12fa10deu, "turing-unlock issue #2 (iogotop) / #22 (both cards)"},
  {0x1e0710deu, 0x12a410deu, "turing-unlock issue #2 (arbulin)"},
  {0x1e0410deu, 0x12ae10deu, "turing-unlock issue #2 (blackxwhite88) - device variant 1e04, not 1e07"},
};

#if defined(GU_HOST_WRITE_TEST) && __STDC_HOSTED__ != 1
#error GU_HOST_WRITE_TEST is forbidden in firmware builds
#endif
#if defined(GU_EXPERIMENTAL_UNLOCK) || (defined(GU_HOST_WRITE_TEST) && __STDC_HOSTED__ == 1)
#define GU_PREFLIGHT_ONLY 0
#else
#define GU_PREFLIGHT_ONLY 1
#endif

/* No GU_ENTRY_LATCHED / GU_ENTRY_LATCHED_SS1_* here (yet). Those are refinements
 * the Ampere port added after real hardware testing revealed a specific SS1
 * preservation requirement on the Ampere reference machine. Nothing analogous is
 * known for Turing yet - it may or may not need the same treatment. Do not copy
 * that logic over speculatively; add it only if and when real Turing hardware
 * testing shows it is actually needed, the same way the Ampere port did. */

typedef struct GU_DMA GU_DMA;
struct GU_DMA {
  GU_DMA *next;
  VOID *cpu, *mapping;
  UINT64 dma;
  UINTN bytes, pages;
  BOOLEAN mapped;
};
typedef struct {
  EFI_BOOT_SERVICES *bs;
  EFI_PCI_IO_PROTOCOL *pci;
  EFI_HANDLE image, handle;
  UINT64 bar_base, bar_size, ticks;
  UINT16 pm_cap, pcie_cap, msi_cap, msix_cap;
  UINT32 saved_config[64];
  UINT16 saved_devctl, saved_devctl2;
  UINT8 pcie_version;
  BOOLEAN fault, retained, started, quiesced, armed, owned, display_ok, saved;
  BOOLEAN profile_verified;
  /* Which row of GuTargets actually matched this card, set once by GuTargetConfig
   * and then checked by every later PCI-identity verification (GuSavePci, GuFlr,
   * GuEnableMaster, GuClearMaster) instead of a single compiled-in constant, since
   * there is no one reference identity here the way GU_BOARD_ID is for Ampere. */
  UINT32 target_device, target_subsystem;
  BOOLEAN master_attributes_pending;
  UINT64 master_original_attributes;
  UINT16 master_original_command;
  EFI_PCI_IO_PROTOCOL *master_pci;
  /* Containment may run at TPL_NOTIFY. Capture diagnostics without file IO;
     preserve first errors even if a later containment retry succeeds. */
  EFI_STATUS master_attribute_status, master_hardware_status;
  UINT64 master_last_attributes;
  UINT16 master_last_command;
  UINT32 flrs, mmio_writes, dma_count;
  GU_DMA *allocations;
  BOOLEAN (*display_check)(VOID);
} GU_PLATFORM;
extern GU_PLATFORM Gu;

VOID GuInit(EFI_BOOT_SERVICES *Bs, EFI_PCI_IO_PROTOCOL *Pci);
/* True only for an exact (device, subsystem) match against GuTargets above, in
 * which case Gu.target_device/target_subsystem are set to the matching row for
 * later checks to use. Deliberately narrower than nlzy/turing-unlock's own
 * supported_ids[] list (kernel/turing_unlock/main.c), which includes several
 * TU102 device IDs plus TU104 and TU106 marked "(not tested)" in its own source.
 * Refuse rather than guess, the same posture sargentmajord/Windows-GPU-UNLOCK's
 * installer already takes for Ampere cards. */
BOOLEAN GuTargetConfig(const UINT32 *Config, UINTN Segment, UINTN Bus, UINTN Device, UINTN Function);
BOOLEAN GuSafeToReturn(VOID);
EFI_STATUS GuDmaAllocate(UINTN Bytes, VOID **Cpu, UINT64 *Dma);
EFI_STATUS GuDmaFree(VOID *Cpu, UINT64 Dma);
UINT32 GuRead32(UINT32 Offset);
VOID GuWrite32(UINT32 Offset, UINT32 Value);
EFI_STATUS GuSavePci(VOID);
EFI_STATUS GuRestorePci(VOID);
EFI_STATUS GuFlr(VOID);
EFI_STATUS GuClearMaster(VOID);
EFI_STATUS GuEnableMaster(VOID);
EFI_STATUS GuConfigRead(EFI_PCI_IO_PROTOCOL_WIDTH Width, UINT32 Offset, VOID *Value);
EFI_STATUS GuConfigWrite(EFI_PCI_IO_PROTOCOL_WIDTH Width, UINT32 Offset, VOID *Value);
VOID GuStarted(VOID);
VOID GuQuiesced(VOID);
VOID GuLog(const char *Format, ...);
VOID GuLogText(const char *Text);
VOID GuCopyFromIo(VOID *Destination, const VOID *Source, size_t Bytes);
VOID *GuAlloc(size_t Bytes);
VOID GuFree(VOID *Memory);
VOID GuStall(UINTN Microseconds);
