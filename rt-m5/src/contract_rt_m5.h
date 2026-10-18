// The M5Stack Stopwatch (ESP32-S3) as a contract_rt board: an M5GFX canvas in
// PSRAM pushed to the 468 x 466 CO5300 panel, FreeSans faces, USB serial,
// BtnA/BtnB through the chord grammar, CST820 touch, the BMI270
// accelerometer, the vibration motor, and, for an app with sounds, the speaker.
#pragma once

#include <M5Unified.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <deque>

#include "buttons.h"
#include "contract_rt.h"

namespace rt_m5 {

class Display final : public rt::Display {
 public:
  Display();
  bool begin() override;
  int width() const override;
  int height() const override;
  int band_rows() const override;
  void begin_area(rt::Rect area) override;
  void push_area() override;
  void fill_rect(rt::Rect r, uint32_t rgb) override;
  void fill_round_rect(rt::Rect r, int radius, uint32_t rgb) override;
  void draw_round_rect(rt::Rect r, int radius, uint32_t rgb) override;
  void fill_circle(int cx, int cy, int radius, uint32_t rgb) override;
  void draw_circle(int cx, int cy, int radius, uint32_t rgb) override;
  int face(float px, int weight) const override;
  int text_width(int face, const char* text) const override;
  int line_height(int face) const override;
  void draw_text(int face, const char* text, int x, int y, uint32_t rgb) override;
  const uint8_t* frame(uint32_t* bytes) const override;
  uint16_t* row(int y) override;
  bool big_endian() const override { return true; }

 private:
  mutable M5Canvas canvas_;
  rt::Rect area_{0, 0, 0, 0};
};

class Link final : public rt::Link {
 public:
  void begin();
  int read() override;
  void write_line(const std::string& line, bool bulk) override;
};

// The BMI270.
class Accelerometer final : public rt::Accelerometer {
 public:
  bool read(float& x, float& y, float& z) override;
};

// The ES8311 codec and speaker amplifier, through M5Unified's mixer. A FreeRTOS task starts
// each voice at its time, so drawing a frame never delays a click; each voice takes the next
// of the mixer's channels, at its gain. A sketch whose app has sounds makes one and gives it
// to `Board::begin`; one without does not link it.
class Speaker final : public rt::Speaker {
 public:
  explicit Speaker(const rt::App& app) : app_(&app) {}
  // Starts the voice task, once M5Unified has started the speaker.
  void start();
  void play(uint32_t id, uint16_t sound, int64_t at_us, float gain) override;
  void cancel_from(int64_t at_us) override;

 private:
  struct Voice {
    int64_t at_us;
    uint16_t sound;
    float gain;
  };
  static void task(void* self);
  void run();

  const rt::App* app_;
  SemaphoreHandle_t lock_ = nullptr;
  TaskHandle_t task_ = nullptr;
  std::deque<Voice> queue_;  // in start order
  uint8_t channel_ = 0;
};

class Board final : public rt::Board {
 public:
  // Starts M5Unified for the Stopwatch, the panel, and the serial link. `brightness` is
  // 0–255. The speaker stays off, and its driver is not linked.
  void begin(uint8_t brightness);
  // The same, with the codec and amplifier on and `speaker`'s voice task started
  // (rt_m5_speaker.cpp, linked only by a sketch that calls it).
  void begin(uint8_t brightness, Speaker& speaker);
  rt::Speaker* speaker() override { return speaker_; }
  int64_t micros() const override;
  rt::Display* display() override { return &display_; }
  rt::Link* link() override { return &link_; }
  rt::Accelerometer* accelerometer() override { return &accel_; }
  rt::Inputs inputs() const override { return {true, true}; }
  bool input(rt::Input& out) override;
  std::vector<rt::Counter> counters() const override;
  bool can(const std::string& name) const override { return name == "haptic"; }
  void command(const std::string& name, const std::vector<rt::V>& args) override;
  void step(uint32_t now_ms) override;
  void idle() override;
  // The RX8130's time (kept in UTC, as the badge firmware keeps it) and the
  // local offset the badge firmware saved (NVS `conf-clock`/`offset`, minutes
  // east of UTC); false if the clock was never set (before 2024 or past 2099).
  bool wall_clock(int64_t& epoch_ms, int& utc_offset) const override;

 private:
  // M5Unified's configuration for the Stopwatch. Each `begin` passes `speaker` as a constant,
  // so the speaker's setup is compiled only where it is on.
  static m5::M5Unified::config_t config(bool speaker) {
    auto cfg = M5.config();
    cfg.internal_imu = true;
    cfg.internal_rtc = true;
    cfg.internal_mic = false;
    cfg.internal_spk = speaker;
    cfg.fallback_board = m5::board_t::board_M5StopWatch;
    return cfg;
  }
  // What both `begin`s do once M5Unified has started: the panel and the saved clock offset.
  void start_panel(uint8_t brightness);
  void poll();

  Display display_;
  Link link_;
  Accelerometer accel_;
  Speaker* speaker_ = nullptr;
  c25k::ButtonGrammar grammar_;
  std::deque<rt::Input> inputs_;
  bool polled_ = false;
  bool touching_ = false;
  int touch_x_ = 0, touch_y_ = 0;
  uint32_t haptic_until_ = 0;
  int utc_offset_ = 0;
};

}  // namespace rt_m5
