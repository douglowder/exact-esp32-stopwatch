// The init() badge, Plan B: the app is apps/badge/app.contract compiled to
// C++ by plan2cpp (app_gen.cpp) over exact_rt, on the Stopwatch board
// (exact_rt_m5). BtnA and BtnB arrive as ArrowLeft and ArrowRight,
// holding both as Escape; touch arrives as taps and pans.
//
// The app's `init-badge` extension (badge_data) answers from what the badge
// firmware saved on this board, read only: the profile record on the `ffat`
// partition (LittleFS, /conference-manual-v1.bin), and the settings in NVS
// (`conference_ui`: `prefs`, `after_dark_v1`). It reads the battery once a
// second (M5PM1, 3.3–4.2 V as 0–100 %, as the badge firmware does) and sets
// the panel's brightness when the app sends `setBrightness`. Nothing here
// writes to the board's storage.
#include <LittleFS.h>
#include <Preferences.h>
#include <exact_rt.h>
#include <exact_rt_m5.h>
#include <esp_heap_caps.h>

#include "badge_data.h"

namespace app {
extern const rt::App kApp;
}

// The app's view nests deeply (route rows, page arms, component arms), and
// an agent op reaches it through the host and the action: Arduino's 8 KB
// loop stack overflowed on a pan. `stats` reports `loop_stack_free`.
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

namespace {

constexpr uint32_t kBatteryMs = 1000;
constexpr size_t kMaxRecord = 64 + 768 + 160 * 160 * 2;

rt_m5::Board board;
badge::Data data;
rt::Host host(app::kApp, board, {&data});
bool running = false;
uint32_t battery_at = 0;
int32_t battery_mv = 0;

uint8_t panel_level(int percent) { return static_cast<uint8_t>(percent * 255 / 100); }

badge::Saved read_settings() {
  Preferences prefs;
  if (!prefs.begin("conference_ui", true)) return badge::Saved{};
  const badge::Saved saved = badge::saved_from(prefs.getUInt("prefs", 0), prefs.getUChar("after_dark_v1", 0xa0));
  prefs.end();
  return saved;
}

// The profile record, if the badge firmware saved one. The partition is
// mounted without formatting and unmounted after the read.
void read_profile() {
  if (!LittleFS.begin(false, "/badgefs", 2, "ffat")) return;
  File f = LittleFS.open("/conference-manual-v1.bin", "r");
  if (f && f.size() <= kMaxRecord) {
    std::vector<uint8_t> bytes(f.size());
    const size_t got = f.read(bytes.data(), bytes.size());
    badge::Profile profile;
    std::string error;
    if (got == bytes.size() && badge::read_profile(bytes.data(), bytes.size(), profile, error)) {
      data.set_profile(profile);
    }
  }
  if (f) f.close();
  LittleFS.end();
}

// The battery, smoothed as the badge firmware smooths it; -1 if it cannot
// be read.
int read_battery() {
  const int32_t mv = M5.Power.getBatteryVoltage();
  if (mv <= 0) return -1;
  battery_mv = battery_mv ? (battery_mv * 7 + mv + 4) / 8 : mv;
  const int percent = static_cast<int>((battery_mv - 3300) * 100 / 900);
  return percent < 0 ? 0 : percent > 100 ? 100 : percent;
}

}  // namespace

void setup() {
  const badge::Saved saved = read_settings();
  board.begin(panel_level(saved.brightness));
  data.set_saved(saved);
  read_profile();
  data.set_battery(read_battery());
  data.on_brightness = [](int percent) { M5.Display.setBrightness(panel_level(percent)); };
  running = host.boot();
}

void loop() {
  if (!running) return;
  const uint32_t now = millis();
  if (now - battery_at >= kBatteryMs) {
    battery_at = now;
    data.set_battery(read_battery());
  }
  host.turn();
}
