// A Halloween skull, Plan B: apps/skull/app.contract compiled to C++ by plan2cpp (app_gen.cpp)
// over contract_rt, on the Stopwatch board (contract_rt_m5). It has no extensions and no
// sounds. The screen is turned 180 degrees, so the skull is upright when the board hangs as a
// pendant; agent screenshots read the canvas, so they stay upright. Same agent lines as the Mac
// host, so tools/agent.py drives both.
#include <M5Unified.h>
#include <contract_rt.h>
#include <contract_rt_m5.h>

namespace app {
extern const rt::App kApp;
}

namespace {

constexpr uint8_t kBrightness = 77;  // 30 %
constexpr uint8_t kUpsideDown = 2;   // M5GFX rotation: 180 degrees

rt_m5::Board board;
rt::Host host(app::kApp, board, {});
bool running = false;

}  // namespace

void setup() {
  board.begin(kBrightness);
  M5.Display.setRotation(kUpsideDown);
  running = host.boot();
}

void loop() {
  if (!running) return;
  host.turn();
}
