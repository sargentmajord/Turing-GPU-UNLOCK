/* Turing GPU UNLOCK - Windows UEFI port. Adapted from
   sargentmajord/Windows-GPU-UNLOCK's internal-boot/boot/probe.h (GPL-2.0-only); see
   THIRD_PARTY_NOTICES.md. No machine-profile.h dependency here (unlike Ampere's) - this
   tool has to run before any profile.h exists for this machine; see probe.c. */
#pragma once
#include <Uefi.h>
#include <Protocol/SimpleFileSystem.h>
#include "diagnostic.h"
EFI_STATUS GuProbeGpu(EFI_SYSTEM_TABLE *t, CAPTURE *c);
