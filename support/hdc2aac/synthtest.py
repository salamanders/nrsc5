#!/usr/bin/env python3
"""
synthtest.py - offline validation for hdc2aac.c (NO radio hardware needed)

Modes:

  GENERATE:
      python3 synthtest.py gen <outdir> [--frames N] [--seed S]
      Builds <outdir>/input.hdc - ADTS-wrapped *synthetic* HDC frames that
      exercise the exact grammar hdc2aac.c must parse (random sections,
      global gains, TNS blocks, MS masks, real Huffman codewords sampled
      from hcb_trees.h, optional FIL/SBR tails) - plus
      <outdir>/expected.json describing every frame semantically.

  VERIFY:
      python3 synthtest.py verify <outdir> <converted.aac>
      Re-parses the synthetic HDC input (HDC grammar) and hdc2aac's output
      (STANDARD AAC grammar) and asserts, per frame:
        - identical window/ics fields, section tables, TNS parameters,
          global gains, MS masks;
        - the section_data+scale_factor_data bit span and the
          spectral_data bit span are BIT-IDENTICAL between input and
          output - the core claim of the transcoder ("verbatim copy");
        - frame counts match (a skipped frame is a failure here because
          synthetic frames are always well-formed).

Exit status 0 means everything verified.

Limitations: generates only LONG-window frames (window_sequence=0).
The EIGHT_SHORT path in hdc2aac.c is exercised only by real captures.

Read this file alongside hdc2aac.c: it is an executable specification of
the same grammar, written independently.
"""

import json
import os
import random
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

ID_SCE, ID_CPE, ID_FIL, ID_END = 0, 1, 6, 7
EIGHT_SHORT = 2
NUM_SWB_LONG = 47
SF_INDEX = 7

SWB_LONG = [0, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 44, 52, 60, 68,
            76, 84, 92, 100, 108, 116, 124, 136, 148, 160, 172, 188, 204,
            220, 240, 260, 284, 308, 336, 364, 396, 432, 468, 508, 552,
            600, 652, 704, 768, 832, 896, 960, 1024]
assert len(SWB_LONG) == NUM_SWB_LONG + 1

ZERO_HCB, FIRST_PAIR_HCB, ESC_HCB = 0, 5, 11


# ------------------------------------------------------------------ #
# hcb_trees.h loading                                                #

def load_trees(path=None):
    """Parse hcb_trees.h back into leaf lists:
       {1..11,'sf': [(bits_tuple, nsign, esc), ...]}."""
    text = open(path or os.path.join(HERE, "hcb_trees.h")).read()
    tabs = {}
    for m in re.finditer(r"static const hdc_hcb_node (\w+)\[\] = \{(.*?)\};",
                         text, re.S):
        name, body = m.group(1), m.group(2)
        rows = [(int(a), int(b), int(c)) for a, b, c in
                re.findall(r"\{\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*\}",
                           body)]
        leaves = []

        def walk(idx, path):
            leaf, x, y = rows[idx]
            if leaf:
                leaves.append((tuple(path), x, y))
                return
            for bit, nxt in ((0, x), (1, y)):
                if nxt >= 0:
                    walk(nxt, path + [bit])

        walk(0, [])
        tabs[name] = leaves

    out = {cb: tabs["hdc_cb%d" % cb] for cb in range(1, 12)}
    out["sf"] = tabs["hdc_cb_sf"]
    return out


def check_trees(trees):
    """Kraft equality per book - guards against a corrupted hcb_trees.h."""
    for name, leaves in sorted(trees.items(), key=lambda kv: str(kv[0])):
        kraft = sum(2.0 ** -len(p) for p, _, _ in leaves)
        if abs(kraft - 1.0) > 1e-9:
            raise SystemExit("hcb_trees.h corrupt (%s kraft=%f)"
                             % (name, kraft))


def build_trie(leaves):
    """dict-of-dicts trie; leaves are (nsign, esc) tuples."""
    root = {}
    for path, nsign, esc in leaves:
        node = root
        for b in path[:-1]:
            node = node.setdefault(b, {})
        if not path:
            raise SystemExit("zero-length codeword in hcb_trees.h")
        node[path[-1]] = (nsign, esc)
    return root


class Codec:
    """Per-codebook tries + codeword sample/consume helpers."""

    def __init__(self):
        self.trees = load_trees()
        check_trees(self.trees)
        self.trie = {k: build_trie(v) for k, v in self.trees.items()}

    def write_codeword(self, w, rng, key):
        """Append one random valid codeword (+ signs/escapes).
           key: int codebook 1..11, or 'sf'."""
        path, nsign, nesc = rng.choice(self.trees[key])
        w.wbits_str("".join(map(str, path)))
        if key == "sf":
            return
        for _ in range(nsign):
            w.wb(rng.getrandbits(1))
        for _ in range(nesc):
            i = rng.randrange(4, 9)         # escape magnitude width 4..8
            w.wbits_str("1" * (i - 4) + "0")
            w.w(rng.getrandbits(i), i)

    def read_codeword(self, r, key):
        """Consume one codeword (+ signs/escapes); returns its bit length."""
        node = self.trie[key]
        depth = 0
        while isinstance(node, dict):
            node = node[r.bit()]
            depth += 1
        nsign, nesc = node
        for _ in range(nsign):
            r.bit()
        for _ in range(nesc):
            i = 4
            while r.bit():
                i += 1
                if i >= 16:
                    raise AssertionError("escape overrun")
            r.bits(i)
        return depth

# ------------------------------------------------------------------ #
# bit IO                                                             #

class Writer:
    def __init__(self):
        self.bits = []

    def w(self, value, n):
        for i in range(n - 1, -1, -1):
            self.bits.append((value >> i) & 1)

    def wb(self, b):
        self.bits.append(b & 1)

    def wbits_str(self, s):
        self.bits.extend(1 if c == "1" else 0 for c in s)

    def align(self):
        while len(self.bits) % 8:
            self.bits.append(0)

    def to_bytes(self):
        assert len(self.bits) % 8 == 0
        out = bytearray(len(self.bits) // 8)
        for i, b in enumerate(self.bits):
            if b:
                out[i >> 3] |= 0x80 >> (i & 7)
        return bytes(out)


class Reader:
    def __init__(self, data):
        self.data = data
        self.pos = 0

    def bit(self):
        b = self.pos >> 3
        self.pos += 1
        if b >= len(self.data):
            raise EOFError("reader overran frame")
        return (self.data[b] >> (7 - ((self.pos - 1) & 7))) & 1

    def bits(self, n):
        v = 0
        for _ in range(n):
            v = (v << 1) | self.bit()
        return v

    def peek(self, n):
        save = self.pos
        try:
            return self.bits(n)
        finally:
            self.pos = save


def bits_hex(bits):
    bs = list(bits)
    while len(bs) % 8:
        bs.append(0)
    v = 0
    for b in bs:
        v = (v << 1) | b
    return "%0*x" % (2 * (len(bs) // 8), v)


def adts_header(total_len, stereo):
    chan = 2 if stereo else 1
    h = bytearray(7)
    h[0], h[1] = 0xFF, 0xF1
    h[2] = (1 << 6) | (SF_INDEX << 2) | ((chan >> 1) & 1)
    h[3] = ((chan & 1) << 7) | ((total_len >> 11) & 3)
    h[4] = (total_len >> 3) & 0xFF
    h[5] = ((total_len & 7) << 5) | 0x1F
    h[6] = 0xFC
    return bytes(h)


# ------------------------------------------------------------------ #
# shared channel parsing                                             #
# Both grammars share: section_data, scale_factor_data, spectral_data #
# and the TNS filter parameters.  They differ in framing, which is    #
# handled by the callers.                                            #

def parse_sections(r, max_sfb, winseq):
    sect_bits = 3 if winseq == EIGHT_SHORT else 5
    esc = (1 << sect_bits) - 1
    sections = []
    k = 0
    while k < max_sfb:
        cb = r.bits(4)
        if cb == 12:
            raise AssertionError("invalid codebook 12")
        ln = 0
        incr = r.bits(sect_bits)
        while incr == esc:                  # escape chain
            ln += incr
            incr = r.bits(sect_bits)
        ln += incr
        if ln <= 0 or k + ln > max_sfb:
            raise AssertionError("bad section length")
        sections.append([cb, k, k + ln])
        k += ln
    if k != max_sfb:
        raise AssertionError("sections do not sum to max_sfb")
    return sections


def parse_scalefactor_region(r, codec, sections):
    """Consume scale factors exactly as faad's decode_scale_factors does."""
    noise_pcm_seen = False
    sf_count = 0
    for cb, start, end in sections:
        for _sfb in range(start, end):
            if cb == ZERO_HCB:
                continue
            if cb == 13:                    # NOISE_HCB
                if not noise_pcm_seen:
                    noise_pcm_seen = True
                    r.bits(9)
                else:
                    codec.read_codeword(r, "sf")
                sf_count += 1
            else:                           # spectral + intensity books
                codec.read_codeword(r, "sf")
                sf_count += 1
    return sf_count


def parse_spectral_region(r, codec, sections):
    """Consume spectral data; returns number of coefficient codewords."""
    count = 0
    for cb, start, end in sections:
        if cb in (ZERO_HCB, 13, 14, 15):    # zero/noise/intensity: no data
            continue
        inc = 2 if cb >= FIRST_PAIR_HCB else 4
        k = SWB_LONG[start]
        while k < SWB_LONG[end]:
            codec.read_codeword(r, cb)
            count += 1
            k += inc
    return count


def parse_tns(r):
    """Parse one channel's TNS block (long windows: n_filt is either
       implicit (HDC) or pre-consumed by the caller (AAC); exactly one
       filter with n_filt=1 in synthetic streams)."""
    coef_res = r.bit()
    filt = {"coef_res": coef_res, "filters": []}
    # long windows: exactly one window with n_filt = 1
    length = r.bits(6)
    order = r.bits(5)
    entry = {"length": length, "order": order}
    if order > 0:
        entry["direction"] = r.bit()
        compress = r.bit()
        entry["compress"] = compress
        cbits = 3 + coef_res - compress
        entry["coefs"] = [r.bits(cbits) for _ in range(order)]
    filt["filters"].append(entry)
    return filt

# ------------------------------------------------------------------ #
# frame-level parsers                                                #

def parse_ics_info_aac(r):
    """ICS info on the AAC side; element id/tag/common already consumed."""
    assert r.bit() == 0, "ics_reserved_bit must be 0"
    winseq = r.bits(2)
    shape = r.bit()
    ics = {"winseq": winseq, "shape": shape}
    if winseq == EIGHT_SHORT:
        raise AssertionError("synthetic streams are long-window only")
    ics["max_sfb"] = r.bits(6)
    pred = r.bit()
    assert pred == 0, "predictor_data_present must be 0"
    return ics


def bits_between(data, a, b):
    """Extract bits [a, b) from a byte buffer as a list of 0/1."""
    return [(data[i >> 3] >> (7 - (i & 7))) & 1 for i in range(a, b)]


def parse_aac_element(r, codec):
    eid = r.bits(3)
    tag = r.bits(4)
    assert tag == 0, "element_instance_tag should be 0"
    frame = {}

    def body(ics, gg=None):
        """gg + sections/scalefactors + pulse/tns/gain + spectral.
           Pass gg when it was already consumed (SCE field order)."""
        if gg is None:
            gg = r.bits(8)
        s0 = r.pos
        sections = parse_sections(r, ics["max_sfb"], ics["winseq"])
        sf_count = parse_scalefactor_region(r, codec, sections)
        s1 = r.pos
        pulse = r.bit()
        tns_present = r.bit()
        if tns_present:
            n_filt = r.bits(2)              # emitter writes n_filt=1
            assert n_filt == 1, "expected n_filt=1"
            tns = parse_tns(r)
        else:
            tns = None
        gain = r.bit()
        sp0 = r.pos
        ncoef = parse_spectral_region(r, codec, sections)
        sp1 = r.pos
        assert pulse == 0 and gain == 0, "pulse/gain must be absent"
        return {"gg": gg, "sections": sections, "sf_count": sf_count,
                "tns": tns,
                "secsf_hex": bits_hex(bits_between(r.data, s0, s1)),
                "spec_hex": bits_hex(bits_between(r.data, sp0, sp1)),
                "ncoef": ncoef}

    if eid == ID_CPE:
        common = r.bit()
        assert common == 1, "CPE must use common_window"
        ics = parse_ics_info_aac(r)
        ms_present = r.bits(2)
        ms_hex = None
        if ms_present == 1:
            ms_hex = bits_hex([r.bit() for _ in range(ics["max_sfb"])])
        frame.update({"stereo": True, **ics, "ms_present": ms_present,
                      "ms_hex": ms_hex})
        frame["ch"] = [body(ics), body(ics)]
    elif eid == ID_SCE:
        gg = r.bits(8)                      # SCE reads gg BEFORE ics_info
        ics = parse_ics_info_aac(r)
        frame.update({"stereo": False, **ics})
        frame["ch"] = [body(ics, gg=gg)]
        frame["ms_present"] = 0
        frame["ms_hex"] = None
    else:
        raise AssertionError("unexpected element id %d" % eid)

    end = r.peek(3)
    assert end == ID_END, "expected END element, got %d" % end
    return frame


def parse_hdc_frame(data, codec):
    r = Reader(data)
    bt = r.bits(3)
    if bt in (0, 1, 5, 6):
        stereo = False
    elif bt in (2, 7):
        stereo = True
    else:
        raise AssertionError("bad HDC block type %d" % bt)

    assert r.bit() == 0, "HDC reserved bit must be 0"
    shape = r.bit()
    winseq = r.bits(2)
    assert winseq != EIGHT_SHORT, "generator emits long windows only"
    max_sfb = r.bits(6)
    ics = {"winseq": winseq, "shape": shape, "max_sfb": max_sfb}

    ms_present, ms_hex = 0, None
    if stereo:
        ms_present = r.bits(2)
        if ms_present == 1:
            ms_hex = bits_hex([r.bit() for _ in range(max_sfb)])

    num_ch = 2 if stereo else 1
    tns_list = []
    for _c in range(num_ch):
        present = r.bit()
        tns_list.append(parse_tns(r) if present else None)

    chs = []
    for _c in range(num_ch):
        gg = r.bits(8)
        s0 = r.pos
        sections = parse_sections(r, max_sfb, winseq)
        sf_count = parse_scalefactor_region(r, codec, sections)
        s1 = r.pos
        sp0 = r.pos
        ncoef = parse_spectral_region(r, codec, sections)
        sp1 = r.pos
        chs.append({"gg": gg, "sections": sections, "sf_count": sf_count,
                    "secsf_hex": bits_hex(bits_between(r.data, s0, s1)),
                    "spec_hex": bits_hex(bits_between(r.data, sp0, sp1)),
                    "ncoef": ncoef})

    has_fil = False
    try:
        if r.peek(3) == ID_FIL:
            r.bits(3)
            amp = r.bit()
            if amp:
                has_fil = True              # rest is SBR: ignored by tool
    except EOFError:
        pass

    return {"stereo": stereo, **ics, "ms_present": ms_present,
            "ms_hex": ms_hex, "tns": tns_list, "ch": chs, "fil": has_fil}

# ------------------------------------------------------------------ #
# generator                                                          #

def gen_frame(codec, rng):
    """Build one synthetic HDC frame payload + its expected description."""
    w = Writer()
    stereo = rng.random() < 0.6
    block_type = rng.choice((2, 7) if stereo else (0, 1, 5, 6))
    num_ch = 2 if stereo else 1

    w.w(block_type, 3)
    w.wb(0)                                 # reserved
    w.wb(rng.getrandbits(1))                # window_shape
    w.w(0, 2)                               # ONLY_LONG_SEQUENCE
    max_sfb = rng.randrange(16, NUM_SWB_LONG + 1)
    w.w(max_sfb, 6)

    if stereo:
        ms_present = rng.choice((0, 1, 1, 2))
        w.w(ms_present, 2)
        if ms_present == 1:
            for _ in range(max_sfb):        # 1 group on long windows
                w.wb(rng.getrandbits(1))

    tns_meta = []
    for _c in range(num_ch):
        present = rng.random() < 0.4
        w.wb(1 if present else 0)
        if present:
            coef_res = rng.getrandbits(1)
            w.wb(coef_res)
            length = rng.randrange(0, 64)
            order = rng.randrange(0, 16)
            w.w(length, 6)
            w.w(order, 5)
            filt = {"coef_res": coef_res,
                    "filters": [{"length": length, "order": order}]}
            if order > 0:
                direction = rng.getrandbits(1)
                compress = rng.getrandbits(1)
                cbits = 3 + coef_res - compress
                coefs = [rng.getrandbits(cbits) for _ in range(order)]
                w.wb(direction)
                w.wb(compress)
                for cval in coefs:
                    w.w(cval, cbits)
                filt["filters"][0].update({"direction": direction,
                                           "compress": compress,
                                           "coefs": coefs})
            tns_meta.append(filt)
        else:
            tns_meta.append(None)

    # random section partition of [0, max_sfb)
    special_cbs = (0, 13, 14, 15)
    chs_meta = []
    for _c in range(num_ch):
        gg = rng.getrandbits(8)
        w.w(gg, 8)
        nsec = rng.randrange(1, 7)
        cuts = sorted(rng.sample(range(1, max_sfb), min(nsec - 1, max_sfb - 1)))
        bounds = [0] + cuts + [max_sfb]
        sections = []
        sf_bits_writer = Writer()
        spec_bits_writer = Writer()
        for i in range(len(bounds) - 1):
            start, end = bounds[i], bounds[i + 1]
            cb = rng.choice(special_cbs) if rng.random() < 0.15 \
                else rng.randrange(1, 12)
            ln = end - start
            assert ln < 31                  # keep away from escape coding
            w.w(cb, 4)
            w.w(ln, 5)
            sections.append([cb, start, end])
            # scale factors
            for _sfb in range(start, end):
                if cb == ZERO_HCB:
                    pass
                elif cb == 13:              # first noise energy is pcm(9)
                    if not any(x[0] == 13 for x in sections[:-1]):
                        w.w(rng.getrandbits(9), 9)
                    else:
                        codec.write_codeword(w, rng, "sf")
                else:
                    codec.write_codeword(w, rng, "sf")
            # spectral data
            if cb in (ZERO_HCB, 13, 14, 15):
                continue
            inc = 2 if cb >= FIRST_PAIR_HCB else 4
            for k in range(SWB_LONG[start], SWB_LONG[end], inc):
                codec.write_codeword(w, rng, cb)

        chs_meta.append({"gg": gg, "sections": sections})
        del sf_bits_writer, spec_bits_writer

    has_fil = rng.random() < 0.5
    if has_fil:
        w.w(ID_FIL, 3)
        w.wb(1)                                 # SBR follows (dropped by tool)
        w.w(rng.getrandbits(32), 32)

    w.align()
    payload = w.to_bytes()
    return payload, {"stereo": stereo, "winseq": 0, "max_sfb": max_sfb,
                     "ms_present": ms_present if stereo else 0,
                     "tns": tns_meta, "ch": chs_meta, "fil": has_fil}


def iter_adts(data):
    pos = 0
    while pos + 7 <= len(data):
        if data[pos] != 0xFF or (data[pos + 1] & 0xF0) != 0xF0:
            pos += 1
            continue
        hdr = 7 if (data[pos + 1] & 1) else 9
        flen = ((data[pos + 3] & 3) << 11) | (data[pos + 4] << 3) \
            | (data[pos + 5] >> 5)
        if flen < hdr or pos + flen > len(data):
            raise AssertionError("bad ADTS length at offset %d" % pos)
        yield data[pos + hdr: pos + flen]
        pos += flen

# ------------------------------------------------------------------ #
# commands                                                           #

def cmd_gen(outdir, nframes, seed):
    codec = Codec()
    rng = random.Random(seed)
    os.makedirs(outdir, exist_ok=True)

    frames = []
    expected = []
    for i in range(nframes):
        payload, meta = gen_frame(codec, rng)
        # self-check: our own HDC parser must agree with the generator
        got = parse_hdc_frame(payload, codec)
        assert got["stereo"] == meta["stereo"]
        assert got["max_sfb"] == meta["max_sfb"]
        assert [c["gg"] for c in got["ch"]] == [c["gg"] for c in meta["ch"]]
        assert [c["sections"] for c in got["ch"]] == \
               [c["sections"] for c in meta["ch"]]
        assert got["fil"] == meta["fil"]

        hdr = adts_header(len(payload) + 7, meta["stereo"])
        frames.append(hdr + payload)
        expected.append({"index": i, **meta})

    with open(os.path.join(outdir, "input.hdc"), "wb") as f:
        f.write(b"".join(frames))
    with open(os.path.join(outdir, "expected.json"), "w") as f:
        json.dump({"seed": seed, "frames": expected}, f, indent=1)

    print("wrote %s (%d frames) and expected.json"
          % (os.path.join(outdir, "input.hdc"), nframes))
    print("next: hdc2aac %s %s/converted.aac"
          % (os.path.join(outdir, "input.hdc"), outdir))
    print("then: python3 synthtest.py verify %s %s/converted.aac"
          % (outdir, outdir))


def diff_field(path, want, got, errors):
    if want != got:
        errors.append("%s: want %r, got %r" % (path, want, got))


def cmd_verify(outdir, aac_path):
    codec = Codec()
    exp = json.load(open(os.path.join(outdir, "expected.json")))["frames"]
    inp = open(os.path.join(outdir, "input.hdc"), "rb").read()
    out = open(aac_path, "rb").read()

    in_frames = list(iter_adts(inp))
    out_frames = list(iter_adts(out))

    errors = []
    if len(in_frames) != len(exp):
        errors.append("input frame count %d != expected %d"
                      % (len(in_frames), len(exp)))
    if len(out_frames) != len(exp):
        errors.append("OUTPUT FRAME COUNT %d != expected %d "
                      "(hdc2aac skipped frames?)"
                      % (len(out_frames), len(exp)))

    for i in range(min(len(exp), len(in_frames), len(out_frames))):
        e = exp[i]
        try:
            I = parse_hdc_frame(in_frames[i], codec)
            O = parse_aac_element(Reader(out_frames[i]), codec)
        except (AssertionError, EOFError) as exc:
            errors.append("frame %d: parse failure: %s" % (i, exc))
            continue

        pre = "frame %d" % i
        diff_field(pre + ".stereo", e["stereo"], I["stereo"], errors)
        diff_field(pre + ".max_sfb", e["max_sfb"], I["max_sfb"], errors)
        diff_field(pre + ".fil", e["fil"], I["fil"], errors)

        diff_field(pre + ".out.stereo", I["stereo"], O["stereo"], errors)
        diff_field(pre + ".out.winseq", I["winseq"], O["winseq"], errors)
        diff_field(pre + ".out.shape", I["shape"], O["shape"], errors)
        diff_field(pre + ".out.max_sfb", I["max_sfb"], O["max_sfb"], errors)
        diff_field(pre + ".out.ms_present", I["ms_present"],
                   O["ms_present"], errors)
        if I["ms_present"] == 1:
            diff_field(pre + ".out.ms_hex", I["ms_hex"], O["ms_hex"],
                       errors)

        for c in range(len(I["ch"])):
            p = "%s.ch%d" % (pre, c)
            ic, oc = I["ch"][c], O["ch"][c]
            diff_field(p + ".gg", ic["gg"], oc["gg"], errors)
            diff_field(p + ".sections", ic["sections"], oc["sections"],
                       errors)
            diff_field(p + ".sf_count", ic["sf_count"], oc["sf_count"],
                       errors)
            diff_field(p + ".ncoef", ic["ncoef"], oc["ncoef"], errors)
            diff_field(p + ".tns", I["tns"][c], O["ch"][c]["tns"], errors)
            # THE core assertions: verbatim copy of the compressed payload
            diff_field(p + ".secsf VERBATIM", ic["secsf_hex"],
                       oc["secsf_hex"], errors)
            diff_field(p + ".spec VERBATIM", ic["spec_hex"],
                       oc["spec_hex"], errors)

    if errors:
        print("VERIFY FAILED - %d problem(s):" % len(errors))
        for err in errors[:40]:
            print("  " + err)
        sys.exit(1)
    print("VERIFY OK: %d frames; section+scalefactor and spectral spans "
          "bit-identical between HDC input and AAC output." % len(exp))


def main(argv):
    usage = ("usage: synthtest.py gen <outdir> [--frames N] [--seed S]\n"
             "       synthtest.py verify <outdir> <converted.aac>")
    if len(argv) < 2:
        print(usage)
        return 2
    if argv[1] == "gen":
        outdir = argv[2]
        nframes, seed = 200, 1
        args = argv[3:]
        if "--frames" in args:
            nframes = int(args[args.index("--frames") + 1])
        if "--seed" in args:
            seed = int(args[args.index("--seed") + 1])
        cmd_gen(outdir, nframes, seed)
        return 0
    if argv[1] == "verify":
        cmd_verify(argv[2], argv[3])
        return 0
    print(usage)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))




