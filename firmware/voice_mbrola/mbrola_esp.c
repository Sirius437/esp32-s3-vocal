/*
 * Compile the MBROLA amalgamation for the ESP32-S3.
 *
 * stdlib is included first, then malloc / realloc / free / exit are
 * redirected. The macros must not be in force while stdlib itself is
 * parsed, or its prototypes get rewritten.
 *
 * LIBRARY is defined by lib1.c. ROMDATABASE_INIT and LITTLE_ENDIAN are
 * set here, not on the sketch, so they do not leak into the C++ file.
 * Xtensa is not in Misc/common.h's endian list.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

volatile int mbrola_failed = 0;

static void *psram_malloc(size_t n) {
  if (n == 0) n = 1;
  return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void *psram_realloc(void *p, size_t n) {
  if (n == 0) n = 1;
  return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void psram_free(void *p) {
  if (p) heap_caps_free(p);
}

static void mbrola_halt(int code) __attribute__((noreturn));

static void mbrola_halt(int code) {
  mbrola_failed = code ? code : -1;
  printf("MBROLA halt %d\n", code);
  fflush(stdout);
  for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
}

#define malloc psram_malloc
#define realloc psram_realloc
#define free psram_free
#define exit(code) mbrola_halt(code)

#define ROMDATABASE_INIT
/*
 * newlib's endian.h defines both LITTLE_ENDIAN and BIG_ENDIAN as numbers.
 * common.h treats that pair as a broken port. The headers are already
 * included, so dropping BIG_ENDIAN here sticks: unistd.h will not define
 * it again. Xtensa is not in common.h's endian list.
 */
#undef BIG_ENDIAN
#undef LITTLE_ENDIAN
#define LITTLE_ENDIAN 1234

#include "../../third_party/MBROLA/LibOneChannel/lib1.c"
