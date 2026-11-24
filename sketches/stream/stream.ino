// Runs Exact apps on the Stopwatch with no app compiled in, in either of the
// two ways exact-cpp's stream/ host offers:
//
//   Thin client - the app runs on the Mac. Each AREA packet is decoded into the
//   rt_m5 canvas and pushed to the panel; BtnA, BtnB and touch go back as
//   INPUT packets.
//
//   Plan mode - the host sends the app itself as a plan blob. It is read into
//   PSRAM, each CodeFn becomes a trampoline into the rt_vm interpreter, and
//   rt::Host runs it here on the rt_m5 board, with the board's own buttons and
//   touch. A new app needs no flash. The agent line is tunnelled through AGENT
//   and LOG packets, since the stream protocol has the serial port.
//
// An AREA arriving while a plan runs ends the plan, as the protocol says.
//
// A payload is read whole into PSRAM and decoded once its CRC checks, rather
// than decoded as it arrives: a bad packet can then never reach the panel.
#include <esp_rom_crc.h>
#include <esp_timer.h>

#include <deque>
#include <memory>
#include <string>
#include <vector>

#include <exact_rt.h>
#include <exact_rt_m5.h>
#include <exact_stream_proto.h>
#include <plan_blob.h>

SET_LOOP_TASK_STACK_SIZE(32 * 1024);

namespace {

constexpr int kW = 468, kH = 466;
constexpr uint8_t kBrightness = 77;  // 30 %, as the app sketches use
// A full-screen AREA is 468 * 466 * 2 bytes of pixels plus its header; ops
// never encode larger than raw, so this holds any area the host can send.
constexpr uint32_t kMaxPayload = uint32_t(kW) * kH * 2 + XS_AREA_HEADER;

rt_m5::Board board;
uint8_t* payload = nullptr;
// Inputs go nowhere until a host says HELLO: with none attached they would
// otherwise fill the serial link's 8 KB send buffer and arrive, stale, ahead
// of the INFO that answers its first HELLO.
bool greeted = false;
uint32_t last_byte_ms = 0;

void put16(uint8_t* p, uint16_t v) {
  p[0] = uint8_t(v);
  p[1] = uint8_t(v >> 8);
}

void put32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v);
  p[1] = uint8_t(v >> 8);
  p[2] = uint8_t(v >> 16);
  p[3] = uint8_t(v >> 24);
}

uint16_t get16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }

// rt_m5::Link leaves Serial with no send timeout, so a write drops whatever the
// buffer cannot take. The header has already promised `len` bytes, so a dropped
// tail leaves the host waiting for a packet that never finishes and deaf to
// every packet after it. A big answer, such as a screenshot, therefore waits
// for the buffer to drain instead.
constexpr uint32_t kBulkBytes = 1024;
constexpr uint32_t kBulkTimeoutMs = 2000;

void send_packet(uint8_t type, const uint8_t* body, uint32_t len) {
  uint8_t head[6] = {XS_MAGIC0, XS_MAGIC1, type, uint8_t(len), uint8_t(len >> 8), uint8_t(len >> 16)};
  uint32_t crc = esp_rom_crc32_le(0, head + 2, 4);
  if (len) crc = esp_rom_crc32_le(crc, body, len);
  uint8_t tail[4];
  put32(tail, crc);
  const bool bulk = len >= kBulkBytes;
  if (bulk) Serial.setTxTimeoutMs(kBulkTimeoutMs);
  Serial.write(head, 6);
  if (len) Serial.write(body, len);
  Serial.write(tail, 4);
  if (bulk) Serial.setTxTimeoutMs(0);
}

void send_info() {
  uint8_t body[8];
  put16(body, kW);
  put16(body + 2, kH);
  put16(body + 4, XS_VERSION);
  put16(body + 6, 0);
  send_packet(XS_INFO, body, sizeof(body));
}

void send_ack(uint16_t seq, bool ok, uint32_t decode_us, uint32_t push_us) {
  uint8_t body[11];
  put16(body, seq);
  body[2] = ok ? 1 : 0;
  put32(body + 3, decode_us);
  put32(body + 7, push_us);
  send_packet(XS_ACK, body, sizeof(body));
}

uint8_t key_code(const char* key) {
  if (!key) return 0;
  if (!strcmp(key, "ArrowLeft")) return XS_KEY_LEFT;
  if (!strcmp(key, "ArrowRight")) return XS_KEY_RIGHT;
  if (!strcmp(key, "Enter")) return XS_KEY_ENTER;
  if (!strcmp(key, "Escape")) return XS_KEY_ESCAPE;
  return 0;
}

void send_input(const rt::Input& in) {
  uint8_t body[6];
  uint8_t kind = XS_IN_KEY;
  if (in.k == rt::Input::K::Down) kind = XS_IN_DOWN;
  if (in.k == rt::Input::K::Move) kind = XS_IN_MOVE;
  if (in.k == rt::Input::K::Up) kind = XS_IN_UP;
  body[0] = kind;
  put16(body + 1, uint16_t(int16_t(in.x)));
  put16(body + 3, uint16_t(int16_t(in.y)));
  body[5] = kind == XS_IN_KEY ? key_code(in.key) : 0;
  if (kind == XS_IN_KEY && body[5] == 0) return;  // a key the host has no code for
  send_packet(XS_INPUT, body, sizeof(body));
}

// Writes one AREA's pixels into the canvas. False if its ops run past the
// rectangle or its payload ends early, which leaves the canvas as it was
// outside the rows already written and makes the host send a keyframe.
bool apply_area(const uint8_t* body, uint32_t len) {
  const int x = get16(body), y = get16(body + 2), w = get16(body + 4), h = get16(body + 6);
  const uint8_t enc = body[10];
  if (w <= 0 || h <= 0 || x < 0 || y < 0 || x + w > kW || y + h > kH) return false;
  rt::Display* display = board.display();
  const uint8_t* p = body + XS_AREA_HEADER;
  const uint8_t* end = body + len;
  const uint32_t total = uint32_t(w) * uint32_t(h);
  uint32_t at = 0;  // pixels written, in row order over the rectangle

  // Writes `n` pixels from `src`, a row of the rectangle at a time: a row is
  // contiguous in the canvas, the rectangle is not.
  auto write = [&](uint32_t n, const uint8_t* src) {
    while (n) {
      const int row = y + int(at / uint32_t(w));
      const int col = x + int(at % uint32_t(w));
      uint32_t run = uint32_t(w) - uint32_t(at % uint32_t(w));
      if (run > n) run = n;
      memcpy(reinterpret_cast<uint8_t*>(display->row(row)) + size_t(col) * 2, src, run * 2);
      src += run * 2;
      at += run;
      n -= run;
    }
  };

  if (enc == XS_ENC_RAW) {
    if (uint32_t(end - p) < total * 2) return false;
    write(total, p);
    return at == total;
  }
  if (enc != XS_ENC_OPS) return false;
  while (at < total) {
    if (p >= end) return false;
    const uint8_t op = *p++;
    const uint8_t kind = op >> 6;
    uint32_t n = uint32_t(op & 63);
    if (n == 63) {
      if (end - p < 2) return false;
      n = get16(p);
      p += 2;
    } else {
      n += 1;
    }
    if (n == 0 || at + n > total) return false;
    if (kind == XS_OP_SKIP) {
      at += n;
      continue;
    }
    if (kind == XS_OP_FILL) {
      if (end - p < 2) return false;
      // One colour: fill it a row at a time, since a row is contiguous.
      while (n) {
        const int row = y + int(at / uint32_t(w));
        const int col = x + int(at % uint32_t(w));
        uint32_t run = uint32_t(w) - uint32_t(at % uint32_t(w));
        if (run > n) run = n;
        uint8_t* dst = reinterpret_cast<uint8_t*>(display->row(row)) + size_t(col) * 2;
        for (uint32_t i = 0; i < run; ++i) {
          dst[i * 2] = p[0];
          dst[i * 2 + 1] = p[1];
        }
        at += run;
        n -= run;
      }
      p += 2;
      continue;
    }
    if (kind == XS_OP_LIT) {
      if (uint32_t(end - p) < n * 2) return false;
      write(n, p);
      p += n * 2;
      continue;
    }
    return false;
  }
  return at == total;
}

// ---- Plan mode: the app runs here ----

// The agent line, tunnelled: a request arrives in an AGENT packet and each
// answer leaves as a LOG packet, because the stream protocol owns the port.
class PlanLink final : public rt::Link {
 public:
  int read() override {
    if (in_.empty()) return -1;
    const int c = static_cast<unsigned char>(in_.front());
    in_.pop_front();
    return c;
  }
  void write_line(const std::string& line, bool) override {
    send_packet(XS_LOG, reinterpret_cast<const uint8_t*>(line.data()), uint32_t(line.size()));
  }
  void push(const char* text, size_t n) {
    in_.insert(in_.end(), text, text + n);
    in_.push_back('\n');
  }
  void clear() { in_.clear(); }

 private:
  std::deque<char> in_;
};

// The rt_m5 board, with the agent line replaced. rt_m5::Board is final, so this
// forwards to it rather than deriving from it; everything the app sees - panel,
// buttons, touch, IMU, vibration, counters - is the board's own.
class PlanBoard final : public rt::Board {
 public:
  int64_t micros() const override { return board.micros(); }
  rt::Display* display() override { return board.display(); }
  rt::Link* link() override { return &link_; }
  rt::Accelerometer* accelerometer() override { return board.accelerometer(); }
  rt::Inputs inputs() const override { return board.inputs(); }
  bool input(rt::Input& out) override { return board.input(out); }
  std::vector<rt::Counter> counters() const override { return board.counters(); }
  bool can(const std::string& name) const override { return board.can(name); }
  void command(const std::string& name, const std::vector<rt::V>& args) override { board.command(name, args); }
  void step(uint32_t now_ms) override { board.step(now_ms); }
  // No idling: the loop has a serial port to drain.
  void idle() override {}
  // The board's own RX8130 if it is set, else what the host sent with the plan.
  bool wall_clock(int64_t& epoch_ms, int& utc_offset) const override {
    if (board.wall_clock(epoch_ms, utc_offset)) return true;
    if (!host_clock_) return false;
    epoch_ms = host_epoch_ms_ + (micros() - host_clock_at_us_) / 1000;
    utc_offset = host_offset_;
    return true;
  }
  void set_host_clock(int64_t epoch_ms, int utc_offset) {
    host_clock_ = epoch_ms > 0;
    host_epoch_ms_ = epoch_ms;
    host_offset_ = utc_offset;
    host_clock_at_us_ = micros();
  }
  PlanLink& agent() { return link_; }

 private:
  PlanLink link_;
  bool host_clock_ = false;
  int64_t host_epoch_ms_ = 0, host_clock_at_us_ = 0;
  int host_offset_ = 0;
};

PlanBoard plan_board;
std::unique_ptr<plan_blob::Loaded> plan_loaded;
std::unique_ptr<rt::Host> plan_host;
uint8_t* plan_bytes = nullptr;

void send_plan_ok(bool ok, uint32_t load_us, uint32_t boot_us, const char* text) {
  std::vector<uint8_t> body(17);
  body[0] = ok ? 1 : 0;
  put32(&body[1], load_us);
  put32(&body[5], boot_us);
  put32(&body[9], uint32_t(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
  put32(&body[13], uint32_t(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
  body.insert(body.end(), text, text + strlen(text));
  send_packet(XS_PLAN_OK, body.data(), uint32_t(body.size()));
}

void stop_plan() {
  plan_host.reset();
  plan_loaded.reset();
  if (plan_bytes) heap_caps_free(plan_bytes);
  plan_bytes = nullptr;
  plan_board.agent().clear();
}

// `bytes` (from PSRAM, `len` long, taken over here): epoch_ms, utc_offset, then
// the blob. The blob's bytes must outlive the Loaded that reads them, since its
// strings and pixels point into them.
void start_plan(uint8_t* bytes, uint32_t len) {
  stop_plan();
  plan_bytes = bytes;
  if (len < 10) {
    send_plan_ok(false, 0, 0, "the plan is too short");
    stop_plan();
    return;
  }
  int64_t epoch_ms = 0;
  int16_t offset = 0;
  memcpy(&epoch_ms, bytes, 8);
  memcpy(&offset, bytes + 8, 2);
  plan_board.set_host_clock(epoch_ms, offset);
  const int64_t t0 = esp_timer_get_time();
  plan_loaded = std::make_unique<plan_blob::Loaded>();
  std::string error;
  if (!plan_loaded->load(bytes + 10, len - 10, error)) {
    send_plan_ok(false, 0, 0, error.c_str());
    stop_plan();
    return;
  }
  const int64_t t1 = esp_timer_get_time();
  plan_host = std::make_unique<rt::Host>(plan_loaded->app(), plan_board, std::vector<rt::Extension*>{});
  const bool booted = plan_host->boot();
  const int64_t t2 = esp_timer_get_time();
  if (!booted) {
    send_plan_ok(false, uint32_t(t1 - t0), 0, "the plan did not boot");
    stop_plan();
    return;
  }
  send_plan_ok(true, uint32_t(t1 - t0), uint32_t(t2 - t1), "");
}

// ---- The packet reader ----

enum class Rx { Magic0, Magic1, Head, Payload, Crc };

struct Reader {
  Rx state = Rx::Magic0;
  uint8_t head[4];
  uint32_t head_n = 0;
  uint8_t type = 0;
  uint32_t len = 0, got = 0;
  uint32_t crc = 0;
  uint8_t crc_bytes[4];
  uint32_t crc_n = 0;
  bool keep = false;      // this packet's payload fits and is wanted
  uint8_t* dest = nullptr;  // where its bytes go: `payload`, or a PLAN's own buffer
  uint8_t* plan = nullptr;  // a PLAN's buffer, taken over by start_plan
} rx;

void free_plan_buffer() {
  if (rx.plan) heap_caps_free(rx.plan);
  rx.plan = nullptr;
}

void handle(uint8_t type, const uint8_t* body, uint32_t len) {
  switch (type) {
    case XS_HELLO:
      greeted = true;
      send_info();
      return;
    case XS_LIGHT:
      if (len >= 1) M5.Display.setBrightness(uint8_t((int(body[0]) * 255) / 100));
      return;
    case XS_PLAN:
      start_plan(rx.plan, len);  // takes the buffer over, freed by stop_plan
      rx.plan = nullptr;
      return;
    case XS_AGENT:
      if (plan_host) plan_board.agent().push(reinterpret_cast<const char*>(body), len);
      return;
    case XS_AREA: {
      if (len < XS_AREA_HEADER) return;
      stop_plan();  // an area ends a plan, as the protocol says
      const uint16_t seq = get16(body + 8);
      const int64_t t0 = esp_timer_get_time();
      const bool ok = apply_area(body, len);
      const int64_t t1 = esp_timer_get_time();
      if (ok) {
        const int x = get16(body), y = get16(body + 2), w = get16(body + 4), h = get16(body + 6);
        board.display()->begin_area({x, y, w, h});
        board.display()->push_area();
      }
      const int64_t t2 = esp_timer_get_time();
      send_ack(seq, ok, uint32_t(t1 - t0), uint32_t(t2 - t1));
      return;
    }
    default:
      return;  // PLAN and AGENT need the interpreter; this build has neither
  }
}

void read_serial() {
  static uint8_t buf[4096];
  int avail = Serial.available();
  while (avail > 0) {
    const int n = Serial.read(buf, size_t(avail < int(sizeof(buf)) ? avail : int(sizeof(buf))));
    if (n <= 0) break;
    last_byte_ms = millis();
    for (int i = 0; i < n;) {
      switch (rx.state) {
        case Rx::Magic0:
          rx.state = buf[i++] == XS_MAGIC0 ? Rx::Magic1 : Rx::Magic0;
          break;
        case Rx::Magic1:
          if (buf[i] == XS_MAGIC1) {
            rx.state = Rx::Head;
            rx.head_n = 0;
          } else {
            rx.state = buf[i] == XS_MAGIC0 ? Rx::Magic1 : Rx::Magic0;
          }
          ++i;
          break;
        case Rx::Head:
          rx.head[rx.head_n++] = buf[i++];
          if (rx.head_n == 4) {
            rx.type = rx.head[0];
            rx.len = rx.head[1] | (uint32_t(rx.head[2]) << 8) | (uint32_t(rx.head[3]) << 16);
            rx.crc = esp_rom_crc32_le(0, rx.head, 4);
            rx.got = 0;
            rx.crc_n = 0;
            free_plan_buffer();
            if (rx.type == XS_PLAN) {
              rx.plan = rx.len && rx.len <= XS_MAX_PACKET
                            ? static_cast<uint8_t*>(heap_caps_malloc(rx.len, MALLOC_CAP_SPIRAM))
                            : nullptr;
              rx.dest = rx.plan;
            } else {
              rx.dest = rx.len <= kMaxPayload ? payload : nullptr;
            }
            rx.keep = rx.dest != nullptr;
            rx.state = rx.len ? Rx::Payload : Rx::Crc;
          }
          break;
        case Rx::Payload: {
          uint32_t take = rx.len - rx.got;
          if (take > uint32_t(n - i)) take = uint32_t(n - i);
          rx.crc = esp_rom_crc32_le(rx.crc, buf + i, take);
          if (rx.keep) memcpy(rx.dest + rx.got, buf + i, take);
          rx.got += take;
          i += int(take);
          if (rx.got == rx.len) rx.state = Rx::Crc;
          break;
        }
        case Rx::Crc:
          rx.crc_bytes[rx.crc_n++] = buf[i++];
          if (rx.crc_n == 4) {
            const uint32_t want = uint32_t(rx.crc_bytes[0]) | (uint32_t(rx.crc_bytes[1]) << 8) |
                                  (uint32_t(rx.crc_bytes[2]) << 16) | (uint32_t(rx.crc_bytes[3]) << 24);
            if (want == rx.crc && rx.keep) {
              handle(rx.type, rx.dest, rx.len);
            } else if (rx.type == XS_AREA && rx.len >= XS_AREA_HEADER && rx.keep) {
              send_ack(get16(payload + 8), false, 0, 0);  // ask for a keyframe
            } else if (rx.type == XS_PLAN) {
              send_plan_ok(false, 0, 0, rx.keep ? "the plan's CRC failed" : "no room for the plan");
            }
            free_plan_buffer();
            rx.state = Rx::Magic0;
          }
          break;
      }
    }
    avail = Serial.available();
  }
}

// A packet whose bytes stopped arriving is dropped, so the next one is read as
// a packet rather than as the tail of this one.
void drop_stalled_packet() {
  if (rx.state == Rx::Magic0 || millis() - last_byte_ms <= 500) return;
  if (rx.type == XS_AREA && rx.got >= XS_AREA_HEADER && rx.keep) send_ack(get16(payload + 8), false, 0, 0);
  if (rx.type == XS_PLAN) send_plan_ok(false, 0, 0, "the plan stopped arriving");
  free_plan_buffer();
  rx.state = Rx::Magic0;
}

}  // namespace

// Says why the sketch stopped, straight on the panel: with no canvas there is
// nothing to push, and with no host attached a LOG packet goes nowhere.
void fail(const char* why) {
  M5.Display.fillScreen(0);
  M5.Display.setTextColor(0xffff);
  M5.Display.setCursor(20, 220);
  M5.Display.print(why);
}

void setup() {
  // Before board.begin, which is where rt_m5::Link starts Serial and the
  // buffer size is fixed. A full-screen area is tens of KB, and the default
  // buffer loses bytes in the middle of one.
  Serial.setRxBufferSize(65536);
  plan_blob::allocate = [](size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); };
  plan_blob::release = [](void* q) { heap_caps_free(q); };
  board.begin(kBrightness);
  // rt::Host::boot() is what normally creates the canvas (rt_runtime.cpp, through
  // View::begin); this sketch runs no app, so it creates it itself.
  if (!board.display()->begin()) {
    fail("stream: no canvas");
    return;
  }
  payload = static_cast<uint8_t*>(heap_caps_malloc(kMaxPayload, MALLOC_CAP_SPIRAM));
  if (!payload) {
    fail("stream: no PSRAM for the packet buffer");
    return;
  }
  send_info();
}

void loop() {
  if (!payload) return;
  read_serial();
  drop_stalled_packet();
  // Only between packets: touch and the buttons are I2C, and a decode that
  // waits behind them loses bytes faster than the buffer can hold them.
  if (plan_host) {
    plan_host->turn();  // the app reads the board's own inputs through PlanBoard
    return;
  }
  if (Serial.available()) return;
  rt::Input in;
  while (board.input(in)) {
    if (greeted) send_input(in);
  }
}
