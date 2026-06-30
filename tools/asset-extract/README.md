# asset-extract — offline Xbox 360 disc + milo asset utilities

Reusable, standalone Python utilities for pulling original assets off Harmonix
milo-engine games (Rock Band, Dance Central, …) on Xbox 360. No dependencies
beyond CPython stdlib. Derived + verified byte-exact during the RB3 festival
crowd-movie hunt (see the worked example below).

## `god2iso.py` — Xbox 360 Games-on-Demand (GoD/`LIVE`) → raw XDVDFS `.iso`

A retail 360 disc ripped to GoD ships as a `LIVE`-signed container: a header file
(`<contentid>`) plus a `<contentid>.data/Data0000..NNNN` chunk set. The raw disc
image is interleaved with SVOD hash blocks (one L0 block per 0xCC=204 data blocks,
plus an L1 master block at each group boundary). This script reads the
`DataBlockCount` from the LIVE/SVOD descriptor (24-bit BE @ header `0x379+0x19`),
strips the hash blocks, and reconstructs the raw `.iso`.

```bash
# 1. unzip the GoD container (often flagged a false-positive zip-bomb):
UNZIP_DISABLE_ZIPBOMB_DETECTION=TRUE unzip "Game (RF).zip" -d god/
# 2. GoD -> ISO  (header file = the one WITHOUT a .data suffix; its .data dir sits beside it)
python3 god2iso.py "god/Game/<titleid>/00007000/<contentid>" out.iso
# 3. read the XDVDFS filesystem (handles the XGD video-partition offset):
cargo install xdvdfs-cli        # antangelo/xdvdfs, tested @ 0.8.3
xdvdfs unpack out.iso fs/       # or:  xdvdfs ls out.iso
```
The reconstructed size + the XDVDFS magic landing at ISO `0x10000` are the
byte-exact self-checks.

## `milo_decompress.py` — inflate a milo archive + scan for Bink/paths

Decompresses a milo archive (`0xC?BEDEAF` block format: header at `0x0`, per-block
sizes with a 24-bit length mask + 8-bit flag, zlib/raw blocks) WITHOUT parsing the
`ObjectDir`, then reports embedded Bink container magic (`BIKi`/`KB2*`), `.bik`
path strings, and `TexMovie`/`Movie` class occurrences. Use it to tell whether a
movie is embedded in a milo vs referenced as an external `bink_movie_file`.

```bash
python3 milo_decompress.py venue.milo_xbox [out.bin]
```
Handles RB3-era milo **v28**, which the C# mackiloha `superfreq` can't parse.

## Worked example (RB3, 2026-06-30)

Used to settle whether the RB3 festival jumbotron crowd movies
(`fest{1,2}_mass*.bik`) could be recovered: reconstructed the full retail RB3 360
disc with `god2iso.py` + `xdvdfs`, and found the XDVDFS filesystem is **only the
ARK** (no loose `world/` tree) — `milo_decompress.py` confirmed the festival milos
only *reference* the biks as external paths (zero embedded Bink magic). Conclusion:
those movies were cut from the 360 build. Full write-up in the rb3 repo:
`docs/native/bink-albumart-2026-06-30/disc-extract.md`.
