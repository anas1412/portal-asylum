"""Convert Portal 2's portal gun (from the user's own install) into cache/gun.bin for the Outlast mod.
Reads MDL v49 + VVD + DX90 VTX + VMT/VTF (DXT1/DXT5/BGRA). Nothing from Portal 2 is redistributed: this runs on
the player's PC.  Usage: p2gun.py [model path] [--preview out.png] [--skip-bodypart name]"""
import struct, os, sys, math
sys.path.insert(0, os.path.dirname(__file__))
from vpk import open_all, find

OUT = os.path.expanduser('~/outlast-portal-gun/cache')

def cstr(b, o):
    return b[o:b.index(b'\0', o)].decode('latin1')

# ---------------------------------------------------------------- VTF
def dxt_color(c0, c1, bits, out, opaque):
    def rgb(c): return ((c >> 11 & 31) * 255 // 31, (c >> 5 & 63) * 255 // 63, (c & 31) * 255 // 31)
    a, b = rgb(c0), rgb(c1)
    if c0 > c1 or not opaque:
        pal = [a, b, tuple((2 * x + y) // 3 for x, y in zip(a, b)), tuple((x + 2 * y) // 3 for x, y in zip(a, b))]
    else:
        pal = [a, b, tuple((x + y) // 2 for x, y in zip(a, b)), (0, 0, 0)]
    for i in range(16): out[i] = pal[bits >> (2 * i) & 3]

def decode_vtf(data):
    assert data[:4] == b'VTF\0', 'not a VTF'
    major, minor, hsize = struct.unpack_from('<III', data, 4)
    w, h = struct.unpack_from('<HH', data, 16)
    fmt, = struct.unpack_from('<i', data, 52)
    bpp = {0: 4, 2: 3, 3: 3, 12: 4, 16: 4}
    if fmt == 13: size = max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * 8
    elif fmt in (14, 15): size = max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * 16
    elif fmt in bpp: size = w * h * bpp[fmt]
    else: raise ValueError(f'VTF format {fmt}')
    frames, = struct.unpack_from('<H', data, 24)
    # largest mip of frame 0 is last in the high-res block; with several frames, frame 0 of mip 0 is frames*size from the end
    end = len(data)
    if minor >= 3:
        nres, = struct.unpack_from('<I', data, 68)
        for k in range(nres):
            tag, flags, off = struct.unpack_from('<3sBI', data, 80 + 8 * k)
            if tag == b'\x30\0\0': hi_off = off
        # high-res data runs to the end unless another resource follows it; mips smallest first
    img = data[end - size * frames: end - size * (frames - 1)] if frames > 1 else data[end - size:]
    px = [(0, 0, 0, 255)] * (w * h)
    if fmt in (13, 15):
        bs = 8 if fmt == 13 else 16
        blk = [None] * 16
        k = 0
        for by in range(0, h, 4):
            for bx in range(0, w, 4):
                o = k * bs; k += 1
                alpha = [255] * 16
                if fmt == 15:
                    a0, a1 = img[o], img[o + 1]
                    abits = int.from_bytes(img[o + 2:o + 8], 'little')
                    pal = [a0, a1] + ([((6 - i) * a0 + (1 + i) * a1) // 7 for i in range(6)] if a0 > a1 else
                                      [((4 - i) * a0 + (1 + i) * a1) // 5 for i in range(4)] + [0, 255])
                    alpha = [pal[abits >> (3 * i) & 7] for i in range(16)]
                    o += 8
                c0, c1, bits = struct.unpack_from('<HHI', img, o)
                dxt_color(c0, c1, bits, blk, fmt == 13)
                for i in range(16):
                    x, y = bx + i % 4, by + i // 4
                    if x < w and y < h: px[y * w + x] = blk[i] + (alpha[i],)
    else:
        b = bpp[fmt]
        for i in range(w * h):
            q = img[i * b:i * b + b]
            if fmt == 0: px[i] = (q[0], q[1], q[2], q[3])
            elif fmt == 2: px[i] = (q[0], q[1], q[2], 255)
            elif fmt == 3: px[i] = (q[2], q[1], q[0], 255)
            elif fmt == 12: px[i] = (q[2], q[1], q[0], q[3])
            elif fmt == 16: px[i] = (q[2], q[1], q[0], 255)
    return w, h, px

def vmt_basetexture(vpks, matname, dirs):
    for d in dirs:
        p = f'materials/{d}{matname}.vmt'.replace('\\', '/').lower()
        data = find(p, vpks)
        if not data: continue
        txt = data.decode('latin1').lower()
        for line in txt.splitlines():
            parts = line.replace('"', ' ').split()
            if len(parts) >= 2 and parts[0] == '$basetexture':
                return parts[1].replace('\\', '/')
        return None
    return None

# ---------------------------------------------------------------- model
def load_model(vpks, path):
    mdl = find(path, vpks); vvd = find(path[:-4] + '.vvd', vpks); vtx = find(path[:-4] + '.dx90.vtx', vpks)
    assert mdl and vvd and vtx, 'model files missing'
    ver, = struct.unpack_from('<i', mdl, 4)
    numtex, texidx = struct.unpack_from('<ii', mdl, 204)
    numcd, cdidx = struct.unpack_from('<ii', mdl, 212)
    numskinref, numfam, skinidx = struct.unpack_from('<iii', mdl, 220)
    numbp, bpidx = struct.unpack_from('<ii', mdl, 232)
    textures = [cstr(mdl, texidx + 64 * i + struct.unpack_from('<i', mdl, texidx + 64 * i)[0]) for i in range(numtex)]
    cddirs = [cstr(mdl, struct.unpack_from('<i', mdl, cdidx + 4 * i)[0]) for i in range(numcd)]
    skin = list(struct.unpack_from(f'<{numskinref}h', mdl, skinidx))  # family 0
    # VVD
    numlods, = struct.unpack_from('<i', vvd, 12)
    nfix, fixstart, vstart = struct.unpack_from('<iii', vvd, 48)
    raw = [struct.unpack_from('<3f3f2f', vvd, vstart + 48 * i + 16) for i in range((len(vvd) - vstart) // 48)]
    if nfix:
        verts = []
        for k in range(nfix):
            lod, src, n = struct.unpack_from('<iii', vvd, fixstart + 12 * k)
            if lod >= 0: verts += raw[src:src + n]
    else:
        verts = raw
    # VTX: detect 25/27 vs 33/35 strip group / strip header sizes
    nbp_vtx, bpoff = struct.unpack_from('<ii', vtx, 28)
    out_tris = []  # (material index, v0, v1, v2) using global vertex ids
    bp_names = []
    for b in range(numbp):
        bo = bpidx + 16 * b
        nameoff, nmodels, base, modelidx = struct.unpack_from('<iiii', mdl, bo)
        bp_names.append(cstr(mdl, bo + nameoff))
        vb = bpoff + 8 * b
        vnm, vmo = struct.unpack_from('<ii', vtx, vb)
        for m in range(min(1, nmodels)):  # first model of each body part
            mo = bo + modelidx + 148 * m
            nmesh, meshidx, nverts, vindex = struct.unpack_from('<iiii', mdl, mo + 72)
            vm = vb + vmo + 8 * m
            nl, lodo = struct.unpack_from('<ii', vtx, vm)
            lod0 = vm + lodo
            nmeshv, mesho = struct.unpack_from('<ii', vtx, lod0)
            for me in range(nmesh):
                mmo = mo + meshidx + 116 * me
                mat, _, mnv, mvoff = struct.unpack_from('<iiii', mdl, mmo)
                vme = lod0 + mesho + 9 * me
                nsg, sgo = struct.unpack_from('<ii', vtx, vme)
                sgsize = 25
                if nsg > 1:
                    # stride check: second strip group must start where a 25-byte stride says
                    sgsize = 25 if struct.unpack_from('<i', vtx, vme + sgo + 25)[0] < 100000 else 33
                for g in range(nsg):
                    sg = vme + sgo + sgsize * g
                    nv, vo, ni, io, ns, so = struct.unpack_from('<iiiiii', vtx, sg)
                    vids = [struct.unpack_from('<H', vtx, sg + vo + 9 * k + 4)[0] for k in range(nv)]
                    idx = struct.unpack_from(f'<{ni}H', vtx, sg + io)
                    base_v = vindex // 48 + mvoff
                    for t in range(0, ni, 3):
                        out_tris.append((mat, bp_names[-1], base_v + vids[idx[t]], base_v + vids[idx[t + 1]], base_v + vids[idx[t + 2]]))
    return dict(textures=textures, cddirs=cddirs, skin=skin, verts=verts, tris=out_tris, bodyparts=bp_names)

# ---------------------------------------------------------------- main
def main():
    args = sys.argv[1:]
    path = next((a for a in args if a.endswith('.mdl')), 'models/weapons/v_portalgun.mdl')
    preview = args[args.index('--preview') + 1] if '--preview' in args else None
    skip = [args[i + 1] for i, a in enumerate(args) if a == '--skip-bodypart']
    vpks = open_all()
    m = load_model(vpks, path)
    print('bodyparts', m['bodyparts'], 'textures', m['textures'], 'dirs', m['cddirs'])
    tris = [t for t in m['tris'] if t[1] not in skip]
    used = sorted({m['skin'][t[0]] for t in tris})
    print('verts', len(m['verts']), 'tris', len(tris), 'materials used', [m['textures'][u] for u in used])
    # textures -> atlas: slots of S x S in a grid
    S = 512
    cols = 2
    rows = (len(used) + cols - 1) // cols
    AW, AH = S * cols, S * rows
    atlas = bytearray(AW * AH * 4)
    slot = {}
    for k, u in enumerate(used):
        name = m['textures'][u]
        bt = vmt_basetexture(vpks, name, m['cddirs'])
        px = None
        if bt:
            data = find(f'materials/{bt}.vtf', vpks)
            if data:
                w, h, px = decode_vtf(data)
        print(' material', name, '->', bt, f'{w}x{h}' if px else 'MISSING')
        sx, sy = (k % cols) * S, (k // cols) * S
        slot[u] = (sx / AW, sy / AH, S / AW, S / AH)
        for y in range(S):
            for x in range(S):
                if px:
                    r, g, b, a = px[(y * h // S) * w + (x * w // S)]
                else:
                    r, g, b, a = 128, 128, 128, 255
                o = ((sy + y) * AW + sx + x) * 4
                atlas[o:o + 4] = bytes((b, g, r, 255))
    # vertices: Source (x fwd, y left, z up, right-handed) -> Unreal (x fwd, y right, z up): negate y, flip winding
    out_v, out_i, remap = [], [], {}
    for mat, bp, *ids in tris:
        u = m['skin'][mat]
        ox, oy, sw, sh = slot[u]
        tri = []
        for vid in ids:
            key = (vid, u)
            if key not in remap:
                px_, py_, pz_, nx, ny, nz, tu, tv = m['verts'][vid]
                tu, tv = tu % 1.0, tv % 1.0  # wrapped UVs fold into the slot
                remap[key] = len(out_v)
                out_v.append((px_, -py_, pz_, nx, -ny, nz, ox + tu * sw, oy + tv * sh))
            tri.append(remap[key])
        out_i += [tri[0], tri[2], tri[1]]
    os.makedirs(OUT, exist_ok=True)
    with open(f'{OUT}/gun.bin', 'wb') as f:
        f.write(b'OLPG' + struct.pack('<iiii', len(out_v), len(out_i), AW, AH))
        for v in out_v: f.write(struct.pack('<8f', *v))
        f.write(struct.pack(f'<{len(out_i)}H', *out_i))
        f.write(atlas)
    lo = [min(v[k] for v in out_v) for k in range(3)]; hi = [max(v[k] for v in out_v) for k in range(3)]
    print(f'wrote {OUT}/gun.bin: {len(out_v)} verts, {len(out_i)//3} tris, atlas {AW}x{AH}, bounds {lo} {hi}')
    if preview:
        from PIL import Image, ImageDraw
        W = 800; img = Image.new('RGB', (W, W), (40, 40, 40)); d = ImageDraw.Draw(img)
        sc = 0.9 * W / max(hi[0] - lo[0], hi[2] - lo[2])
        P = lambda v: ((v[0] - lo[0]) * sc + 0.05 * W, W - ((v[2] - lo[2]) * sc + 0.05 * W))  # side view: x right, z up
        order = sorted(range(0, len(out_i), 3), key=lambda t: out_v[out_i[t]][1])
        for t in order:
            a, b, c = (out_v[out_i[t + k]] for k in range(3))
            u, v = (a[6] + b[6] + c[6]) / 3, (a[7] + b[7] + c[7]) / 3
            o = (int(v * AH) % AH * AW + int(u * AW) % AW) * 4
            col = (atlas[o + 2], atlas[o + 1], atlas[o])
            d.polygon([P(a), P(b), P(c)], fill=col)
        img.save(preview); print('preview', preview)

SOUNDS = {  # mod sound name -> Portal 2 files (a random one plays)
    'fire0': ['wpn_portal_gun_fire_blue_01', 'wpn_portal_gun_fire_blue_02', 'wpn_portal_gun_fire_blue_03'],
    'fire1': ['wpn_portal_gun_fire_red_01', 'wpn_portal_gun_fire_red_02', 'wpn_portal_gun_fire_red_03'],
    'open0': ['portal_open_blue_01'], 'open1': ['portal_open_red_01', 'portal_open_red_02'],
    'enter': ['portal_enter_01', 'portal_enter_02', 'portal_enter_03'], 'exit': ['portal_exit_01', 'portal_exit_02'],
    'invalid': ['portal_invalid_surface_01', 'portal_invalid_surface_02', 'portal_invalid_surface_03', 'portal_invalid_surface_04'],
    'close': ['portal_close1'],
}

def sounds():
    """Portal 2 sounds -> cache/sounds/<name>_<k>.wav as 44.1 kHz 16-bit stereo (ffmpeg)."""
    import subprocess, tempfile
    vpks = open_all()
    os.makedirs(f'{OUT}/sounds', exist_ok=True)
    for name, files in SOUNDS.items():
        for k, fn in enumerate(files):
            data = find(f'sound/weapons/portalgun/{fn}.wav', vpks)
            if not data: print('missing', fn); continue
            with tempfile.NamedTemporaryFile(suffix='.wav') as t:
                t.write(data); t.flush()
                subprocess.run(['ffmpeg', '-loglevel', 'error', '-y', '-i', t.name, '-ar', '44100', '-ac', '2', '-sample_fmt', 's16',
                                f'{OUT}/sounds/{name}_{k}.wav'], check=True)
    print('sounds ->', f'{OUT}/sounds')

if __name__ == '__main__':
    main()
    sounds()
