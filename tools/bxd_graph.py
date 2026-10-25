#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Print the resolved signal flow of a BioExplorer .bxd design."""
import sys, os
from bxd_parse import CFB
from bxd_objects import read_props, find, props_of


def main():
    path = sys.argv[1]
    c = CFB(open(path, 'rb').read())
    design = find(c, 0, 'Design')
    objects = find(c, design['id'], 'Objects')

    idmap = {}      # port ObjectID -> (objidx, name, portidx, dir)
    objs = {}
    for ch in c.children(objects['id']):
        if ch['type'] != 1:
            continue
        p = props_of(c, find(c, ch['id'], 'Properties'))
        name = p.get('UserName1') or ('obj' + ch['name'])
        objs[ch['name']] = name
        for dname, tag in (('InputPorts', 'IN'), ('OutputPorts', 'OUT')):
            st = find(c, ch['id'], dname)
            if not st:
                continue
            for pch in c.children(st['id']):
                if pch['type'] != 1:
                    continue
                pp = props_of(c, find(c, pch['id'], 'Properties'))
                idmap[pp['ObjectID']] = (ch['name'], name, pch['name'], tag)
                # port display name, if the design stored one
                for k in ('UserName1', 'Name', 'PortName', 'Description'):
                    if k in pp:
                        idmap[pp['ObjectID']] = (ch['name'], name, pch['name'], tag,
                                                 pp[k])

    conn = find(c, design['id'], 'Connections')
    print('=== signal flow: %s ===' % os.path.basename(path))
    rows = []
    for pch in c.children(conn['id']):
        if pch['type'] != 1:
            continue
        cp = props_of(c, find(c, pch['id'], 'Properties'))
        src = idmap.get(cp.get('OutputPortID'))
        dst = idmap.get(cp.get('InputPortID'))
        rows.append((src, dst))

    def fmt(t):
        if not t:
            return '???'
        extra = (' "%s"' % t[4]) if len(t) > 4 else ''
        return '[%s]%s.%s%s%s' % (t[0], t[1], t[3], t[2], extra)

    for src, dst in rows:
        print('  %-40s ->  %s' % (fmt(src), fmt(dst)))

    print()
    print('=== in-degree per object ===')
    deg = {}
    for src, dst in rows:
        if dst:
            deg[dst[0]] = deg.get(dst[0], 0) + 1
    for k in sorted(deg, key=lambda x: int(x)):
        print('  [%s] %-14s %d input(s)' % (k, objs.get(k, '?'), deg[k]))


if __name__ == '__main__':
    main()
