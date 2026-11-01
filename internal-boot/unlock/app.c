/* Turing GPU UNLOCK - Windows UEFI port. Adapted from
   sargentmajord/Windows-GPU-UNLOCK's internal-boot/unlock/app.c (GPL-2.0-only);
   see THIRD_PARTY_NOTICES.md. Compiles clean (both build.ps1 modes) and has been
   boot-tested only in the sense that the project's EFI build plumbing has been
   verified end to end - never against real hardware. See PORTING-PLAN.md.

   IMPORTANT: this file is honest about what it can and cannot do yet. The safety
   functions (Halt, ReleaseHalt, LogContainment) are complete and need nothing
   machine-specific. BarCheck, PromCheck and ColdCheck (2026-11-01) are real too -
   their logic was always generic, parameterized by profile.h's own fields, not
   dependent on any undiscovered Turing-specific hardware behavior. DisplayCheck
   is the one still a clearly marked stub, not a guess dressed up as an
   implementation - it genuinely needs this test machine's real console/display
   topology, which no profile exists for yet. See the long comment in
   machine-profile.h and profiles/TEMPLATE/profile.h for exactly why, and
   PORTING-PLAN.md for the two-stage (capture, then unlock) architecture this
   mirrors from the Ampere project. Against profiles/TEMPLATE (the only profile
   that exists), BarCheck/PromCheck/ColdCheck will all correctly refuse rather
   than pass, since every value they check is still a placeholder. */
#include "app.h"
#include "app_io.h"
#include "compat/compat.h"
#include "checks.h"
#include "machine-profile.h"
#include "ownership.h"
#include "sha256.h"
#include "vendor/unlock.h"
#include "vendor/gfw_eval.h"
#include <Protocol/GraphicsOutput.h>
#include <IndustryStandard/Acpi.h>
#include <Guid/GlobalVariable.h>

static EFI_GUID PciGuid = EFI_PCI_IO_PROTOCOL_GUID, GlobalGuid = EFI_GLOBAL_VARIABLE;
static EFI_SYSTEM_TABLE *Table;
static UINT32 InitialConfig[64];
static const char *Stage;
static GU_RELEASE_OWNER PendingOwner;

static EFI_STATUS Step(EFI_STATUS Status) {
  GuLog("CHECK stage=%s status=%#llx\n", Stage, (unsigned long long)Status);
  return !GuLogReady() && !EFI_ERROR(Status) ? EFI_DEVICE_ERROR : Status;
}
static VOID ConfigLog(const UINT32 *Config) {
  for (UINTN i = 0; i < 64; i += 4) GuLog("config[%02x]=%08x %08x %08x %08x\n",
    (unsigned)(i * 4), Config[i], Config[i + 1], Config[i + 2], Config[i + 3]);
}

/* ---- The actual safeguard: complete, needs no machine profile ---- */

/* Stops the machine rather than let it continue to Windows. No ResetSystem,
 * ExitBootServices, retry, protocol close or memory release - cleanup cannot be
 * proven safe, so none is attempted. The only way out of this loop is a hard
 * power cycle, which is always safe here: nothing this project does writes to
 * persistent storage (flash/EEPROM/VBIOS) - see PORTING-PLAN.md's safety
 * invariants. Ported unchanged from the Ampere original; this is exactly the
 * mechanism the person asked for when they asked for safeguards. */
static VOID Halt(VOID) {
  Gu.retained = TRUE;
  GuLogText("STOP: cleanup cannot be proven. Resources remain reserved.\nPOWER OFF using the physical power button; do not continue to Windows.\n");
  for (;;) Gu.bs->Stall(1000000);
}
static VOID LogContainment(VOID) {
  GuLog("BME containment: attribute_status=%#llx hardware_status=%#llx command=%#x cached=%#llx pending=%u\n",
    (unsigned long long)Gu.master_attribute_status, (unsigned long long)Gu.master_hardware_status,
    Gu.master_last_command, (unsigned long long)Gu.master_last_attributes, Gu.master_attributes_pending);
}
static VOID ReleaseHalt(EFI_STATUS Status, BOOLEAN Attempted) {
  EFI_TPL old = Gu.bs->RaiseTPL(TPL_NOTIFY);
  EFI_STATUS contained = GuClearMaster();
  Gu.bs->RestoreTPL(old);
  LogContainment();
  GuLog("FINAL status=%#llx stage=%s attempted=%u containment=%#llx pci_available=%u\n",
    (unsigned long long)Status, Stage, Attempted, (unsigned long long)contained, (unsigned)(Gu.pci != NULL));
  Halt();
}

/* ---- Target identification: complete, uses port.h's sourced GuTargets allow-list ---- */

static EFI_STATUS ReadConfig(UINT32 *Config) {
  for (UINTN i = 0; i < 64; i++) if (EFI_ERROR(GuConfigRead(EfiPciIoWidthUint32, (UINT32)i * 4, &Config[i]))) {
    GuLog("PCI config read failed: offset=%#x\n", (unsigned)(i * 4));
    return EFI_DEVICE_ERROR;
  }
  return EFI_SUCCESS;
}
static EFI_STATUS FindTarget(VOID) {
  EFI_HANDLE *handles = NULL; UINTN count = 0, found = 0;
  EFI_STATUS status = Gu.bs->LocateHandleBuffer(ByProtocol, &PciGuid, NULL, &count, &handles);
  GuLog("PCI enumeration: status=%#llx handles=%zu\n", (unsigned long long)status, (size_t)count);
  if (EFI_ERROR(status) || !handles || !count || count > 256) { if (handles) GuFree(handles); return EFI_NOT_FOUND; }
  for (UINTN i = 0; i < count; i++) {
    EFI_PCI_IO_PROTOCOL *pci = NULL; UINT32 config[12] = {0}; UINTN segment, bus, device, function;
    status = Gu.bs->HandleProtocol(handles[i], &PciGuid, (VOID **)&pci);
    if (!EFI_ERROR(status) && pci) status = pci->GetLocation(pci, &segment, &bus, &device, &function);
    if (!EFI_ERROR(status) && pci) for (UINTN w = 0; w < 12 && !EFI_ERROR(status); w++)
      status = pci->Pci.Read(pci, EfiPciIoWidthUint32, (UINT32)w * 4, 1, &config[w]);
    if (EFI_ERROR(status) || !pci) { GuLog("PCI scan failed: index=%zu status=%#llx\n", (size_t)i, (unsigned long long)status); status = EFI_DEVICE_ERROR; break; }
    if (GuTargetConfig(config, segment, bus, device, function)) { found++; Gu.handle = handles[i]; Gu.pci = pci; }
  }
  GuFree(handles); GuLog("PCI target matches=%zu status=%#llx\n", (size_t)found, (unsigned long long)status);
  if (EFI_ERROR(status) || found != 1) return EFI_UNSUPPORTED;
  status = ReadConfig(InitialConfig);
  if (!EFI_ERROR(status)) { Gu.owned = FALSE; ConfigLog(InitialConfig); }
  return status;
}

/* ---- BarCheck/PromCheck/ColdCheck: real now, not stubs - 2026-11-01. Each one's
 * underlying logic was already generic (profile.h's own TEMPLATE comments said so
 * for BarCheck/PromCheck specifically); ported from the Ampere app.c with only the
 * register offsets and profile fields swapped to Turing's. DisplayCheck stays a
 * stub below - unlike these three, it genuinely depends on this specific test
 * machine's console/display topology, which no amount of porting work can supply
 * without a real capture. Still built and run only against profiles/TEMPLATE
 * (placeholders, #error-guarded) and a synthetic QEMU boot, same as everything
 * else here - see PORTING-PLAN.md. */

/* TU_FEAT_PLM/SS0/SS1/READOUT are vendor/unlock.c's own private #defines (not
 * exposed through any header) - restated here with the exact same literal values,
 * cited back to their source, the same way probe.c already cites unlock.c's
 * register list rather than pulling in the vendor file's internals. Two separate
 * translation units, so no name collision; if unlock.c's values ever change,
 * these must be updated to match by hand. */
#define TU_FEAT_PLM 0x00409650u     /* vendor/unlock.c */
#define TU_FEAT_SS0 0x00409664u     /* vendor/unlock.c */
#define TU_FEAT_SS1 0x0040966cu     /* vendor/unlock.c */
#define TU_FEAT_READOUT 0x00409668u /* vendor/unlock.c */

/* Query BAR0 through the EDK2 PCI I/O protocol and confirm it is the 16 MiB memory
 * aperture both GA10x and TU10x expose (profile.h's own comment on GU_BAR0_END).
 * AddrRangeMax is accepted in either of its two valid readings - UEFI's own
 * inclusive-end and pinned EDK2 PciBusDxe's alignment value - both describe the
 * same aperture; this is a documented EDK2 implementation detail, not a guess
 * about undiscovered Turing-specific behavior. */
static EFI_STATUS BarCheck(VOID) {
  EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR *r = NULL;
  EFI_STATUS status = Gu.pci->GetBarAttributes(Gu.pci, 0, NULL, (VOID **)&r);
  GuLog("bar0 query: status=%#llx\n", (unsigned long long)status);
  if (!EFI_ERROR(status)) {
    if (r && r->Desc == ACPI_ADDRESS_SPACE_DESCRIPTOR && r->Len == sizeof(*r) - 3)
      GuLog("bar0 descriptor: type=%u granularity=%llu min=%#llx max=%#llx length=%#llx translation=%#llx\n",
        (unsigned)r->ResType, (unsigned long long)r->AddrSpaceGranularity, (unsigned long long)r->AddrRangeMin,
        (unsigned long long)r->AddrRangeMax, (unsigned long long)r->AddrLen, (unsigned long long)r->AddrTranslationOffset);
    if (!r || r->Desc != ACPI_ADDRESS_SPACE_DESCRIPTOR || r->Len != sizeof(*r) - 3 || r->ResType != 0 ||
      r->AddrRangeMin != GU_BAR0_BASE || r->AddrLen != 0x1000000 || r->AddrTranslationOffset ||
      (r->AddrRangeMax != GU_BAR0_END && r->AddrRangeMax != 0x00ffffff) ||
      r->AddrSpaceGranularity != 32 || ((UINT8 *)(r + 1))[0] != ACPI_END_TAG_DESCRIPTOR) status = EFI_UNSUPPORTED;
    else {
      UINT8 *raw = (UINT8 *)r, sum = 0;
      for (UINTN i = 0; i < sizeof(*r) + 2; i++) sum = (UINT8)(sum + raw[i]);
      if (raw[sizeof(*r) + 1] && sum) status = EFI_COMPROMISED_DATA;
      else { Gu.bar_base = r->AddrRangeMin; Gu.bar_size = r->AddrLen; }
    }
  }
  if (r) GuFree(r);
  return status;
}
/* GU_PROM_SHA256 is this exact card's own 1 MiB expansion-ROM hash, read once from
 * a real capture - same read (BAR0+0x300000, 1 MiB) gsp_fwsec_frts() and probe.c's
 * own PROM dump already use. Generic once Gu.bar_base is set, which only BarCheck
 * sets - this must run after it. */
static EFI_STATUS PromCheck(VOID) {
  Gu.prom_verified = FALSE;
  Gu.profile_verified = FALSE;
  VOID *prom = GuAlloc(1048576); if (!prom) return EFI_OUT_OF_RESOURCES;
  GuCopyFromIo(prom, (VOID *)(UINTN)(Gu.bar_base + 0x300000), 1048576);
  BOOLEAN good = !Gu.fault && GuHashMatches(prom, 1048576, GU_PROM_SHA256);
  GuFree(prom); Gu.prom_verified = good; GuLog("live PROM hash matched=%u\n", (unsigned)good);
  return good ? EFI_SUCCESS : EFI_CRC_ERROR;
}
/* Chip id, device memory size and "has the firmware finished booting" are generic,
 * profile-driven checks - turing_unlock_gfw_assess() already encapsulates exactly
 * what "finished" means (PLM level-0, boot byte 0xff, CPUCTL halted) without this
 * file needing to know Turing's own exact cold-boot register values, which nobody
 * has captured yet. Deliberately NOT gating on an expected cold SS0/SS1 value the
 * way Ampere's ColdCheck does (its GU_STOCK_PROFILE_SS0/GU_ENTRY_LATCHED machinery
 * is itself the product of real-hardware research this project has none of yet -
 * see PORTING-PLAN.md's note on not copying that speculatively). SS0/SS1 are only
 * logged here for visibility; nothing is gated on their value until a real capture
 * says what a genuinely cold Turing card's selectors actually read. */
static EFI_STATUS ColdCheck(VOID) {
  Gu.profile_verified = FALSE;
  UINT32 chip = GuRead32(0);
  struct turing_unlock_gfw_sample s = { .config_ok = true, .plm = GuRead32(GU_GFW_PLM_OFFSET),
    .boot = GuRead32(GU_GFW_BOOT_OFFSET), .cpuctl = GuRead32(GU_GFW_CPUCTL_OFFSET) };
  UINT32 wpr_hi = GuRead32(0x1fa828), ss0 = GuRead32(TU_FEAT_SS0), ss1 = GuRead32(TU_FEAT_SS1);
  UINT32 memory = GuRead32(0x1183a4);
  GuLog("cold state: chip=%#x boot_state=%#x gfw=%#x/%#x/%#x WPR2_HI=%#x SS0=%#x SS1=%#x memory=%#x\n",
    chip, GuRead32(0x1180f8), s.plm, s.boot, s.cpuctl, wpr_hi, ss0, ss1, memory);
#if !defined(GU_EXPERIMENTAL_UNLOCK)
  GuLog("DIAGNOSTIC PREFLIGHT ONLY: chip baseline=UNKNOWN memory=%#x expected=%#x; cannot arm; no GPU writes/reset/DMA/driver release\n", memory, GU_DEVICE_MEMORY_SIZE);
  return Gu.fault ? EFI_DEVICE_ERROR : EFI_NOT_READY;
#else
  GuLog("cold expected: profile=%s chip=%#x memory=%#x\n", GU_PROFILE_NAME, GU_CHIP_ID, GU_DEVICE_MEMORY_SIZE);
  if (Gu.fault) return EFI_DEVICE_ERROR;
  /* The same "reset required?" condition unlock.c's own entry point checks before
   * ever starting a transaction (wpr_hi>>4 nonzero, or boot_state's top 12 bits
   * set) - checked again here as a second, independent layer before arming, not
   * because unlock.c's own check is trusted any less. */
  if (chip != GU_CHIP_ID || memory != GU_DEVICE_MEMORY_SIZE || turing_unlock_gfw_assess(&s) ||
    (wpr_hi >> 4) || !Gu.prom_verified)
    return EFI_NOT_READY;
  Gu.profile_verified = TRUE;
  GuLog("EXPERIMENTAL exact-card profile %s verified; SS0=%#x SS1=%#x observed, not gated on - no real cold-state capture exists yet; hardware untested.\n",
    GU_PROFILE_NAME, ss0, ss1);
  return EFI_SUCCESS;
#endif
}
/* Needs the real console/display topology (which GPU, if any, drives the
 * display; its PCI path) from a real capture - this varies per test machine and
 * cannot be assumed. Port from app.c's DisplayCheck once that exists. */
static BOOLEAN DisplayCheck(VOID) {
  GuLogText("DisplayCheck: not implemented - needs a real machine profile, see PORTING-PLAN.md.\n");
  return FALSE;
}
/* ---- These two are real now: checks.c and ownership.c are both ported (see
 * their own file headers - neither has any Ampere-specific content, so nothing
 * here is blocked on a real machine profile the way BarCheck/PromCheck/ColdCheck/
 * DisplayCheck above are). GuCheckConfig still checks GU_CONFIG_CLASS_REV/
 * GU_BOARD_ID/etc. from whatever profile.h is in use - against the TEMPLATE
 * profile those are all placeholders, so this will correctly refuse, not
 * silently pass, until a real profile exists. ---- */
static EFI_STATUS ConfigCheck(BOOLEAN Save) {
  UINT32 config[64]; GU_CAPS caps; UINTN segment, bus, device, function;
  EFI_STATUS status = Gu.pci->GetLocation(Gu.pci, &segment, &bus, &device, &function);
  GuLog("target location: status=%#llx\n", (unsigned long long)status);
  if (EFI_ERROR(status)) return status;
  status = ReadConfig(config); if (EFI_ERROR(status)) return status;
  BOOLEAN checked = GuCheckConfig(config, segment, bus, device, function, &caps);
  BOOLEAN valid = checked && config[2] == GU_CONFIG_CLASS_REV && config[GU_MSIX_CAP_OFFSET / 4] == GU_MSIX_CAP_DWORD &&
    config[0x10 / 4] == GU_BAR0_BASE && !(config[(caps.pcie + 8) / 4] & 0x00200000U);
  BOOLEAN same = Save || !memcmp(config, InitialConfig, sizeof(config));
  if (Save || !valid || !same) ConfigLog(config);
  GuLog("target BDF=%zu:%zu:%zu.%zu checks=%u pm=%#x pcie=%#x msi=%#x msix=%#x\n",
    (size_t)segment, (size_t)bus, (size_t)device, (size_t)function, checked,
    (unsigned)caps.pm, (unsigned)caps.pcie, (unsigned)caps.msi, (unsigned)caps.msix);
  if (!valid) return EFI_UNSUPPORTED;
  if (!same) return EFI_NOT_READY;
  if (Save) memcpy(InitialConfig, config, sizeof(config));
  Gu.pm_cap = caps.pm; Gu.pcie_cap = caps.pcie; Gu.pcie_version = caps.pcie_version; Gu.msi_cap = caps.msi; Gu.msix_cap = caps.msix;
  return EFI_SUCCESS;
}
/* Manual-unlock keypress flow only - no automatic-boot mode here (see the file
 * header). Still correctly gated behind Gu.profile_verified, which only
 * ColdCheck ever sets, and ColdCheck is a stub - so this cannot actually arm
 * anything yet either, but the mechanics (prompt, read U/Esc, 30s timeout) are
 * real and need nothing machine-specific. */
static EFI_STATUS Arm(VOID) {
  if (!Gu.profile_verified || Gu.fault || !Gu.display_ok) return EFI_ACCESS_DENIED;
  if (!Table->ConIn || !Table->ConIn->Reset || !Table->ConIn->ReadKeyStroke) return EFI_UNSUPPORTED;
  EFI_STATUS status = Table->ConIn->Reset(Table->ConIn, FALSE); if (EFI_ERROR(status)) return status;
  GuLogText("EXPERIMENTAL MANUAL UNLOCK: verified PROM and assets.\n");
  if (PendingOwner.agent) GuLogText("U will release the RTX firmware driver, restore verified access settings if needed, then recheck and attempt once.\n");
  GuLogText("Press U to attempt once; Esc or 30-second timeout cancels.\n");
  if (!GuLogReady()) return EFI_DEVICE_ERROR;
  for (UINTN i = 0; i < 300; i++) {
    EFI_INPUT_KEY key; status = Table->ConIn->ReadKeyStroke(Table->ConIn, &key);
    if (!EFI_ERROR(status)) {
      if (key.ScanCode == 0x17 || key.UnicodeChar == 27) return EFI_ABORTED;
      if (key.UnicodeChar == 'u' || key.UnicodeChar == 'U') return Gu.profile_verified && !Gu.fault ? EFI_SUCCESS : EFI_ACCESS_DENIED;
    } else if (status != EFI_NOT_READY) return status;
    GuStall(100000);
  }
  return EFI_TIMEOUT;
}

/* ---- Entry point. The flow mirrors the Ampere app.c exactly in shape - read
 * checks before any write, target before ownership, asset loading before
 * arming - but most of it will return EFI_UNSUPPORTED until the stubs above are
 * real, which is the correct, safe behavior for code that has never compiled,
 * let alone run against real hardware: it should refuse to proceed, loudly,
 * rather than silently skip a check. ---- */
EFI_STATUS EFIAPI efi_main(EFI_HANDLE Image, EFI_SYSTEM_TABLE *St) {
  if (!St || !St->BootServices || !St->RuntimeServices) return EFI_INVALID_PARAMETER;
  Table = St; GuInit(St->BootServices, NULL); Gu.image = Image; Gu.display_check = DisplayCheck;
  Stage = "usb-log"; EFI_STATUS status = GuIoOpen(Image, St); BOOLEAN attempted = FALSE, release_attempted = FALSE;
  if (EFI_ERROR(status)) goto finish;
  GuLogText("BUILD MODE: early-stage port, most checks are stubs - see PORTING-PLAN.md.\n");
  UINT8 secure = 0xff; UINTN bytes = 1;
  Stage = "secure-boot";
  status = St->RuntimeServices->GetVariable(L"SecureBoot", &GlobalGuid, NULL, &bytes, &secure);
  if (EFI_ERROR(status) || bytes != 1 || secure) { status = EFI_SECURITY_VIOLATION; goto finish; }
  status = Step(status); if (EFI_ERROR(status)) goto finish;
  Stage = "watchdog"; status = Step(Gu.bs->SetWatchdogTimer(0, 0, 0, NULL)); if (EFI_ERROR(status)) goto finish;
  Stage = "target"; status = Step(FindTarget()); if (EFI_ERROR(status)) goto finish;
  Stage = "display"; Gu.display_ok = DisplayCheck(); status = Step(Gu.display_ok ? EFI_SUCCESS : EFI_UNSUPPORTED); if (EFI_ERROR(status)) goto finish;
  Stage = "bar0"; status = Step(BarCheck()); if (EFI_ERROR(status)) goto finish;
  Stage = "prom"; status = Step(PromCheck()); if (EFI_ERROR(status)) goto finish;
  Stage = "diagnostic-cold-state"; status = Step(ColdCheck());
#if !defined(GU_EXPERIMENTAL_UNLOCK)
  goto finish;
#else
  if (EFI_ERROR(status)) goto finish;
#endif
  Stage = "ownership"; status = GuClaimController();
  if (status == EFI_ACCESS_DENIED) {
    status = GuFindReleaseOwner(St, &PendingOwner, TRUE);
    if (!EFI_ERROR(status)) GuLogText("Exclusive ownership deferred until manual U and explicit driver release.\n");
  }
  status = Step(status); if (EFI_ERROR(status)) goto finish;
  Stage = "config-recheck"; status = Step(ConfigCheck(FALSE)); if (EFI_ERROR(status)) goto finish;
  Stage = "bar0"; status = Step(BarCheck()); if (EFI_ERROR(status)) goto finish;
  Stage = "prom"; status = Step(PromCheck()); if (EFI_ERROR(status)) goto finish;
  Stage = "cold-state"; status = Step(ColdCheck()); if (EFI_ERROR(status)) goto finish;
  Stage = "assets"; status = Step(GuLoadAssets()); if (EFI_ERROR(status)) goto finish;
  Stage = "arming"; status = Step(Arm()); if (EFI_ERROR(status)) goto finish;
  Stage = "armed-config"; status = Step(ConfigCheck(FALSE)); if (EFI_ERROR(status)) goto finish;
  Stage = "armed-display"; Gu.display_ok = DisplayCheck(); status = Step(Gu.display_ok ? EFI_SUCCESS : EFI_UNSUPPORTED); if (EFI_ERROR(status)) goto finish;
  Stage = "armed-cold-state"; status = Step(ColdCheck()); if (EFI_ERROR(status)) goto finish;
  if (PendingOwner.agent) {
    GU_RELEASE_OWNER current;
    Stage = "owner-recheck";
    GuLogText("Manual U confirmed: release RTX firmware driver once, then revalidate.\n");
    if (!GuLogReady()) { status = EFI_DEVICE_ERROR; goto finish; }
    status = GuFindReleaseOwner(St, &current, FALSE);
    if (!EFI_ERROR(status) && !GuSameReleaseOwner(&PendingOwner, &current)) status = EFI_ABORTED;
    if (EFI_ERROR(status)) { status = Step(status); goto finish; }
    EFI_PCI_IO_PROTOCOL *prior = Gu.pci;
    Stage = "driver-release"; Gu.armed = TRUE; Gu.pci = NULL; release_attempted = TRUE;
    EFI_STATUS released = Gu.bs->DisconnectController(Gu.handle, current.agent, NULL);
    EFI_TPL refresh_tpl = Gu.bs->RaiseTPL(TPL_NOTIFY);
    EFI_STATUS refreshed = GuRefreshTargetPci();
    Gu.bs->RestoreTPL(refresh_tpl);
    GuLog("driver release: status=%#llx refresh=%#llx same_pci=%u\n",
      (unsigned long long)released, (unsigned long long)refreshed, (unsigned)(Gu.pci == prior));
    status = Step(EFI_ERROR(released) ? released : EFI_ERROR(refreshed) ? refreshed : Gu.pci != prior ? EFI_COMPROMISED_DATA : EFI_SUCCESS);
    if (EFI_ERROR(status)) goto finish;
    Stage = "ownership-after-release"; status = Step(GuClaimController()); if (EFI_ERROR(status)) goto finish;
    Stage = "display-after-release"; Gu.display_ok = DisplayCheck(); status = Step(Gu.display_ok ? EFI_SUCCESS : EFI_UNSUPPORTED); if (EFI_ERROR(status)) goto finish;
    Stage = "config-after-release"; status = Step(ConfigCheck(FALSE)); if (EFI_ERROR(status)) goto finish;
    Stage = "bar0-after-release"; status = Step(BarCheck()); if (EFI_ERROR(status)) goto finish;
    Stage = "prom-after-release"; status = Step(PromCheck()); if (EFI_ERROR(status)) goto finish;
    Stage = "cold-after-release"; status = Step(ColdCheck()); if (EFI_ERROR(status)) goto finish;
  }
  Stage = "final-prom"; status = Step(PromCheck()); if (EFI_ERROR(status)) goto finish;
  Stage = "final-cold"; status = Step(ColdCheck()); if (EFI_ERROR(status)) goto finish;
  if (!Gu.profile_verified || Gu.fault) { status = EFI_ACCESS_DENIED; goto finish; }
  GuLogText("ARMED: one transaction is starting. Do not reset or remove the USB.\n");
  if (!GuLogReady()) { status = EFI_DEVICE_ERROR; goto finish; }
  Stage = "transaction"; Gu.armed = TRUE; attempted = TRUE;
  UINT64 mask = 0xffffffffULL;
  struct pci_dev device = { .dev = { .dma_mask = &mask, .coherent_dma_mask = 0xffffffffULL }, .pm_cap = Gu.pm_cap, .refs = 1 };
  int result = turing_unlock_card(&device);
  EFI_STATUS contained = GuClearMaster();
  LogContainment();
  GuLog("transaction result=%d containment=%#llx started=%u quiesced=%u fault=%u retained=%u maps=%u FLR=%u\n",
    result, (unsigned long long)contained, Gu.started, Gu.quiesced, Gu.fault, Gu.retained, Gu.dma_count, Gu.flrs);
  if (EFI_ERROR(contained) || !GuSafeToReturn() || Gu.allocations || (Gu.started && result)) Halt();
  Gu.armed = FALSE;
  status = result ? EFI_DEVICE_ERROR : EFI_SUCCESS;
 finish:
  if (release_attempted && (EFI_ERROR(status) || Gu.fault)) ReleaseHalt(status, attempted);
  if (attempted && !GuSafeToReturn()) Halt();
  Gu.armed = FALSE;
  if (GuLogReady()) GuLog("FINAL status=%#llx stage=%s attempted=%u driver_release=%u clean=%u\n", (unsigned long long)status, Stage, attempted, (unsigned)release_attempted, GuSafeToReturn());
  else GuConsole("Preflight or logging failed; no new attempt will start.\n");
  if (!GuLogReady()) { status = EFI_DEVICE_ERROR; if (release_attempted) ReleaseHalt(status, attempted); if (Gu.started) Halt(); }
  if (attempted && !GuSafeToReturn()) Halt();
  EFI_STATUS closed = GuIoClose();
  if (EFI_ERROR(closed)) { if (release_attempted) ReleaseHalt(closed, attempted); if (Gu.started) Halt(); status = closed; }
  if (Gu.owned) {
    closed = Gu.bs->CloseProtocol(Gu.handle, &PciGuid, Image, NULL);
    if (EFI_ERROR(closed)) { if (release_attempted) ReleaseHalt(closed, attempted); if (Gu.started) Halt(); status = closed; } else Gu.owned = FALSE;
  }
  GuReleaseAssets();
  if (!EFI_ERROR(status)) GuConsole("UEFI transaction completed and cleaned up. Returning to firmware; Windows validation is pending.\n");
  else GuConsole("Attempt cancelled or rejected. Returning without another attempt.\n");
  return status;
}
