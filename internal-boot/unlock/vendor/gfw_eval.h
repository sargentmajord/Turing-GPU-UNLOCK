/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef TURING_UNLOCK_GFW_EVAL_H
#define TURING_UNLOCK_GFW_EVAL_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdbool.h>
#include <stdint.h>
typedef uint16_t u16;
typedef uint32_t u32;
#endif

/*
 * Offsets and fields from the pinned 610.43.03 TU102 headers:
 * dev_gc6_island.h, dev_gc6_island_addendum.h, dev_gsp.h, dev_falcon_v4.h.
 * GFW completion uses kflcnWaitForHalt_TU102 (Falcon CPUCTL), not RISC-V CPUCTL.
 */
#define GU_GFW_PLM_OFFSET 0x00118128u
#define GU_GFW_BOOT_OFFSET 0x00118234u
#define GU_GFW_CPUCTL_OFFSET 0x00110100u
#define GU_GFW_TIMEOUT_US 2050000u
/* GU_FEAT_PLM/SS0/SS1_OFFSET were here as 0x00823804/0x0082381c/0x00823820 -
 * those are sargentmajord/Windows-GPU-UNLOCK's real, actively-used GA102
 * selector registers, copied into this file unchanged along with everything
 * else in it. They are NOT Turing's equivalent registers and were unused by
 * anything in this project (confirmed by grepping internal-boot/ before
 * removing them) - unlock.c already defines the real, correct, actively-used
 * ones locally as TU_FEAT_PLM=0x00409650, TU_FEAT_SS0=0x00409664,
 * TU_FEAT_SS1=0x0040966c, TU_FEAT_READOUT=0x00409668. Left in place, these
 * would have been a landmine: nearly-identically-named to the real ones, and
 * wrong by a full register block. Removed rather than corrected, to avoid two
 * competing definitions of "the same" constant drifting apart later. */
#define GU_GFW_PLM_LEVEL0 0x1u
#define GU_GFW_BOOT_COMPLETED 0xffu
#define GU_GFW_CPUCTL_HALTED 0x10u
#define GU_PCI_COMMAND_IO 0x1u
#define GU_PCI_COMMAND_MEMORY 0x2u
#define GU_PCI_COMMAND_MASTER 0x4u

struct turing_unlock_gfw_sample {
	bool config_ok;
	u32 plm;
	u32 boot;
	u32 cpuctl;
};

/* 0 complete, -EAGAIN still running, -ENODEV inaccessible. */
int turing_unlock_gfw_assess(const struct turing_unlock_gfw_sample *sample);

/* Put memory and bus-master bits back. Leave every other command bit alone. */
u16 turing_unlock_command_restore(u16 original, u16 live);

#endif
