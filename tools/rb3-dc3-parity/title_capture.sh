#!/bin/bash
# Title-screen screenshots from rb3-native (either GPU flavor).
# usage: title_capture.sh <rb3-native build dir> <outdir> [frames, default 60,200,400]
# env:   RB3_CHECKOUT (run dir, default ~/code/milohax/rb3)
#        RB3_DATA     (default ~/code/milohax/rb3/orig-assets/extracted)
#        MILO_DUMP_RT=1 also writes every live render target beside each shot
#                       (dc3 flavor only)
B=$1; OUT=$(realpath -m "$2"); FR=${3:-60,200,400}
mkdir -p "$OUT"
MAX=$(( $(echo "$FR" | tr ',' '\n' | sort -n | tail -1) + 20 ))
cd "${RB3_CHECKOUT:-$HOME/code/milohax/rb3}" || exit 2
RB3_GAME=1 MILO_HEADLESS=1 MILO_MAX_FRAMES=$MAX MILO_SCREENSHOT_DIR="$OUT" \
  MILO_SCREENSHOT_FRAMES=$FR RB3_DATA="${RB3_DATA:-$HOME/code/milohax/rb3/orig-assets/extracted}" \
  timeout 600 "$B/rb3-native" > "$OUT/run.log" 2>&1
echo "rc=$?"
ls "$OUT"
