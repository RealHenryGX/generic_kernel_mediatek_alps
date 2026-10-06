/* SPDX-License-Identifier: GPL-2.0 */
/* A25 machine-code-verified reconstruction, now fed by the shared sampler.
 * Inputs are stock post-calibration/post-mount counts BEFORE /1024 scaling,
 * not the existing candidate's uncalibrated raw samples.
 * Preserve the exact odd product-domain band; do not round it to mm/s2.
 */
#ifndef A25_STOCK_PS_RECONSTRUCTION_H
#define A25_STOCK_PS_RECONSTRUCTION_H
#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif
struct stock_ps_state { uint32_t hits; unsigned char near; };
/* Return -1 when PS disabled (stock emits nothing); 0 near, 1 far. */
static int stock_ps_step(struct stock_ps_state *s, int enabled,
                         int32_t x_counts, int32_t y_counts, int32_t z_counts)
{
    int32_t px = x_counts * 9807, py = y_counts * 9807;
    int32_t pz = z_counts * 9807;
    int32_t x = px / 1024, y = py / 1024;
    int32_t ax = x < 0 ? -x : x, ay = y < 0 ? -y : y;
    int hit;
    if (enabled != 1)
        return -1;
    hit = ax > 8500 || ax > 7500 || (ay > 7000 && pz < -1023) ||
          (ax >= 5001 && ay > 5000) ||
          ((((uint32_t)py + 5118976U - 10240000U) >> 14) <= 124U &&
           ((uint32_t)pz + 5118976U + 1023U) < 5118976U);
    if (hit) {
        s->hits++; /* stock uses 32-bit wrapping ADD then signed CMP */
        if ((int32_t)s->hits > 3)
            s->near = 1;
    } else {
        s->hits = 0;
        s->near = 0;
    }
    return (~s->near) & 1;
}
#endif
