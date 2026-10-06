"""Minimal Valve VPK (v1/v2) reader for the user's own Portal 2 install. vpk.py [substring] lists matching files."""
import struct, os, sys
P2 = os.path.expanduser('~/.local/share/Steam/steamapps/common/Portal 2/portal2')

class VPK:
    def __init__(self, dirpath):
        self.dirpath = dirpath
        self.base = dirpath[:-len('_dir.vpk')]
        f = open(dirpath, 'rb'); self.data = f.read()
        sig, ver, tree = struct.unpack_from('<III', self.data, 0)
        assert sig == 0x55aa1234
        self.hdr = 12 if ver == 1 else 28
        self.tree_end = self.hdr + tree
        self.files = {}
        p = self.hdr
        def rs():
            nonlocal p
            e = self.data.index(b'\0', p); s = self.data[p:e].decode('latin1'); p = e + 1; return s
        while True:
            ext = rs()
            if not ext: break
            while True:
                d = rs()
                if not d: break
                while True:
                    n = rs()
                    if not n: break
                    crc, pre, arch, off, ln, term = struct.unpack_from('<IHHIIH', self.data, p); p += 18
                    preload = self.data[p:p + pre]; p += pre
                    path = (f'{d}/' if d.strip() else '') + f'{n}.{ext}'
                    self.files[path.lower()] = (arch, off, ln, preload)
    def read(self, path):
        arch, off, ln, pre = self.files[path.lower()]
        if ln == 0: return pre
        if arch == 0x7fff:
            return pre + self.data[self.tree_end + off:self.tree_end + off + ln]
        with open(f'{self.base}_{arch:03d}.vpk', 'rb') as f:
            f.seek(off); return pre + f.read(ln)

def open_all():
    out = []
    for root in [P2, P2 + '_dlc1', P2 + '_dlc2']:
        d = os.path.join(root, 'pak01_dir.vpk')
        if os.path.exists(d): out.append(VPK(d))
    return out

def find(path, vpks=None):
    for v in vpks or open_all():
        if path.lower() in v.files: return v.read(path)
    # loose file fallback
    for root in [P2, P2 + '_dlc1', P2 + '_dlc2']:
        p = os.path.join(root, path)
        if os.path.exists(p): return open(p, 'rb').read()
    return None

if __name__ == '__main__':
    q = sys.argv[1].lower() if len(sys.argv) > 1 else ''
    for v in open_all():
        for k in sorted(v.files):
            if q in k: print(os.path.basename(v.dirpath), k, v.files[k][2] + len(v.files[k][3]))
