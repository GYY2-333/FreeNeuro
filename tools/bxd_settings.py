#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Dump the real per-object settings of a BioExplorer design.

BioExplorer stores the connectable-object parameters NOT in Design/Objects/<n>/
Properties (that only holds Visible/UserName/ObjectID/ClassID) but in
Design/PostConnectionObjects/<n>/Properties, keyed by ObjectID.  Settings like
FrequencyRange, UpperThreshold or CounterType live only there.
"""
import sys, os, struct
from bxd_parse import CFB
from bxd_objects import find, props_of
from bxd_objects import dec_str

TYPE_NAMES = {1: 'guid', 2: 'int', 3: 'str', 4: 'bool', 5: 'dbl',
              6: 'blob', 7: 'range'}


def read_props2(buf):
    """like bxd_objects.read_props but decodes type 7 as a pair of doubles"""
    props = []
    if not buf:
        return props
    p = 1
    count = struct.unpack_from('<I', buf, p)[0]
    p += 4
    for _ in range(count):
        if p + 4 > len(buf):
            break
        nl = struct.unpack_from('<I', buf, p)[0]
        p += 4
        name = buf[p:p + nl].rstrip(b'\x00').decode('latin-1')
        p += nl
        typ, dl = struct.unpack_from('<II', buf, p)
        p += 8
        data = buf[p:p + dl]
        p += dl
        if typ == 1:
            v = (data[0:4].hex() + '-' + data[4:6].hex() + '-' + data[6:8].hex()
                 + '-' + data[8:10].hex() + '-' + data[10:16].hex())
        elif typ in (2, 4):
            v = struct.unpack('<i', data)[0]
        elif typ == 3:
            v = dec_str(data)
        elif typ == 5:
            v = struct.unpack('<d', data)[0]
        elif typ == 7:
            n = dl // 8
            vals = struct.unpack('<%dd' % n, data)
            v = '[%s]' % ', '.join('%.4g' % x for x in vals)
        else:
            v = '<%d bytes>' % len(data)
        props.append((name, typ, v))
    return props


def main():
    path = sys.argv[1]
    c = CFB(open(path, 'rb').read())
    design = find(c, 0, 'Design')

    # object index -> (name, classid)
    objs = {}
    for ch in c.children(find(c, design['id'], 'Objects')['id']):
        if ch['type'] != 1:
            continue
        p = props_of(c, find(c, ch['id'], 'Properties'))
        objs[p.get('ObjectID', '')] = (ch['name'], p.get('UserName1') or '')

    post = find(c, design['id'], 'PostConnectionObjects')
    print('=== real object settings: %s ===' % os.path.basename(path))
    for pch in c.children(post['id']):
        if pch['type'] != 1:
            continue
        props = read_props2(props_of_stream(c, pch))
        pid = ''
        for n, t, v in props:
            if n == 'ObjectID':
                pid = v
        idx, uname = objs.get(pid, ('?', '?'))
        print()
        print('--- object [%s] %s  (post #%s)' % (idx, uname, pch['name']))
        for n, t, v in props:
            if n in ('BackgroundColor', 'RefreshPeriod', 'ShowObjectName',
                     'ShowHeaders', 'ShowLabels'):
                continue
            print('    %-24s %-5s %s' % (n, TYPE_NAMES.get(t, t), v))


def props_of_stream(c, entry):
    p = find(c, entry['id'], 'Properties')
    if p is None:
        return b''
    return c.stream(p)


if __name__ == '__main__':
    main()
