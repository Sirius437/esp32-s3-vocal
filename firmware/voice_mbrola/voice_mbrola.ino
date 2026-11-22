/*
 * esp32-s3-vocal — MBROLA diphones
 * --------------------------------
 * us1 is read from the FAT partition into one PSRAM block. The engine
 * synthesizes at 16 kHz into a ring. This task upsamples 3x to 48 kHz,
 * runs the character FX, and writes both I2S slots with the same sample.
 * Pins: BCK 5, WS 6, DATA 7. A phrase starts on its bar.
 */

#include <Arduino.h>
#include <math.h>
#include <string.h>

#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "mbrola_esp.h"
#include "phrase.h"
#include "sampa.h"
#include "floor.h"
#include "accent.h"

#if ARDUINO_USB_MODE
#error "Set USB Mode = USB-OTG (TinyUSB). This sketch does nothing in Hardware CDC mode."
#endif

SET_USB_MIDI_DEVICE_NAME("esp32-s3-vocal")
#include "USB.h"
#include "USBMIDI.h"
#ifdef TUSB_CDC_0_USE_NOTIFY
TUSB_CDC_0_USE_NOTIFY()
#endif
USBMIDI MIDI;

static const int SR = 48000;
static const int SYNTH = 16000;
static const size_t RING_BYTES = 8192;
static const size_t BLOCK = 320;

static const char HELD[] =
    "_ 100\n"
    "A 1500 (50, 130)\n"
    "_ 100\n"
    "#\n";

static const char FUNK[] =
    "; funk it up brother funk to the beat\n"
    "_ 50\n"
    "f 125 (50, 130.81)\n"
    "V 250 (50, 130.81)\n"
    "N 125 (50, 130.81)\n"
    "k 125 (50, 130.81)\n"
    "I 250 (50, 116.54)\n"
    "t 125 (50, 116.54)\n"
    "V 250 (50, 116.54)\n"
    "p 125 (50, 116.54)\n"
    "b 125 (50, 116.54)\n"
    "r 125 (50, 116.54)\n"
    "V 250 (50, 116.54)\n"
    "D 125 (50, 116.54)\n"
    "r= 250 (50, 116.54)\n"
    "f 125 (50, 103.83)\n"
    "V 250 (50, 103.83)\n"
    "N 125 (50, 103.83)\n"
    "k 125 (50, 103.83)\n"
    "t 125 (50, 116.54)\n"
    "u 250 (50, 116.54)\n"
    "D 125 (50, 130.81)\n"
    "@ 250 (50, 130.81)\n"
    "b 125 (50, 116.54)\n"
    "i 250 (50, 116.54)\n"
    "t 125 (50, 116.54)\n"
    "_ 50\n"
    "#\n";

static_assert(sizeof(HELD) < 8000, "held pho fits the fifo");
static_assert(sizeof(FUNK) < 8000, "line pho fits the fifo");
static_assert(SYNTH * 3 == SR, "48 kHz is an integer 3x of 16 kHz");

struct Biquad {
  float b0, b1, b2, a1, a2, z1, z2;
  void lowpass(float freq) { shelf(freq, true); }
  void highpass(float freq) { shelf(freq, false); }
  void clear() { z1 = 0; z2 = 0; }
  void peak(float freq, float q, float gainDb) {
    if (freq < 80) freq = 80;
    if (freq > 8000) freq = 8000;
    if (q < 0.2f) q = 0.2f;
    float A = powf(10.0f, gainDb / 40.0f);
    float w = 6.2831853f * freq / SR;
    float cw = cosf(w);
    float alpha = sinf(w) / (2.0f * q);
    float a0 = 1.0f + alpha / A;
    b0 = (1.0f + alpha * A) / a0;
    b1 = (-2.0f * cw) / a0;
    b2 = (1.0f - alpha * A) / a0;
    a1 = b1;
    a2 = (1.0f - alpha / A) / a0;
  }
  void shelf(float freq, bool low) {
    if (freq < 40) freq = 40;
    if (freq > 8000) freq = 8000;
    float w = 6.2831853f * freq / SR;
    float alpha = sinf(w) / (2.0f * 0.707f);
    float cw = cosf(w);
    float a0 = 1.0f + alpha;
    if (low) {
      b0 = ((1.0f - cw) * 0.5f) / a0;
      b1 = (1.0f - cw) / a0;
      b2 = b0;
    } else {
      b0 = ((1.0f + cw) * 0.5f) / a0;
      b1 = -(1.0f + cw) / a0;
      b2 = b0;
    }
    a1 = -2.0f * cw / a0;
    a2 = (1.0f - alpha) / a0;
  }
  float tick(float x) {
    float y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
  }
};

static Biquad phoneHp, phoneLp, color, airHp;
static uint8_t knobDrive = 48;
static uint8_t knobCrush = 0;
static uint8_t knobRing = 0;
static uint8_t knobPhone = 0;
static uint8_t knobShift = 64;
static uint8_t knobLevel = 100;
// Speech rate knob. Byte 0 = 1.00 (as authored), 64 ~ 0.75, 127 = 0.50.
static uint8_t knobSpeed = 64;
// Accent depth. 0 is flat, 127 is the full table, 48 is the boot depth.
static uint8_t knobAccent = 48;
// Air exciter mix. 0 is bypassed.
static uint8_t knobAir = 0;
static uint8_t colorKnob = 255;
static uint32_t ringPhase = 0;
static int crushHold = 0;
static float crushMem = 0;

static StreamBufferHandle_t ring = nullptr;
static volatile uint32_t underruns = 0;
static volatile bool expectAudio = false;
static char statusBuf[160] = "boot";
static uint8_t sysexBuf[256];
static uint16_t sysexLen = 0;
static bool inSysex = false;

static portMUX_TYPE phraseMux = portMUX_INITIALIZER_UNLOCKED;
static Phrase pending;
static Phrase speak;
static volatile bool phrasePending = false;
static volatile bool speakReady = false;
static volatile bool stopRequested = false;
static volatile bool running = false;
static volatile uint32_t clocks = 0;
static volatile uint16_t barNo = 0;
static volatile uint32_t lastClockUs = 0;
static volatile uint32_t clockSpanUs = 0;
static volatile uint32_t clocksHeard = 0;
static char phoBuf[4096];

static void logLine(const char *msg) {
  strncpy(statusBuf, msg, sizeof statusBuf - 1);
  statusBuf[sizeof statusBuf - 1] = 0;
  Serial.println(statusBuf);
  Serial0.println(statusBuf);
}

static void statusTask(void *) {
  for (;;) {
    Serial.println(statusBuf);
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

static void logf(const char *fmt, ...) {
  char line[160];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  logLine(line);
}

static void applyPreset(uint8_t preset) {
  knobDrive = 48;
  knobCrush = 0;
  knobRing = 0;
  knobPhone = 0;
  knobShift = 64;
  if (preset == 1) knobShift = 36;
  if (preset == 2) knobShift = 100;
  if (preset == 3) {
    knobPhone = 127;
    knobDrive = 30;
  }
  if (preset == 0) knobDrive = 80;
  logLine(preset == 0 ? "VOICE robot" : preset == 1 ? "VOICE deep" : preset == 2 ? "VOICE nasal" : "VOICE telephone");
}

static void setKnob(uint8_t target, uint8_t knob, uint8_t value) {
  uint8_t *slots[] = {&knobDrive, &knobCrush, &knobRing, &knobPhone, &knobShift};
  if (target == 0 && knob < 5) *slots[knob] = value;
  if (target == 1 && knob == 3) knobLevel = value;
  if (target == 2 && knob == 0) knobSpeed = value;
  if (target == 2 && knob == 1) knobAccent = value;
  if (target == 2 && knob == 3) knobAir = value;
}

static void onBar() {
  barNo++;
  if (!phrasePending) return;
  if (pending.bar != 0 && pending.bar != barNo) return;
  portENTER_CRITICAL(&phraseMux);
  speak = pending;
  phrasePending = false;
  speakReady = true;
  portEXIT_CRITICAL(&phraseMux);
  logLine("SPEAK");
}

static void handleStatus(uint8_t status, uint8_t d1, uint8_t d2) {
  switch (status) {
    case 0xF8:
      if (!running) break;
      clocks++;
      if (clocks % 96 == 0) onBar();
      {
        uint32_t now = micros();
        if (lastClockUs) {
          uint32_t span = now - lastClockUs;
          if (span > 1000 && span < 100000) {
            clockSpanUs = span;
            clocksHeard++;
          }
        }
        lastClockUs = now;
      }
      break;
    case 0xFA:
      running = true;
      clocks = 0;
      barNo = 0;
      logLine("START");
      onBar();
      break;
    case 0xFB:
      running = true;
      logLine("CONTINUE");
      break;
    case 0xFC:
      running = false;
      portENTER_CRITICAL(&phraseMux);
      phrasePending = false;
      stopRequested = true;
      portEXIT_CRITICAL(&phraseMux);
      logLine("STOP");
      break;
    case 0xF2: {
      uint16_t pos16 = (uint16_t)d1 | ((uint16_t)d2 << 7);
      clocks = (uint32_t)pos16 * 6;
      barNo = (uint16_t)(clocks / 96);
      break;
    }
    default:
      break;
  }
}

static void handleSysex() {
  if (sysexLen < 2 || sysexBuf[0] != 0x7D) return;
  if (sysexBuf[1] == 0x01) {
    if (sysexLen < 5) {
      logLine("PHRASE short");
      return;
    }
    uint16_t bar = (uint16_t)sysexBuf[2] | ((uint16_t)sysexBuf[3] << 7);
    uint8_t count = sysexBuf[4];
    if (count > MAX_FRAMES || sysexLen < 5 + (uint16_t)count * 3) {
      logLine("PHRASE short");
      return;
    }
    Phrase next;
    next.bar = bar;
    next.count = count;
    for (uint8_t i = 0; i < count; i++) {
      const uint8_t *frame = sysexBuf + 5 + i * 3;
      next.phoneme[i] = frame[0] & 0x7F;
      uint8_t dur = frame[1] & 0x7F;
      next.dur[i] = dur < 1 ? 1 : dur;
      next.note[i] = frame[2] & 0x7F;
    }
    portENTER_CRITICAL(&phraseMux);
    pending = next;
    phrasePending = true;
    portEXIT_CRITICAL(&phraseMux);
    logf("PHRASE bar %u frames %u", bar, count);
    return;
  }
  if (sysexBuf[1] == 0x02 && sysexLen >= 3) {
    applyPreset(sysexBuf[2] & 0x7F);
    return;
  }
  if (sysexBuf[1] == 0x03 && sysexLen >= 5) {
    setKnob(sysexBuf[2] & 0x7F, sysexBuf[3] & 0x7F, sysexBuf[4] & 0x7F);
  }
}

static void takeByte(uint8_t byte) {
  if (byte >= 0xF8) return;
  if (byte == 0xF0) {
    inSysex = true;
    sysexLen = 0;
    return;
  }
  if (byte == 0xF7) {
    if (inSysex) handleSysex();
    inSysex = false;
    return;
  }
  if (inSysex && sysexLen < sizeof(sysexBuf)) sysexBuf[sysexLen++] = byte;
}

static void handlePacket(uint8_t header, uint8_t b1, uint8_t b2, uint8_t b3) {
  uint8_t cin = header & 0x0F;
  if (cin == 0x04 || cin == 0x07) {
    takeByte(b1);
    takeByte(b2);
    takeByte(b3);
    return;
  }
  if (cin == 0x05) {
    takeByte(b1);
    return;
  }
  if (cin == 0x06) {
    takeByte(b1);
    takeByte(b2);
    return;
  }
  if (cin == 0x0F) {
    handleStatus(b1, 0, 0);
    return;
  }
  handleStatus(b1, b2, b3);
}

static float character(float x) {
  float drive = 1.0f + knobDrive * (6.0f / 127.0f);
  x *= drive;
  x = x / (1.0f + fabsf(x));
  if (knobCrush) {
    int holdEvery = 1 + knobCrush / 24;
    if (++crushHold >= holdEvery) {
      crushHold = 0;
      int steps = 4 + (127 - knobCrush) / 8;
      crushMem = roundf(x * steps) / steps;
    }
    x = crushMem;
  }
  if (knobRing) {
    ringPhase += (uint32_t)(90.0f * 4294967296.0f / SR);
    float carrier = sinf(ringPhase * (6.2831853f / 4294967296.0f));
    float mix = knobRing / 127.0f;
    x = x * ((1.0f - mix) + mix * carrier);
  }
  if (knobPhone) {
    float wet = phoneLp.tick(phoneHp.tick(x));
    float mix = knobPhone / 127.0f;
    x = x * (1.0f - mix) + wet * mix * 1.6f;
  }
  float hp = airHp.tick(x);
  float ex = hp + 2.0f * hp * hp;
  x += (knobAir / 127.0f) * 0.4f * ex;
  return x;
}

static void paintColor() {
  if (colorKnob == knobShift) return;
  colorKnob = knobShift;
  float hz = 350.0f + (knobShift / 127.0f) * 2200.0f;
  color.peak(hz, 0.8f, 6.0f);
}

static int16_t shape(int32_t sample) {
  paintColor();
  float x = color.tick(sample / 32768.0f);
  x = character(x);
  x *= knobLevel / 127.0f;
  if (x > 0.95f) x = 0.95f;
  if (x < -0.95f) x = -0.95f;
  return (int16_t)(x * 32767.0f);
}

static void feedText(const char *text, const char *label) {
  if (label) logLine(label);
  if (write_MBR((char *)text) <= 0) logLine("fifo full");
}

static int sixteenthMs() {
  if (clocksHeard >= 1 && clockSpanUs > 0) {
    int ms = (int)(((uint64_t)clockSpanUs * 6 + 500) / 1000);
    if (ms < 40) ms = 40;
    if (ms > 400) ms = 400;
    return ms;
  }
  return 125;
}

static int scaledMs(uint8_t id, int dur, int six) {
  float speechRate = 1.0f - (knobSpeed / 127.0f) * 0.5f;
  int ms = (int)lroundf((float)dur * six * speechRate);
  int floorMs = id < SAMPA_COUNT ? SAMPA_MIN_MS[id] : 0;
  if (ms < floorMs) ms = floorMs;
  if (ms < 1) ms = 1;
  return ms;
}

static AccentMap accentSlot[2];
static uint8_t accentWrite = 1;
static volatile uint8_t accentLive = 0;
static volatile uint32_t synthPushed = 0;
static float accentGain = 1.0f;

static int8_t accentAt(const AccentMap &map, int32_t rel) {
  if (rel < 0 || map.count == 0) return 0;
  for (uint8_t span = 0; span < map.count; span++) {
    if ((uint32_t)rel < map.end[span]) {
      uint8_t id = map.id[span];
      return id < SAMPA_COUNT ? SAMPA_ACCENT[id] : 0;
    }
  }
  return 0;
}

static void publishAccent(const Phrase &phrase) {
  AccentMap &map = accentSlot[accentWrite];
  int six = sixteenthMs();
  uint32_t cursor = 0;
  uint8_t n = 0;
  map.id[n] = 0;
  cursor += 50u * 16u;
  map.end[n++] = cursor;
  for (uint8_t i = 0; i < phrase.count && n + 1 < ACCENT_SPANS; i++) {
    map.id[n] = phrase.phoneme[i];
    cursor += (uint32_t)scaledMs(phrase.phoneme[i], phrase.dur[i], six) * 16u;
    map.end[n++] = cursor;
  }
  map.id[n] = 0;
  cursor += 50u * 16u;
  map.end[n++] = cursor;
  map.count = n;
  map.origin = synthPushed;
  uint8_t published = accentWrite;
  portENTER_CRITICAL(&phraseMux);
  accentLive = published;
  accentWrite ^= 1;
  portEXIT_CRITICAL(&phraseMux);
}

static bool buildPho(const Phrase &phrase) {
  int six = sixteenthMs();
  int used = snprintf(phoBuf, sizeof phoBuf, "_ 50\n");
  if (used < 0) return false;
  for (uint8_t i = 0; i < phrase.count; i++) {
    uint8_t id = phrase.phoneme[i];
    const char *name = id < SAMPA_COUNT ? SAMPA_NAME[id] : "_";
    int ms = scaledMs(id, phrase.dur[i], six);
    int wrote;
    if (id == 0 || phrase.note[i] == 0) {
      wrote = snprintf(phoBuf + used, sizeof phoBuf - used, "%s %d\n", name, ms);
    } else {
      float hz = 440.0f * powf(2.0f, (phrase.note[i] - 69) / 12.0f);
      wrote = snprintf(phoBuf + used, sizeof phoBuf - used, "%s %d (50, %.2f)\n", name, ms, hz);
    }
    if (wrote < 0 || used + wrote >= (int)sizeof phoBuf) return false;
    used += wrote;
  }
  int tail = snprintf(phoBuf + used, sizeof phoBuf - used, "_ 50\n#\n");
  return tail > 0 && used + tail < (int)sizeof phoBuf;
}

static void armAudio() {
  underruns = 0;
  expectAudio = true;
}

static void synthTask(void *) {
  bool bootLine = true;
  bool busy = false;
  uint32_t said = 0;
  armAudio();
  feedText(HELD, "held");
  int16_t buf[BLOCK];
  uint32_t blocks = 0;
  for (;;) {
    if (mbrola_failed) {
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    bool halt = false;
    bool say = false;
    Phrase phrase;
    portENTER_CRITICAL(&phraseMux);
    if (stopRequested) {
      stopRequested = false;
      speakReady = false;
      halt = true;
    } else if (speakReady) {
      phrase = speak;
      speakReady = false;
      say = true;
    }
    portEXIT_CRITICAL(&phraseMux);
    if (halt || say) {
      // A finished phrase leaves the parser waiting for the next write.
      // reset_MBR is the interrupt path, and it drops that state.
      if (busy) {
        if (!reset_MBR()) {
          mbrola_failed = 1;
          logLine("reset failed");
          continue;
        }
        busy = false;
      }
      bootLine = false;
      said = 0;
      armAudio();
      if (halt) {
        feedText("_ 80\n#\n", nullptr);
      } else if (!buildPho(phrase)) {
        logLine("pho full");
        feedText("_ 80\n#\n", nullptr);
      } else {
        logf("pho %u", (unsigned)strlen(phoBuf));
        publishAccent(phrase);
        feedText(phoBuf, nullptr);
      }
      continue;
    }
    uint32_t t0 = micros();
    int n = read_MBR(buf, (int)BLOCK);
    uint32_t us = micros() - t0;
    if (n < 0) {
      char err[200];
      lastErrorStr_MBR(err, (int)sizeof err);
      mbrola_failed = n;
      logf("synth %d %s", n, err);
      continue;
    }
    if (n == 0) {
      busy = false;
      if (expectAudio) {
        expectAudio = false;
        logf("done %lu underrun %lu", (unsigned long)said, (unsigned long)underruns);
      }
      if (bootLine) {
        bootLine = false;
        said = 0;
        armAudio();
        feedText(FUNK, "line");
        continue;
      }
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    busy = true;
    said += (uint32_t)n;
    blocks++;
    if (blocks <= 4 || blocks % 50 == 0) logf("block %lu us", (unsigned long)us);
    size_t sent = xStreamBufferSend(ring, buf, (size_t)n * sizeof(int16_t), portMAX_DELAY);
    synthPushed += (uint32_t)(sent / sizeof(int16_t));
  }
}

static void i2sTask(void *) {
  i2s_chan_handle_t tx = nullptr;
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chan_cfg.dma_desc_num = 8;
  chan_cfg.dma_frame_num = 256;
  if (i2s_new_channel(&chan_cfg, &tx, nullptr) != ESP_OK) {
    logLine("i2s channel failed");
    vTaskDelete(nullptr);
    return;
  }
  i2s_std_config_t std_cfg = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SR),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
      .gpio_cfg = {
          .mclk = I2S_GPIO_UNUSED,
          .bclk = GPIO_NUM_5,
          .ws = GPIO_NUM_6,
          .dout = GPIO_NUM_7,
          .din = I2S_GPIO_UNUSED,
          .invert_flags = {
              .mclk_inv = false,
              .bclk_inv = false,
              .ws_inv = false,
          },
      },
  };
  if (i2s_channel_init_std_mode(tx, &std_cfg) != ESP_OK || i2s_channel_enable(tx) != ESP_OK) {
    logLine("i2s start failed");
    vTaskDelete(nullptr);
    return;
  }

  uint32_t wait = millis();
  while (xStreamBufferBytesAvailable(ring) < 4000 && !mbrola_failed && millis() - wait < 3000) {
    vTaskDelay(pdMS_TO_TICKS(10));
  }

  int32_t prev = 0;
  int16_t in[BLOCK];
  int16_t stereo[BLOCK * 3 * 2];
  uint32_t lastLog = millis();
  uint32_t i2sReceived = 0;
  for (;;) {
    if (mbrola_failed) {
      memset(stereo, 0, sizeof stereo);
      size_t wrote = 0;
      i2s_channel_write(tx, stereo, sizeof stereo, &wrote, portMAX_DELAY);
      continue;
    }
    size_t got = xStreamBufferReceive(ring, in, sizeof in, pdMS_TO_TICKS(2));
    size_t n = got / sizeof(int16_t);
    bool real = n > 0;
    if (!real) {
      if (expectAudio) {
        underruns++;
        if (underruns == 1) logf("underrun %lu", (unsigned long)underruns);
      }
      memset(in, 0, sizeof in);
      n = BLOCK;
    }
    uint8_t live = 0;
    if (real) {
      portENTER_CRITICAL(&phraseMux);
      live = accentLive;
      portEXIT_CRITICAL(&phraseMux);
    }
    size_t frames = 0;
    for (size_t i = 0; i < n; i++) {
      if (real) {
        int32_t rel = (int32_t)(i2sReceived - accentSlot[live].origin);
        int8_t step = accentAt(accentSlot[live], rel);
        float db = step * (knobAccent / 127.0f);
        float target = powf(10.0f, db / 20.0f);
        accentGain += (target - accentGain) * 0.0124f;
        float y = (float)in[i] * accentGain;
        if (y > 32767.0f) y = 32767.0f;
        if (y < -32768.0f) y = -32768.0f;
        in[i] = (int16_t)lroundf(y);
        i2sReceived++;
      }
      int32_t next = in[i];
      int32_t samples[3] = {prev, (2 * prev + next) / 3, (prev + 2 * next) / 3};
      prev = next;
      for (int k = 0; k < 3; k++) {
        int16_t s = shape(samples[k]);
        stereo[frames++] = s;
        stereo[frames++] = s;
      }
    }
    size_t wrote = 0;
    i2s_channel_write(tx, stereo, frames * sizeof(int16_t), &wrote, portMAX_DELAY);
    if (expectAudio && millis() - lastLog > 2000) {
      lastLog = millis();
      logf("underrun %lu", (unsigned long)underruns);
    }
  }
}

// tools/make_ffat.py writes a raw FAT16 at the ffat partition. us1.rom
// starts at this offset. Arduino FFat mounts through wear leveling, so it
// cannot open that image.
static constexpr size_t ROM_BYTES = 7282628;
static constexpr size_t ROM_AT = 0x9200;

static bool loadRom() {
  const esp_partition_t *part = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, "ffat");
  if (!part) {
    logLine("no ffat partition");
    return false;
  }
  if (part->size < ROM_AT + ROM_BYTES) {
    logf("ffat %u too small", (unsigned)part->size);
    return false;
  }
  logLine("read");
  void *rom = heap_caps_malloc(ROM_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!rom) {
    logLine("rom alloc failed");
    return false;
  }
  uint8_t *dst = (uint8_t *)rom;
  size_t got = 0;
  while (got < ROM_BYTES) {
    size_t chunk = ROM_BYTES - got;
    if (chunk > 16384) chunk = 16384;
    esp_err_t rc = esp_partition_read(part, ROM_AT + got, dst + got, chunk);
    if (rc != ESP_OK) {
      logf("read %d at %u", (int)rc, (unsigned)got);
      heap_caps_free(rom);
      return false;
    }
    got += chunk;
  }
  if (memcmp(dst + 0x30, "MBRO", 4) != 0) {
    logf("header %02x %02x %02x %02x", dst[0], dst[1], dst[2], dst[3]);
    heap_caps_free(rom);
    return false;
  }
  logLine("init");
  int err = init_ROM_MBR(rom);
  size_t freePsram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  logf("rom %u", (unsigned)ROM_BYTES);
  logf("psram %u", (unsigned)freePsram);
  if (err != 0 || mbrola_failed) {
    char msg[120];
    lastErrorStr_MBR(msg, (int)sizeof msg);
    logf("init %d %s", err, msg);
    return false;
  }
  return true;
}

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  Serial.setDebugOutput(true);
  Serial0.begin(115200);
  xTaskCreatePinnedToCore(statusTask, "status", 4096, nullptr, 1, nullptr, 0);
  MIDI.begin();
  phoneHp.highpass(300);
  phoneLp.lowpass(3400);
  airHp.highpass(3500);
  phoneHp.clear();
  phoneLp.clear();
  airHp.clear();
  color.clear();
  paintColor();
  logLine("esp32-s3-vocal");
  if (!loadRom()) return;
  ring = xStreamBufferCreate(RING_BYTES, 2);
  if (!ring) {
    logLine("ring alloc failed");
    return;
  }
  xTaskCreatePinnedToCore(synthTask, "mbrola", 20480, nullptr, 4, nullptr, 0);
  xTaskCreatePinnedToCore(i2sTask, "i2s", 8192, nullptr, 5, nullptr, 1);
}

void loop() {
  midiEventPacket_t packet;
  while (MIDI.readPacket(&packet)) {
    handlePacket(packet.header, packet.byte1, packet.byte2, packet.byte3);
  }
  delay(1);
}
