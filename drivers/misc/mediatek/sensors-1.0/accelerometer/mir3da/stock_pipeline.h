/* SPDX-License-Identifier: GPL-2.0-only */
/* A25 stock Image reconstruction. Pure software, no hardware registration.
 * Caller serializes state. Units are native counts (1024/g), NOT HAL floats.
 * Verified against original ARM64 instructions by verify.py. */
#ifndef A25_STOCK_PIPELINE_H
#define A25_STOCK_PIPELINE_H
#ifdef __KERNEL__
#include <linux/types.h>
typedef s16 a25_s16;
#else
#include <stdint.h>
typedef int16_t a25_s16;
#endif
struct a25_temp_state {
    a25_s16 history[3][30];
    a25_s16 minimum[3], maximum[3];
    unsigned char initialized;
    int z_offset;
};
struct a25_chip { unsigned char reg_value; unsigned int package, asic, mems; };
int a25_detect_static(struct a25_temp_state *, int, int, int);
int a25_temp_calibrate(struct a25_temp_state *, int xyz[3]);
void a25_read_transform(struct a25_temp_state *, const struct a25_chip *, a25_s16 xyz[3]);
void a25_map_scale(const a25_s16 in[3], const a25_s16 cali[3],
                   const signed char sign[3], const unsigned char map[3],
                   int counts[3], int products[3], int scaled[3]);
/* Successful register-read path only. Read errors belong to transport. */
int a25_parse_selector(struct a25_chip *, unsigned char c0, unsigned char c1,
                       unsigned char r8f_first, unsigned char r8f_second);
#endif
