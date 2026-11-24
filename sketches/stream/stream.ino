// A thin client for the Stopwatch: the Exact app runs on a Mac (exact-cpp's
// stream/ host) and this shows its pixels. Each AREA packet the host sends is
// decoded into the rt_m5 canvas and pushed to the panel; BtnA, BtnB and touch
// go back as INPUT packets. No app and no runtime are compiled in, so one
// build shows any app the host runs.
//
// The whole payload is read into PSRAM and decoded once its CRC checks, rather
// than decoded as it arrives: a bad packet can then never reach the panel.
#include <esp_rom_crc.h>
#include <esp_timer.h>

#include <exact_rt.h>
#include <exact_rt_m5.h>
#include <exact_stream_proto.h>

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

void send_packet(uint8_t type, const uint8_t* body, uint32_t len) {
  uint8_t head[6] = {XS_MAGIC0, XS_MAGIC1, type, uint8_t(len), uint8_t(len >> 8), uint8_t(len >> 16)};
  uint32_t crc = esp_rom_crc32_le(0, head + 2, 4);
  if (len) crc = esp_rom_crc32_le(crc, body, len);
  uint8_t tail[4];
  put32(tail, crc);
  Serial.write(head, 6);
  if (len) Serial.write(body, len);
  Serial.write(tail, 4);
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
  bool keep = false;  // this packet's payload fits and is wanted
} rx;

void handle(uint8_t type, const uint8_t* body, uint32_t len) {
  switch (type) {
    case XS_HELLO:
      greeted = true;
      send_info();
      return;
    case XS_LIGHT:
      if (len >= 1) M5.Display.setBrightness(uint8_t((int(body[0]) * 255) / 100));
      return;
    case XS_AREA: {
      if (len < XS_AREA_HEADER) return;
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
            rx.keep = rx.len <= kMaxPayload;
            rx.state = rx.len ? Rx::Payload : Rx::Crc;
          }
          break;
        case Rx::Payload: {
          uint32_t take = rx.len - rx.got;
          if (take > uint32_t(n - i)) take = uint32_t(n - i);
          rx.crc = esp_rom_crc32_le(rx.crc, buf + i, take);
          if (rx.keep) memcpy(payload + rx.got, buf + i, take);
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
              handle(rx.type, payload, rx.len);
            } else if (rx.type == XS_AREA && rx.len >= XS_AREA_HEADER && rx.keep) {
              send_ack(get16(payload + 8), false, 0, 0);  // ask for a keyframe
            }
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
  if (Serial.available()) return;
  rt::Input in;
  while (board.input(in)) {
    if (greeted) send_input(in);
  }
}
