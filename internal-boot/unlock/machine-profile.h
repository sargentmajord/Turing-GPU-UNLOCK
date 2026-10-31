// Turing GPU UNLOCK - Windows UEFI port. Adapted from
// sargentmajord/Windows-GPU-UNLOCK's internal-boot/unlock/machine-profile.h
// (GPL-2.0-only); see THIRD_PARTY_NOTICES.md.
/*
 * Machine profile selector - same mechanism as the Ampere project. A build
 * compiles for exactly one machine, whose constants live in one "profile.h". A
 * build passes -I <profile folder> to use a profile generated for a specific PC;
 * without one it falls back to "profiles/TEMPLATE/profile.h" below, which is NOT
 * a real machine - every value in it is a placeholder, and is #error-guarded so
 * it cannot be built into working firmware.
 *
 * This is not a detail that can be skipped or filled in from research: every
 * value in a real profile.h (chip id, PROM hash, display topology, console
 * driver fingerprints, PCI device paths, BAR0 base) is specific to one person's
 * actual machine, read from a real diagnostic capture of it. There is no
 * universal set of "the right Turing values" the way there might be for, say, a
 * register offset that is the same on every TU102 - see PORTING-PLAN.md.
 */
#pragma once
#if defined(__has_include)
# if __has_include("profile.h")
#  include "profile.h"
# else
#  include "profiles/TEMPLATE/profile.h"
# endif
#else
# include "profiles/TEMPLATE/profile.h"
#endif

#define GU_W2(x) L##x
#define GU_W(x) GU_W2(x)
