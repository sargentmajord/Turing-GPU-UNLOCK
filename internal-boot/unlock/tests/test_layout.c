// SPDX-License-Identifier: GPL-2.0-only
/* Host-side harness for vendor/layout.c - turing_unlock_layout() is pure
 * arithmetic (no readl/writel, no DMA, no PCI - confirmed directly before
 * writing this, see PORTING-PLAN.md's "unlock.c adaptation status"), so it
 * compiles and runs standalone with no compat shim and no mocks at all.
 *
 * Unlike sargentmajord/Windows-GPU-UNLOCK's test_transaction.c, which checks
 * its layout function against a REAL captured card's register values (a
 * genuine hardware oracle), this harness has no equivalent Turing capture to
 * check against - none exists yet (see PORTING-PLAN.md's "two-stage
 * architecture"). Inventing "plausible" real-looking numbers here would be
 * exactly the kind of guess dressed up as a fact the project's own standing
 * rule is to avoid. So every expected value below is independently
 * hand-derived from turing_unlock_layout()'s own published arithmetic
 * (align_down/align_up/sub_u64, read directly from vendor/layout.c) against
 * clearly-synthetic inputs, not from any claim about a real Turing card.
 * This proves the function's arithmetic is self-consistent and its range
 * checks actually fire; it proves nothing about real TU102 hardware, which
 * is a strictly smaller claim than Ampere's test makes and is labeled as
 * such throughout.
 */
#include <assert.h>
#include <stdio.h>
#include <stdint.h>

#ifdef GU_UEFI_PORT
#error The offline layout harness must not compile the live UEFI port.
#endif

#include "../vendor/layout.c"

/* Synthetic 8 GiB card, VGA reserving the top 16 MiB, no MMU lock active.
 * Every expected output below was hand-derived from the five-step
 * reserved -> wpr_end -> frts -> boot -> gsp chain in turing_unlock_layout(),
 * then the nonwpr/meta/heap -> heap_offset -> wpr_start -> nonwpr_start chain,
 * exactly as vendor/layout.c computes them - see the commit message for the
 * full derivation. */
static void test_synthetic_8gib(void) {
	struct turing_unlock_layout_in in = {
		.usable_fb = 0x200000000ull,       /* 8 GiB */
		.vga_base = 0x1FF000000ull,        /* 8 GiB - 16 MiB */
		.mmu_lock_valid = false,
		.frts_size = 0x100000ull,          /* 1 MiB */
		.boot_size = 0x6000ull,            /* 24576 bytes */
		.image_size = 0x5000000ull,        /* 80 MiB */
		.nonwpr_size = 0x80000ull,         /* 512 KiB: exercises align_up rounding to 1 MiB */
		.heap_request = 0x5780000ull,      /* 87.5 MiB: exercises align_up rounding to 88 MiB */
	};
	struct turing_unlock_layout_out out;

	assert(turing_unlock_layout(&in, &out) == 0);
	assert(out.reserved_top == 0x1FF000000ull);
	assert(out.wpr_end == 0x1FF000000ull);
	assert(out.frts_offset == 0x1FEF00000ull);
	assert(out.boot_offset == 0x1FEEFA000ull);
	assert(out.gsp_offset == 0x1F9EF0000ull);
	assert(out.heap_offset == 0x1F4600000ull);
	assert(out.heap_size == 0x5800000ull);     /* 88 MiB - heap_request rounded up, then re-truncated */
	assert(out.wpr_start == 0x1F4500000ull);
	assert(out.nonwpr_start == 0x1F4400000ull);
	/* The ordering invariants turing_unlock_layout() itself enforces. */
	assert(out.nonwpr_start < in.usable_fb);
	assert(out.wpr_end <= in.usable_fb);
	assert(out.wpr_start < out.wpr_end);
	assert(out.gsp_offset < out.boot_offset);
	assert(out.boot_offset < out.frts_offset);
	assert(out.frts_offset < out.wpr_end);
	puts("synthetic 8 GiB layout (hand-derived, not a hardware capture): PASS");
}

/* image_size larger than the space between boot and the WPR top must fail
 * via sub_u64's own underflow check (-ERANGE), before any output is trusted. */
static void test_oversized_image_rejected(void) {
	struct turing_unlock_layout_in in = {
		.usable_fb = 0x200000000ull,
		.vga_base = 0x1FF000000ull,
		.mmu_lock_valid = false,
		.frts_size = 0x100000ull,
		.boot_size = 0x6000ull,
		.image_size = 0x1FEEFA001ull,       /* one byte more than boot_offset itself */
		.nonwpr_size = 0x100000ull,
		.heap_request = 0x5800000ull,
	};
	struct turing_unlock_layout_out out;

	assert(turing_unlock_layout(&in, &out) == -ERANGE);
	puts("oversized image_size rejected via sub_u64 underflow: PASS");
}

/* Zero required inputs must be rejected up front, before any arithmetic. */
static void test_missing_inputs_rejected(void) {
	struct turing_unlock_layout_in in = {0};
	struct turing_unlock_layout_out out;

	assert(turing_unlock_layout(NULL, &out) == -EINVAL);
	assert(turing_unlock_layout(&in, NULL) == -EINVAL);
	assert(turing_unlock_layout(&in, &out) == -EINVAL);
	puts("missing/zero inputs rejected before arithmetic: PASS");
}

/* An MMU lock address below vga_base must be preferred as the reservation
 * boundary - this is the one branch test_synthetic_8gib() never takes. */
static void test_mmu_lock_preferred_when_lower(void) {
	struct turing_unlock_layout_in in = {
		.usable_fb = 0x200000000ull,
		.vga_base = 0x1FF000000ull,
		.mmu_lock_valid = true,
		.mmu_lock_low = 0x1FE000000ull,    /* below vga_base: must win */
		.frts_size = 0x100000ull,
		.boot_size = 0x6000ull,
		.image_size = 0x5000000ull,
		.nonwpr_size = 0x100000ull,
		.heap_request = 0x5800000ull,
	};
	struct turing_unlock_layout_out out;

	assert(turing_unlock_layout(&in, &out) == 0);
	assert(out.reserved_top == 0x1FE000000ull);
	assert(out.wpr_end == 0x1FE000000ull); /* already 128 KiB-aligned, same as vga_base was */
	puts("lower mmu_lock_low preferred over vga_base: PASS");
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	test_missing_inputs_rejected();
	test_synthetic_8gib();
	test_oversized_image_rejected();
	test_mmu_lock_preferred_when_lower();
	puts("All layout arithmetic checks passed (synthetic inputs only - no real Turing hardware capture exists yet).");
	return 0;
}
