#!/usr/bin/env python3
# GPL-2.0-only. Part of Turing GPU UNLOCK.
"""Build a machine profile (the C header app.c/checks.c/port.c actually need to compile and run
BarCheck/PromCheck/ColdCheck) from one read-only hardware-capture of a real card (internal-boot/boot/,
capture-v0.1 release).

Deliberately smaller in scope than sargentmajord/Windows-GPU-UNLOCK's tools/profile_gen.py, which this is
adapted from: that tool also fingerprints a second ("console") adapter's firmware drivers, because the
Ampere installer's architecture needs one. Nothing in this project has that architecture, by design -
DisplayCheck (app.c, real as of 2026-11-04 - see PORTING-PLAN.md) checks that the *target* card itself has
its own bound display output, not a separate console adapter, so this tool still does not build or guess at
a two-adapter design Turing has never needed. This tool covers what's actually needed to COMPILE app.c/checks.c/port.c against a real,
single-adapter capture (confirmed by actually building against a first draft of this file and fixing what
broke, not assumed): BarCheck/PromCheck/ColdCheck's own fields, plus checks.c's GuCheckConfig/ConfigCheck
fields, all of which only need the target card's own capture - except GU_OTHER_GPU_* (a second adapter's
PCI identity/root-port/ADR), which this tool cannot supply without a second-adapter capture nobody has
sent yet. Those three are written as explicit, clearly-commented placeholders: harmless, because
ConfigCheck is only ever reachable behind GU_EXPERIMENTAL_UNLOCK (which nothing can reach yet either), but
still required to exist at compile time since checks.c compiles unconditionally. See this file's own
`PLACEHOLDER_OTHER_GPU` constant.

It also checks the captured (device, subsystem) against port.h's own GuTargets table, read directly out of
the C source rather than duplicated here, so the two can't drift. A capture from a card not on that list is
refused here, correctly, the same way the UEFI port itself refuses it at runtime (GuTargetConfig) - that is
expected for most real-world captures, not a flaw in the capture itself. See GitHub issue
sargentmajord/Windows-GPU-UNLOCK#2 for the first real one (a stock EVGA RTX 2080 Ti FTW3, correctly refused
here for exactly that reason, while everything the capture *can* independently confirm - a clean capture,
a verified PROM hash, firmware-boot-state registers that check out - still gets reported).

Usage:
  python profile_gen.py --capture DIAG00.TXT --profile-name NAME --out profile.h --summary summary.json
Exit codes: 0 profile written; 2 refused (findings printed as JSON); 1 usage or internal error.
"""
import argparse
import json
import re
import sys
from pathlib import Path

HEX = r'0x([0-9a-f]+)'
PORT_H = Path(__file__).resolve().parent.parent / 'internal-boot' / 'unlock' / 'port.h'
ESP_NODE_PCI_ROOT_HID = bytes.fromhex('d041030a')   # PNP0A03, same as sargentmajord/Windows-GPU-UNLOCK's own

# Registers app.c's ColdCheck reads (see app.c's TU_FEAT_* and GU_GFW_*_OFFSET, gfw_eval.h).
OFF_CHIP = 0x000000
# OFF_MEMORY (0x1183a4) was this project's device-memory-size register until 2026-11-06 - ported from
# Ampere's app.c, where it genuinely is a populated, direct MiB readout (real, hardware-verified there).
# It reads the known "not finished" 0xbadfxxxx sentinel on every real Turing capture seen so far
# (run01/02/03), including the two 2026-11-05 captures the GOP-disambiguation fix independently proves
# were POSTed and actively displaying - an architectural GA10x-vs-TU10x difference at that address, not
# a timing issue. Kept only for informational logging (see build_profile's summary); OFF_FB_RANGE below
# is what the check actually uses now.
OFF_MEMORY = 0x1183a4
# The real, hardware-verified Turing register for usable framebuffer size: NV_PFB_PRI_MMU_LOCAL_MEMORY_
# RANGE, read directly out of this project's own pristine, unmodified vendor/unlock.c (its layout step,
# citing kgspPopulateWprMeta_TU102) and vendor/boot.c's turing_unlock_fb_range_ready() - the real unlock
# transaction already depends on this exact register for the same purpose. decode_fb_range_mib() below
# mirrors that decode exactly and must stay byte-for-byte identical to app.c's own TuringUsableFbMib(),
# or a profile this tool generates could silently stop matching what app.c itself would accept.
OFF_FB_RANGE = 0x00100ce0
OFF_PLM = 0x00118128    # GU_GFW_PLM_OFFSET
OFF_BOOT = 0x00118234   # GU_GFW_BOOT_OFFSET
OFF_CPUCTL = 0x00110100  # GU_GFW_CPUCTL_OFFSET
CPUCTL_HALTED = 0x10     # GU_GFW_CPUCTL_HALTED (gfw_eval.h)

# checks.c's GuCheckConfig needs a second adapter's root-port device/function and its ADR value to exist
# at compile time (it compiles unconditionally; only ConfigCheck's *call* is GU_EXPERIMENTAL_UNLOCK-gated,
# and nothing can reach that today). This tool has no second-adapter capture to derive real values from -
# these are never evaluated by anything reachable, only required to exist, so a fixed placeholder is
# honest and harmless. Do not read these as meaningful.
PLACEHOLDER_OTHER_GPU = {'GU_OTHER_GPU_ROOT_DEVICE': 0, 'GU_OTHER_GPU_ROOT_FUNCTION': 0, 'GU_OTHER_GPU_ADR': 0}


class Finding:
    def __init__(self, code, message, fix=''):
        self.code, self.message, self.fix = code, message, fix

    def as_dict(self):
        return {'code': self.code, 'message': self.message, 'fix': self.fix}


class Refused(Exception):
    def __init__(self, findings):
        super().__init__('; '.join(f.message for f in findings))
        self.findings = findings


def refuse(code, message, fix=''):
    raise Refused([Finding(code, message, fix)])


def is_bad_sentinel(value):
    """0xbadfxxxx: 'register not a finished value yet' - upstream turing-unlock/Zagitta issue #42,
    already handled for the GFW registers by gfw_eval.c; app.c's own memory-size read is separate from
    that struct and isn't covered by it (see PORTING-PLAN.md's 2026-11-03 entry) - checked here instead."""
    return (value & 0xffff0000) == 0xbadf0000


def decode_fb_range_mib(range_value):
    """Decode NV_PFB_PRI_MMU_LOCAL_MEMORY_RANGE (OFF_FB_RANGE) into a usable-framebuffer-size-in-MiB
    integer, or None if the register isn't ready yet. Mirrors app.c's TuringUsableFbMib() and, through
    it, vendor/boot.c's turing_unlock_fb_range_ready() and vendor/unlock.c's own layout-step decode
    (mag/scale shift plus the bit-30 ECC-reservation adjustment) exactly - see OFF_FB_RANGE's own
    comment for why this must stay byte-for-byte identical to the C side. Verified against all three
    real Turing captures this project has: decodes to exactly 11264 (11 GiB), correct for Cancretto's
    stock 11GB cards, in every one - run01 (not POSTed) and run02/run03 (confirmed POSTed) alike, unlike
    the old OFF_MEMORY register this replaced."""
    if range_value == 0xffffffff or is_bad_sentinel(range_value):
        return None
    mag, scale = (range_value >> 4) & 0x3f, range_value & 0xf
    if not mag:
        return None  # mirrors turing_unlock_fb_range_ready()'s third condition exactly
    fb_bytes = mag << (scale + 20)
    if range_value & (1 << 30):
        fb_bytes = fb_bytes // 16 * 15
    return fb_bytes >> 20


def pci_device_path(nodes):
    """Return {'raw', 'root_dev', 'root_fn'} for a plain PCI device's own path - PciRoot(0)/Pci(a,b)/
    Pci(0,0)/END, no ADR node (that's only on a *display output's* path - see sargentmajord/
    Windows-GPU-UNLOCK's profile_gen.py, which keeps this case separate from its GOP-path classify_path
    for exactly this reason). A real capture (Cancretto, GitHub issue
    sargentmajord/Windows-GPU-UNLOCK#2) confirmed a plain PCI handle's path really is this 4-node shape on
    real hardware, not 5 - this project's own first draft of this function wrongly required the ADR node
    and refused every real capture because of it."""
    raw = b''.join(nodes)
    if len(raw) != 28 or len(nodes) != 4:
        return None
    n0, n1, n2, n3 = nodes
    if not (n0[:4] == bytes([2, 1, 12, 0]) and n0[4:8] == ESP_NODE_PCI_ROOT_HID and n0[8:12] == bytes(4)):
        return None
    if not (n1[:4] == bytes([1, 1, 6, 0]) and n2[:4] == bytes([1, 1, 6, 0])):
        return None
    if n3 != bytes([0x7f, 0xff, 4, 0]):
        return None
    return {'raw': raw, 'root_fn': n1[4], 'root_dev': n1[5]}


class Handle:
    def __init__(self, hid):
        self.id = hid
        self.path = []
        self.pci_location = None
        self.pci_config = None
        self.gop = None  # dict (framebuffer/bytes/mode/max_mode/[width/height/stride]) if this handle
        # supports EFI_GRAPHICS_OUTPUT_PROTOCOL - see diagnostic.c's Details(), 'gop' branch. width/
        # height/stride are only present if gp->Mode->Info was valid when captured (diagnostic.c logs a
        # separate "gop-info" problem line, not these fields, otherwise - see gop_is_sane()'s own note on
        # why PixelFormat specifically can never be checked here).


class Capture:
    pass


def parse_capture(text):
    """Same log format sargentmajord/Windows-GPU-UNLOCK's diagnostic.c/probe.c produce - the fields this
    function reads are written by the same Text()/Field()/Hex() helpers in both projects' diagnostic.c,
    and probe.c's own header says it's adapted from, not reinvented from, that project's."""
    text = text.replace('\r\n', '\n')
    c = Capture()
    c.complete = 'capture-complete=1\n' in text
    c.result_ok = 'diagnostic-result status=0x0000000000000000' in text
    first = text.find('\nhandle id=')
    head = text[:first] if first >= 0 else text
    body = text[first + 1:] if first >= 0 else ''

    c.probe_targets = [int(m, 16) for m in re.findall(r'^probe targets=([0-9a-f]+)$', head, re.M)]
    c.targets = []
    for m in re.finditer(r'^probe-target seg=%s bus=%s dev=%s fun=%s$' % (HEX, HEX, HEX, HEX), head, re.M):
        c.targets.append({'pos': m.start(), 'seg': int(m[1], 16), 'bus': int(m[2], 16), 'dev': int(m[3], 16), 'fun': int(m[4], 16)})
    # probe.c's own ProbeBar() runs once per matching candidate (see its header comment and the per-target
    # loop in probe.c), so cfg/caps/bar0/mmio/prom-sha each appear ONCE PER TARGET in the head, not once per
    # capture - checked directly against probe.c, not assumed, after noticing this project's own captures
    # never had more than one target before 2026-11-05's multi-card-with-one-display captures existed to
    # show it. Everything below is scoped to each target's own slice of the head (from its own
    # probe-target line to the next one's, or end of head) so a capture with more than one match can never
    # have one candidate's cfg/bar0/mmio/PROM-hash/capabilities silently attributed to a different one -
    # the old code here (a single flat regex scan of the whole head, last-match-wins for scalars) predates
    # any capture with more than one target and would have mixed the two up the first time their values
    # actually differed. Confirmed on Cancretto's two new captures that cfg differs between candidates
    # (device location, obviously) while mmio/prom happened to read identically in both cards there - this
    # scoping is what makes that not an accident the next capture is relied on to not expose.
    bounds = [t['pos'] for t in c.targets] + [len(head)]
    for i, t in enumerate(c.targets):
        seg = head[bounds[i]:bounds[i + 1]]
        m = re.search(r'^cfg dwords=([0-9a-f ]+)$', seg, re.M)
        t['cfg'] = [int(x, 16) for x in m[1].split()] if m else None
        m = re.search(r'^probe-bar0 base=%s$' % HEX, seg, re.M)
        t['bar0'] = int(m[1], 16) if m else None
        t['mmio'] = {}
        for mm in re.finditer(r'^mmio off=%s status=0x([0-9a-f]+) value=%s$' % (HEX, HEX), seg, re.M):
            if int(mm[2], 16) == 0:
                t['mmio'][int(mm[1], 16)] = int(mm[3], 16)
        m = re.search(r'^prom sha256=([0-9a-f]{64})$', seg, re.M)
        t['prom_sha'] = m[1] if m else None
        t['caps'] = [(int(a, 16), int(b, 16)) for a, b in re.findall(r'^cap off=%s\ncap id=%s$' % (HEX, HEX), seg, re.M)]
    # Back-compat top-level fields - unchanged meaning for the single-target case every existing caller
    # (including test_profile_gen.py's RealCapture/SyntheticAcceptedCard/Refusals classes) already relies
    # on; disambiguate_multi_target() below re-populates these from whichever candidate it picks, so
    # build_profile's own checks (which read c.cfg/c.bar0/c.mmio/c.prom_sha/c.caps, not t[...]) keep working
    # unchanged after narrowing from more than one candidate too.
    c.cfg = c.targets[0]['cfg'] if len(c.targets) == 1 else None
    c.bar0 = c.targets[0]['bar0'] if len(c.targets) == 1 else None
    c.mmio = c.targets[0]['mmio'] if len(c.targets) == 1 else {}
    c.prom_sha = c.targets[0]['prom_sha'] if len(c.targets) == 1 else None
    c.caps = c.targets[0]['caps'] if len(c.targets) == 1 else []

    # --- per-handle blocks (diagnostic.c's own handle/protocol walk, not probe.c) ---
    c.handles = {}
    hcur = None
    for line in body.split('\n'):
        m = re.fullmatch(r'handle id=%s' % HEX, line)
        if m:
            hcur = Handle(int(m[1], 16)); c.handles[hcur.id] = hcur
            continue
        if hcur is None:
            continue
        m = re.fullmatch(r'path-node raw=([0-9a-f]+)', line)
        if m: hcur.path.append(bytes.fromhex(m[1])); continue
        m = re.fullmatch(r'pci location-status=%s segment=%s bus=%s device=%s function=%s' % (HEX, HEX, HEX, HEX, HEX), line)
        if m:
            if int(m[1], 16) == 0:
                hcur.pci_location = tuple(int(x, 16) for x in m.groups()[1:])
            continue
        m = re.fullmatch(r'pci config-dwords=([0-9a-f ]+)', line)
        if m: hcur.pci_config = [int(x, 16) for x in m[1].split()]; continue
        m = re.fullmatch(r'gop framebuffer=%s bytes=%s mode=%s max-mode=%s' % (HEX, HEX, HEX, HEX), line)
        if m:
            hcur.gop = {'framebuffer': int(m[1], 16), 'bytes': int(m[2], 16),
                        'mode': int(m[3], 16), 'max_mode': int(m[4], 16)}
            continue
        m = re.fullmatch(r'gop width=%s height=%s stride=%s' % (HEX, HEX, HEX), line)
        if m and hcur.gop is not None:
            hcur.gop.update({'width': int(m[1], 16), 'height': int(m[2], 16), 'stride': int(m[3], 16)})
            continue
    return c


def gop_is_sane(gop):
    """As much of app.c's LocationHasValidGop mode/framebuffer sanity check as the capture log actually
    lets this tool see. One sub-check it cannot replicate: LocationHasValidGop also rejects a PixelFormat
    past PixelBlueGreenRedReserved8BitPerColor, but diagnostic.c's own Details() ('gop' branch) never logs
    Mode->Info->PixelFormat at all - checked directly against diagnostic.c, not assumed. Everything else
    LocationHasValidGop checks (FrameBufferBase/FrameBufferSize nonzero, HorizontalResolution/
    VerticalResolution nonzero, PixelsPerScanLine >= HorizontalResolution, scanline bytes fitting inside
    the framebuffer) is checked here from what diagnostic.c does log."""
    if gop is None or 'width' not in gop:
        return False  # no GOP at all, or Info was missing/too-small (diagnostic.c logs "gop-info" instead)
    if not gop['framebuffer'] or not gop['bytes']:
        return False
    if not gop['width'] or not gop['height']:
        return False
    if gop['stride'] < gop['width']:
        return False
    if gop['stride'] * gop['height'] * 4 > gop['bytes']:
        return False
    return True


def gop_owner_index(c, candidates):
    """Mirrors app.c's FindTarget/LocationHasValidGop multi-candidate disambiguation (2026-11-04): a GOP
    handle's own device path is exactly its owning PCI device's path (root+bridge+device, the same prefix
    pci_device_path() computes) with one extra ACPI ADR node inserted before the END node - confirmed
    directly against Cancretto's two new 'one card displaying, one idle' captures (2026-11-05, GitHub issue
    sargentmajord/Windows-GPU-UNLOCK#2), not guessed: in both, the GOP handle's first three path nodes
    (ACPI root + the two PCI nodes) are byte-for-byte identical to the displaying card's own PCI handle
    path, and differ from the idle card's. `candidates` is [(index, raw_or_None), ...] where raw is
    pci_device_path(...)['raw'] (28 bytes: root+bridge+device+its own END) for each targets[index].
    Returns the single candidate index a sane GOP resolves to, or None if zero or more than one do - the
    same "refuse rather than guess" outcome FindTarget itself falls back to."""
    owner = None
    for h in c.handles.values():
        if not gop_is_sane(h.gop) or len(h.path) < 3:
            continue
        gop_prefix = b''.join(h.path[:-2])  # drop this GOP's own ACPI ADR node + END node
        matches = {i for i, raw in candidates if raw is not None and raw[:-4] == gop_prefix}
        if len(matches) != 1:
            continue  # doesn't resolve to exactly one of our candidates - not a usable signal either way
        i = next(iter(matches))
        if owner is not None and owner != i:
            return None  # a second, different candidate also has a sane GOP - genuinely ambiguous
        owner = i
    return owner


def disambiguate_multi_target(c):
    """Only called when probe.c found more than one PCI device matching GuTargets (c.probe_targets ==
    [len(c.targets)], checked by the caller) - narrows c.targets/c.probe_targets to a single winner in
    place by GOP ownership, the Python-side mirror of FindTarget's own disambiguation. Leaves c unchanged
    (so the caller's existing target-count refusal fires as before) if it can't find exactly one owner."""
    candidates = []
    for t in c.targets:
        cfg_t = t.get('cfg')
        raw = None
        if cfg_t and len(cfg_t) == 64:
            device_t, subsystem_t = cfg_t[0], cfg_t[11]
            th = [h for h in c.handles.values() if h.pci_config and h.pci_location and
                  h.pci_config[0] == device_t and h.pci_config[11] == subsystem_t and h.pci_location[1] == t['bus']]
            if len(th) == 1:
                rpath = pci_device_path(th[0].path)
                raw = rpath['raw'] if rpath else None
        candidates.append(raw)
    owner = gop_owner_index(c, list(enumerate(candidates)))
    if owner is None:
        return  # leave c untouched; build_profile's own target-count refusal below handles this
    chosen = c.targets[owner]
    c.targets = [chosen]
    c.probe_targets = [1]
    # Re-sync every per-target-scoped field parse_capture() attaches (see its own comment on why these
    # are scoped per target, not shared) onto c's top-level attributes, since build_profile reads those
    # (c.cfg/c.bar0/c.mmio/c.prom_sha/c.caps), not t[...] - must be the WINNING candidate's own values,
    # never the other (refused, un-POSTed-or-otherwise-not-chosen) card's.
    c.cfg = chosen.get('cfg')
    c.bar0 = chosen.get('bar0')
    c.mmio = chosen.get('mmio') or {}
    c.prom_sha = chosen.get('prom_sha')
    c.caps = chosen.get('caps') or []


def gu_targets_from_port_h(port_h_text=None):
    """Read port.h's GuTargets table directly from the C source, so this tool and the UEFI port's own
    GuTargetConfig (port.c) can't drift apart. Returns [(device_dword, subsystem_dword, source), ...]."""
    text = port_h_text if port_h_text is not None else PORT_H.read_text(encoding='utf-8')
    m = re.search(r'GuTargets\[GU_TARGET_COUNT\]\s*=\s*\{(.*?)\n\};', text, re.S)
    if not m:
        raise RuntimeError("could not find GuTargets[] in port.h - has it been renamed or reshaped?")
    rows = re.findall(r'\{\s*0x([0-9a-f]+)u,\s*0x([0-9a-f]+)u,\s*"([^"]*)"\s*\}', m[1])
    if not rows:
        raise RuntimeError("GuTargets[] found but no rows matched - has its shape changed?")
    return [(int(d, 16), int(s, 16), src) for d, s, src in rows]


def build_profile(c, profile_name, note=''):
    if not c.complete or not c.result_ok:
        refuse('capture-incomplete', 'The capture did not finish cleanly.', 'Run the capture again.')
    # Mirrors app.c's FindTarget (2026-11-04): more than one PCI device matching GuTargets no longer means
    # an automatic refusal - try to pick the one actually driving a display first, the same signal
    # DisplayCheck/TargetGopCheck already rely on at runtime. Only attempted when probe.c's own target
    # count and the number of probe-target lines actually agree; any other mismatch falls straight through
    # to the unchanged refusal below rather than guessing.
    if len(c.targets) > 1 and c.probe_targets == [len(c.targets)]:
        disambiguate_multi_target(c)
    if c.probe_targets != [1] or len(c.targets) != 1:
        refuse('target-count', 'The diagnostic found %s matching card(s) instead of exactly one.' % (c.probe_targets or 'no'),
               'Two matching cards are not supported yet, unless exactly one of them has its own active '
               'display and the other does not.' if c.probe_targets and c.probe_targets[0] > 1 else 'Run the capture again.')
    t = c.targets[0]
    if t['seg'] != 0 or t['dev'] != 0 or t['fun'] != 0:
        refuse('target-location', 'The target card is not at device 0, function 0 of a PCI bus on segment 0.', 'This layout is not supported.')
    cfg = t.get('cfg')
    if not cfg or len(cfg) != 64:
        refuse('config-missing', 'The capture has no complete PCI configuration of the target card.', 'Run the capture again.')
    if (cfg[2] >> 24) != 3:
        refuse('not-display', 'The captured device is not a display controller.')
    if (cfg[4] & 0xf) != 0 or not (cfg[4] & ~0xf):
        refuse('bar0', "The card's first memory window (BAR0) is not a 32-bit memory range the boot programs support.")
    if cfg[1] & 4:
        refuse('bus-master', 'The card already has bus mastering on in the capture.', 'Power the PC off completely and run the capture again.')

    device, subsystem = cfg[0], cfg[11]
    targets = gu_targets_from_port_h()
    # subsystem 0 in the table means "any subsystem" - same convention port.c's
    # GuTargetConfig uses (port.h, changed 2026-11-04). Must match that C-side
    # semantics exactly, not just parse the same bytes: a literal subsystem==0
    # check here would refuse every real capture, since no real card reports
    # subsystem 0x00000000.
    matches = [src for d, s, src in targets if d == device and (s == 0 or s == subsystem)]
    if not matches:
        refuse('not-a-target-card',
               'Device 0x%08x is not one of the device identities in port.h\'s GuTargets table.' % device,
               "This project accepts the Turing TU102 device-ID family (see port.h's own header comment and "
               "README.md's \"What real-world reports actually show about the risk\"), not an exact per-board "
               "allow-list - a device ID outside that family correctly refuses here, the same way the UEFI port "
               "itself would refuse it at runtime.")

    m = c.mmio
    for off in (OFF_CHIP, OFF_FB_RANGE, OFF_PLM, OFF_BOOT, OFF_CPUCTL):
        if off not in m:
            refuse('registers-missing', 'The capture is missing register 0x%06x.' % off, 'Run the capture again.')
    # 0xffffffff (every bit set) is the classic PCI/MMIO "nothing is actually responding at this
    # address" sentinel - distinct from is_bad_sentinel()'s 0xbadfxxxx ("firmware hasn't finished
    # booting yet", a real, mapped read). It is NOT caught by is_bad_sentinel() (0xffff0000 !=
    # 0xbadf0000), and it defeats the CPUCTL_HALTED bitmask check below by construction (ANDing
    # any mask against all-ones is always true) - found by actually testing a real capture where
    # BAR0 was not decoding at all (every single mmio register in it read exactly 0xffffffff), not
    # assumed. Checked on the chip-id register specifically: a real GA10x/TU10x chip id is never
    # literally 0xffffffff, so this is a safe, specific signal that the capture couldn't actually
    # talk to the card - not a guess about what caused that, which is a BIOS/ReBAR/boot-mode
    # question for the person re-running the capture, not something this tool can diagnose further.
    if m[OFF_CHIP] == 0xffffffff:
        refuse('mmio-unmapped',
               'Every register this capture read, starting with the chip-id register (0x%06x), reads '
               '0xffffffff - BAR0 was not actually decoding during this capture, not a firmware-state '
               'problem the other checks below are designed to catch.' % OFF_CHIP,
               'Run the capture again. If it persists, check Above 4G Decoding/Resizable BAR/CSM in '
               'the system BIOS and try a different boot/display configuration - a card whose BAR0 '
               'genuinely never decodes cannot be captured no matter how many times this is run.')
    for off in (OFF_PLM, OFF_BOOT, OFF_CPUCTL):
        if is_bad_sentinel(m[off]):
            refuse('gfw-not-ready', 'Register 0x%06x reads %#x, a known "firmware boot not finished" value.' % (off, m[off]),
                   'Run the capture again; if it persists, the firmware may not have finished booting the GPU.')
    if not (m[OFF_CPUCTL] & CPUCTL_HALTED):
        refuse('gfw-not-halted', 'The GFW falcon is not halted (cpuctl=%#x) - firmware boot has not finished.' % m[OFF_CPUCTL],
               'Run the capture again.')
    # Fixed 2026-11-06: this used to check OFF_MEMORY (0x1183a4) directly, which reads a "not finished"
    # sentinel on every real Turing capture regardless of POST state - see OFF_MEMORY's own comment and
    # PORTING-PLAN.md's 2026-11-06 entry for the full root-cause investigation. OFF_FB_RANGE is the
    # register Turing's own real unlock transaction actually uses for this.
    memory_mib = decode_fb_range_mib(m[OFF_FB_RANGE])
    if memory_mib is None:
        refuse('memory-size-not-ready',
               'The usable-framebuffer-size register (0x%06x) reads %#x, which NV_PFB_PRI_MMU_LOCAL_MEMORY_RANGE\'s '
               'own "not ready" conditions (all-ones, a "not a finished value" sentinel, or a zero magnitude '
               'field) reject.' % (OFF_FB_RANGE, m[OFF_FB_RANGE]),
               'Run the capture again.')
    if not c.prom_sha:
        refuse('prom-missing', 'The capture has no video BIOS fingerprint.', 'Run the capture again.')

    bar0 = c.bar0
    if bar0 is None:
        refuse('bar0-missing', "The capture has no BAR0 base address.", 'Run the capture again.')

    # ---- capabilities: checks.c's GuCheckConfig (ported unchanged from Ampere) requires exactly one
    # PCI Express and one MSI-X capability, MSI-X pinned at a fixed offset - same as Ampere's own tool
    # requires, since checks.c's capability-walk logic is literally the same code. ----
    def cap_offset(cid):
        offs = [o for o, i in c.caps if i == cid]
        return offs[0] if len(offs) == 1 else None
    pcie = cap_offset(0x10)
    msix = cap_offset(0x11)
    if msix is None and (cfg[0xc8 // 4] & 0xff) == 0x11:
        msix = 0xc8
    if pcie is None or msix is None:
        refuse('capabilities', 'The card does not show exactly one PCI Express and one MSI-X capability.')
    if msix != 0xc8:
        refuse('capabilities', "The card's MSI-X capability is at offset %#x; the boot programs only pin offset 0xc8." % msix)

    # ---- the target's own device path, for checks.c's GU_ROOT_PORT_DEVICE/FUNCTION ----
    th = [h for h in c.handles.values() if h.pci_config and h.pci_location and
          h.pci_config[0] == device and h.pci_config[11] == subsystem and h.pci_location[1] == t['bus']]
    if len(th) != 1:
        refuse('target-handle', 'The firmware lists %d PCI devices matching the target card.' % len(th))
    rpath = pci_device_path(th[0].path)
    if rpath is None:
        refuse('pci-path', 'The target card is not directly under a CPU root port in the firmware device tree '
               '(a bridge or switch is in between).', 'This layout is not supported yet.')

    values = {
        'GU_PROFILE_NAME': '"%s"' % re.sub(r'[^A-Za-z0-9._-]', '-', profile_name)[:80],
        'GU_BAR0_BASE': '0x%08xULL' % bar0, 'GU_BAR0_END': '0x%08xULL' % (bar0 + 0xffffff),
        'GU_CHIP_ID': '0x%08xU' % m[OFF_CHIP], 'GU_DEVICE_MEMORY_SIZE': '0x%xU' % memory_mib,
        'GU_PROM_SHA256': '"%s"' % c.prom_sha,
        'GU_BOARD_ID': '0x%08xU' % device, 'GU_BOARD_SUBSYSTEM': '0x%08xU' % subsystem,
        'GU_TARGET_BUS': '%dU' % t['bus'], 'GU_CONFIG_CLASS_REV': '0x%08xU' % cfg[2],
        'GU_MSIX_CAP_OFFSET': '0x%xU' % msix, 'GU_MSIX_CAP_DWORD': '0x%08xU' % cfg[msix // 4],
        'GU_ROOT_PORT_DEVICE': '%dU' % rpath['root_dev'], 'GU_ROOT_PORT_FUNCTION': '%dU' % rpath['root_fn'],
        'GU_OTHER_GPU_ROOT_DEVICE': '%dU' % PLACEHOLDER_OTHER_GPU['GU_OTHER_GPU_ROOT_DEVICE'],
        'GU_OTHER_GPU_ROOT_FUNCTION': '%dU' % PLACEHOLDER_OTHER_GPU['GU_OTHER_GPU_ROOT_FUNCTION'],
        'GU_OTHER_GPU_ADR': '0x%08xU' % PLACEHOLDER_OTHER_GPU['GU_OTHER_GPU_ADR'],
    }
    summary = {
        'target': {'bus': t['bus'], 'id': '%04x:%04x' % (device & 0xffff, device >> 16),
                   'subsystem': '%04x:%04x' % (subsystem & 0xffff, subsystem >> 16),
                   'gu_targets_source': matches[0], 'chip': '0x%08x' % m[OFF_CHIP],
                   'memory_mib': memory_mib, 'fb_range_register': '0x%x' % m[OFF_FB_RANGE],
                   'legacy_unused_memory_register_0x1183a4': '0x%x' % m.get(OFF_MEMORY, 0),
                   'bar0': '0x%08x' % bar0, 'gfw_halted': True},
        'not_covered': ['GU_OTHER_GPU_* is a compile-time placeholder (no second-adapter capture needed - '
                         'DisplayCheck checks the target card itself, not a separate console adapter) - '
                         'ConfigCheck is GU_EXPERIMENTAL_UNLOCK-gated and unreachable today regardless'],
    }
    return values, summary


HEADER = '''// GPL-2.0-only. Generated by tools/profile_gen.py from a read-only capture of one real PC. Do not edit
// by hand. Local file: it describes one PC and must not be shared or committed.
/*
 * Covers what app.c's BarCheck/PromCheck/ColdCheck/DisplayCheck and checks.c's GuCheckConfig/ConfigCheck
 * need to compile and run against a real, single-adapter capture. GU_OTHER_GPU_* is a placeholder, not
 * real data - see this file's own header comment for why that's safe (DisplayCheck needs no second-adapter
 * capture at all).
 * %(note)s
 */
#pragma once
#define GU_PROFILE_GENERATED 1
'''

ORDER = [
    'GU_PROFILE_NAME', 'GU_BAR0_BASE', 'GU_BAR0_END', 'GU_CHIP_ID', 'GU_DEVICE_MEMORY_SIZE', 'GU_PROM_SHA256',
    'GU_BOARD_ID', 'GU_BOARD_SUBSYSTEM', 'GU_TARGET_BUS', 'GU_CONFIG_CLASS_REV',
    'GU_MSIX_CAP_OFFSET', 'GU_MSIX_CAP_DWORD', 'GU_ROOT_PORT_DEVICE', 'GU_ROOT_PORT_FUNCTION',
    'GU_OTHER_GPU_ROOT_DEVICE', 'GU_OTHER_GPU_ROOT_FUNCTION', 'GU_OTHER_GPU_ADR',
]


def render_header(values, note=''):
    out = [HEADER % {'note': re.sub(r'[^A-Za-z0-9 .,:/()_-]', '', note)}]
    assert set(ORDER) == set(values), set(ORDER) ^ set(values)
    for k in ORDER:
        out.append('#define %s %s' % (k, values[k]))
    return '\n'.join(out) + '\n'


def generate(capture_path, profile_name, note=''):
    text = Path(capture_path).read_text(encoding='utf-8', errors='replace')
    cap = parse_capture(text)
    values, summary = build_profile(cap, profile_name, note)
    return render_header(values, note), summary


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--capture', required=True)
    ap.add_argument('--profile-name', default='generated')
    ap.add_argument('--note', default='')
    ap.add_argument('--out', required=True)
    ap.add_argument('--summary')
    a = ap.parse_args(argv)
    try:
        header, summary = generate(a.capture, a.profile_name, a.note)
    except Refused as r:
        print(json.dumps({'ok': False, 'findings': [x.as_dict() for x in r.findings]}, indent=1))
        return 2
    Path(a.out).write_text(header, encoding='ascii', newline='\n')
    if a.summary:
        Path(a.summary).write_text(json.dumps({'ok': True, 'summary': summary}, indent=1), encoding='ascii')
    print(json.dumps({'ok': True, 'summary': summary}, indent=1))
    return 0


if __name__ == '__main__':
    sys.exit(main())
