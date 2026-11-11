# exact-esp32-stopwatch

[Exact](https://github.com/expo/exact) apps on the M5Stack StopWatch (ESP32-S3, 468 × 466 round AMOLED). The apps, `plan2cpp` and the `exact_rt` core are in [exact-cpp](https://github.com/douglowder/exact-cpp), the `exact-cpp/` submodule. This repo has the Stopwatch's board library and a sketch for each app. No Rust runs on the board: each app is compiled to C++ ahead of time, and the generated C++ is committed in exact-cpp.

To run the same apps on a Mac, with no board, see [On the Mac](#on-the-mac).

## Layout

| Path | What it is |
| --- | --- |
| `exact-cpp/` | exact-cpp, as a submodule (it has expo/exact as its own submodule, `exact-cpp/exact/`) |
| `rt-m5/` | The Stopwatch board (`exact_rt_m5`, an Arduino library): an M5GFX canvas, FreeSans, the USB serial link, buttons, touch, IMU, vibration, and the speaker's voice task (`rt_m5_speaker.cpp`, linked only by a sketch with sounds) |
| `sketches/<app>/<app>.ino` | Each app's sketch: `level`, `badge`, `metronome` |
| `tools/build.sh` | Builds an app |
| `tools/flash.sh` | Writes an app to the board, with the board lock |

## Requirements

Tested on macOS 26 on Apple silicon.

| Item | Version | Install |
| --- | --- | --- |
| [arduino-cli](https://arduino.github.io/arduino-cli/) | 1.5.1 | `brew install arduino-cli` |
| Arduino core `esp32:esp32` | 3.3.10 | below |
| M5Unified | 0.2.19 | below |
| M5GFX | 0.2.26 | below |
| Python 3 | standard library only | for the agent tools |
| An M5Stack StopWatch and a USB-C data cable | | |

Install the core and libraries once:

```sh
arduino-cli config init                 # only if you have no arduino-cli config yet
arduino-cli config add board_manager.additional_urls \
  https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32@3.3.10
arduino-cli lib install --no-deps M5Unified@0.2.19
arduino-cli lib install M5GFX@0.2.26
```

`--no-deps` keeps M5Unified from installing the latest M5GFX in place of 0.2.26.

## Get the code

```sh
git clone --recursive https://github.com/douglowder/exact-esp32-stopwatch.git
cd exact-esp32-stopwatch
```

In a clone made without `--recursive`, run `git submodule update --init --recursive`.

## Build

```sh
tools/build.sh level        # or badge, metronome
```

`build.sh` puts the sketch and the app's C++ from `exact-cpp/apps/<app>/cpp/` in `build/<app>/<app>/` and compiles it with `exact-cpp/rt` and `rt-m5` as libraries. The binaries go to `build/<app>/bin/`. The firmware uses the Arduino `app3M_fat9M_16MB` partition layout, with USB CDC on boot.

## Flash

Connect the board and find its port:

```sh
arduino-cli board list      # a /dev/cu.usbmodemNNNN port
tools/flash.sh level /dev/cu.usbmodemNNNN
```

`flash.sh` holds `/tmp/esptember-m5stopwatch.lock` while it writes, and stops if another process holds it. Make sure no other process, such as a serial monitor, has the port open. After an upload the board sometimes stays dark until it is powered off and on. If the upload cannot connect, hold the power button about 2 s until the green LED comes on (download mode), flash, then power the board off and on.

## The apps

| App | What it does |
| --- | --- |
| `level` | A bubble level from the IMU |
| `badge` | The pages of the init() conference badge: the profile, schedule, party and settings |
| `metronome` | A metronome: a tempo, Start and Stop, and an accented beat 1 on the speaker |

The two buttons are keys: BtnA is ArrowLeft, BtnB is ArrowRight, both pressed briefly are Enter, and both held for 600 ms are Escape. The screen takes touch.

The badge reads its profile from a record in the board's flash, in the init() badge firmware's format. `exact-cpp/tools/badge_profile.py` makes such a record from a made-up profile; see its `--help`.

## Driving an app

The agent tools are in `exact-cpp/tools/`. They talk JSON lines over the USB serial port, and open the port without touching DTR or RTS: on the ESP32-S3's USB-Serial/JTAG those lines reset the chip or select download mode. A coding agent can use them to read the app's state, press keys and take screenshots.

```sh
python3 -I exact-cpp/tools/agent.py /dev/cu.usbmodemNNNN state
python3 -I exact-cpp/tools/agent.py /dev/cu.usbmodemNNNN key Enter
python3 -I exact-cpp/tools/agent.py /dev/cu.usbmodemNNNN stats
python3 -I exact-cpp/tools/agent.py /dev/cu.usbmodemNNNN screenshot out.png
```

## On the Mac

exact-cpp builds every app for the Mac, with a window or headless, from the same C++ and the same core. Its README has the steps:

```sh
make -C exact-cpp/mac container      # needs Xcode Command Line Tools and M5GFX
exact-cpp/mac/build/ExactMac level   # a window: arrows, Return, Escape, the mouse as touch
```

## Changing an app

An app's Exact source is `exact-cpp/apps/<app>/app.contract`. Edit it in exact-cpp, generate its C++ there with `plan2cpp` (see exact-cpp's README), commit, and move this repo's `exact-cpp` submodule to that commit. A new app also needs a sketch in `sketches/<app>/`.

## Sizes (2026-11-10, exact-cpp 98c93f6)

| App | Program storage |
| --- | --- |
| level | 878,995 bytes |
| badge | 1,610,683 bytes |
| metronome | 885,899 bytes |

## License

MIT, the same as [Exact](https://github.com/expo/exact). See [LICENSE](LICENSE).
