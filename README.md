# contract-stopwatch

Contract apps on the M5Stack Stopwatch (ESP32-S3). The apps, `plan2cpp` and the `contract_rt`
core are in contract-cpp, the `core/` submodule. This repo has the Stopwatch's board and a
sketch for each app.

## Layout

| Path | What it is |
| --- | --- |
| `core/` | contract-cpp, as a submodule (it has exact2 as its own submodule) |
| `rt-m5/` | The Stopwatch board (`contract_rt_m5`, an Arduino library): an M5GFX canvas, FreeSans, the USB serial link, buttons, touch, IMU, vibration, and the speaker's voice task (`rt_m5_speaker.cpp`, linked only by a sketch with sounds) |
| `sketches/<app>/<app>.ino` | Each app's sketch: `level`, `badge`, `metronome` |
| `tools/build.sh` | Builds an app |
| `tools/flash.sh` | Writes an app to the board, with the board lock |

## Build and flash

```sh
git submodule update --init --recursive
tools/build.sh level                            # build/level/bin/
tools/flash.sh level /dev/cu.usbmodemNNNN
```

`build.sh` puts the sketch and the app's C++ from `core/apps/<app>/cpp/` in `build/<app>/<app>/`
and compiles it with `core/rt` and `rt-m5` as libraries. It needs `arduino-cli` with the
`esp32:esp32` core, M5Unified and M5GFX. To change an app, edit it in contract-cpp, generate its
C++ there, and move the `core` submodule.

`flash.sh` holds `/tmp/esptember-m5stopwatch.lock` while it writes, and stops if another
process holds it. Make sure no other process has the port open. After an upload the board
sometimes stays dark until it is powered off and on. The firmware uses the Arduino
`app3M_fat9M_16MB` partition layout.

## Driving the app

The agent tools are in `core/tools/`. They open the port without touching DTR/RTS; on the
ESP32-S3's USB-Serial/JTAG those lines reset the chip or select download mode.

```sh
python3 -I core/tools/agent.py /dev/cu.usbmodemNNNN state
python3 -I core/tools/agent.py /dev/cu.usbmodemNNNN key Enter
python3 -I core/tools/agent.py /dev/cu.usbmodemNNNN stats
python3 -I core/tools/agent.py /dev/cu.usbmodemNNNN screenshot out.png
```

## Sizes (2026-10-18, core cf6b52b)

| App | Program storage |
| --- | --- |
| level | 878,247 bytes |
| badge | 1,609,335 bytes |
| metronome | 885,155 bytes |
