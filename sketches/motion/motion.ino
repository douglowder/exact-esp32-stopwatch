// A demo of CSS animations, Plan B: apps/motion/app.contract compiled to C++ by plan2cpp
// (app_gen.cpp) over exact_rt, on the Stopwatch board (exact_rt_m5). The core samples each
// animation every loop turn and repaints only what moved. Same agent lines as the Mac host,
// so tools/agent.py drives both.
#include <exact_rt.h>
#include <exact_rt_m5.h>

namespace app {
extern const rt::App kApp;
}

namespace {

constexpr uint8_t kBrightness = 77;  // 30 %

rt_m5::Board board;
rt::Host host(app::kApp, board, {});
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
