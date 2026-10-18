// The Stopwatch board. Display: one PSRAM canvas the size of the panel, so an
// area is drawn in place and pushed through a clip. Faces: FreeSans 9/12/18/24
// pt (about CSS 18/24/36/48 px), bold from weight 600.
#include "contract_rt_m5.h"

#include <Preferences.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>

#include <cmath>

namespace rt_m5 {

namespace {

constexpr int kWidth = 468;
constexpr int kHeight = 466;
constexpr uint32_t kHapticMs = 40;
constexpr uint32_t kBulkTimeoutMs = 2000;

struct Face {
  const lgfx::IFont* regular;
  const lgfx::IFont* bold;
  float px;
};

// FreeSans N pt has about the cap height of CSS text at 2N px.
const Face kFaces[] = {
    {&fonts::FreeSans9pt7b, &fonts::FreeSansBold9pt7b, 18},
    {&fonts::FreeSans12pt7b, &fonts::FreeSansBold12pt7b, 24},
    {&fonts::FreeSans18pt7b, &fonts::FreeSansBold18pt7b, 36},
    {&fonts::FreeSans24pt7b, &fonts::FreeSansBold24pt7b, 48},
};

// Face ids: 2 * size index + 1 for bold.
const lgfx::IFont* font(int face) {
  const Face& f = kFaces[face / 2];
  return face % 2 ? f.bold : f.regular;
}

const char* key_of(c25k::Input in) {
  switch (in) {
    case c25k::Input::Prev: return "ArrowLeft";
    case c25k::Input::Next: return "ArrowRight";
    case c25k::Input::Enter: return "Enter";
    case c25k::Input::Escape: return "Escape";
    case c25k::Input::None: return nullptr;
  }
  return nullptr;
}

}  // namespace

Display::Display() : canvas_(&M5.Display) {}

bool Display::begin() {
  canvas_.setPsram(true);
  canvas_.setColorDepth(16);
  if (canvas_.createSprite(kWidth, kHeight) == nullptr) return false;
  canvas_.setTextDatum(textdatum_t::top_left);
  return true;
}

int Display::width() const { return kWidth; }
int Display::height() const { return kHeight; }
int Display::band_rows() const { return kHeight; }

void Display::begin_area(rt::Rect area) {
  area_ = area;
  canvas_.setClipRect(area.x, area.y, area.w, area.h);
}

void Display::push_area() {
  canvas_.clearClipRect();
  M5.Display.setClipRect(area_.x, area_.y, area_.w, area_.h);
  canvas_.pushSprite(0, 0);
  M5.Display.clearClipRect();
}

void Display::fill_rect(rt::Rect r, uint32_t rgb) { canvas_.fillRect(r.x, r.y, r.w, r.h, rgb); }

void Display::fill_round_rect(rt::Rect r, int radius, uint32_t rgb) {
  canvas_.fillRoundRect(r.x, r.y, r.w, r.h, radius, rgb);
}

void Display::draw_round_rect(rt::Rect r, int radius, uint32_t rgb) {
  canvas_.drawRoundRect(r.x, r.y, r.w, r.h, radius, rgb);
}

void Display::fill_circle(int cx, int cy, int radius, uint32_t rgb) { canvas_.fillCircle(cx, cy, radius, rgb); }

void Display::draw_circle(int cx, int cy, int radius, uint32_t rgb) { canvas_.drawCircle(cx, cy, radius, rgb); }

int Display::face(float px, int weight) const {
  int best = 0;
  for (int i = 1; i < 4; ++i) {
    if (std::fabs(kFaces[i].px - px) < std::fabs(kFaces[best].px - px)) best = i;
  }
  return best * 2 + (weight >= 600 ? 1 : 0);
}

int Display::text_width(int face, const char* text) const {
  canvas_.setFont(font(face));
  return canvas_.textWidth(text);
}

int Display::line_height(int face) const {
  canvas_.setFont(font(face));
  return canvas_.fontHeight();
}

void Display::draw_text(int face, const char* text, int x, int y, uint32_t rgb) {
  canvas_.setFont(font(face));
  canvas_.setTextColor(rgb);
  canvas_.drawString(text, x, y);
}

const uint8_t* Display::frame(uint32_t* bytes) const {
  if (bytes) *bytes = kWidth * kHeight * 2;
  return static_cast<const uint8_t*>(canvas_.getBuffer());
}

// The canvas holds the whole panel, so any row of the area is in it.
uint16_t* Display::row(int y) {
  if (y < 0 || y >= kHeight) return nullptr;
  return static_cast<uint16_t*>(canvas_.getBuffer()) + static_cast<size_t>(y) * kWidth;
}

void Link::begin() {
  Serial.setTxBufferSize(8192);
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
}

int Link::read() { return Serial.available() > 0 ? Serial.read() : -1; }

void Link::write_line(const std::string& line, bool bulk) {
  if (!bulk) {
    Serial.println(line.c_str());
    return;
  }
  Serial.setTxTimeoutMs(kBulkTimeoutMs);
  Serial.println(line.c_str());
  Serial.setTxTimeoutMs(0);
  delay(1);
}

void Board::begin(uint8_t brightness) {
  link_.begin();
  M5.begin(config(false));
  start_panel(brightness);
}

void Board::start_panel(uint8_t brightness) {
  auto* panel = M5.Display.getPanel();
  auto pc = panel->config();
  pc.panel_width = kWidth;
  pc.panel_height = kHeight;
  pc.offset_x = 6;
  pc.offset_y = 0;
  panel->config(pc);
  M5.Display.setRotation(0);
  M5.Display.setBrightness(brightness);
  Preferences clock;
  if (clock.begin("conf-clock", true)) {
    const int32_t offset = clock.getInt("offset", 0);
    utc_offset_ = offset >= -840 && offset <= 840 ? offset : 0;
    clock.end();
  }
}

int64_t Board::micros() const { return esp_timer_get_time(); }

std::vector<rt::Counter> Board::counters() const {
  return {
      {"internal_free", static_cast<int64_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL))},
      {"internal_min", static_cast<int64_t>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL))},
      {"psram_free", static_cast<int64_t>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM))},
      // The least stack the loop task has had free, in bytes.
      {"loop_stack_free", static_cast<int64_t>(uxTaskGetStackHighWaterMark(nullptr))},
  };
}

// The buttons and touch, read once per loop turn: a chord becomes a key, and
// the contact's phases go to the core, which tells taps from pans.
void Board::poll() {
  M5.update();
  const char* key = key_of(grammar_.update(millis(), M5.BtnA.isPressed(), M5.BtnB.isPressed(),
                                           M5.BtnA.wasClicked(), M5.BtnB.wasClicked()));
  if (key) {
    rt::Input in;
    in.k = rt::Input::K::Key;
    in.key = key;
    inputs_.push_back(in);
  }
  lgfx::touch_point_t tp;
  rt::Input in;
  if (M5.Display.getTouch(&tp) > 0) {
    in.k = touching_ ? rt::Input::K::Move : rt::Input::K::Down;
    touching_ = true;
    touch_x_ = in.x = tp.x;
    touch_y_ = in.y = tp.y;
    inputs_.push_back(in);
  } else if (touching_) {
    touching_ = false;
    in.k = rt::Input::K::Up;
    in.x = touch_x_;
    in.y = touch_y_;
    inputs_.push_back(in);
  }
}

bool Board::input(rt::Input& out) {
  if (!polled_) {
    poll();
    polled_ = true;
  }
  if (inputs_.empty()) {
    polled_ = false;
    return false;
  }
  out = inputs_.front();
  inputs_.pop_front();
  return true;
}

bool Accelerometer::read(float& x, float& y, float& z) {
  if (!M5.Imu.update()) return false;
  M5.Imu.getAccel(&x, &y, &z);
  return true;
}

bool Board::wall_clock(int64_t& epoch_ms, int& utc_offset) const {
  if (!M5.Rtc.isEnabled()) return false;
  m5::rtc_datetime_t t;
  if (!M5.Rtc.getDateTime(&t)) return false;
  const int y = t.date.year, mo = t.date.month, d = t.date.date;
  if (y < 2024 || y > 2099 || mo < 1 || mo > 12 || d < 1 || d > 31) return false;
  // Days from 1970-01-01 to the date (the proleptic Gregorian calendar).
  const int yy = mo <= 2 ? y - 1 : y;
  const int era = yy / 400, yoe = yy - era * 400;
  const int doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const int64_t days = era * 146097LL + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
  epoch_ms = ((days * 24 + t.time.hours) * 60 + t.time.minutes) * 60000LL + t.time.seconds * 1000LL;
  utc_offset = utc_offset_;
  return true;
}

void Board::command(const std::string& name, const std::vector<rt::V>& args) {
  (void)args;
  if (name == "haptic") {
    M5.Power.setVibration(255);
    haptic_until_ = millis() + kHapticMs;
  }
}

void Board::step(uint32_t now_ms) {
  if (haptic_until_ && now_ms >= haptic_until_) {
    M5.Power.setVibration(0);
    haptic_until_ = 0;
  }
}

void Board::idle() { delay(2); }

}  // namespace rt_m5
