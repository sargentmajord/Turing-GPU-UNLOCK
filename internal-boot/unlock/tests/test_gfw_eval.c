// SPDX-License-Identifier: GPL-2.0-only
/* Host check for the firmware-boot assessment - pure bit-testing against the
 * three GU_GFW_* constants in gfw_eval.h (CPUCTL_HALTED/PLM_LEVEL0/
 * BOOT_COMPLETED), no GPU access, no compat shim. Values below are synthetic
 * bit patterns chosen to exercise each branch, not a real card's cold-boot
 * capture (none exists for Turing yet - same reasoning as test_layout.c).
 * Includes upstream issue 42: register-error values (0xbadfxxxx) must not
 * count as a completed firmware boot - carried into this file's own
 * gfw_eval.c, see its header comment. */
#include <stdio.h>
#include <errno.h>

#ifdef GU_UEFI_PORT
#error The offline gfw_eval harness must not compile the live UEFI port.
#endif

#include "../vendor/gfw_eval.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s (line %d)\n", #c, __LINE__); failures++; } } while (0)

int main(void) {
	/* Synthetic "boot complete" sample: CPUCTL halted, PLM level-0 set,
	 * boot byte fully 0xff - satisfies every bit test in turing_unlock_gfw_assess(). */
	struct turing_unlock_gfw_sample ok = {.config_ok = true, .plm = 0x8b8f, .boot = 0x3ff, .cpuctl = 0x10};
	CHECK(turing_unlock_gfw_assess(&ok) == 0);
	CHECK(turing_unlock_gfw_assess(NULL) == -ENODEV);

	struct turing_unlock_gfw_sample s = ok; s.config_ok = false;
	CHECK(turing_unlock_gfw_assess(&s) == -ENODEV);
	s = ok; s.plm = ~0U; CHECK(turing_unlock_gfw_assess(&s) == -ENODEV);
	s = ok; s.boot = ~0U; CHECK(turing_unlock_gfw_assess(&s) == -ENODEV);
	s = ok; s.cpuctl = ~0U; CHECK(turing_unlock_gfw_assess(&s) == -ENODEV);

	/* upstream issue 42 reproduction, then each field alone */
	s = (struct turing_unlock_gfw_sample){.config_ok = true, .plm = 0xbadf0001, .boot = 0xbadf00ff, .cpuctl = 0xbadf0010};
	CHECK(turing_unlock_gfw_assess(&s) == -ENODEV);
	s = ok; s.plm = 0xbadf0001; CHECK(turing_unlock_gfw_assess(&s) == -ENODEV);
	s = ok; s.boot = 0xbadf00ff; CHECK(turing_unlock_gfw_assess(&s) == -ENODEV);
	s = ok; s.cpuctl = 0xbadf0010; CHECK(turing_unlock_gfw_assess(&s) == -ENODEV);

	s = ok; s.cpuctl = 0x0; CHECK(turing_unlock_gfw_assess(&s) == -EAGAIN);  /* still running */
	s = ok; s.plm = 0x8b8e; CHECK(turing_unlock_gfw_assess(&s) == -EAGAIN); /* level-0 bit clear */
	s = ok; s.boot = 0x3fe; CHECK(turing_unlock_gfw_assess(&s) == -EAGAIN); /* not completed */

	/* command_restore: memory + bus-master bits (mask 0x6) come from "original";
	 * every other bit comes from "live", not "original" - verify the direction,
	 * not just that mask bits get restored. */
	CHECK(turing_unlock_command_restore(0x0006, 0x0003) == 0x0007); /* orig MEM+MASTER(0x6), live IO(0x1): 0x1|0x6=0x7 */
	CHECK(turing_unlock_command_restore(0x0000, 0x0007) == 0x0001); /* orig neither mask bit set: both cleared from live */
	CHECK(turing_unlock_command_restore(0x0006, 0x0410) == 0x0416); /* unrelated bits (0x410) come from live, mask bits (0x6) from original */

	if (failures) { printf("%d check(s) failed\n", failures); return 1; }
	puts("PASS firmware-boot assessment and command_restore, including register-error values");
	return 0;
}
