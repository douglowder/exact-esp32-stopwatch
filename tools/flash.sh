#!/usr/bin/env bash
# Writes an app that tools/build.sh built to the Stopwatch: tools/flash.sh APP PORT
# (PORT is /dev/cu.usbmodem…). It holds /tmp/esptember-m5stopwatch.lock while it writes, so
# no other process uses the board; it stops if another process holds the lock.
set -euo pipefail

FQBN='esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=opi'
LOCK=/tmp/esptember-m5stopwatch.lock

if [ $# -ne 2 ]; then
  echo "usage: tools/flash.sh APP PORT" >&2
  exit 2
fi
app=$1
port=$2
root=$(cd "$(dirname "$0")/.." && pwd)
bin="$root/build/$app/bin"
if [ ! -d "$bin" ]; then
  echo "flash.sh: no build of '$app' in $bin; run tools/build.sh $app first" >&2
  exit 1
fi

if ! (set -o noclobber; echo "contract-stopwatch flash.sh $$" >"$LOCK") 2>/dev/null; then
  echo "flash.sh: $LOCK is held: $(cat "$LOCK")" >&2
  exit 1
fi
trap 'trash "$LOCK"' EXIT

arduino-cli upload -p "$port" --fqbn "$FQBN" --input-dir "$bin" "$root/build/$app/$app"
