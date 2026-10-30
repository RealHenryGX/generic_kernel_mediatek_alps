/* SPDX-License-Identifier: GPL-2.0-only */
#include "stock_pipeline.h"
static int a25_abs(int x) { return x < 0 ? -x : x; }
static a25_s16 a25_short(int v)
{
    unsigned int u = (unsigned int)v & 65535u;
    return (a25_s16)(u < 32768u ? (int)u : (int)u - 65536);
}
static int a25_sqrt(int value)
{
    unsigned int root = 0, bit = 0x40000000u;
    if (value < 0) return 0;
    while (bit) {
        if ((unsigned int)value >= root + bit) {
            value -= (int)(root + bit);
            root = (root >> 1) + bit;
        } else root >>= 1;
        bit >>= 2;
    }
    return (int)root;
}
int a25_detect_static(struct a25_temp_state *s, int x, int y, int z)
{
    a25_s16 sample[3]; int a, i, delta = 0;
    sample[0] = a25_short(x); sample[1] = a25_short(y); sample[2] = a25_short(z);
    if (!s->initialized) {
        for (a = 0; a < 3; ++a)
            for (i = 0; i < 30; ++i) s->history[a][i] = sample[a];
        s->initialized = 1;
        /* Stock first call keeps BSS-zero min/max, hence reports static. */
    } else {
        for (a = 0; a < 3; ++a) {
            int lo = s->history[a][0], hi = lo;
            for (i = 0; i < 29; ++i) {
                int v = s->history[a][i + 1];
                s->history[a][i] = (a25_s16)v;
                if (v < lo) lo = v;
                if (v > hi) hi = v;
            }
            /* Stock tests the OLD window, then appends current sample. */
            s->minimum[a] = (a25_s16)lo; s->maximum[a] = (a25_s16)hi;
            s->history[a][29] = sample[a];
        }
    }
    for (a = 0; a < 3; ++a) delta += a25_abs(s->maximum[a] - s->minimum[a]);
    return delta < 60;
}
int a25_temp_calibrate(struct a25_temp_state *s, int xyz[3])
{
    int *x = &xyz[0], *y = &xyz[1], *z = &xyz[2];
    int tx, rem, stable, sign;
    *z += s->z_offset;
    rem = *z % 10;
    if (a25_abs(*x) < 200 && a25_abs(*y) < 200) {
        stable = a25_detect_static(s, *x, *y, *z - s->z_offset);
        tx = a25_sqrt(1048576 - *x * *x - *y * *y) + rem;
        sign = *z >= 0 ? 1 : -1;
        if (s->z_offset) {
            if (stable) {
                if (a25_abs(a25_abs(*z) - 1024) > 130) s->z_offset = 0;
                *z = sign * tx;
            }
        } else {
            if (stable) s->z_offset = sign * tx - *z;
            *z = sign * tx;
        }
        *x = *x * 130 / 200;
        *y = *y * 130 / 200;
    } else if (a25_abs(a25_abs(*x) - 1024) < 200 && a25_abs(*y) < 200 && s->z_offset) {
        if (a25_abs(*x) > 1024)
            *x += (*x > 0 ? -1 : 1) * ((a25_abs(*x) - 1024) * 70 / 200);
        else
            *x += (*x > 0 ? 1 : -1) * ((1024 - a25_abs(*x)) * 70 / 200);
        *y = *y * 130 / 200;
    } else if (a25_abs(a25_abs(*y) - 1024) < 200 && a25_abs(*x) < 200 && s->z_offset) {
        if (a25_abs(*y) > 1024)
            *y += (*y > 0 ? -1 : 1) * ((a25_abs(*y) - 1024) * 70 / 200);
        else
            *y += (*y > 0 ? 1 : -1) * ((1024 - a25_abs(*y)) * 70 / 200);
        *x = *x * 130 / 200;
    } else if (!s->z_offset) {
        sign = *z >= 0 ? 1 : -1;
        if (a25_abs(*x) < 200 && a25_abs(1024 - a25_abs(*y)) < 200)
            *z = sign * a25_abs(*x) * 130 / 200;
        else if (a25_abs(*y) < 200 && a25_abs(1024 - a25_abs(*x)) < 200)
            *z = sign * a25_abs(*y) * 130 / 200;
        else {
            tx = a25_sqrt(1048576 - *x * *x - *y * *y) + rem;
            *z = sign * tx;
        }
    }
    return s->z_offset ? 0 : -1; /* read_data ignores this status, as stock does */
}
void a25_read_transform(struct a25_temp_state *s, const struct a25_chip *c, a25_s16 xyz[3])
{
    int v[3];
    if (c->reg_value == 0x4b || c->reg_value == 0x8c || c->reg_value == 0xca || c->mems == 5) {
        xyz[2] = 0;
        return;
    }
    v[0] = xyz[0]; v[1] = xyz[1]; v[2] = xyz[2];
    a25_temp_calibrate(s, v);
    xyz[0] = a25_short(v[0]); xyz[1] = a25_short(v[1]); xyz[2] = a25_short(v[2]);
}
void a25_map_scale(const a25_s16 in[3], const a25_s16 cali[3],
                   const signed char sign[3], const unsigned char map[3],
                   int counts[3], int products[3], int scaled[3])
{
    int a;
    for (a = 0; a < 3; ++a)
        counts[map[a]] = sign[a] * a25_short(in[a] + sign[a] * cali[map[a]]);
    if (a25_abs(cali[2]) >= 1301) counts[map[2]] -= 2048;
    for (a = 0; a < 3; ++a) { products[a] = counts[a] * 9807; scaled[a] = products[a] / 1024; }
}
int a25_parse_selector(struct a25_chip *c, unsigned char c0, unsigned char c1,
                       unsigned char r8f_first, unsigned char r8f_second)
{
    unsigned int t;
    c->reg_value = c0;
    if (c0 < 64) return -1;
    c->package = c0 >> 6; c->asic = 2; c->mems = 2;
    if ((c0 & 0x38) == 0x10) c->asic = 3;
    if ((c0 & 0x38) == 0x18) c->asic = 4;
    t = c->asic == 2 ? ((c1 >> 6) | ((r8f_first & 1) << 2)) : c1 >> 5;
    if (t == 0) c->mems = (r8f_second & 128) ? 3 : 2;
    else if (t == 1) c->mems = 4;
    else if (t == 3) c->mems = (c->asic == 4 && c0 != 0x5a) ? 6 : 5;
    else if (t == 4) c->mems = 6;
    return 0;
}
