/* Turing GPU UNLOCK - Windows UEFI port. New, not a port of anything - see this file's
   own design note below. GPL-2.0-only; see THIRD_PARTY_NOTICES.md.

   This is NOT a port of sargentmajord/Windows-GPU-UNLOCK's internal-boot/boot/handoff.c.
   That file (and the installer around it) assumes a dedicated, persistent boot
   partition with firmware-registered NVRAM boot entries and a Windows-partition
   chainload back to bootmgfw.efi - infrastructure this project has no installer for at
   all yet. Building that whole installer just to ask a few volunteers to run a
   read-only capture would be a lot of new, unvalidated surface for something that only
   needs to run once.

   Instead, this is meant to run as \EFI\BOOT\BOOTX64.EFI on an ordinary FAT32 USB
   stick - the one UEFI boot path that is a de facto universal convention across
   firmware vendors for removable media, with no firmware NVRAM registration needed at
   all (see the Windows-GPU-UNLOCK GitHub issue this project's own maintainer identity
   answered on 2026-10-31: fixed-disk ESP partitions don't reliably get an automatic
   NVRAM entry across OEM firmware, but removable media booting is universally
   supported). It writes its log to that same USB stick, never touches the PC's
   internal disks, boot order or NVRAM, and does not attempt to hand off to Windows
   itself - it just says when it's done and halts; the person removes the USB stick and
   restarts normally. */
#include <Uefi.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>
#include "diagnostic.h"

static EFI_GUID LoadedGuid = EFI_LOADED_IMAGE_PROTOCOL_GUID, FsGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
static EFI_SYSTEM_TABLE *Table;
static EFI_FILE_PROTOCOL *Root, *Log;

static VOID Say(const CHAR16 *Text) {
  if (Table && Table->ConOut) Table->ConOut->OutputString(Table->ConOut, (CHAR16 *)Text);
}
static VOID Stop(VOID) {
  Say(L"\r\nHALTED. Remove this USB drive, then restart your PC - it will start Windows normally; nothing on your internal disks was touched.\r\n");
  if (Table && Table->BootServices) {
    if (Table->BootServices->SetWatchdogTimer(0, 0, 0, NULL) != EFI_SUCCESS)
      Say(L"WARNING: firmware watchdog could not be disabled.\r\n");
  }
  for (;;) __asm__ volatile("pause");
}
static EFI_STATUS OpenLog(EFI_HANDLE OwnDevice) {
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
  EFI_STATUS s = Table->BootServices->HandleProtocol(OwnDevice, &FsGuid, (VOID **)&fs);
  if (s != EFI_SUCCESS || !fs) return EFI_UNSUPPORTED;
  s = fs->OpenVolume(fs, &Root); if (s != EFI_SUCCESS || !Root) return EFI_DEVICE_ERROR;
  /* Best-effort: a missing directory just means the capture log/PROM dump land at the
   * USB stick's root instead of in TURING2080TI-CAPTURE\ - not fatal either way. */
  EFI_FILE_PROTOCOL *dir = NULL;
  (void)Root->Open(Root, &dir, L"TURING2080TI-CAPTURE", EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, EFI_FILE_DIRECTORY);
  if (dir) dir->Close(dir);
  for (UINTN i = 0; i < 100; i++) {
    CHAR16 name[] = L"\\TURING2080TI-CAPTURE\\DIAG00.TXT";
    UINTN n = 0; while (name[n]) n++;
    name[n - 6] = (CHAR16)('0' + i / 10); name[n - 5] = (CHAR16)('0' + i % 10);
    EFI_FILE_PROTOCOL *existing = NULL;
    s = Root->Open(Root, &existing, name, EFI_FILE_MODE_READ, 0);
    if (s == EFI_SUCCESS) { if (existing) existing->Close(existing); continue; }
    if (s != EFI_NOT_FOUND) return s;
    s = Root->Open(Root, &Log, name, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
    return s == EFI_SUCCESS && Log ? EFI_SUCCESS : EFI_DEVICE_ERROR;
  }
  return EFI_VOLUME_FULL;
}
static EFI_STATUS Record(const char *Event, EFI_STATUS Status) {
  if (!Log) return EFI_DEVICE_ERROR;
  char text[160]; UINTN n = 0;
  while (*Event && n < 100) text[n++] = *Event++;
  const char *label = " status=0x"; while (*label) text[n++] = *label++;
  for (int i = 15; i >= 0; i--) text[n++] = "0123456789abcdef"[((UINT64)Status >> (i * 4)) & 15];
  text[n++] = '\r'; text[n++] = '\n'; UINTN written = n;
  EFI_STATUS s = Log->Write(Log, &written, text);
  if (s != EFI_SUCCESS || written != n) return EFI_DEVICE_ERROR;
  return Log->Flush(Log);
}
EFI_STATUS EFIAPI efi_main(EFI_HANDLE Image, EFI_SYSTEM_TABLE *St) {
  Table = St;
  if (!St || !St->BootServices) Stop();
  if (St->BootServices->SetWatchdogTimer(0, 0, 0, NULL) != EFI_SUCCESS) Stop();
  Say(L"Turing GPU UNLOCK - read-only hardware capture (not an unlock attempt).\r\n"
      L"This only reads PCI configuration space, memory-mapped registers and your\r\n"
      L"card's own expansion ROM, and writes one log file back to this USB drive.\r\n"
      L"Nothing on your internal disks, boot order or firmware NVRAM is touched.\r\n"
      L"Collecting data...\r\n");
  EFI_LOADED_IMAGE_PROTOCOL *loaded = NULL;
  EFI_STATUS s = St->BootServices->HandleProtocol(Image, &LoadedGuid, (VOID **)&loaded);
  if (s != EFI_SUCCESS || !loaded) Stop();
  s = OpenLog(loaded->DeviceHandle);
  if (s != EFI_SUCCESS) {
    Say(L"Could not open a log file on this USB drive. status=0x");
    CHAR16 hex[17]; for (int i = 15; i >= 0; i--) hex[15 - i] = L"0123456789abcdef"[((UINT64)s >> (i * 4)) & 15];
    hex[16] = 0; Say(hex); Say(L"\r\n");
    Stop();
  }
  (void)Record("turing-diagnostic-v1", EFI_SUCCESS);
  s = GuCaptureGraph(Table, Log);
  (void)Record("diagnostic-result", s);
  if (Log) { Log->Close(Log); Log = NULL; }
  if (Root) { Root->Close(Root); Root = NULL; }
  if (s == EFI_SUCCESS) Say(L"Capture complete and written to this USB drive.\r\n");
  else Say(L"Capture finished with some incomplete sections (see the log) - still safe, nothing was changed.\r\n");
  Stop();
  return EFI_ABORTED; /* unreachable: Stop() never returns */
}
