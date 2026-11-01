/* Turing GPU UNLOCK - Windows UEFI port. Copied unchanged from
   sargentmajord/Windows-GPU-UNLOCK's internal-boot/boot/diagnostic.h (GPL-2.0-only);
   see THIRD_PARTY_NOTICES.md. Fully generic - nothing chip-specific in it. */
/* Observe the UEFI handle database; never detach a device. */
#pragma once
#include <Uefi.h>
#include <Protocol/SimpleFileSystem.h>
typedef struct CAPTURE_S {
  EFI_SYSTEM_TABLE *Table;
  EFI_FILE_PROTOCOL *File;
  EFI_STATUS Io;
  BOOLEAN Complete;
  UINTN Used, Total;
  char Buffer[4096];
} CAPTURE;
EFI_STATUS GuCaptureGraph(EFI_SYSTEM_TABLE *Table, EFI_FILE_PROTOCOL *Log);
