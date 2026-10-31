/* Turing GPU UNLOCK - Windows UEFI port. Adapted from
   sargentmajord/Windows-GPU-UNLOCK's internal-boot/unlock/app_io.c (GPL-2.0-only);
   see THIRD_PARTY_NOTICES.md. Not compiled or tested yet - see PORTING-PLAN.md.

   Deliberately scoped down from the Ampere original: no GU_AUTOMATIC_BOOT, no
   GU_HEADLESS_RESEARCH, no GU_INTERNAL_BOOT modes here. Those are refinements the
   Ampere project added well after its basic manual-unlock path was validated on
   real hardware. Porting them speculatively, before this project has even
   compiled once, would be adding untested complexity on top of untested
   complexity. This file only knows GU_PREFLIGHT_ONLY (read-only checks) and
   GU_EXPERIMENTAL_UNLOCK (one manual transaction) - add the rest later, the same
   way the Ampere project actually did, if and when there's a real reason to. */
#include "app_io.h"
#include "compat/compat.h"
#include "format.h"
#include "sha256.h"
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>
#include <Guid/FileInfo.h>
static EFI_GUID LoadedGuid = EFI_LOADED_IMAGE_PROTOCOL_GUID, PathGuid = EFI_DEVICE_PATH_PROTOCOL_GUID,
  FsGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID, InfoGuid = EFI_FILE_INFO_ID;
static EFI_SYSTEM_TABLE *Table;
static EFI_FILE_PROTOCOL *Root, *Parent, *Run, *Report;
static BOOLEAN LogFault;

typedef struct { const char *name; CHAR16 *file; UINTN size; const char *hash; } ASSET_SPEC;
/* Sizes/hashes from firmware-tools/artifacts-610.43.03.json, nlzy/turing-unlock's
 * own pinned manifest - not guessed. Three sizes (marked 0, below) are not pinned
 * anywhere: extract_boot_assets.py builds those three blobs from parsed NVIDIA
 * driver source text at generation time, so their exact byte count is a property
 * of actually running that generator against the real downloaded NVIDIA package,
 * not a fixed a priori constant. Getting a size wrong here only makes a correctly
 * generated file fail Metadata()'s exact-size check and refuse to load - a safe
 * failure, not a dangerous one - but 0 must still be replaced with the real
 * number (by running the generator once and reading its output) before this
 * actually works, not guessed at either. */
static const ASSET_SPEC Specs[] = {
 {"turing-unlock/610.43.03/booter_load_tu102.bin", L"ASSETS\\booter_load_tu102.bin", 0 /* TODO: run extract_boot_assets.py, fill in */, "51507de61777cf46f3703a084cdfa896c21f6bc0c8af8f245bd09b2e9c49bf54"},
 {"turing-unlock/610.43.03/v67-tu102-610.43.03.bin", L"ASSETS\\v67-tu102-610.43.03.bin", 64000, "b0ec90294d1de3c67e024631c0a11000f281470d76eed806fc24c66b7fd1ef41"},
 {"turing-unlock/610.43.03/gsp_tu10x.bin", L"ASSETS\\gsp_tu10x.bin", 29352832, "73065619db9ec921d19fc4e519dd04d91a9199b525eaca9b257b89fb8c5ec52c"},
 {"turing-unlock/610.43.03/gsp_boot_tu102.bin", L"ASSETS\\gsp_boot_tu102.bin", 0 /* TODO: run extract_boot_assets.py, fill in */, "8765694cd38beaa4e2b8b1b83a31fb7aee6cb9d8b7c6bc7a86e6d931a3a056c2"},
 {"turing-unlock/610.43.03/gsp_boot_desc_tu102.bin", L"ASSETS\\gsp_boot_desc_tu102.bin", 0 /* TODO: run extract_boot_assets.py, fill in */, "9e7cca2cfdc5ce601b0adba39967850046108df6c2d23fff95578ca3f0ccbe01"},
 {"turing-unlock/610.43.03/sec2_bl_tu102.bin", L"ASSETS\\sec2_bl_tu102.bin", 768, "f21f1cfbb8fffa8067685314367fc4c50f3f84dd99eda7c19db4292d9269b9d9"}
};
#define GU_ASSET_COUNT 6

static VOID LogError(VOID) { LogFault = TRUE; if (Gu.armed) Gu.fault = TRUE; }
VOID GuConsole(const char *Text) {
  if (!Table || !Table->ConOut || !Table->ConOut->OutputString) { LogError(); return; }
  while (*Text) {
    CHAR16 line[128]; UINTN n = 0;
    while (*Text && n < 126) { if (*Text == '\n') line[n++] = '\r'; line[n++] = (UINT8)*Text++; }
    line[n] = 0; if (EFI_ERROR(Table->ConOut->OutputString(Table->ConOut, line))) { LogError(); return; }
  }
}
VOID GuLogText(const char *Text) {
  GuConsole(Text);
  if (!Report || LogFault) { LogError(); return; }
  UINTN expected = strlen(Text), written = expected;
  EFI_STATUS status = Report->Write(Report, &written, (VOID *)Text);
  if (EFI_ERROR(status) || written != expected || EFI_ERROR(Report->Flush(Report))) LogError();
}
VOID GuLog(const char *Format, ...) {
  char text[1024]; va_list args; va_start(args, Format);
  size_t n = GuFormatV(text, sizeof(text), Format, args); va_end(args);
  if (n >= sizeof(text)) { LogError(); GuConsole("Log format or length error.\n"); return; }
  GuLogText(text);
}
BOOLEAN GuLogReady(VOID) { return Report && !LogFault; }
static BOOLEAN UsbPath(EFI_HANDLE Handle) {
  EFI_DEVICE_PATH_PROTOCOL *path = NULL;
  if (EFI_ERROR(Table->BootServices->HandleProtocol(Handle, &PathGuid, (VOID **)&path)) || !path) return FALSE;
  UINTN total = 0; BOOLEAN usb = FALSE;
  for (UINTN nodes = 0; nodes < 64 && total + 4 <= 4096; nodes++) {
    UINTN n = path->Length[0] + ((UINTN)path->Length[1] << 8);
    if (n < 4 || n > 4096 - total) return FALSE;
    if (path->Type == MESSAGING_DEVICE_PATH && path->SubType == MSG_USB_DP && n == sizeof(USB_DEVICE_PATH)) usb = TRUE;
    if (path->Type == END_DEVICE_PATH_TYPE) return usb && n == 4 && path->SubType == END_ENTIRE_DEVICE_PATH_SUBTYPE;
    total += n; path = (VOID *)((UINT8 *)path + n);
  }
  return FALSE;
}
static EFI_STATUS Metadata(EFI_FILE_PROTOCOL *File, BOOLEAN Directory, UINTN Expected) {
  union { EFI_FILE_INFO info; UINT8 bytes[512]; } data; UINTN n = sizeof(data);
  EFI_STATUS status = File->GetInfo(File, &InfoGuid, &n, &data);
  if (EFI_ERROR(status)) return status;
  if (n < SIZE_OF_EFI_FILE_INFO || n > sizeof(data) || data.info.Size > n || data.info.Size < SIZE_OF_EFI_FILE_INFO ||
    !!(data.info.Attribute & EFI_FILE_DIRECTORY) != !!Directory ||
    (!Directory && data.info.FileSize != Expected)) return EFI_COMPROMISED_DATA;
  return EFI_SUCCESS;
}
EFI_STATUS GuIoOpen(EFI_HANDLE Image, EFI_SYSTEM_TABLE *St) {
  Table = St; Root = Parent = Run = Report = NULL; LogFault = FALSE; memset(GuAssets, 0, sizeof(GuAssets));
  EFI_LOADED_IMAGE_PROTOCOL *loaded = NULL; EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
  EFI_STATUS status = Table->BootServices->HandleProtocol(Image, &LoadedGuid, (VOID **)&loaded);
  if (EFI_ERROR(status) || !loaded || !UsbPath(loaded->DeviceHandle)) return EFI_UNSUPPORTED;
  status = Table->BootServices->HandleProtocol(loaded->DeviceHandle, &FsGuid, (VOID **)&fs);
  if (EFI_ERROR(status) || !fs) return EFI_UNSUPPORTED;
  status = fs->OpenVolume(fs, &Root); if (EFI_ERROR(status) || !Root) return EFI_DEVICE_ERROR;
#if defined(GU_EXPERIMENTAL_UNLOCK)
  status = Root->Open(Root, &Parent, L"\\RTX2080TI-UNLOCK", EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
#else
  status = Root->Open(Root, &Parent, L"\\RTX2080TI-PREFLIGHT", EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
#endif
  if (EFI_ERROR(status) || !Parent) return EFI_NOT_FOUND;
  status = Metadata(Parent, TRUE, 0); if (EFI_ERROR(status)) return status;
  for (UINTN slot = 0; slot < 100; slot++) {
    CHAR16 name[] = L"RUN00"; name[3] = (CHAR16)('0' + slot / 10); name[4] = (CHAR16)('0' + slot % 10);
    EFI_FILE_PROTOCOL *existing = NULL;
    status = Parent->Open(Parent, &existing, name, EFI_FILE_MODE_READ, 0);
    if (!EFI_ERROR(status)) {
      if (!existing || EFI_ERROR(existing->Close(existing))) return EFI_DEVICE_ERROR; continue;
    }
    if (status != EFI_NOT_FOUND) return status;
    status = Parent->Open(Parent, &Run, name, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, EFI_FILE_DIRECTORY);
    if (EFI_ERROR(status) || !Run) return EFI_DEVICE_ERROR;
    status = Metadata(Run, TRUE, 0); if (EFI_ERROR(status)) return status;
    status = Run->Open(Run, &existing, L"REPORT.TXT", EFI_FILE_MODE_READ, 0);
    if (!EFI_ERROR(status)) { if (existing) existing->Close(existing); return EFI_ALREADY_STARTED; }
    if (status != EFI_NOT_FOUND) return status;
    status = Run->Open(Run, &Report, L"REPORT.TXT", EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
    if (EFI_ERROR(status) || !Report) return EFI_DEVICE_ERROR;
#if defined(GU_EXPERIMENTAL_UNLOCK)
    GuLog("TURING RTX 2080 TI EXPERIMENTAL MANUAL UNLOCK\nRUN%02u\nManual U starts one hardware transaction; Esc or timeout cancels. Hardware success unverified - see PORTING-PLAN.md.\n", (unsigned)slot);
#else
    GuLog("TURING RTX 2080 TI read-only cold-state preflight\nRUN%02u\nCannot arm: GPU writes, DMA, reset and driver release are blocked.\n", (unsigned)slot);
#endif
    return GuLogReady() ? EFI_SUCCESS : EFI_DEVICE_ERROR;
  }
  return EFI_VOLUME_FULL;
}
EFI_STATUS GuLoadAssets(VOID) {
  for (UINTN i = 0; i < GU_ASSET_COUNT; i++) {
    EFI_FILE_PROTOCOL *file = NULL; const ASSET_SPEC *spec = &Specs[i];
    EFI_STATUS status = Parent->Open(Parent, &file, spec->file, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status) || !file) return EFI_NOT_FOUND;
    status = Metadata(file, FALSE, spec->size);
    UINT8 *data = NULL;
    if (!EFI_ERROR(status)) {
      data = GuAlloc(spec->size); if (!data) status = EFI_OUT_OF_RESOURCES;
    }
    for (UINTN done = 0; !EFI_ERROR(status) && done < spec->size;) {
      UINTN n = spec->size - done; if (n > 1048576) n = 1048576; UINTN want = n;
      status = file->Read(file, &n, data + done);
      if (!EFI_ERROR(status) && n != want) status = EFI_END_OF_FILE; done += n;
    }
    EFI_STATUS closed = file->Close(file); if (!EFI_ERROR(status)) status = closed;
    if (!EFI_ERROR(status) && !GuHashMatches(data, spec->size, spec->hash)) status = EFI_CRC_ERROR;
    if (EFI_ERROR(status)) { GuFree(data); return status; }
    GuAssets[i].name = spec->name; GuAssets[i].firmware.data = data; GuAssets[i].firmware.size = spec->size;
    GuLog("asset verified: %s bytes=%zu sha256=%s\n", spec->name, (size_t)spec->size, spec->hash);
    if (!GuLogReady()) return EFI_DEVICE_ERROR;
  }
  return EFI_SUCCESS;
}
VOID GuReleaseAssets(VOID) {
  if (!GuSafeToReturn()) return;
  for (UINTN i = 0; i < GU_ASSET_COUNT; i++) { GuFree((VOID *)GuAssets[i].firmware.data); memset(&GuAssets[i], 0, sizeof(GuAssets[i])); }
}
EFI_STATUS GuIoClose(VOID) {
  EFI_FILE_PROTOCOL **files[] = { &Report, &Run, &Parent, &Root }; EFI_STATUS result = EFI_SUCCESS;
  if (!GuSafeToReturn()) return EFI_ACCESS_DENIED;
  for (UINTN i = 0; i < 4; i++) if (*files[i]) {
    /* EFI_FILE_PROTOCOL.Close invalidates the handle even on error. */
    EFI_FILE_PROTOCOL *file = *files[i]; *files[i] = NULL;
    EFI_STATUS status = file->Close(file);
    if (EFI_ERROR(status) && !EFI_ERROR(result)) result = status;
  }
  return result;
}
