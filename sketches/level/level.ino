// ESPtember Day 10 Level, Plan B: the app is apps/level/app.contract compiled
// to C++ by plan2cpp (app_gen.cpp) over contract_rt, on the Stopwatch board
// (contract_rt_m5), with the `attitude` extension (level_glue). No Rust runs on
// the device. Same agent lines as the path A firmware and the Mac host, so
// tools/agent.py drives all three.
#include <contract_rt.h>
#include <contract_rt_m5.h>

#include "level_glue.h"

namespace app {
extern const rt::App kApp;
}

namespace {

constexpr uint8_t kBrightness = 77;  // 30 %

rt_m5::Board board;
level::Glue glue;
rt::Host host(app::kApp, board, {&glue});
bool running = false;

}  // namespace

void setup() {
  board.begin(kBrightness);
  running = host.boot();
}

void loop() {
  if (!running) return;
  host.turn();
}
