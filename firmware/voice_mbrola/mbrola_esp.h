#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Set when the engine takes the fatal path. The audio task writes silence. */
extern volatile int mbrola_failed;

int init_ROM_MBR(void *image);
int read_MBR(void *buffer_out, int nb_wanted);
int write_MBR(char *buffer_in);
int reset_MBR(void);
int lastError_MBR(void);
int lastErrorStr_MBR(char *buffer_err, int nb_wanted);

#ifdef __cplusplus
}
#endif
