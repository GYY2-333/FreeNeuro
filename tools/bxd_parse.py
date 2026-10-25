#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Minimal OLE2 / Compound File Binary parser for BioExplorer .bxd designs.

Usage:
    python bxd_parse.py <file.bxd> [--dump DIR] [--strings]

Prints the CFB directory tree and optionally writes every stream to DIR.
"""
import sys, os, struct, io

FREESECT = 0xFFFFFFFF
ENDOFCHAIN = 0xFFFFFFFE
FATSECT = 0xFFFFFFFD
DIFSECT = 0xFFFFFFFC


class CFB:
    def __init__(self, data):
        if data[:8] != b'\xd0\xcf\x11\xe0\xa1\xb1\x1a\xe1':
            raise ValueError('not a compound file')
        self.data = data
        self.sector_size = 1 << struct.unpack_from('<H', data, 30)[0]
        self.mini_sector_size = 1 << struct.unpack_from('<H', data, 32)[0]
        (num_fat, dir_start, _, self.mini_cutoff, minifat_start,
         num_minifat, difat_start, num_difat) = struct.unpack_from('<IIIIIIII', data, 44)

        # DIFAT: first 109 entries in the header
        difat = list(struct.unpack_from('<109I', data, 76))
        sec = difat_start
        per = self.sector_size // 4 - 1
        while num_difat > 0 and sec not in (FREESECT, ENDOFCHAIN):
            raw = self._sector(sec)
            vals = struct.unpack_from('<%dI' % (per + 1), raw, 0)
            difat.extend(vals[:per])
            sec = vals[per]
            num_difat -= 1

        self.fat = []
        for s in difat[:num_fat]:
            if s in (FREESECT, ENDOFCHAIN, FATSECT, DIFSECT):
                continue
            self.fat.extend(struct.unpack('<%dI' % (self.sector_size // 4),
                                          self._sector(s)))

        self.dir_chain = self._chain(dir_start)
        self.entries = self._read_dir()

        self.minifat = []
        if num_minifat and minifat_start not in (FREESECT, ENDOFCHAIN):
            for s in self._chain(minifat_start):
                self.minifat.extend(struct.unpack('<%dI' % (self.sector_size // 4),
                                                  self._sector(s)))
        # mini stream container = root entry stream
        self.ministream = b''
        if self.entries and self.entries[0]['type'] == 5:
            self.ministream = self._read_chain(self.entries[0]['start'],
                                               self.entries[0]['size'], mini=False)

    def _sector(self, i):
        off = (i + 1) * self.sector_size
        return self.data[off:off + self.sector_size]

    def _chain(self, start):
        out = []
        s = start
        seen = set()
        while s not in (FREESECT, ENDOFCHAIN) and s < len(self.fat) and s not in seen:
            out.append(s)
            seen.add(s)
            s = self.fat[s]
        return out

    def _read_chain(self, start, size, mini):
        if mini:
            out = bytearray()
            s = start
            seen = set()
            while s not in (FREESECT, ENDOFCHAIN) and s < len(self.minifat) and s not in seen:
                seen.add(s)
                off = s * self.mini_sector_size
                out += self.ministream[off:off + self.mini_sector_size]
                s = self.minifat[s]
            return bytes(out[:size])
        out = bytearray()
        for s in self._chain(start):
            out += self._sector(s)
        return bytes(out[:size])

    def _read_dir(self):
        raw = b''.join(self._sector(s) for s in self.dir_chain)
        entries = []
        for i in range(len(raw) // 128):
            e = raw[i * 128:(i + 1) * 128]
            name_len = struct.unpack_from('<H', e, 64)[0]
            name = e[:max(0, name_len - 2)].decode('utf-16-le', 'replace')
            etype = e[66]
            left, right, child = struct.unpack_from('<III', e, 68)
            start, size = struct.unpack_from('<II', e, 116)
            entries.append({'id': i, 'name': name, 'type': etype, 'left': left,
                            'right': right, 'child': child, 'start': start,
                            'size': size})
        return entries

    def children(self, idx):
        """walk the red-black tree of a storage's child"""
        out = []

        def walk(i, depth):
            if i == FREESECT or i >= len(self.entries) or depth > 64:
                return
            e = self.entries[i]
            walk(e['left'], depth + 1)
            out.append(e)
            walk(e['right'], depth + 1)

        if idx < 0:
            return out
        walk(self.entries[idx]['child'], 0)
        return out

    def tree(self, idx=0, depth=0):
        lines = []
        e = self.entries[idx]
        lines.append((depth, e))
        if e['type'] == 5 or e['type'] == 1:
            for c in self.children(idx):
                lines.extend(self.tree(c['id'], depth + 1))
        return lines

    def stream(self, entry):
        if entry['type'] != 2:
            return b''
        mini = entry['size'] < self.mini_cutoff
        return self._read_chain(entry['start'], entry['size'], mini)


def ascii_runs(buf, minlen=4):
    out = []
    cur = bytearray()
    for b in buf:
        if 0x20 <= b <= 0x7E:
            cur.append(b)
        else:
            if len(cur) >= minlen:
                out.append(cur.decode('1252'))
            cur = bytearray()
    if len(cur) >= minlen:
        out.append(cur.decode('1252'))
    return out


def unicode_runs(buf, minlen=3):
    out = []
    s = buf.decode('utf-16-le', 'replace')
    cur = []
    for ch in s:
        if 0x20 <= ord(ch) <= 0x7E or 0x4e00 <= ord(ch) <= 0x9fff or ch in '　':
            cur.append(ch)
        else:
            if len(cur) >= minlen:
                out.append(''.join(cur))
            cur = []
    if len(cur) >= minlen:
        out.append(''.join(cur))
    return out


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    path = sys.argv[1]
    dumpdir = None
    if '--dump' in sys.argv:
        dumpdir = sys.argv[sys.argv.index('--dump') + 1]
        os.makedirs(dumpdir, exist_ok=True)
    strings = '--strings' in sys.argv

    data = open(path, 'rb').read()
    c = CFB(data)
    print('file   :', path, len(data), 'bytes  sector', c.sector_size,
          'cutoff', c.mini_cutoff)
    print('--- directory ---')
    for depth, e in c.tree():
        tname = {0: '', 1: 'STORAGE', 2: 'STREAM', 5: 'ROOT'}.get(e['type'], '?')
        print('  ' * depth + '%-32s %-8s start=%-6d size=%d' %
              (repr(e['name']), tname, e['start'], e['size']))
    if dumpdir:
        print('--- streams ---')
        for e in c.entries:
            if e['type'] != 2:
                continue
            buf = c.stream(e)
            safe = ''.join(ch if ch.isalnum() or ch in '_-.' else '_' for ch in e['name'])
            out = os.path.join(dumpdir, '%02d_%s.bin' % (e['id'], safe))
            open(out, 'wb').write(buf)
            print('  wrote', out, len(buf), 'bytes')
            if strings:
                for s in unicode_runs(buf):
                    print('    U|', s)
                for s in ascii_runs(buf):
                    print('    A|', s)
    return 0


if __name__ == '__main__':
    sys.exit(main())
