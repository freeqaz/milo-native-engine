#!/usr/bin/env python3
"""god2iso — reconstruct a raw XDVDFS .iso from an Xbox 360 Games-on-Demand (GoD/LIVE) container.

Layout (SVOD), verified byte-exact against this RB3 container:
  - block size            = 0x1000
  - raw stream = concat of <contentid>.data/Data0000..NNNN (each a whole # of blocks)
  - structure repeats per "L0 group" (covers up to 0xCC=204 data blocks):
        [L1 master hash block]   (only when group_index % 0xCC == 0; incl. the very first)
        [L0 hash block]
        up to 0xCC data blocks
  - SVOD descriptor in the LIVE header @0x379 gives DataBlockCount (24-bit BE @+0x19).

Usage: god2iso.py <header_file> <out.iso>
  (the .data dir is found as <header_file>.data)
"""
import sys, os, math

BLK = 0x1000
GROUP = 0xCC  # 204 data blocks per L0 hash block; also L0-groups per L1 master

def read_data_block_count(header_path):
    with open(header_path, "rb") as f:
        f.seek(0)
        if f.read(4) != b"LIVE":
            raise SystemExit("not a LIVE/GoD header (magic mismatch)")
        f.seek(0x379)
        desc = f.read(0x24)
    # SVOD volume descriptor: +0x19 = 24-bit big-endian DataBlockCount
    dbc = (desc[0x19] << 16) | (desc[0x1A] << 8) | desc[0x1B]
    return dbc

class StreamReader:
    """Sequential reader over the concatenated Data files."""
    def __init__(self, files):
        self.files = files
        self.idx = 0
        self.fh = open(files[0], "rb")
    def read(self, n):
        out = bytearray()
        while n > 0:
            chunk = self.fh.read(n)
            if not chunk:
                self.idx += 1
                if self.idx >= len(self.files):
                    break
                self.fh.close()
                self.fh = open(self.files[self.idx], "rb")
                continue
            out += chunk
            n -= len(chunk)
        return bytes(out)
    def skip(self, n):
        # seek within current file when possible, else fall through read()
        while n > 0:
            cur = self.fh.tell()
            size = os.fstat(self.fh.fileno()).st_size
            avail = size - cur
            if avail >= n:
                self.fh.seek(n, os.SEEK_CUR); n = 0
            else:
                self.fh.seek(0, os.SEEK_END)
                n -= avail
                self.idx += 1
                if self.idx >= len(self.files):
                    break
                self.fh.close()
                self.fh = open(self.files[self.idx], "rb")

def main():
    header, out_iso = sys.argv[1], sys.argv[2]
    data_dir = header + ".data"
    files = sorted(os.path.join(data_dir, n) for n in os.listdir(data_dir) if n.startswith("Data"))
    dbc = read_data_block_count(header)
    g_count = math.ceil(dbc / GROUP)
    print(f"DataBlockCount={dbc} ({dbc*BLK/1e9:.3f} GB ISO), L0 groups={g_count}, data files={len(files)}")
    r = StreamReader(files)
    remaining = dbc
    written = 0
    GROUP_BYTES = GROUP * BLK
    with open(out_iso, "wb", buffering=1024*1024) as out:
        g = 0
        while remaining > 0:
            if g % GROUP == 0:
                r.skip(BLK)      # L1 master
            r.skip(BLK)          # L0
            n = min(GROUP, remaining)
            data = r.read(n * BLK)
            if len(data) != n * BLK:
                raise SystemExit(f"short read at group {g}: got {len(data)} want {n*BLK}")
            out.write(data)
            written += len(data)
            remaining -= n
            g += 1
            if g % 500 == 0:
                print(f"  group {g}/{g_count}  written {written/1e9:.2f} GB", flush=True)
    print(f"DONE: wrote {written} bytes ({written/1e9:.3f} GB) to {out_iso}")

if __name__ == "__main__":
    main()
