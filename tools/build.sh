#!/usr/bin/env bash
# Builds one app for the Stopwatch: tools/build.sh APP (level, badge or metronome).
# The sketch is sketches/APP/APP.ino with the app's C++ from core/apps/APP/cpp/ beside it, in
# build/APP/APP/. The binaries go to build/APP/bin/ (tools/flash.sh writes them to the board).
set -euo pipefail

FQBN='esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=opi'

if [ $# -ne 1 ]; then
  echo "usage: tools/build.sh APP" >&2
  exit 2
fi
app=$1
root=$(cd "$(dirname "$0")/.." && pwd)
ino="$root/sketches/$app/$app.ino"
cpp="$root/core/apps/$app/cpp"
if [ ! -f "$ino" ] || [ ! -d "$cpp" ]; then
  echo "build.sh: no app '$app' (need $ino and $cpp; is the core submodule checked out?)" >&2
  exit 1
fi

sketch="$root/build/$app/$app"
mkdir -p "$sketch"
find "$sketch" -maxdepth 1 -type f -delete
cp "$ino" "$cpp"/* "$sketch/"

arduino-cli compile --fqbn "$FQBN" \
  --library "$root/core/rt" --library "$root/rt-m5" \
  --build-path "$root/build/$app/cache" --output-dir "$root/build/$app/bin" "$sketch"
