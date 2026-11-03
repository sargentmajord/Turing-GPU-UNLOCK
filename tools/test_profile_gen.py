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
PORT_H = (REPO / 'internal-boot' / 'unlock' / 'port.h').read_text(encoding='utf-8')


def refused(testcase, text, code, **kw):
    with testcase.assertRaises(pg.Refused) as cm:
        pg.build_profile(pg.parse_capture(text), 'test', **kw)
    testcase.assertEqual(cm.exception.findings[0].code, code, cm.exception.findings[0].message)
    return cm.exception.findings[0]


# A synthetic capture built from the real one: mutated to match an actual GuTargets row (device/subsystem
# only - nothing about the card's own real, as-captured hardware state) and to clear the memory-size
# register's "not ready" sentinel this capture happened to show (see PORTING-PLAN.md's 2026-11-03 entry).
# Everything else - the device path shape, the register values, the PROM hash - is genuinely from real
# hardware. This is what first proved build.ps1 actually compiles against a non-TEMPLATE profile at all.
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

    text = re.sub(r'^mmio off=0x001183a4 status=0x0+ value=0x[0-9a-f]+$',
                  'mmio off=0x001183a4 status=0x0000000000000000 value=0x00003000', text, count=1, flags=re.M)
    return text


class RealCapture(unittest.TestCase):
    """Against Cancretto's actual, unmutated capture - a stock card, correctly outside GuTargets."""

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

    def test_memory_register_is_the_known_not_ready_sentinel(self):
        # The actual, as-captured finding this tool exists partly to record precisely - see
        # PORTING-PLAN.md's 2026-11-03 entry. Not a parsing bug: this is genuinely what the register read.
        self.assertTrue(pg.is_bad_sentinel(self.cap.mmio[pg.OFF_MEMORY]))

    def test_refused_for_not_being_a_target_card(self):
        f = refused(self, self.text, 'not-a-target-card')
        self.assertIn('0x1e0710de', f.message)
        self.assertIn('0xa0161b4c', f.message)

    def test_gu_targets_from_port_h_finds_the_real_table(self):
        rows = pg.gu_targets_from_port_h()
        self.assertEqual(len(rows), 3)
        device, subsystem = self.cap.cfg[0], self.cap.cfg[11]
        self.assertFalse(any(d == device and s == subsystem for d, s, _ in rows),
                          'this card must not already be in GuTargets - it is a stock card, not a confirmed unlock')


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

    def test_gfw_register_bad_sentinel(self):
        f = refused(self, self.mutate(r'^(mmio off=0x00118128 status=0x0+ value=)0x[0-9a-f]+$', r'\g<1>0xbadf1234'), 'gfw-not-ready')
        self.assertIn('0x118128', f.message)

    def test_gfw_not_halted(self):
        refused(self, self.mutate(r'^(mmio off=0x00110100 status=0x0+ value=)0x[0-9a-f]+$', r'\g<1>0x00000000'), 'gfw-not-halted')

    def test_memory_register_still_not_ready_is_refused_not_baked_in(self):
        f = refused(self, self.mutate(r'^(mmio off=0x001183a4 status=0x0+ value=)0x[0-9a-f]+$', r'\g<1>0xbadf0000'), 'memory-size-not-ready')
        self.assertIn('0x1183a4', f.message)

    def test_prom_missing(self):
        refused(self, self.mutate(r'^prom sha256=.*$', ''), 'prom-missing')

    def test_bus_master_on(self):
        refused(self, self.mutate(r'^(cfg dwords=\S+ )00100003', r'\g<1>00100007'), 'bus-master')


if __name__ == '__main__':
    unittest.main()
