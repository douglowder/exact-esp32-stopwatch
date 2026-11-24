#!/usr/bin/env bash
# Builds one sketch for the Stopwatch: tools/build.sh NAME (level, badge, metronome, motion
# or stream). The sketch is sketches/NAME/NAME.ino; where exact-cpp/apps/NAME/cpp/ exists its
# C++ goes beside it, in build/NAME/NAME/. A sketch with no app of its own, such as stream,
# builds alone. The binaries go to build/NAME/bin/ (tools/flash.sh writes them to the board).
set -euo pipefail

FQBN='esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=opi'

if [ $# -ne 1 ]; then
  echo "usage: tools/build.sh APP" >&2
  exit 2
fi
app=$1
root=$(cd "$(dirname "$0")/.." && pwd)
ino="$root/sketches/$app/$app.ino"
cpp="$root/exact-cpp/apps/$app/cpp"
if [ ! -f "$ino" ]; then
  echo "build.sh: no sketch '$app' (need $ino)" >&2
  exit 1
fi
if [ ! -d "$root/exact-cpp/rt" ]; then
  echo "build.sh: is exact-cpp checked out? git submodule update --init --recursive" >&2
  exit 1
fi

sketch="$root/build/$app/$app"
mkdir -p "$sketch"
find "$sketch" -maxdepth 1 -type f -delete
cp "$ino" "$sketch/"
if [ -d "$cpp" ]; then cp "$cpp"/* "$sketch/"; fi

arduino-cli compile --fqbn "$FQBN" \
  --library "$root/exact-cpp/rt" --library "$root/exact-cpp/rt-vm" \
  --library "$root/exact-cpp/stream-proto" --library "$root/rt-m5" \
  --build-path "$root/build/$app/cache" --output-dir "$root/build/$app/bin" "$sketch"
