#pragma once

#include <stdint.h>

static constexpr int MAX_FRAMES = 48;

struct Phrase {
  uint16_t bar;
  uint8_t count;
  uint8_t phoneme[MAX_FRAMES];
  uint8_t dur[MAX_FRAMES];
  uint8_t note[MAX_FRAMES];
};

static constexpr int ACCENT_SPANS = MAX_FRAMES + 2;

struct AccentMap {
  uint32_t origin;
  uint8_t count;
  uint8_t id[ACCENT_SPANS];
  uint32_t end[ACCENT_SPANS];
};
