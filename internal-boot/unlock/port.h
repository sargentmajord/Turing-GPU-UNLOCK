/* Turing GPU UNLOCK - Windows UEFI port. Adapted from
   sargentmajord/Windows-GPU-UNLOCK's internal-boot/unlock/port.h (GPL-2.0-only);
   see THIRD_PARTY_NOTICES.md. Not compiled or tested yet - see PORTING-PLAN.md. */
#pragma once
#include <Uefi.h>
#include <Protocol/PciIo.h>
#include <Protocol/DevicePath.h>
#include <stddef.h>

/* Only nlzy/turing-unlock's own confirmed result, not its whole "supported" list -
 * see the long comment on GuTargetConfig below before changing this. */
#define GU_BOARD_ID 0x1e0710deu /* vendor 0x10de (NVIDIA), device 0x1e07 (TU102) */
#error GU_BOARD_SUBSYSTEM is not filled in. The one card nlzy/turing-unlock has \
  confirmed results on is a Dell-made RTX 2080 Ti 22GB (PCI ID 10de:1e07) - an \
  unusual OEM workstation variant - but the exact subsystem vendor/device ID (the \
  other half of a full PCI identity, usually what actually distinguishes one \
  board partner's card from another's on the same device ID) was not available \
  from the research that produced this scaffold. Checking device ID 0x1e07 alone \
  is not enough: a standard retail 2080 Ti may well share that same device ID \
  while being a genuinely different, unvalidated board - exactly the gap \
  PORTING-PLAN.md and README.md warn about. Fill this in from a real, sourced \
  read of the actual confirmed card (its own PCI config space, or nlzy's own \
  test records) before removing this #error, not from a guess.
#define GU_BOARD_SUBSYSTEM 0x00000000u /* placeholder - see the #error above */

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
/* True only for the one PCI identity (device + subsystem) this project is willing
 * to touch at all - see the long comment above GU_BOARD_SUBSYSTEM. Deliberately
 * narrower than nlzy/turing-unlock's own supported_ids[] list (kernel/turing_unlock/main.c),
 * which includes several TU102 device IDs plus TU104 and TU106 marked "(not tested)"
 * in its own source, none of which have any confirmed result anywhere. Refuse
 * rather than guess, the same posture sargentmajord/Windows-GPU-UNLOCK's installer
 * already takes for Ampere cards. */
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
