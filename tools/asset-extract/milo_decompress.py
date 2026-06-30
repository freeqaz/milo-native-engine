#!/usr/bin/env python3
"""Decompress a milo archive (0xC?BEDEAF block format) and search the inflated
bytes for embedded Bink magic + .bik path strings. Does NOT parse the ObjectDir."""
import sys, struct, zlib, re

def decompress(path):
    with open(path, "rb") as f:
        data = f.read()
    magic = struct.unpack_from("<I", data, 0)[0]
    block_offset = struct.unpack_from("<I", data, 4)[0]
    num_blocks = struct.unpack_from("<I", data, 8)[0]
    max_block = struct.unpack_from("<I", data, 0xC)[0]
    print(f"magic=0x{magic:08X} block_offset=0x{block_offset:X} num_blocks={num_blocks} max_block=0x{max_block:X}")
    sizes = []
    for i in range(num_blocks):
        sizes.append(struct.unpack_from("<I", data, 0x10 + i*4)[0])
    out = bytearray()
    pos = block_offset
    inflated_blocks = 0
    raw_blocks = 0
    for s in sizes:
        masked = s & 0x00FFFFFF
        flag = s & 0xFF000000
        chunk = data[pos:pos+masked]
        pos += masked
        # try zlib (handle the gzip-ish variant: skip optional 2-byte len prefix)
        decoded = None
        for attempt in (chunk, chunk[2:] if len(chunk) > 2 else chunk):
            for wb in (zlib.MAX_WBITS, -zlib.MAX_WBITS):
                try:
                    decoded = zlib.decompress(attempt, wb)
                    break
                except Exception:
                    decoded = None
            if decoded is not None:
                break
        if decoded is None:
            out += chunk
            raw_blocks += 1
        else:
            out += decoded
            inflated_blocks += 1
    print(f"inflated={inflated_blocks} raw/stored={raw_blocks} total_decompressed={len(out)} bytes ({len(out)/1e6:.1f} MB)")
    return bytes(out)

def main():
    path = sys.argv[1]
    blob = decompress(path)
    # search for Bink magic
    biks = {}
    for m in (b"BIKi", b"BIKb", b"BIKg", b"BIKh", b"BIKf", b"KB2a", b"KB2d", b"KB2f", b"KB2g", b"KB2i", b"KB2j"):
        c = blob.count(m)
        if c:
            biks[m.decode()] = c
    print("Bink container magic counts:", biks if biks else "NONE")
    # .bik path strings
    bik_paths = sorted(set(re.findall(rb'[ -~]{3,}\.bik', blob)))
    print(f".bik path strings ({len(bik_paths)}):")
    for p in bik_paths[:40]:
        print("   ", p.decode(errors="replace"))
    # TexMovie / Movie object class names present?
    for cls in (b"TexMovie", b"Movie", b"BinkMovie", b"Bink"):
        print(f"  class '{cls.decode()}' occurrences: {blob.count(cls)}")
    if len(sys.argv) > 2:
        with open(sys.argv[2], "wb") as o:
            o.write(blob)
        print("wrote decompressed blob to", sys.argv[2])

if __name__ == "__main__":
    main()
