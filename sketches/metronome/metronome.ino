// A minimal metronome, Plan B: apps/metronome/app.contract compiled to C++ by plan2cpp
// (app_gen.cpp) over exact_rt, on the Stopwatch board (exact_rt_m5) with its speaker.
// The app's `sequence` plays on the transport's clock (rt_transport.cpp), and the board's
// voice task starts each click at its time. Same agent lines as the Mac host, so
// tools/agent.py drives both.
#include <exact_rt.h>
#include <exact_rt_m5.h>

namespace app {
extern const rt::App kApp;
}

namespace {

constexpr uint8_t kBrightness = 77;  // 30 %

rt_m5::Board board;
rt_m5::Speaker speaker(app::kApp);
rt::Host host(app::kApp, board, {});
bool running = false;

}  // namespace

void setup() {
  board.begin(kBrightness, speaker);
  running = host.boot();
}

void loop() {
  if (!running) return;
  host.turn();
}
