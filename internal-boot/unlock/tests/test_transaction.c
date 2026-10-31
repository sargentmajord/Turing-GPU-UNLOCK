// SPDX-License-Identifier: GPL-2.0-only
/* Host-side register-level harness for vendor/unlock.c - the "big one" PORTING-PLAN.md
 * deliberately deferred (see its "unlock.c adaptation status" / item 3). Unlike
 * sargentmajord/Windows-GPU-UNLOCK's tests/test_transaction.c, which checks its mock
 * against a REAL captured card (a genuine hardware oracle), no Turing capture exists
 * (see PORTING-PLAN.md's "two-stage architecture" and "Known validation gap"). So this
 * harness has no external oracle at all: every register offset and side effect below was
 * derived directly from reading vendor/unlock.c, vendor/boot.c, vendor/vbios.c and
 * vendor/prep.c's own code (cited inline), not assumed or copied from the Ampere project's
 * differently-shaped mock (Turing's falcon reset/boot sequencing genuinely differs from
 * Ampere's - e.g. "no BCR switch on Turing", see flcn_reset()'s own comment).
 *
 * This proves vendor/unlock.c is internally self-consistent end-to-end against a
 * synthetic, hand-built fixture (fake firmware blobs + a synthetic FWSEC/VBIOS image,
 * all clearly synthetic, no real board firmware contents - same posture as
 * tests/transaction_shim/vbios_fixture.h in the Ampere project). It proves nothing about
 * real TU102 hardware - a strictly smaller claim than Ampere's test makes, exactly like
 * tests/test_layout.c and tests/test_gfw_eval.c already say about themselves.
 *
 * Scope of this first pass - deliberately not full parity with Ampere's ~40-scenario
 * suite, which took real hardware-capture-driven iteration to build:
 *   - scenario 0: the full happy-path transaction succeeds, with a register-level
 *     verification of the published WPR metadata, the radix-walked GSP image (every
 *     byte, not just a checksum), the boot image copy, and the Booter signature patch.
 *   - scenario 1: the SEC2 payload never reports halted -> the busy-wait loop correctly
 *     times out (-ETIMEDOUT) and the error-path PCIe FLR cleanup still runs, frees every
 *     DMA buffer, and restores the original PCI command register.
 * NOT covered here yet (would need their own careful pass, same reasoning PORTING-PLAN.md
 * already gives for not attempting this file's full scope in one sitting): the
 * entry-time "reset required" PCIe-FLR branch, FWSEC's "WPR2 already up" fast path, GSP
 * (as opposed to SEC2) failing to boot, DMA/firmware-request failure injection, the
 * UEFI-port-only safety hooks (GuStarted/GuQuiesced/live-identity-recheck - this harness
 * never defines GU_UEFI_PORT, so none of that code compiles here at all), and a
 * non-page-aligned image size (this fixture's GSP image is an exact 2*4096 bytes).
 *
 * Found while writing this harness, not assumed: vendor/unlock.c calls ioremap()/
 * iounmap()/pci_resource_start() and udelay()/msleep()/pci_err() directly, none of which
 * compat/compat.h declared (it only had pci_iomap()/pci_iounmap(), which this file never
 * calls, and no delay function past usleep_range). Fixed in compat/compat.h and
 * compat.c in the same commit as this file - a real gap in the "compat shim needs no
 * changes" conclusion PORTING-PLAN.md drew about firmware.h specifically, not unlock.c's
 * own direct calls.
 */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void transaction_assert_failed(const char *expression, const char *file, int line)
{
	fprintf(stderr, "ASSERTION FAILED: %s:%d: %s\n", file, line, expression);
	exit(1);
}
#undef assert
#define assert(expression) ((expression) ? (void)0 : transaction_assert_failed(#expression, __FILE__, __LINE__))

#ifndef __KERNEL__
#define __KERNEL__
#endif
#ifdef GU_UEFI_PORT
#error The offline transaction harness must not compile the live UEFI port.
#endif

#include "../vendor/unlock.c"

/* ---- synthetic BAR0: covers everything unlock.c touches, including the 1 MiB VBIOS
 * window at 0x300000 and SEC_FALCON2+0x1000 (the minimum pci_resource_len it requires). */
static u32 regs[0x900000u / 4u];

/* ---- DMA bookkeeping: fake, 64 KiB-stepped addresses (always 4096-aligned, which
 * turing_unlock_radix_page() requires of every child pointer it writes), with a side
 * table mapping back to the real host buffer - same technique the Ampere project's own
 * test_transaction.c uses, generalized to not depend on allocation/free order. */
#define MAX_DMA 64
struct dma_entry { void *cpu; dma_addr_t dma; bool live; };
static struct dma_entry dma_table[MAX_DMA];
static int dma_alloc_count, dma_live_count, dma_free_count;

static struct {
	u16 cmd;
	unsigned long long now;
	int writes;
	int selector_writes, ss1_writes;
	int fw_requests;
	int group_live, group_before;
	int gsp_fires, sec_fires;
	bool gsp_ok, sec_ok; /* gate the "firmware ran to completion" side effect */
	int fail_alloc;      /* 1-based dma_alloc_coherent() call index to fail, 0 = never */
} fake;

static void *last_work;
static struct pci_dev dev;
static u64 dma_mask_storage;

/* ---- synthetic firmware blobs: no real board firmware contents, same posture as
 * tests/transaction_shim/vbios_fixture.h. Geometry constants (sizes, offsets, the
 * Booter header fields) are the fixed values vendor/boot.c itself requires - quoted
 * directly from reading it, not invented. */
static u8 g_booter_buf[72 + 59136 + 16];
static struct firmware g_booter_fw;
static u8 g_v67_buf[0xfa00];
static struct firmware g_v67_fw;
static u8 g_gsp_buf[266 + 8192];
static struct firmware g_gsp_fw;
static u8 g_boot_buf[4096];
static struct firmware g_boot_fw;
static u8 g_desc_buf[76];
static struct firmware g_desc_fw;
static u8 g_bl_buf[768];
static struct firmware g_bl_fw;

static void w16(u8 *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void w32b(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }
static u64 get64(const void *p) { const u8 *s = p; u64 v = 0; for (int i = 7; i >= 0; i--) v = (v << 8) | s[i]; return v; }

/* Fixed geometry vendor/boot.c's turing_unlock_booter_parse() requires exactly - quoted
 * from its own checks, not chosen freely. */
static void build_booter_fixture(void)
{
	memset(g_booter_buf, 0, sizeof(g_booter_buf));
	w32b(g_booter_buf + 0, 0x314c5442u); /* BOOTER_MAGIC */
	w32b(g_booter_buf + 4, 1);           /* version */
	u8 *h = g_booter_buf + 8;
	w32b(h + 0, 0);      w32b(h + 4, 256);    /* os_code_off/size */
	w32b(h + 8, 34048);  w32b(h + 12, 25088); /* os_data_off/size */
	w32b(h + 16, 1);                          /* num_apps */
	w32b(h + 20, 256);   w32b(h + 24, 33792); /* app_code_off/size */
	w32b(h + 28, 256);   w32b(h + 32, 0);     /* app_data_off/size */
	w32b(h + 36, 34560);                      /* patch_loc */
	w32b(h + 40, 13);                         /* ucode_id */
	w32b(h + 44, 1);                          /* engine_id */
	w32b(h + 48, 1);                          /* num_sigs */
	w32b(h + 52, 16);                         /* sig_bytes */
	w32b(g_booter_buf + 64, 59136); /* image_size == TU_BOOTER_IMAGE_SIZE */
	w32b(g_booter_buf + 68, 16);    /* sig_bytes == TU_BOOTER_SIG_TOTAL */
	for (size_t i = 0; i < 59136; i++)
		g_booter_buf[72 + i] = (u8)(i * 7 + 3);
	for (size_t i = 0; i < 16; i++)
		g_booter_buf[72 + 59136 + i] = (u8)(0xa0 + i);
	g_booter_fw = (struct firmware){ sizeof(g_booter_buf), g_booter_buf };
}

static void build_v67_fixture(void)
{
	for (size_t i = 0; i < sizeof(g_v67_buf); i++)
		g_v67_buf[i] = (u8)(i * 13 + 5);
	g_v67_fw = (struct firmware){ sizeof(g_v67_buf), g_v67_buf };
}

/* A minimal, valid ELF64 object with a ".fwimage" section - everything
 * turing_unlock_elf_section() checks (vendor/prep.c), nothing it does not. */
static void build_gsp_fixture(void)
{
	memset(g_gsp_buf, 0, sizeof(g_gsp_buf));
	u8 *e = g_gsp_buf;
	e[0] = 0x7f; e[1] = 'E'; e[2] = 'L'; e[3] = 'F';
	e[4] = 2; e[5] = 1; e[6] = 1;  /* ELFCLASS64, ELFDATA2LSB, EV_CURRENT */
	w32b(e + 20, 1);               /* e_version */
	w16(e + 52, 64);                /* e_ehsize */
	w16(e + 58, 64);                /* e_shentsize */
	w16(e + 60, 3);                  /* e_shnum: null, .shstrtab, .fwimage */
	w16(e + 62, 1);                  /* e_shstrndx */
	e[40] = 64; /* e_shoff = 64 (u64 LE, low byte only - well within range) */
	u8 *sh1 = e + 64 + 64;  /* section[1]: .shstrtab */
	w32b(sh1 + 0, 0);
	sh1[24] = 0; sh1[25] = 1; /* sh_offset = 256 (u64 LE) */
	sh1[32] = 10;              /* sh_size = 10 */
	u8 *sh2 = e + 64 + 128; /* section[2]: .fwimage */
	w32b(sh2 + 0, 1); /* sh_name -> shstrtab+1 == ".fwimage" */
	sh2[24] = 10; sh2[25] = 1; /* sh_offset = 266 (u64 LE) */
	sh2[32] = 0; sh2[33] = 0x20; /* sh_size = 8192 (u64 LE) */
	u8 *strtab = e + 256;
	strtab[0] = '\0';
	memcpy(strtab + 1, ".fwimage", 9); /* includes the trailing NUL */
	u8 *payload = e + 266;
	for (size_t i = 0; i < 8192; i++)
		payload[i] = (u8)(i * 3 + 1);
	g_gsp_fw = (struct firmware){ sizeof(g_gsp_buf), g_gsp_buf };
}

static void build_boot_fixture(void)
{
	for (size_t i = 0; i < sizeof(g_boot_buf); i++)
		g_boot_buf[i] = (u8)(i * 5 + 2);
	g_boot_fw = (struct firmware){ sizeof(g_boot_buf), g_boot_buf };
}

static void build_desc_fixture(void)
{
	memset(g_desc_buf, 0, sizeof(g_desc_buf));
	w32b(g_desc_buf, 4); /* turing_unlock_boot_desc_v4() requires version == 4 */
	g_desc_fw = (struct firmware){ sizeof(g_desc_buf), g_desc_buf };
}

static void build_bl_fixture(void)
{
	for (size_t i = 0; i < sizeof(g_bl_buf); i++)
		g_bl_buf[i] = (u8)(i * 11 + 7);
	g_bl_fw = (struct firmware){ sizeof(g_bl_buf), g_bl_buf };
}

/* Synthetic VBIOS image at BAR0+0x300000, where gsp_fwsec_frts() reads it from. The PCI
 * Option ROM / BIT-table chain (bytes 0..155ish) is the generic container format
 * vendor/vbios.c's expansion()/rom_signature()/data_signature() parse - not GPU-vendor-
 * specific, confirmed by reading that code directly. The FWSEC descriptor at offset 256
 * is version 2 (the only version gsp_fwsec_frts() accepts - it rejects version 3), with
 * every field and range constraint checked by hand against vbios.c's descriptor()
 * function before picking these numbers, not guessed. */
static void build_vbios_fixture(void)
{
	u8 *rom = (u8 *)regs + 0x300000;

	rom[0] = 0x55; rom[1] = 0xaa;        /* image signature 0xaa55 */
	w16(rom + 0x18, 32);                  /* PCIR pointer */
	memcpy(rom + 32, "PCIR", 4);
	w16(rom + 32 + 0x0a, 24);             /* PCI Data Structure length */
	w16(rom + 32 + 0x10, 8);              /* image size: 8 * 512 = 4096 bytes */
	rom[32 + 0x14] = 0;                   /* type: base image */
	rom[32 + 0x15] = 0x80;                /* last image in the chain */

	/* BIT table at offset 64. */
	rom[64] = 0xff; rom[65] = 0xb8; memcpy(rom + 66, "BIT", 3);
	rom[71] = 1; rom[72] = 12; rom[73] = 6; rom[74] = 1; /* header=12 stride=6 tokens=1 */
	{
		unsigned sum = 0;
		for (int i = 64; i < 75; i++) sum += rom[i];
		rom[75] = (u8)(0 - sum);
	}
	/* One token, pointing at a ucode table at offset 128. */
	rom[76] = 0x70; rom[77] = 2; rom[78] = 4;
	w32b(rom + 80, 100);
	w32b(rom + 100, 128);
	/* ucode table: version=1 header=6 entry_stride=6 entry_count=2. Entry 0 is a
	 * different ucode (skipped); entry 1 (type 0x85) is the FWSEC descriptor pointer. */
	rom[128] = 1; rom[129] = 6; rom[130] = 6; rom[131] = 2;
	rom[134] = 0x45;
	rom[140] = 0x85; w32b(rom + 142, 256);

	/* FWSEC descriptor (version 2) at offset 256. */
	u8 *d = rom + 256;
	w32b(d + 0, (60u << 16) | (2u << 8) | 1u); /* size=60 version=2 vDesc-bit set */
	w32b(d + 4, 300);   /* stored size -> image_size rounds up to 512 */
	w32b(d + 16, 0);    /* interface_off, relative to the dmem region below */
	w32b(d + 20, 0);    /* imem_pa */
	w32b(d + 24, 256);  /* imem_size */
	w32b(d + 28, 0);    /* imem_va */
	w32b(d + 32, 0);    /* imem_sec_base == imem_va: no secure/non-secure split needed */
	w32b(d + 36, 0);    /* sec_size */
	w32b(d + 40, 256);  /* data_off */
	w32b(d + 44, 0);    /* dmem_pa */
	w32b(d + 48, 256);  /* dmem_size */

	/* The "image" payload starts at rom+256+60=316: 256 unused code bytes, then the
	 * 256-byte dmem region fwsec_patch_frts() scans for its interface/mapper tables. */
	u8 *dmem = rom + 316 + 256;
	dmem[0] = 1; dmem[1] = 4; dmem[2] = 8; dmem[3] = 2; /* version=1 header=4 stride=8 entries=2 */
	w32b(dmem + 4, 4);   /* entry 0 type: the GSP-mapper descriptor */
	w32b(dmem + 8, 64);  /* entry 0 dest: mapper lives at dmem+64 */
	u8 *mapper = dmem + 64;
	w16(mapper + 4, 3);    /* mapper version 3 - the only one fwsec_patch_frts() accepts */
	w16(mapper + 6, 64);   /* mapper size */
	w32b(mapper + 8, 128); /* cmd_off, relative to dmem */
	w32b(mapper + 12, 44); /* cmd_size */
}

/* ---- register mocks ---- */
u32 readl(const void *p)
{
	size_t n = ((const u8 *)p - (const u8 *)regs) / 4;
	assert(n < ARRAY_SIZE(regs));
	return regs[n];
}

static void check_published_image(void);

void writel(u32 v, void *p)
{
	size_t off = (u8 *)p - (u8 *)regs;
	assert(off < sizeof(regs));
	fake.writes++;
	regs[off / 4] = v;
	if (off == GSP_BASE + 0x100 || off == GSP_BASE + 0x130) {
		fake.gsp_fires++;
		if (fake.gsp_ok) {
			regs[(GSP_BASE + 0x100) / 4] |= 1u << 4; /* falcon halted: unblocks poll() */
			regs[0x1fa824 / 4] = 0x1ffe000; /* WPR2 lo: matches this fixture's frts_offset>>12<<4 */
			regs[0x1fa828 / 4] = 0x10;      /* WPR2 hi: "active" */
		}
	}
	if (off == SEC_BASE + 0x100 || off == SEC_BASE + 0x130) {
		fake.sec_fires++;
		if (last_work) check_published_image();
		if (fake.sec_ok) {
			regs[(SEC_BASE + 0x100) / 4] |= 1u << 4;
			regs[(SEC_BASE + 0x40) / 4] = 0; /* mailbox0: firmware reports success */
			regs[TU_FEAT_PLM / 4] = ~0u;     /* PLM fully released */
		}
	}
	if (off == SEC_BASE + 0x118) {
		/* sec_dma()'s DMA-controller trigger: each call transfers exactly one 256-byte
		 * chunk (used only by sec_hs_fire()'s readback-verification block), so marking
		 * it "idle and done" (bit0 clear, bit1 set) right away satisfies both the
		 * per-chunk poll (mask=1 expect=0) and the final poll (mask=2 expect=2). */
		regs[off / 4] = 2;
	}
	if (off == TU_FEAT_SS0) fake.selector_writes++;
	if (off == TU_FEAT_SS1) fake.ss1_writes++;
}

void GuCopyFromIo(void *d, const void *s, size_t n)
{
	size_t off = (const u8 *)s - (const u8 *)regs;
	assert(off < sizeof(regs) && n <= sizeof(regs) - off);
	memcpy(d, (const u8 *)regs + off, n);
}

/* ---- allocation ---- */
VOID GuFree(VOID *p) { free(p); }
void *kzalloc(size_t n, int flags) { (void)flags; last_work = calloc(1, n); return last_work; }
void *kvmalloc(size_t n, int flags) { (void)flags; return malloc(n ? n : 1); }
void *kvcalloc(size_t n, size_t s, int flags) { (void)flags; return calloc(n ? n : 1, s ? s : 1); }

void *dma_alloc_coherent(struct device *d, size_t n, dma_addr_t *out, int flags)
{
	(void)d; (void)flags;
	dma_alloc_count++;
	if (fake.fail_alloc && dma_alloc_count == fake.fail_alloc)
		return NULL;
	void *p = calloc(1, n ? n : 1);
	assert(p);
	dma_addr_t a = 0x100000ull + (dma_addr_t)dma_alloc_count * 0x10000ull;
	int slot = -1;
	for (int i = 0; i < MAX_DMA; i++) if (!dma_table[i].live) { slot = i; break; }
	assert(slot >= 0);
	dma_table[slot].cpu = p; dma_table[slot].dma = a; dma_table[slot].live = true;
	dma_live_count++;
	*out = a;
	return p;
}

void dma_free_coherent(struct device *d, size_t n, void *p, dma_addr_t a)
{
	(void)d; (void)n;
	for (int i = 0; i < MAX_DMA; i++)
		if (dma_table[i].live && dma_table[i].cpu == p && dma_table[i].dma == a) {
			dma_table[i].live = false;
			free(p);
			dma_live_count--; dma_free_count++;
			return;
		}
	assert(0);
}

static void *dma_page(dma_addr_t a)
{
	for (int i = 0; i < MAX_DMA; i++)
		if (dma_table[i].live && dma_table[i].dma == a) return dma_table[i].cpu;
	assert(0);
	return NULL;
}

int dma_set_mask(struct device *d, u64 v) { *d->dma_mask = v; return 0; }
int dma_set_coherent_mask(struct device *d, u64 v) { d->coherent_dma_mask = v; return 0; }
void dma_wmb(void) {}

/* ---- devres / firmware ---- */
void *devres_open_group(struct device *d, void *id, int flags)
{
	(void)flags;
	assert(!fake.group_live && !id);
	fake.group_live = 1;
	fake.group_before = (int)d->devres_head.count;
	d->devres_head.count++;
	return &fake.group_live;
}

int devres_release_group(struct device *d, void *id)
{
	assert(fake.group_live && id == &fake.group_live);
	int released = (int)d->devres_head.count - fake.group_before - 1;
	d->devres_head.count = (unsigned int)fake.group_before;
	fake.group_live = 0;
	return released;
}

int request_firmware(const struct firmware **f, const char *name, struct device *d)
{
	fake.fw_requests++;
	const struct firmware *match = NULL;
	if (!strcmp(name, BOOTER_FILE)) match = &g_booter_fw;
	else if (!strcmp(name, V67_FILE)) match = &g_v67_fw;
	else if (!strcmp(name, GSP_FILE)) match = &g_gsp_fw;
	else if (!strcmp(name, BOOT_FILE)) match = &g_boot_fw;
	else if (!strcmp(name, BOOT_DESC_FILE)) match = &g_desc_fw;
	else if (!strcmp(name, SEC2_BL_FILE)) match = &g_bl_fw;
	if (!match) return -ENOENT;
	*f = match;
	d->devres_head.count++; /* Linux CONFIG_FW_CACHE: a successful request adds a devres */
	return 0;
}
void release_firmware(const struct firmware *f) { (void)f; }
void __module_get(void *p) { (void)p; }

/* ---- PCI config / resource mocks ---- */
int pci_read_config_word(struct pci_dev *p, int r, u16 *v) { (void)p; *v = r == PCI_COMMAND ? fake.cmd : 0; return 0; }
int pci_write_config_word(struct pci_dev *p, int r, u16 v) { (void)p; assert(r == PCI_COMMAND); fake.cmd = v; return 0; }
int pcie_capability_read_word(struct pci_dev *p, int r, u16 *v) { (void)p; *v = r == PCI_EXP_DEVSTA ? 9 : 0; return 0; }
int aperture_remove_conflicting_pci_devices(struct pci_dev *p, const char *n) { (void)p; (void)n; return 0; }
int pci_enable_device_mem(struct pci_dev *p) { (void)p; fake.cmd |= PCI_COMMAND_MEMORY; return 0; }
void pci_disable_device(struct pci_dev *p) { (void)p; }
resource_size_t pci_resource_len(struct pci_dev *p, int bar) { (void)p; return bar ? 0 : sizeof(regs); }
resource_size_t pci_resource_start(struct pci_dev *p, int bar) { (void)p; return bar ? 0 : 0; }
void *ioremap(resource_size_t start, resource_size_t len) { (void)start; assert(len <= sizeof(regs)); return regs; }
void iounmap(void *p) { (void)p; }
int pci_save_state(struct pci_dev *p) { (void)p; return 0; }
void pci_restore_state(struct pci_dev *p) { (void)p; }
void pci_set_master(struct pci_dev *p) { (void)p; fake.cmd |= PCI_COMMAND_MASTER; }
void pci_clear_master(struct pci_dev *p) { (void)p; fake.cmd &= (u16)~(u16)PCI_COMMAND_MASTER; }
int pcie_flr(struct pci_dev *p) { (void)p; return 0; }
struct pci_dev *pci_dev_get(struct pci_dev *p) { p->refs++; return p; }
void pci_dev_put(struct pci_dev *p) { p->refs--; }

/* ---- timing: a fake monotonic clock, same technique as the Ampere project's test. ---- */
ktime_t ktime_get(void) { return (ktime_t)fake.now; }
void usleep_range(unsigned long a, unsigned long b) { (void)b; fake.now += a; }
void udelay(unsigned long us) { fake.now += us; }
void msleep(unsigned long ms) { fake.now += (unsigned long long)ms * 1000; }

VOID GuLog(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

/* Called from writel()'s SEC2-fire branch, before any success/failure side effect is
 * applied - everything is still populated and nothing has been freed yet, the same hook
 * point the Ampere project's test uses for its own check_published_image(). */
static void check_published_image(void)
{
	struct tu_work *w = last_work;
	const u8 *meta = w->meta_cpu;

	assert(get64(meta + 0) == GU_WPR_META_MAGIC);
	assert(get64(meta + 8) == 1);
	assert(get64(meta + 16) == w->pages[0].dma);
	assert(get64(meta + 24) == w->image_span.size);
	assert(get64(meta + 72) == w->v67_dma);
	assert(get64(meta + 80) == TU_V67_SIZE);
	assert(!memcmp(w->v67_cpu, w->v67_fw->data, TU_V67_SIZE));
	assert(!memcmp(w->boot_cpu, w->boot_fw->data, w->boot_desc.image_size));
	/* fuse_ver=0, num_sigs=1 -> turing_unlock_booter_patch() always picks slot 0. */
	assert(!memcmp((const u8 *)w->booter_cpu + 34560, w->booter_fw->data + 72 + 59136, 16));

	u64 image_bytes = w->image_span.size;
	for (u64 page = 0, off = 0; off < image_bytes; page++, off += 4096) {
		const u8 *level0 = dma_page(get64(meta + 16));
		const u8 *level1 = dma_page(get64(level0 + (page >> 18) * 8));
		const u8 *level2 = dma_page(get64(level1 + ((page >> 9) & 511) * 8));
		const u8 *leaf = dma_page(get64(level2 + (page & 511) * 8));
		size_t bytes = (size_t)(image_bytes - off < 4096 ? image_bytes - off : 4096);
		assert(!memcmp(leaf, w->gsp_fw->data + w->image_span.offset + off, bytes));
		for (size_t i = bytes; i < 4096; i++) assert(!leaf[i]);
	}
}

static void setup(void)
{
	memset(&fake, 0, sizeof(fake));
	memset(regs, 0, sizeof(regs));
	memset(dma_table, 0, sizeof(dma_table));
	dma_alloc_count = dma_live_count = dma_free_count = 0;
	last_work = NULL;
	dma_mask_storage = ~0ull;
	dev = (struct pci_dev){ 0 };
	dev.dev.dma_mask = &dma_mask_storage;
	dev.refs = 1;
	fake.cmd = PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER; /* realistic post-power-on state */
	fake.gsp_ok = true;
	fake.sec_ok = true;

	build_vbios_fixture();
	/* Pure "hardware" inputs: nothing in unlock.c writes these before reading them, so
	 * the mock must preset them, the same way real devinit/straps already would have.
	 * usable_fb has to be large enough for turing_unlock_layout()'s fixed 88 MiB heap
	 * request to actually fit (unlock.c always asks for 88 MiB, unconditionally) - an
	 * 8 GiB card, the same scale tests/test_layout.c's own synthetic case already uses. */
	regs[0x100ce0 / 4] = 0x208; /* FB range: mag=32 scale=8 -> usable_fb = 8 GiB */
	regs[0x625f04 / 4] = 0;     /* VGA workspace: bit3 clear -> default vga_base */
	regs[GU_GFW_PLM_OFFSET / 4] = 0x1;    /* PLM level-0 */
	regs[GU_GFW_BOOT_OFFSET / 4] = 0xff;  /* boot completed */
	regs[GU_GFW_CPUCTL_OFFSET / 4] = 0x10; /* GSP falcon halted (== GSP_BASE+0x100) */
	regs[(GSP_BASE + 0x108) / 4] = 4;     /* HWCFG: imem_blks=4, room for the bootloader */
	/* wpr/boot-state registers start at 0, so entry's own "reset required?" check is
	 * false and the clean first-fire path (no initial PCIe FLR) is the one exercised. */
}

int main(void)
{
	build_booter_fixture();
	build_v67_fixture();
	build_gsp_fixture();
	build_boot_fixture();
	build_desc_fixture();
	build_bl_fixture();

	/* Scenario 0: the full happy-path transaction. */
	setup();
	{
		u16 original_cmd = fake.cmd;
		int ret = turing_unlock_card(&dev);

		if (ret) fprintf(stderr, "scenario 0 (happy path): unexpected error=%d\n", ret);
		assert(!ret);
		assert(fake.gsp_fires == 1 && fake.sec_fires == 1);
		assert(fake.selector_writes == 1 && fake.ss1_writes == 1);
		assert(regs[TU_FEAT_SS0 / 4] == TU_SS0_UNLOCK && regs[TU_FEAT_SS1 / 4] == TU_SS1_UNLOCK);
		assert(!regs[TU_FEAT_READOUT / 4]);
		assert(dma_live_count == 0 && dma_alloc_count == dma_free_count);
		assert(dev.dev.devres_head.count == 0);
		assert(fake.cmd == (u16)(original_cmd & ~(u16)PCI_COMMAND_MASTER));
		puts("scenario 0 (happy-path full transaction, synthetic fixture only): PASS");
	}

	/* Scenario 1: SEC2 never reports halted -> timeout, then the error-path PCIe FLR
	 * cleanup must still run to completion (every DMA buffer freed, PCI command
	 * register restored), not leave the card "retained". */
	setup();
	fake.sec_ok = false;
	{
		u16 original_cmd = fake.cmd;
		int ret = turing_unlock_card(&dev);

		if (ret != -ETIMEDOUT) fprintf(stderr, "scenario 1 (SEC2 never halts): unexpected error=%d\n", ret);
		assert(ret == -ETIMEDOUT);
		assert(fake.sec_fires == 1 && fake.gsp_fires == 1);
		assert(dma_live_count == 0 && dma_alloc_count == dma_free_count);
		assert(dev.dev.devres_head.count == 0);
		assert(fake.cmd == (u16)(original_cmd & ~(u16)PCI_COMMAND_MASTER));
		puts("scenario 1 (SEC2 timeout -> error-path FLR cleanup): PASS");
	}

	puts("All synthetic-fixture transaction checks passed; no hardware executed, no real firmware contents involved.");
	return 0;
}
