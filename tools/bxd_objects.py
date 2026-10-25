#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Decode a BioExplorer .bxd design: design objects, ports and their wiring."""
import sys, os, struct
from bxd_parse import CFB

TYPE_NAMES = {1: 'guid', 2: 'int', 3: 'str', 4: 'bool', 5: 'dbl', 6: 'blob'}


def dec_str(b):
    for enc in ('gbk', 'utf-8', 'latin-1'):
        try:
            return b.rstrip(b'\x00').decode(enc)
        except Exception:
            pass
    return repr(b)


def read_props(buf):
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
            v = (data[0:4].hex() + '-' + data[4:6].hex() + '-' + data[6:8].hex() +
                 '-' + data[8:10].hex() + '-' + data[10:16].hex())
        elif typ in (2, 4):
            v = struct.unpack('<i', data)[0]
        elif typ == 3:
            v = dec_str(data)
        elif typ == 5:
            v = struct.unpack('<d', data)[0]
        else:
            v = '<%d bytes>' % len(data)
        props.append((name, typ, v))
    return props


def find(c, parent_idx, name):
    for ch in c.children(parent_idx):
        if ch['name'] == name:
            return ch
    return None


def props_of(c, entry):
    if entry is None or entry['type'] != 2:
        return {}
    return {n: v for n, t, v in read_props(c.stream(entry))}


SKIP = {'ObjectID', 'ClassID', 'ClassIDString', 'Ordinal', 'Visible'}


def main():
    path = sys.argv[1]
    c = CFB(open(path, 'rb').read())
    design = find(c, 0, 'Design')
    print('=== %s ===' % os.path.basename(path))
    print('-- Design props:', props_of(c, find(c, design['id'], 'Properties')))
    print()

    objects = find(c, design['id'], 'Objects')
    items = []
    for ch in c.children(objects['id']):
        if ch['type'] != 1:
            continue
        p = props_of(c, find(c, ch['id'], 'Properties'))
        ins, outs = [], []
        ip = find(c, ch['id'], 'InputPorts')
        if ip:
            for pch in c.children(ip['id']):
                if pch['type'] != 1:
                    continue
                d = props_of(c, find(c, pch['id'], 'Properties'))
                d['_i'] = pch['name']
                ins.append(d)
        op = find(c, ch['id'], 'OutputPorts')
        if op:
            for pch in c.children(op['id']):
                if pch['type'] != 1:
                    continue
                d = props_of(c, find(c, pch['id'], 'Properties'))
                d['_i'] = pch['name']
                outs.append(d)
        items.append({'idx': ch['name'], 'props': p, 'in': ins, 'out': outs})

    def sk(o):
        try:
            return int(o['idx'])
        except Exception:
            return 9999
    items.sort(key=sk)
    for o in items:
        print('### [%s] %s   ordinal=%s' % (o['idx'], o['props'].get('UserName1', '?'),
                                           o['props'].get('Ordinal')))
        print('    class=%s' % (o['props'].get('ClassIDString') or o['props'].get('ClassID')))
        for n, v in o['props'].items():
            if n in SKIP or n == 'UserName1':
                continue
            print('      %-30s %s' % (n, v))
        for p in o['in']:
            print('      IN %-4s %s' % (p.get('_i'), {k: v for k, v in p.items() if not k.startswith('_')}))
        for p in o['out']:
            print('      OUT %-4s %s' % (p.get('_i'), {k: v for k, v in p.items() if not k.startswith('_')}))
        print()

    conn = find(c, design['id'], 'Connections')
    if conn:
        print('-- Connections storage, props:', props_of(c, find(c, conn['id'], 'Properties')))
        for pch in c.children(conn['id']):
            if pch['type'] == 1:
                print('   conn[%s] %s' % (pch['name'],
                                          props_of(c, find(c, pch['id'], 'Properties'))))
            else:
                print('   conn stream', pch['name'], list(read_props(c.stream(pch))))
    post = find(c, design['id'], 'PostConnectionObjects')
    if post:
        print('-- PostConnectionObjects:', [x['name'] for x in c.children(post['id'])])


if __name__ == '__main__':
    main()
