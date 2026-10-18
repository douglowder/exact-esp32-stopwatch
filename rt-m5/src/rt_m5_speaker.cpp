// The Stopwatch's speaker for the transport's voices (rt_transport.cpp). The transport issues
// each voice up to a second ahead; this task sleeps until the first one is due and hands it
// to M5Unified's mixer (`M5.Speaker.playRaw`), which plays it from DMA. A voice cancelled
// before its time (an edit, a stop) leaves the queue and never sounds.
#include <esp_timer.h>

#include <algorithm>

#include "contract_rt_m5.h"

namespace rt_m5 {

namespace {

constexpr uint8_t kVolume = 128;    // the mixer's master volume, 0–255
constexpr int kChannels = 8;        // M5Unified's mixer channels
constexpr int64_t kEarlyUs = 500;   // a voice this close to its time starts now
constexpr uint32_t kIdleMs = 20;    // the longest sleep with nothing due sooner

}  // namespace

void Board::begin(uint8_t brightness, Speaker& speaker) {
  link_.begin();
  M5.begin(config(true));
  start_panel(brightness);
  speaker_ = &speaker;
  speaker.start();
}

void Speaker::start() {
  lock_ = xSemaphoreCreateMutex();
  M5.Speaker.setVolume(kVolume);
  xTaskCreatePinnedToCore(task, "voices", 4096, this, 3, &task_, 0);
}

void Speaker::play(uint32_t /*id*/, uint16_t sound, int64_t at_us, float gain) {
  xSemaphoreTake(lock_, portMAX_DELAY);
  const auto at = std::upper_bound(queue_.begin(), queue_.end(), at_us,
                                   [](int64_t t, const Voice& v) { return t < v.at_us; });
  queue_.insert(at, {at_us, sound, gain});
  xSemaphoreGive(lock_);
  xTaskNotifyGive(task_);
}

void Speaker::cancel_from(int64_t at_us) {
  xSemaphoreTake(lock_, portMAX_DELAY);
  while (!queue_.empty() && queue_.back().at_us >= at_us) queue_.pop_back();
  xSemaphoreGive(lock_);
}

void Speaker::task(void* self) { static_cast<Speaker*>(self)->run(); }

void Speaker::run() {
  for (;;) {
    xSemaphoreTake(lock_, portMAX_DELAY);
    uint32_t sleep_ms = kIdleMs;
    bool due = false;
    Voice v{};
    if (!queue_.empty()) {
      const int64_t wait_us = queue_.front().at_us - esp_timer_get_time();
      if (wait_us <= kEarlyUs) {
        v = queue_.front();
        queue_.pop_front();
        due = true;
      } else {
        sleep_ms = static_cast<uint32_t>(std::min<int64_t>(kIdleMs, wait_us / 1000));
      }
    }
    xSemaphoreGive(lock_);
    if (due) {
      const rt::Sound& s = app_->sounds[v.sound];
      M5.Speaker.setChannelVolume(channel_, static_cast<uint8_t>(v.gain * 255.0f));
      M5.Speaker.playRaw(s.samples, s.frames, s.rate, false, 1, channel_, false);
      channel_ = static_cast<uint8_t>((channel_ + 1) % kChannels);
      continue;
    }
    // A new voice, which may be due sooner, wakes the task early.
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(std::max<uint32_t>(1, sleep_ms)));
  }
}

}  // namespace rt_m5
