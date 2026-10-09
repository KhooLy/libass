#include "config.h"
#include "ass_compat.h"

#include <arm_neon.h>
#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include "ass_utils.h"
#include "ass_rasterizer.h"

#define TILE_SIZE 16
#define TILE_ORDER 4
#define FULL_VALUE (1 << (14 - TILE_ORDER))
#define RESCALE_AB(ab, scale) \
    (((ab) * (int64_t) (scale) + ((int64_t) 1 << (45 + TILE_ORDER))) >> (46 + TILE_ORDER))
#define RESCALE_C(c, scale) \
    (((int32_t) ((c) >> (7 + TILE_ORDER)) * (int64_t) (scale) + ((int64_t) 1 << 44)) >> 45)

void ass_fill_solid_tile16_neon(uint8_t *buf, ptrdiff_t stride, int set)
{
    uint8x16_t value = vdupq_n_u8(set ? 255 : 0);
    for (int y = 0; y < TILE_SIZE; y++) {
        vst1q_u8(buf, value);
        buf += stride;
    }
}

void ass_fill_halfplane_tile16_neon(uint8_t *buf, ptrdiff_t stride,
                                    int32_t a, int32_t b, int64_t c, int32_t scale)
{
    int16_t aa = RESCALE_AB(a, scale), bb = RESCALE_AB(b, scale);
    int16_t cc = RESCALE_C(c, scale) + FULL_VALUE / 2 - ((aa + bb) >> 1);

    int16_t abs_a = aa < 0 ? -aa : aa;
    int16_t abs_b = bb < 0 ? -bb : bb;
    int16_t delta = (FFMIN(abs_a, abs_b) + 2) >> 2;

    static const int16_t xs[16] __attribute__((aligned(16))) =
        { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
    int16x8_t x0 = vld1q_s16(xs), x1 = vld1q_s16(xs + 8);
    int16x8_t vd = vdupq_n_s16(delta);
    int16x8_t ax0 = vmulq_n_s16(x0, aa), ax1 = vmulq_n_s16(x1, aa);
    int16x8_t va1_0 = vsubq_s16(ax0, vd), va1_1 = vsubq_s16(ax1, vd);
    int16x8_t va2_0 = vaddq_s16(ax0, vd), va2_1 = vaddq_s16(ax1, vd);
    int16x8_t zero = vdupq_n_s16(0), full = vdupq_n_s16(FULL_VALUE), cap = vdupq_n_s16(255);

    for (int y = 0; y < TILE_SIZE; y++) {
        int16x8_t vc = vdupq_n_s16(cc);
        int16x8_t r[2];
        int16x8_t c1a = vminq_s16(vmaxq_s16(vsubq_s16(vc, va1_0), zero), full);
        int16x8_t c2a = vminq_s16(vmaxq_s16(vsubq_s16(vc, va2_0), zero), full);
        int16x8_t c1b = vminq_s16(vmaxq_s16(vsubq_s16(vc, va1_1), zero), full);
        int16x8_t c2b = vminq_s16(vmaxq_s16(vsubq_s16(vc, va2_1), zero), full);
        r[0] = vminq_s16(vshrq_n_s16(vaddq_s16(c1a, c2a), 7 - TILE_ORDER), cap);
        r[1] = vminq_s16(vshrq_n_s16(vaddq_s16(c1b, c2b), 7 - TILE_ORDER), cap);
        vst1q_u8(buf, vcombine_u8(vmovn_u16(vreinterpretq_u16_s16(r[0])),
                                  vmovn_u16(vreinterpretq_u16_s16(r[1]))));
        buf += stride;
        cc -= bb;
    }
}

static inline void update_border_line(int16_t res[TILE_SIZE], int16_t abs_a,
                                      const int16x8_t va[2], int16_t b, int16_t abs_b,
                                      int16_t c, int up, int dn)
{
    int16_t size = dn - up;
    int16_t w = FULL_VALUE + (size << (8 - TILE_ORDER)) - abs_a;
    w = FFMIN(w, FULL_VALUE) << (2 * TILE_ORDER - 5);

    int16_t dc_b = abs_b * (int32_t) size >> 6;
    int16_t dc = (FFMIN(abs_a, dc_b) + 2) >> 2;

    int16_t base = (int32_t) b * (int16_t) (up + dn) >> 7;
    int16_t offs1 = size - ((base + dc) * (int32_t) w >> 16);
    int16_t offs2 = size - ((base - dc) * (int32_t) w >> 16);

    size <<= 1;
    int16x4_t vcs = vdup_n_s16(c);
    int16x8_t o1 = vdupq_n_s16(offs1), o2 = vdupq_n_s16(offs2);
    int16x8_t zero = vdupq_n_s16(0), vsz = vdupq_n_s16(size);
    for (int h = 0; h < 2; h++) {
        int32x4_t d0 = vsubl_s16(vcs, vget_low_s16(va[h]));
        int32x4_t d1 = vsubl_s16(vcs, vget_high_s16(va[h]));
        int16x8_t cw = vcombine_s16(vmovn_s32(vshrq_n_s32(vmulq_n_s32(d0, w), 16)),
                                    vmovn_s32(vshrq_n_s32(vmulq_n_s32(d1, w), 16)));
        int16x8_t c1 = vminq_s16(vmaxq_s16(vaddq_s16(cw, o1), zero), vsz);
        int16x8_t c2 = vminq_s16(vmaxq_s16(vaddq_s16(cw, o2), zero), vsz);
        int16x8_t r = vld1q_s16(res + 8 * h);
        vst1q_s16(res + 8 * h, vaddq_s16(r, vaddq_s16(c1, c2)));
    }
}

void ass_fill_generic_tile16_neon(uint8_t *restrict buf, ptrdiff_t stride,
                                  const struct segment *restrict line, size_t n_lines,
                                  int winding)
{
    int16_t res[TILE_SIZE][TILE_SIZE] __attribute__((aligned(16))) = {{0}};
    int16_t delta[TILE_SIZE + 2] = {0};

    static const int16_t xs[16] __attribute__((aligned(16))) =
        { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
    int16x8_t x0 = vld1q_s16(xs), x1 = vld1q_s16(xs + 8);
    int16x8_t zero = vdupq_n_s16(0), full = vdupq_n_s16(FULL_VALUE);

    const struct segment *end = line + n_lines;
    for (; line != end; ++line) {
        assert(line->y_min >= 0 && line->y_min < 64 << TILE_ORDER);
        assert(line->y_max > 0 && line->y_max <= 64 << TILE_ORDER);
        assert(line->y_min <= line->y_max);

        int16_t up_delta = line->flags & SEGFLAG_DN ? 4 : 0;
        int16_t dn_delta = up_delta;
        if (!line->x_min && (line->flags & SEGFLAG_EXACT_LEFT)) dn_delta ^= 4;
        if (line->flags & SEGFLAG_UL_DR) {
            int16_t tmp = up_delta;
            up_delta = dn_delta;
            dn_delta = tmp;
        }

        int up = line->y_min >> 6, dn = line->y_max >> 6;
        int16_t up_pos = line->y_min & 63;
        int16_t up_delta1 = up_delta * up_pos;
        int16_t dn_pos = line->y_max & 63;
        int16_t dn_delta1 = dn_delta * dn_pos;
        delta[up + 1] -= up_delta1;
        delta[up] -= (up_delta << 6) - up_delta1;
        delta[dn + 1] += dn_delta1;
        delta[dn] += (dn_delta << 6) - dn_delta1;
        if (line->y_min == line->y_max)
            continue;

        int16_t a = RESCALE_AB(line->a, line->scale);
        int16_t b = RESCALE_AB(line->b, line->scale);
        int16_t c = RESCALE_C(line->c, line->scale) - (a >> 1) - b * up;

        int16x8_t va[2] = { vmulq_n_s16(x0, a), vmulq_n_s16(x1, a) };
        int16_t abs_a = a < 0 ? -a : a;
        int16_t abs_b = b < 0 ? -b : b;
        int16_t dc = (FFMIN(abs_a, abs_b) + 2) >> 2;
        int16_t base = FULL_VALUE / 2 - (b >> 1);
        int16_t dc1 = base + dc;
        int16_t dc2 = base - dc;

        if (up_pos) {
            if (dn == up) {
                update_border_line(res[up], abs_a, va, b, abs_b, c, up_pos, dn_pos);
                continue;
            }
            update_border_line(res[up], abs_a, va, b, abs_b, c, up_pos, 64);
            up++;
            c -= b;
        }
        int16x8_t vdc1 = vdupq_n_s16(dc1), vdc2 = vdupq_n_s16(dc2);
        for (int y = up; y < dn; y++) {
            int16x8_t vc = vdupq_n_s16(c);
            for (int h = 0; h < 2; h++) {
                int16x8_t t = vsubq_s16(vc, va[h]);
                int16x8_t c1 = vminq_s16(vmaxq_s16(vaddq_s16(t, vdc1), zero), full);
                int16x8_t c2 = vminq_s16(vmaxq_s16(vaddq_s16(t, vdc2), zero), full);
                int16x8_t r = vld1q_s16(res[y] + 8 * h);
                vst1q_s16(res[y] + 8 * h,
                          vaddq_s16(r, vshrq_n_s16(vaddq_s16(c1, c2), 7 - TILE_ORDER)));
            }
            c -= b;
        }
        if (dn_pos)
            update_border_line(res[dn], abs_a, va, b, abs_b, c, 0, dn_pos);
    }

    int16_t cur = 256 * (int8_t) winding;
    int16x8_t cap = vdupq_n_s16(255);
    for (int y = 0; y < TILE_SIZE; y++) {
        cur += delta[y];
        int16x8_t vcur = vdupq_n_s16(cur);
        uint8x8_t out[2];
        for (int h = 0; h < 2; h++) {
            int16x8_t val = vaddq_s16(vld1q_s16(res[y] + 8 * h), vcur);
            val = vminq_s16(vmaxq_s16(val, vnegq_s16(val)), cap);
            out[h] = vmovn_u16(vreinterpretq_u16_s16(val));
        }
        vst1q_u8(buf, vcombine_u8(out[0], out[1]));
        buf += stride;
    }
}

void ass_merge_tile16_neon(uint8_t *restrict buf, ptrdiff_t stride,
                           const uint8_t *restrict tile)
{
    for (int y = 0; y < TILE_SIZE; y++) {
        vst1q_u8(buf, vmaxq_u8(vld1q_u8(buf), vld1q_u8(tile)));
        buf += stride;
        tile += TILE_SIZE;
    }
}
