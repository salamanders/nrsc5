#!/usr/bin/env python3
"""
gen_hcb_trees.py - generate hcb_trees.h for hdc2aac.c

Reads the Huffman codebook tables shipped with faad2 (libfaad/codebook/*.h)
and derives, for every codeword of every codebook:
  - the number of sign bits that follow the codeword (count of non-zero
    values in the decoded tuple, for the *signed* codebooks), and
  - the number of escape sequences that follow (count of +/-16 values,
    codebook 11 only).

This is everything a bit-accounting transcoder needs in order to walk an
AAC spectral-data payload without decoding coefficient values.  The result
is emitted as compact binary-trie tables (hcb_trees.h).

Usage:  python3 gen_hcb_trees.py <path-to-faad2-libfaad-dir> <output-header>

The upstream repository is https://github.com/knik0/faad2 (nrsc5 pins tag
2.11.2 and applies support/faad2-hdc-support.patch, which does not modify
the Huffman tables).
"""

import re
import sys
import os


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    return text


def parse_array(text, name):
    """Extract `static const <type> name[...] = { ... };` body as list of tuples."""
    m = re.search(r"\b%s(?:\s*\[[^\]]*\])+\s*=\s*\{" % re.escape(name), text)
    if not m:
        raise RuntimeError("array %s not found" % name)
    depth = 0
    i = m.end() - 1
    assert text[i] == "{"
    while True:
        c = text[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                break
        i += 1
    body = text[m.end():i]

    # split top-level {...} groups
    groups = []
    depth = 0
    cur = ""
    for c in body:
        if c == "{":
            depth += 1
            if depth == 1:
                cur = ""
                continue
        elif c == "}":
            depth -= 1
            if depth == 0:
                groups.append(cur)
                continue
        if depth >= 1:
            cur += c
    if not groups and body.strip():  # flat array without inner braces
        groups = [body]

    out = []
    for g in groups:
        nums = [int(x) for x in re.findall(r"-?\d+", g)]
        out.append(tuple(nums))
    return out


class Trie:
    __slots__ = ("children", "leaf")

    def __init__(self):
        self.children = None  # dict bit->Trie
        self.leaf = None      # (nsign, esc)

    def insert(self, path, payload):
        node = self
        for b in path:
            if node.leaf is not None:
                raise RuntimeError("codeword is prefixed by a shorter codeword")
            if node.children is None:
                node.children = {}
            node = node.children.setdefault(b, Trie())
        if node.children is not None:
            raise RuntimeError("shorter codeword prefixed by a longer one")
        if node.leaf is not None:
            # Duplicate lookup result (the 2-step tables contain redundant
            # root entries).  Harmless for decoding, but the payloads must
            # agree - otherwise our enumeration is buggy.
            if node.leaf != payload:
                raise RuntimeError("inconsistent duplicate codeword")
            return False
        node.leaf = payload
        return True

    def flatten(self):
        """Return list of nodes: (is_leaf, a, b).
        internal: a,b = child offsets (bit0 -> a, bit1 -> b)
        leaf: a = nsign, b = esc_count"""
        nodes = []

        def visit(t):
            idx = len(nodes)
            nodes.append(None)  # reserve slot
            if t.leaf is not None:
                nsign, esc = t.leaf
                nodes[idx] = (1, nsign, esc)
            else:
                kids = []
                for b in (0, 1):
                    child = t.children.get(b)
                    if child is None:
                        kids.append(-1)  # unreachable prefix: error leaf
                    else:
                        kids.append(visit(child))
                nodes[idx] = (0, kids[0], kids[1])
            return idx

        visit(self)
        return nodes


def leaf_payload(vals, signed):
    if signed:
        nsign = sum(1 for v in vals if v != 0)
    else:
        nsign = 0
    esc = sum(1 for v in vals if abs(v) == 16)
    return (nsign, esc)


def enum_two_step(root, second, signed, dim):
    """Enumerate leaves of a 2-step codebook the way faad decodes it.
    Root table size comes from faad's hcbN[]: 6 bits for cb10, else 5."""
    root_bits = 6 if len(root) == 64 else 5
    for prefix in range(1 << root_bits):
        off, extra = root[prefix]
        prefix_path = tuple((prefix >> (root_bits - 1 - i)) & 1
                            for i in range(root_bits))
        if extra == 0:
            e = second[off]
            vals = e[1:1 + dim]
            yield prefix_path[:e[0]], leaf_payload(vals, signed)
        else:
            for suf in range(1 << extra):
                e = second[off + suf]
                vals = e[1:1 + dim]
                path = prefix_path + tuple((suf >> (extra - 1 - i)) & 1
                                           for i in range(extra))
                yield path[:e[0]], leaf_payload(vals, signed)


def enum_binary(table, dim, signed):
    """DFS over a binary-search table: entry = (is_leaf, d0, d1[, ...]).
    Internal nodes: d0/d1 are index deltas for bit 0/1.
    Leaves: d0..d(dim-1) are the values."""

    def dfs(offset, path):
        e = table[offset]
        if e[0]:  # leaf
            vals = e[1:1 + dim]
            yield tuple(path), leaf_payload(vals, signed)
        else:
            for b in (0, 1):
                step = e[1 + b]
                if step == 0:
                    raise RuntimeError("cycle in binary table at %d" % offset)
                yield from dfs(offset + step, path + [b])

    yield from dfs(0, [])


def enum_sf(table):
    """hcb_sf rows: (value_or_unused, step0, step1)? No - rows are pairs
    [offset][0] = value at leaf, [offset][1..2]? faad uses hcb_sf[offset][2]
    where [1] != 0 means internal and [b] is the index delta."""
    def dfs(offset, path):
        row = table[offset]
        # hcb_sf rows are pairs; internal rows use index 1 for bit0? No:
        # faad: offset += hcb_sf[offset][b] for b in {0,1}, while [offset][1] != 0
        if row[1] == 0:
            yield tuple(path), (0, 0)
            return
        for b in (0, 1):
            yield from dfs(offset + row[b], path + [b])

    yield from dfs(0, [])


def build(name, leaves):
    trie = Trie()
    n = 0
    kraft = 0.0
    for path, payload in leaves:
        if len(path) > 32:
            raise RuntimeError("codeword too long in " + name)
        if trie.insert(path, payload):
            n += 1
            kraft += 2.0 ** -len(path)
    # A complete prefix code must satisfy the Kraft equality exactly.
    if abs(kraft - 1.0) > 1e-9:
        raise RuntimeError("%s: Kraft sum %.9f != 1 (incomplete code)"
                           % (name, kraft))
    nodes = trie.flatten()
    return name, nodes, n


def max_depth(nodes):
    best = 0
    stack = [(0, 0)]
    while stack:
        i, d = stack.pop()
        leaf, a, b = nodes[i]
        if leaf:
            best = max(best, d)
        else:
            for c in (a, b):
                if c >= 0:
                    stack.append((c, d + 1))
    return best


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(1)
    libdir, outpath = sys.argv[1], sys.argv[2]
    cbdir = os.path.join(libdir, "codebook")
    texts = {}
    for n in list(range(1, 12)) + ["sf"]:
        fn = os.path.join(cbdir, "hcb_%s.h" % n)
        with open(fn, "r") as f:
            texts[n] = strip_comments(f.read())

    defs = []

    # quads: 1,2 unsigned two-step; 4 signed two-step
    for cb in (1, 2, 4):
        root = parse_array(texts[cb], "hcb%d_1" % cb)
        second = parse_array(texts[cb], "hcb%d_2" % cb)
        defs.append(build("hdc_cb%d" % cb,
                          enum_two_step(root, second, cb == 4, 4)))

    # codebook 3: binary search, quads with sign bits (unsigned in table)
    t3 = parse_array(texts[3], "hcb3")
    defs.append(build("hdc_cb3", enum_binary(t3, 4, True)))

    # pairs: 6 signed in table (no sign bits); 8,10,11 unsigned in table (has sign bits)
    for cb in (6, 8, 10, 11):
        root = parse_array(texts[cb], "hcb%d_1" % cb)
        second = parse_array(texts[cb], "hcb%d_2" % cb)
        defs.append(build("hdc_cb%d" % cb,
                          enum_two_step(root, second, cb != 6, 2)))
    # pairs: 5 signed in table (no sign bits); 7,9 unsigned in table (has sign bits)
    for cb in (5, 7, 9):
        tbl = parse_array(texts[cb], "hcb%d" % cb)
        defs.append(build("hdc_cb%d" % cb, enum_binary(tbl, 2, cb != 5)))

    sf = parse_array(texts["sf"], "hcb_sf")
    defs.append(build("hdc_cb_sf", enum_sf(sf)))

    with open(outpath, "w") as f:
        f.write("/* GENERATED FILE - do not edit.\n")
        f.write(" * Generated by support/hdc2aac/gen_hcb_trees.py from the\n")
        f.write(" * faad2 Huffman codebook tables (https://github.com/knik0/faad2).\n")
        f.write(" *\n")
        f.write(" * Node encoding: {leaf, a, b}\n")
        f.write(" *   internal node: walk bit b (0 -> a, 1 -> b); -1 = invalid prefix\n")
        f.write(" *   leaf:          a = sign bits that follow the codeword,\n")
        f.write(" *                  b = escape sequences that follow (cb 11 only)\n")
        f.write(" */\n#ifndef HCB_TREES_H\n#define HCB_TREES_H\n\n")
        f.write("typedef struct { short leaf, a, b; } hdc_hcb_node;\n\n")
        for name, nodes, nleaves in defs:
            f.write("/* %d codewords, max depth %d */\n"
                    "static const hdc_hcb_node %s[] = {\n"
                    % (nleaves, max_depth(nodes), name))
            for leaf, a, b in nodes:
                f.write("    { %d, %4d, %4d },\n" % (leaf, a, b))
            f.write("};\n\n")
        f.write("#define HDC_CB_COUNT 12\n")
        f.write("static const hdc_hcb_node * const hdc_cbs[HDC_CB_COUNT] = {\n")
        f.write("    NULL, hdc_cb1, hdc_cb2, hdc_cb3, hdc_cb4, hdc_cb5,\n")
        f.write("    hdc_cb6, hdc_cb7, hdc_cb8, hdc_cb9, hdc_cb10, hdc_cb11\n")
        f.write("};\n")
        f.write("\n#endif /* HCB_TREES_H */\n")

    print("wrote %s (%d tables)" % (outpath, len(defs)))
    for name, nodes, nleaves in defs:
        print("  %-10s nodes=%4d codewords=%4d maxdepth=%d"
              % (name, len(nodes), nleaves, max_depth(nodes)))


if __name__ == "__main__":
    main()

