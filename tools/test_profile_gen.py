"""Tests for profile_gen.py.

Unlike sargentmajord/Windows-GPU-UNLOCK's equivalent, the real-capture fixture here (run01) is checked
into the repo, not kept private: Cancretto posted it publicly (GitHub issue
sargentmajord/Windows-GPU-UNLOCK#2) specifically for this purpose, already sanitized by him before
posting, and it describes a stock, publicly-identifiable card rather than anyone's own research machine.

Run: python -m unittest tools/test_profile_gen.py -v
"""
import re
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import profile_gen as pg  # noqa: E402

REPO = Path(__file__).resolve().parent.parent
RUN01 = REPO / 'tools' / 'captures' / 'run01-cancretto-2080ti-ftw3'
RUN02 = REPO / 'tools' / 'captures' / 'run02-cancretto-2x2080ti-gpu2-display'
RUN03 = REPO / 'tools' / 'captures' / 'run03-cancretto-2x2080ti-gpu1-display'
PORT_H = (REPO / 'internal-boot' / 'unlock' / 'port.h').read_text(encoding='utf-8')


def refused(testcase, text, code, **kw):
    with testcase.assertRaises(pg.Refused) as cm:
        pg.build_profile(pg.parse_capture(text), 'test', **kw)
    testcase.assertEqual(cm.exception.findings[0].code, code, cm.exception.findings[0].message)
    return cm.exception.findings[0]


# A synthetic capture built from the real one: mutated to match an actual GuTargets row (device/subsystem
# only - nothing about the card's own real, as-captured hardware state). Everything else - the device
# path shape, the register values, the PROM hash - is genuinely from real hardware. This is what first
# proved build.ps1 actually compiles against a non-TEMPLATE profile at all.
#
# Until 2026-11-06 this also cleared the (then-checked) OFF_MEMORY register's "not ready" sentinel
# (PORTING-PLAN.md's 2026-11-03 entry) - no longer needed: OFF_FB_RANGE (the register the check now
# actually uses) already reads a ready, correct value in run01's own real, unmutated capture, including
# before this function's device/subsystem swap. Confirmed directly, not assumed, by running
# build_profile() against RUN01's raw text with no mutation at all (see RealCapture's own
# test_device_id_is_now_accepted_and_produces_a_working_profile) - it now succeeds on its own.
def synthetic_accepted_text():
    text = (RUN01 / 'DIAG00.TXT').read_text(encoding='utf-8')
    original = pg.parse_capture(text)
    old_device = '%08x' % original.cfg[0]
    new_device_int, new_subsystem_int, _ = pg.gu_targets_from_port_h(PORT_H)[0]
    new_device, new_subsystem = '%08x' % new_device_int, '%08x' % new_subsystem_int

    # Two lines carry this card's (device, subsystem): probe.c's own "cfg dwords=" (64 dwords, parsed as
    # c.cfg) and diagnostic.c's per-handle "pci config-dwords=" (16 dwords, parsed as Handle.pci_config) -
    # there are several of the latter (one per PCI handle in the body section), so match specifically the
    # one that is this target card's own, by its original (pre-mutation) device id, not just the first.
    for prefix in ('cfg dwords=', 'pci config-dwords='):
        m = re.search(r'^%s%s( [0-9a-f ]+)$' % (re.escape(prefix), old_device), text, re.M)
        dwords = m[1].split()      # dwords[0] is overall dword index 1 (device is dword 0, already matched)
        dwords[10] = new_subsystem  # overall dword index 11 = dwords[10]
        text = text[:m.start()] + prefix + new_device + ' ' + ' '.join(dwords) + text[m.end():]
    return text


class RealCapture(unittest.TestCase):
    """Against Cancretto's actual, unmutated capture - a stock card, in GuTargets' device-id family
    since 2026-11-04 (see port.h). Produces a working profile since 2026-11-06 (the OFF_FB_RANGE fix,
    see PORTING-PLAN.md's entry on that date) - before that, this capture was refused on the old,
    wrong OFF_MEMORY register, which reads an un-POSTed value here (this capture's iGPU was
    boot-primary, so the card itself was never POSTed - see PORTING-PLAN.md's "DisplayCheck: what the
    captures actually showed"); OFF_FB_RANGE doesn't depend on POST state the way OFF_MEMORY seemed to
    and reads the correct 11264 MiB regardless."""

    @classmethod
    def setUpClass(cls):
        if not (RUN01 / 'DIAG00.TXT').is_file():
            raise unittest.SkipTest('run01 fixture missing')
        cls.text = (RUN01 / 'DIAG00.TXT').read_text(encoding='utf-8')
        cls.cap = pg.parse_capture(cls.text)

    def test_capture_parses_as_clean(self):
        self.assertTrue(self.cap.complete)
        self.assertTrue(self.cap.result_ok)
        self.assertEqual(self.cap.probe_targets, [1])
        self.assertEqual(len(self.cap.targets), 1)

    def test_prom_hash_matches_what_the_reporter_computed_independently(self):
        self.assertEqual(self.cap.prom_sha, '281602497e3c00024dbc44db576d2781071f8b23e811121f728823df7ddedc9a')

    def test_gfw_registers_are_a_normal_halted_stock_card(self):
        self.assertEqual(self.cap.mmio[pg.OFF_PLM], 0xffffff8f)
        self.assertEqual(self.cap.mmio[pg.OFF_BOOT], 0x3ff)
        self.assertEqual(self.cap.mmio[pg.OFF_CPUCTL], 0x10)
        self.assertFalse(pg.is_bad_sentinel(self.cap.mmio[pg.OFF_PLM]))
        self.assertTrue(self.cap.mmio[pg.OFF_CPUCTL] & pg.CPUCTL_HALTED)

    def test_legacy_off_memory_register_still_reads_the_not_ready_sentinel(self):
        # OFF_MEMORY (0x1183a4) is no longer what the real check uses (see OFF_FB_RANGE's own comment,
        # PORTING-PLAN.md's 2026-11-06 entry) - kept only for informational logging now. This capture's
        # own un-POSTed state is exactly why that register looked like a POST-timing problem for a
        # while: it genuinely does read the sentinel here. Recorded so the historical reasoning in
        # PORTING-PLAN.md stays checkable against real data, not just prose.
        self.assertTrue(pg.is_bad_sentinel(self.cap.mmio[pg.OFF_MEMORY]))

    def test_device_id_is_now_accepted_and_produces_a_working_profile(self):
        # Changed 2026-11-04: device-id alone is enough to pass GuTargets now (port.h). Changed again
        # 2026-11-06: the OFF_FB_RANGE fix means this capture - the very first real one this project
        # ever got, un-POSTed and all - produces a usable profile. OFF_FB_RANGE (0x100ce0) reads 11264
        # MiB here even though this card was never POSTed, confirming the old "needs POST first" theory
        # (PORTING-PLAN.md's 2026-11-03 entry) was never the real explanation - the old register
        # (OFF_MEMORY) was just the wrong one, on every capture including this one.
        values, summary = pg.build_profile(pg.parse_capture(self.text), 'test')
        self.assertEqual(values['GU_DEVICE_MEMORY_SIZE'], '0x2c00U')
        self.assertEqual(summary['target']['memory_mib'], 11264)

    def test_gu_targets_from_port_h_finds_the_real_table(self):
        rows = pg.gu_targets_from_port_h()
        self.assertEqual(len(rows), 2)
        device = self.cap.cfg[0]
        self.assertTrue(any(d == device and s == 0 for d, s, _ in rows),
                         'this card\'s device id should now match a wildcard-subsystem row (changed 2026-11-04)')


class SyntheticAcceptedCard(unittest.TestCase):
    """Real hardware data (device path, registers, PROM hash) with only the (device, subsystem) and the
    one known-bad register swapped, to exercise the happy path - see synthetic_accepted_text()'s own
    comment for exactly what is and isn't real here."""

    @classmethod
    def setUpClass(cls):
        if not (RUN01 / 'DIAG00.TXT').is_file():
            raise unittest.SkipTest('run01 fixture missing')
        cls.text = synthetic_accepted_text()

    def test_produces_a_profile(self):
        values, summary = pg.build_profile(pg.parse_capture(self.text), 'synthetic-test')
        for key in pg.ORDER:
            self.assertIn(key, values)
        self.assertEqual(summary['target']['id'], '10de:1e07')

    def test_rendered_header_has_every_macro_once(self):
        values, _ = pg.build_profile(pg.parse_capture(self.text), 'synthetic-test')
        header = pg.render_header(values)
        for key in pg.ORDER:
            self.assertEqual(header.count('#define %s ' % key), 1, key)

    def test_other_gpu_fields_are_the_documented_placeholder(self):
        values, _ = pg.build_profile(pg.parse_capture(self.text), 'synthetic-test')
        self.assertEqual(values['GU_OTHER_GPU_ROOT_DEVICE'], '0U')
        self.assertEqual(values['GU_OTHER_GPU_ROOT_FUNCTION'], '0U')
        self.assertEqual(values['GU_OTHER_GPU_ADR'], '0x00000000U')


class DecodeFbRangeMib(unittest.TestCase):
    """Pure-function tests for decode_fb_range_mib() (2026-11-06 fix) - the Python mirror of app.c's
    TuringUsableFbMib(), which itself mirrors vendor/unlock.c's own layout-step decode and vendor/
    boot.c's turing_unlock_fb_range_ready(). These don't need a real capture - they pin the formula
    itself, independent of real_capture_values which only exercises the one value Cancretto's cards
    happen to produce (0x2c8 -> 11264)."""

    def test_the_real_captured_value_decodes_to_11_gib(self):
        self.assertEqual(pg.decode_fb_range_mib(0x2c8), 11264)

    def test_not_ready_conditions_all_return_none(self):
        self.assertIsNone(pg.decode_fb_range_mib(0xffffffff))      # unmapped/all-ones
        self.assertIsNone(pg.decode_fb_range_mib(0xbadf0000))      # bad-sentinel pattern
        self.assertIsNone(pg.decode_fb_range_mib(0xbadf1234))      # bad-sentinel, non-zero low bits
        self.assertIsNone(pg.decode_fb_range_mib(0x00000008))      # mag field is 0 (bits 4-9 of value)

    def test_ecc_reservation_bit_30_reduces_by_one_sixteenth(self):
        # Same adjustment vendor/unlock.c applies: fb_bytes = fb_bytes / 16 * 15 when bit 30 is set.
        without_ecc = pg.decode_fb_range_mib(0x2c8)
        with_ecc = pg.decode_fb_range_mib(0x2c8 | (1 << 30))
        self.assertEqual(with_ecc, without_ecc // 16 * 15)
        self.assertEqual(with_ecc, 10560)  # 11264 / 16 * 15

    def test_a_small_but_nonzero_magnitude_still_decodes(self):
        # mag=1, scale=0 -> 1 << 20 = 1 MiB - not a realistic card, but confirms the formula doesn't
        # special-case small values as "not ready" the way all-ones/bad-sentinel/mag-zero are.
        self.assertEqual(pg.decode_fb_range_mib(0x10), 1)


class MultiCardOneDisplayDisambiguation(unittest.TestCase):
    """Real hardware data, 2026-11-05 (GitHub issue sargentmajord/Windows-GPU-UNLOCK#2): both of
    Cancretto's 2080 Tis installed at once, iGPU disabled, only one card connected to a monitor at a
    time (swapped between the two captures) - exactly the 'one active, one idle' case PORTING-PLAN.md's
    'tools/profile_gen.py was deliberately NOT given the matching fix' note said this tool had no real
    data to verify against. Confirms the Python-side mirror of app.c's FindTarget/LocationHasValidGop
    disambiguation (added here) actually picks the displaying card, not merely that it stops refusing."""

    def test_picks_gpu2_when_only_gpu2_has_a_display(self):
        if not (RUN02 / 'DIAG00.TXT').is_file():
            raise unittest.SkipTest('run02 fixture missing')
        text = (RUN02 / 'DIAG00.TXT').read_text(encoding='utf-8')
        cap = pg.parse_capture(text)
        self.assertEqual(cap.probe_targets, [2])
        self.assertEqual(sorted(t['bus'] for t in cap.targets), [1, 2])
        pg.disambiguate_multi_target(cap)
        self.assertEqual(cap.probe_targets, [1])
        self.assertEqual(cap.targets[0]['bus'], 2)

    def test_picks_gpu1_when_only_gpu1_has_a_display(self):
        if not (RUN03 / 'DIAG00.TXT').is_file():
            raise unittest.SkipTest('run03 fixture missing')
        text = (RUN03 / 'DIAG00.TXT').read_text(encoding='utf-8')
        cap = pg.parse_capture(text)
        self.assertEqual(cap.probe_targets, [2])
        self.assertEqual(sorted(t['bus'] for t in cap.targets), [1, 2])
        pg.disambiguate_multi_target(cap)
        self.assertEqual(cap.probe_targets, [1])
        self.assertEqual(cap.targets[0]['bus'], 1)

    def test_both_captures_now_produce_a_working_profile(self):
        # Until 2026-11-06 disambiguation succeeding was not the same as a usable profile: both these
        # real captures hit the same 0xbadf5040 "not ready" sentinel run01's own capture did, on what
        # turned out to be the wrong register (OFF_MEMORY, 0x1183a4) - see PORTING-PLAN.md's 2026-11-06
        # root-cause entry. OFF_FB_RANGE (0x100ce0), the register Turing's own real unlock transaction
        # actually uses, reads a ready, correct 11264 MiB on both, on the card each one's own
        # disambiguation picks - the first real captures this project has that produce ok: true.
        for run, expected_bus in ((RUN02, 2), (RUN03, 1)):
            if not (run / 'DIAG00.TXT').is_file():
                raise unittest.SkipTest('fixture missing')
            text = (run / 'DIAG00.TXT').read_text(encoding='utf-8')
            values, summary = pg.build_profile(pg.parse_capture(text), 'test')
            self.assertEqual(summary['target']['bus'], expected_bus)
            self.assertEqual(summary['target']['memory_mib'], 11264)
            self.assertEqual(values['GU_DEVICE_MEMORY_SIZE'], '0x2c00U')

    def test_cfg_bar0_mmio_prom_are_not_mixed_up_between_the_two_real_cards(self):
        # Regression test for a real bug this fix's own first draft had, caught by actually testing
        # against these two captures rather than assuming the single-target parsing generalized: cfg/
        # bar0/mmio/prom-sha were parsed as single, capture-wide values (a flat regex scan of the whole
        # head, last-match-wins for scalars) - harmless on every capture before this one because none
        # had more than one probe-target block, but silently capable of attributing one card's own data
        # to the other the first time a two-target capture existed to expose it (probe.c's own ProbeBar
        # runs once per matching candidate, so these fields really do repeat once per target - checked
        # directly against probe.c, not assumed). Mutates only the non-chosen (bus=1) card's own PROM
        # hash to a value that belongs to neither real card, so a regression back to capture-wide
        # parsing would leak it into the result instead of the real, chosen (bus=2) card's own hash.
        if not (RUN02 / 'DIAG00.TXT').is_file():
            raise unittest.SkipTest('run02 fixture missing')
        text = (RUN02 / 'DIAG00.TXT').read_text(encoding='utf-8')
        first_prom = re.search(r'^prom sha256=[0-9a-f]{64}$', text, re.M)
        fake = 'ab' * 32
        mutated = text[:first_prom.start()] + 'prom sha256=' + fake + text[first_prom.end():]
        cap = pg.parse_capture(mutated)
        self.assertEqual(cap.targets[0]['bus'], 1)
        self.assertEqual(cap.targets[0]['prom_sha'], fake)       # the corruption landed on bus=1 only
        self.assertNotEqual(cap.targets[1]['prom_sha'], fake)    # bus=2's own prom is untouched
        pg.disambiguate_multi_target(cap)
        self.assertEqual(cap.targets[0]['bus'], 2)                # GOP disambiguation still picks bus=2
        self.assertNotEqual(cap.prom_sha, fake)                   # the corrupted bus=1 value must not leak through
        self.assertEqual(cap.prom_sha, '281602497e3c00024dbc44db576d2781071f8b23e811121f728823df7ddedc9a')


class Refusals(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not (RUN01 / 'DIAG00.TXT').is_file():
            raise unittest.SkipTest('run01 fixture missing')
        cls.text = synthetic_accepted_text()

    def mutate(self, pattern, repl, count=1):
        new, n = re.subn(pattern, repl, self.text, count=count, flags=re.M)
        self.assertGreaterEqual(n, 1, pattern)
        return new

    def test_incomplete_capture(self):
        refused(self, self.text.replace('capture-complete=1', 'capture-complete=0'), 'capture-incomplete')
        refused(self, self.text.replace('diagnostic-result status=0x0000000000000000', 'diagnostic-result status=0x8000000000000007'), 'capture-incomplete')

    def test_two_matching_cards(self):
        refused(self, self.mutate(r'^probe targets=0001$', 'probe targets=0002'), 'target-count')

    def test_mmio_unmapped_chip_id_all_ones(self):
        # Found against a real capture, not invented: one of Cancretto's six multi-adapter captures
        # (1x 2080 Ti + iGPU primary, Rebar on) had BAR0 not decoding at all - every single mmio
        # register in it, not just chip-id, read exactly 0xffffffff. Before this check existed,
        # profile_gen.py accepted that capture (ok: true) and would have baked 0xffffffff in as the
        # expected chip id, a profile that could never pass ColdCheck on any real hardware.
        f = refused(self, self.mutate(r'^(mmio off=0x00000000 status=0x0+ value=)0x[0-9a-f]+$', r'\g<1>0xffffffff'), 'mmio-unmapped')
        self.assertIn('0x000000', f.message)

    def test_gfw_register_bad_sentinel(self):
        f = refused(self, self.mutate(r'^(mmio off=0x00118128 status=0x0+ value=)0x[0-9a-f]+$', r'\g<1>0xbadf1234'), 'gfw-not-ready')
        self.assertIn('0x118128', f.message)

    def test_gfw_not_halted(self):
        refused(self, self.mutate(r'^(mmio off=0x00110100 status=0x0+ value=)0x[0-9a-f]+$', r'\g<1>0x00000000'), 'gfw-not-halted')

    def test_fb_range_register_not_ready_is_refused_not_baked_in(self):
        # OFF_FB_RANGE (0x100ce0), not the old OFF_MEMORY (0x1183a4) - see PORTING-PLAN.md's 2026-11-06
        # entry. All three of turing_unlock_fb_range_ready()'s own conditions, mirrored here:
        f = refused(self, self.mutate(r'^(mmio off=0x00100ce0 status=0x0+ value=)0x[0-9a-f]+$', r'\g<1>0xbadf0000'), 'memory-size-not-ready')
        self.assertIn('0x100ce0', f.message)
        refused(self, self.mutate(r'^(mmio off=0x00100ce0 status=0x0+ value=)0x[0-9a-f]+$', r'\g<1>0xffffffff'), 'memory-size-not-ready')
        refused(self, self.mutate(r'^(mmio off=0x00100ce0 status=0x0+ value=)0x[0-9a-f]+$', r'\g<1>0x00000008'), 'memory-size-not-ready')  # mag=0

    def test_prom_missing(self):
        refused(self, self.mutate(r'^prom sha256=.*$', ''), 'prom-missing')

    def test_bus_master_on(self):
        refused(self, self.mutate(r'^(cfg dwords=\S+ )00100003', r'\g<1>00100007'), 'bus-master')


if __name__ == '__main__':
    unittest.main()
