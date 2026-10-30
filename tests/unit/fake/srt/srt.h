#ifndef FAKE_SRT_H
#define FAKE_SRT_H

/* Stands in for libsrt's <srt/srt.h> in the unit tests, which build without libsrt
 * installed. It declares only what tracker.c takes from libsrt; the test defines
 * srt_close() to record what was closed. Keep these equal to libsrt's. */

typedef int SRTSOCKET;
#define SRT_INVALID_SOCK (-1)

int srt_close(SRTSOCKET u);

#endif
