/* Turing GPU UNLOCK - Windows UEFI port. Adapted from
   sargentmajord/Windows-GPU-UNLOCK's internal-boot/unlock/app.c (GPL-2.0-only);
   see THIRD_PARTY_NOTICES.md. Not compiled or tested yet - see PORTING-PLAN.md.

   IMPORTANT: this file is honest about what it can and cannot do yet. The safety
   functions (Halt, ReleaseHalt, LogContainment) are complete and need nothing
   machine-specific. Everything that depends on a real machine profile
   (ColdCheck, BarCheck, DisplayCheck, the full ConfigCheck) is a clearly marked
   stub, not a guess dressed up as an implementation - see the long comment in
   machine-profile.h and profiles/TEMPLATE/profile.h for exactly why, and
   PORTING-PLAN.md for the two-stage (capture, then unlock) architecture this
   mirrors from the Ampere project. */
#include "app.h"
#include "app_io.h"
#include "compat/compat.h"
#include "sha256.h"
#include "vendor/unlock.h"
#include "vendor/gfw_eval.h"
#include <Protocol/GraphicsOutput.h>
#include <IndustryStandard/Acpi.h>
#include <Guid/GlobalVariable.h>

static EFI_GUID PciGuid = EFI_PCI_IO_PROTOCOL_GUID, GopGuid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID,
  PathGuid = EFI_DEVICE_PATH_PROTOCOL_GUID, GlobalGuid = EFI_GLOBAL_VARIABLE;
static EFI_SYSTEM_TABLE *Table;
static UINT32 InitialConfig[64];
static const char *Stage;

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

/* ---- Everything below needs a real machine profile and is not implemented yet.
 * Each one explains what it needs and points at the real Ampere equivalent to
 * port from once a real profiles/<name>/profile.h exists - see PORTING-PLAN.md. ---- */

/* Needs: a capture-stage diagnostic program (internal-boot/boot/-equivalent) to
 * have already run on the real test machine, a profile_gen.py-equivalent to
 * have converted its output into a real profile.h, and this file built against
 * that profile instead of profiles/TEMPLATE. Port from app.c's BarCheck once
 * that exists - the logic itself (validate the BAR0 ACPI descriptor, confirm a
 * 16 MiB aperture) is not machine-specific, only GU_BAR0_BASE/GU_BAR0_END are. */
static EFI_STATUS BarCheck(VOID) {
  GuLogText("BarCheck: not implemented - needs a real machine profile, see PORTING-PLAN.md.\n");
  return EFI_UNSUPPORTED;
}
/* Needs the same real profile (GU_PROM_SHA256 is this exact card's own PROM
 * image hash, read once from a real capture). Port from app.c's PromCheck
 * unchanged once a real profile exists - the logic is generic. */
static EFI_STATUS PromCheck(VOID) {
  GuLogText("PromCheck: not implemented - needs a real machine profile, see PORTING-PLAN.md.\n");
  return EFI_UNSUPPORTED;
}
/* Needs GU_CHIP_ID, GU_DEVICE_MEMORY_SIZE and the exact expected cold-boot GFW
 * register values from a real capture of a real, currently-locked, confirmed
 * card - none of which can be known without one. Port from app.c's ColdCheck
 * once that exists. */
static EFI_STATUS ColdCheck(VOID) {
  GuLogText("ColdCheck: not implemented - needs a real machine profile, see PORTING-PLAN.md.\n");
  return EFI_UNSUPPORTED;
}
/* Needs the real console/display topology (which GPU, if any, drives the
 * display; its PCI path) from a real capture - this varies per test machine and
 * cannot be assumed. Port from app.c's DisplayCheck once that exists. */
static BOOLEAN DisplayCheck(VOID) {
  GuLogText("DisplayCheck: not implemented - needs a real machine profile, see PORTING-PLAN.md.\n");
  return FALSE;
}
/* Needs GU_CONFIG_CLASS_REV/GU_MSIX_CAP_OFFSET/GU_MSIX_CAP_DWORD from a real
 * capture, plus checks.c (GuCheckConfig/GU_CAPS) ported from the Ampere
 * project - neither exists here yet. */
static EFI_STATUS ConfigCheck(BOOLEAN Save) {
  (void)Save;
  GuLogText("ConfigCheck: not implemented - needs checks.c ported and a real machine profile, see PORTING-PLAN.md.\n");
  return EFI_UNSUPPORTED;
}
/* Needs ownership.c (GuClaimController/GuFindReleaseOwner/GuRefreshTargetPci)
 * ported from the Ampere project - not done here yet, and lower priority than
 * the profile-dependent checks above since it has no Turing-specific content to
 * work out, just needs porting. */
static EFI_STATUS Arm(VOID) {
  GuLogText("Arm: not implemented - needs ownership.c ported, see PORTING-PLAN.md.\n");
  return EFI_UNSUPPORTED;
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
  Stage = "usb-log"; EFI_STATUS status = GuIoOpen(Image, St); BOOLEAN attempted = FALSE;
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
  goto finish; /* GU_PREFLIGHT_ONLY is always true until the checks above are real - see PORTING-PLAN.md */
 finish:
  if (attempted && !GuSafeToReturn()) Halt();
  Gu.armed = FALSE;
  if (GuLogReady()) GuLog("FINAL status=%#llx stage=%s attempted=%u clean=%u\n", (unsigned long long)status, Stage, attempted, GuSafeToReturn());
  else GuConsole("Preflight or logging failed; no new attempt will start.\n");
  if (!GuLogReady()) { status = EFI_DEVICE_ERROR; if (Gu.started) Halt(); }
  if (attempted && !GuSafeToReturn()) Halt();
  EFI_STATUS closed = GuIoClose();
  if (EFI_ERROR(closed)) { if (Gu.started) Halt(); status = closed; }
  if (Gu.owned) {
    closed = Gu.bs->CloseProtocol(Gu.handle, &PciGuid, Image, NULL);
    if (EFI_ERROR(closed)) { if (Gu.started) Halt(); status = closed; } else Gu.owned = FALSE;
  }
  GuReleaseAssets();
  if (!EFI_ERROR(status)) GuConsole("UEFI preflight completed. Returning to firmware.\n");
  else GuConsole("Preflight stopped early. Returning without attempting anything.\n");
  (void)ReleaseHalt; (void)turing_unlock_card; (void)turing_unlock_gfw_assess; (void)ConfigCheck; (void)Arm;
  return status;
}
