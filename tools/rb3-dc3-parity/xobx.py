"""Parse RB3's Xbox 360 shader container `xbox_shaders` (XOBX v1), in the
layout DxShaderMgr::LoadShaderFile reads, and dump one shader type's
microcode per permutation (file names carry the 64-bit permutation mask).

usage: xobx.py <xbox_shaders> [list | dump <type> <outdir>]

The container ships in the game's root, e.g.
  rb3/orig-assets/extracted-xbox-full/(.)/xbox_shaders
Disassemble a dumped permutation with xenia's shader compiler:
  xenia-gpu-shader-compiler --shader_input=<f>.ps --shader_input_type=ps \
      --shader_output=<f>.txt --shader_output_type=ucode
Used to port RB3's post chain (gfx/RB3RetailPost); see
docs/native/dc3-backend-for-rb3-wii.md section 8."""
import struct, sys, os
d = open(sys.argv[1], 'rb').read()
o = 0
def u32():
    global o; v = struct.unpack_from('<I', d, o)[0]; o += 4; return v
def u64():
    global o; v = struct.unpack_from('<Q', d, o)[0]; o += 8; return v
magic = d[0:4]; o = 4
ver = u32(); num = u32()
types = []
for i in range(num):
    n = u32(); name = d[o:o+n].decode(); o += n
    alloc = u32()
    pools = []
    for j in range(2):
        s1 = u32(); s2 = u32()
        b = d[o:o+s1]; o += s1
        p = d[o:o+s2]; o += s2
        pools.append((b, p))
    recs = []
    for k in range(alloc):
        mask = u64()
        offs = []
        for kk in range(2):
            ic0 = u32(); ibc = u32(); offs.append((ic0, ibc))
        recs.append((mask, offs))
    types.append((name, pools, recs))
mode = sys.argv[2] if len(sys.argv) > 2 else 'list'
if mode == 'list':
    for name, pools, recs in types:
        print(f"{name:28s} perms={len(recs):4d} ps_hdr={len(pools[0][0])} ps_ucode={len(pools[0][1])} vs_hdr={len(pools[1][0])} vs_ucode={len(pools[1][1])}")
elif mode == 'dump':
    want = sys.argv[3]; out = sys.argv[4]; os.makedirs(out, exist_ok=True)
    for name, pools, recs in types:
        if name != want: continue
        for which, ext in ((0, 'ps'), (1, 'vs')):
            hdr, phys = pools[which]
            starts = sorted(set(r[1][which][1] for r in recs)) + [len(phys)]
            for idx, (mask, offs) in enumerate(recs):
                ic0, ibc = offs[which]
                end = starts[starts.index(ibc) + 1]
                open(os.path.join(out, f"{name}_{mask:016x}.{ext}"), 'wb').write(phys[ibc:end])
                open(os.path.join(out, f"{name}_{mask:016x}.{ext}.hdr"), 'wb').write(hdr[ic0:ic0+64])
        print("dumped", len(recs), "perms")
