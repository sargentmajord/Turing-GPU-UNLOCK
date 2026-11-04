/* Turing GPU UNLOCK - Windows UEFI port. Adapted from
   sargentmajord/Windows-GPU-UNLOCK's internal-boot/unlock/port.h (GPL-2.0-only);
   see THIRD_PARTY_NOTICES.md. Not compiled or tested yet - see PORTING-PLAN.md. */
#pragma once
#include <Uefi.h>
#include <Protocol/PciIo.h>
#include <Protocol/DevicePath.h>
#include <stddef.h>

/* Changed 2026-11-04: broadened from an exact (device, subsystem) allow-list to the
 * known Turing TU102 device-ID family, matching the same scope the read-only capture
 * tool (internal-boot/boot/probe.c) already uses. Previously this matched only the
 * specific board variants with a community-confirmed report in nlzy/turing-unlock's
 * own issue tracker (all VRAM-modified "22GB" cards) and refused everything else,
 * including a standard stock 2080 Ti, on purpose. That was the right call with no
 * real-world evidence to go on. Since then: README.md's "What real-world reports
 * actually show about the risk" section documents what full-thread reading of every
 * relevant public report actually shows - no report of this unlock mechanism
 * permanently damaging a card, including a detailed factory-stock 11GB TU102 success
 * report (physicsdolphin/nv-uefi-unlocker#103). What is still unproven is whether
 * *this project's own code* - never run against real hardware - handles any given
 * card correctly; that is what BarCheck/PromCheck/ColdCheck/DisplayCheck (app.c) are
 * for, and why this project ships a public risk disclaimer instead of only a private
 * one. This list is now a device-family filter, not a claim that every board sharing
 * one of these IDs has been individually confirmed - read README.md before running
 * this on real hardware. Device IDs below are the Turing-family IDs already
 * referenced throughout this project's docs/credits (10de:1e04, 10de:1e07); add
 * others here only once there is a real reason to believe this port's register
 * offsets apply to them too (e.g. same TU102/TU106 die family). */
#define GU_TARGET_COUNT 2
typedef struct { UINT32 Device, Subsystem; const char *Source; } GU_TARGET;
static const GU_TARGET GuTargets[GU_TARGET_COUNT] = {
  {0x1e0710deu, 0x0u, "TU102 2080 Ti family (10de:1e07) - any subsystem, see README.md"},
  {0x1e0410deu, 0x0u, "TU102 2080 Ti family, device variant 1e04 - any subsystem, see README.md"},
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
  /* TRUE if cpu came from GuDmaAllocateHigh's raw Gu.bs->AllocatePages(AllocateAddress,
   * ...) above 4 GiB, FALSE if from the normal Gu.pci->AllocateBuffer(AllocateAnyPages,
   * ...) path - GuDmaFree must call the matching Gu.bs->FreePages/Gu.pci->FreeBuffer, not
   * whichever one is convenient, since EDK2 does not guarantee these two allocators share
   * a free list. See PORTING-PLAN.md's "unlock.c adaptation status" for why the >4 GiB
   * preference exists at all (physicsdolphin's reported mailbox 0x91 on sub-4 GiB staging
   * reads) and why it is a safe try-first/fall-back-to-the-Ampere-validated-path addition,
   * not a speculative port of unconfirmed Turing-specific behavior. */
  BOOLEAN via_pages;
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
  /* profile_verified is set by ColdCheck, prom_verified by PromCheck - both reset to
   * FALSE at the start of their own check, same as the Ampere original's GU_PLATFORM
   * (this field was missing here; added while porting PromCheck/ColdCheck for real). */
  BOOLEAN profile_verified, prom_verified;
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
/* True for a device-id match against GuTargets above (subsystem 0 in the table
 * means "any subsystem" - see that table's own comment for why, changed
 * 2026-11-04), in which case Gu.target_device/target_subsystem are set from the
 * card's own live config space for later checks to use. Deliberately narrower
 * than nlzy/turing-unlock's own supported_ids[] list (kernel/turing_unlock/main.c),
 * which includes several TU102 device IDs plus TU104 and TU106 marked "(not
 * tested)" in its own source - this project only accepts device IDs with a real
 * reason (a community-confirmed report, or this project's own register-layout
 * assumptions) to believe they work. Refuse rather than guess, the same posture
 * sargentmajord/Windows-GPU-UNLOCK's installer already takes for Ampere cards. */
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
