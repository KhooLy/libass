#include "config.h"
#include "ass_compat.h"

#include <arm_neon.h>
#include <stddef.h>
#include <stdint.h>

static const int16_t zero_line[16] __attribute__((aligned(16)));

static inline const int16_t *get_line(const int16_t *ptr, size_t offs, size_t size)
{
    return offs < size ? ptr + offs : zero_line;
}

static inline uint16x8_t ld(const int16_t *p)
{
    return vreinterpretq_u16_s16(vld1q_s16(p));
}

static inline void st(int16_t *p, uint16x8_t v)
{
    vst1q_s16(p, vreinterpretq_s16_u16(v));
}

static inline uint16x8_t shrink(uint16x8_t p1p, uint16x8_t p1n, uint16x8_t z0p,
                                uint16x8_t z0n, uint16x8_t n1p, uint16x8_t n1n)
{
    uint16x8_t z = vaddq_u16(z0p, z0n);
    uint16x8_t r = vhaddq_u16(vaddq_u16(p1p, p1n), vaddq_u16(n1p, n1n));
    r = vhaddq_u16(r, z);
    r = vhaddq_u16(r, vaddq_u16(p1n, n1p));
    return vrshrq_n_u16(vhaddq_u16(r, z), 1);
}

static inline void expand(uint16x8_t p1, uint16x8_t z0, uint16x8_t n1,
                          uint16x8_t *rp, uint16x8_t *rn)
{
    uint16x8_t r = vhaddq_u16(vhaddq_u16(p1, n1), z0);
    *rp = vrhaddq_u16(vhaddq_u16(r, p1), z0);
    *rn = vrhaddq_u16(vhaddq_u16(r, n1), z0);
}

void ass_stripe_unpack16_neon(int16_t *restrict dst, const uint8_t *restrict src,
                              ptrdiff_t src_stride, size_t width, size_t height)
{
    for (size_t y = 0; y < height; y++) {
        int16_t *ptr = dst;
        for (size_t x = 0; x < width; x += 8) {
            uint16x8_t v = vmovl_u8(vld1_u8(src + x));
            uint16x8_t t = vorrq_u16(vshlq_n_u16(v, 7), vshrq_n_u16(v, 1));
            st(ptr, vshrq_n_u16(vaddq_u16(t, vdupq_n_u16(1)), 1));
            ptr += 8 * height;
        }
        dst += 8;
        src += src_stride;
    }
}

void ass_stripe_pack16_neon(uint8_t *restrict dst, ptrdiff_t dst_stride,
                            const int16_t *restrict src, size_t width, size_t height)
{
    static const int16_t dither[2][8] __attribute__((aligned(16))) = {
        {  8, 40,  8, 40,  8, 40,  8, 40 },
        { 56, 24, 56, 24, 56, 24, 56, 24 },
    };
    int16x8_t d0 = vld1q_s16(dither[0]);
    int16x8_t d1 = vld1q_s16(dither[1]);

    for (size_t x = 0; x < width; x += 8) {
        uint8_t *ptr = dst;
        for (size_t y = 0; y < height; y++) {
            int16x8_t s = vld1q_s16(src);
            int16x8_t t = vaddq_s16(vsubq_s16(s, vshrq_n_s16(s, 8)), y & 1 ? d1 : d0);
            vst1_u8(ptr, vmovn_u16(vshrq_n_u16(vreinterpretq_u16_s16(t), 6)));
            ptr += dst_stride;
            src += 8;
        }
        dst += 8;
    }
    size_t left = dst_stride - ((width + 7) & ~(size_t) 7);
    for (size_t y = 0; y < height; y++) {
        for (size_t x = 0; x < left; x++)
            dst[x] = 0;
        dst += dst_stride;
    }
}

void ass_shrink_horz16_neon(int16_t *restrict dst, const int16_t *restrict src,
                            size_t src_width, size_t src_height)
{
    size_t dst_width = (src_width + 5) >> 1;
    size_t size = ((src_width + 7) & ~(size_t) 7) * src_height;
    size_t step = 8 * src_height;

    size_t offs = 0;
    for (size_t x = 0; x < dst_width; x += 8) {
        for (size_t y = 0; y < src_height; y++) {
            uint16x8_t prev = ld(get_line(src, offs - step, size));
            uint16x8_t cur = ld(get_line(src, offs, size));
            uint16x8_t next = ld(get_line(src, offs + step, size));
            uint16x8x2_t p1 = vuzpq_u16(vextq_u16(prev, cur, 4), vextq_u16(cur, next, 4));
            uint16x8x2_t z0 = vuzpq_u16(vextq_u16(prev, cur, 6), vextq_u16(cur, next, 6));
            uint16x8x2_t n1 = vuzpq_u16(cur, next);
            st(dst, shrink(p1.val[0], p1.val[1], z0.val[0], z0.val[1],
                           n1.val[0], n1.val[1]));
            dst += 8;
            offs += 8;
        }
        offs += step;
    }
}

void ass_shrink_vert16_neon(int16_t *restrict dst, const int16_t *restrict src,
                            size_t src_width, size_t src_height)
{
    size_t dst_height = (src_height + 5) >> 1;
    size_t step = 8 * src_height;

    for (size_t x = 0; x < src_width; x += 8) {
        size_t offs = 0;
        for (size_t y = 0; y < dst_height; y++) {
            uint16x8_t p1p = ld(get_line(src, offs - 32, step));
            uint16x8_t p1n = ld(get_line(src, offs - 24, step));
            uint16x8_t z0p = ld(get_line(src, offs - 16, step));
            uint16x8_t z0n = ld(get_line(src, offs - 8, step));
            uint16x8_t n1p = ld(get_line(src, offs, step));
            uint16x8_t n1n = ld(get_line(src, offs + 8, step));
            st(dst, shrink(p1p, p1n, z0p, z0n, n1p, n1n));
            dst += 8;
            offs += 16;
        }
        src += step;
    }
}

void ass_expand_horz16_neon(int16_t *restrict dst, const int16_t *restrict src,
                            size_t src_width, size_t src_height)
{
    size_t dst_width = 2 * src_width + 4;
    size_t size = ((src_width + 7) & ~(size_t) 7) * src_height;
    size_t step = 8 * src_height;

    size_t offs = 0;
    for (size_t x = 8; x < dst_width; x += 16) {
        for (size_t y = 0; y < src_height; y++) {
            uint16x8_t prev = ld(get_line(src, offs - step, size));
            uint16x8_t cur = ld(get_line(src, offs, size));
            uint16x8_t rp, rn;
            expand(vextq_u16(prev, cur, 6), vextq_u16(prev, cur, 7), cur, &rp, &rn);
            uint16x8x2_t z = vzipq_u16(rp, rn);
            st(dst, z.val[0]);
            st(dst + step, z.val[1]);
            dst += 8;
            offs += 8;
        }
        dst += step;
    }
    if ((dst_width - 1) & 8)
        return;

    for (size_t y = 0; y < src_height; y++) {
        uint16x8_t prev = ld(get_line(src, offs - step, size));
        uint16x8_t cur = ld(get_line(src, offs, size));
        uint16x8_t rp, rn;
        expand(vextq_u16(prev, cur, 6), vextq_u16(prev, cur, 7), cur, &rp, &rn);
        st(dst, vzipq_u16(rp, rn).val[0]);
        dst += 8;
        offs += 8;
    }
}

void ass_expand_vert16_neon(int16_t *restrict dst, const int16_t *restrict src,
                            size_t src_width, size_t src_height)
{
    size_t dst_height = 2 * src_height + 4;
    size_t step = 8 * src_height;

    for (size_t x = 0; x < src_width; x += 8) {
        size_t offs = 0;
        for (size_t y = 0; y < dst_height; y += 2) {
            uint16x8_t p1 = ld(get_line(src, offs - 16, step));
            uint16x8_t z0 = ld(get_line(src, offs - 8, step));
            uint16x8_t n1 = ld(get_line(src, offs, step));
            uint16x8_t rp, rn;
            expand(p1, z0, n1, &rp, &rn);
            st(dst, rp);
            st(dst + 8, rn);
            dst += 16;
            offs += 8;
        }
        src += step;
    }
}

static inline void blur_horz(int16_t *restrict dst, const int16_t *restrict src,
                             size_t src_width, size_t src_height,
                             const int16_t *restrict param, const int n)
{
    size_t dst_width = src_width + 2 * n;
    size_t size = ((src_width + 7) & ~(size_t) 7) * src_height;
    size_t step = 8 * src_height;
    int back = (2 * n + 7) / 8;

    size_t offs = 0;
    int16_t buf[24] __attribute__((aligned(16)));
    int16_t *ptr = buf + 16;
    for (size_t x = 0; x < dst_width; x += 8) {
        for (size_t y = 0; y < src_height; y++) {
            for (int i = -back; i <= 0; i++)
                vst1q_s16(ptr + 8 * i, vld1q_s16(get_line(src, offs - (size_t) -i * step, size)));
            int16x8_t center = vld1q_s16(ptr - n);
            int32x4_t lo = vdupq_n_s32(0x8000);
            int32x4_t hi = lo;
            for (int i = n; i > 0; i--) {
                int16_t c = param[i - 1];
                int16x8_t a = vsubq_s16(vld1q_s16(ptr - n - i), center);
                int16x8_t b = vsubq_s16(vld1q_s16(ptr - n + i), center);
                lo = vmlal_n_s16(lo, vget_low_s16(a), c);
                hi = vmlal_n_s16(hi, vget_high_s16(a), c);
                lo = vmlal_n_s16(lo, vget_low_s16(b), c);
                hi = vmlal_n_s16(hi, vget_high_s16(b), c);
            }
            vst1q_s16(dst, vaddq_s16(center, vcombine_s16(vshrn_n_s32(lo, 16),
                                                           vshrn_n_s32(hi, 16))));
            dst += 8;
            offs += 8;
        }
    }
}

static inline void blur_vert(int16_t *restrict dst, const int16_t *restrict src,
                             size_t src_width, size_t src_height,
                             const int16_t *restrict param, const int n)
{
    size_t dst_height = src_height + 2 * n;
    size_t step = 8 * src_height;
    int32_t sum = 0;
    for (int i = 0; i < n; i++)
        sum += param[i];

    for (size_t x = 0; x < src_width; x += 8) {
        size_t offs = 0;
        for (size_t y = 0; y < dst_height; y++) {
            int16x8_t center;
            int32x4_t lo = vdupq_n_s32(0x8000);
            int32x4_t hi = lo;
            if (offs >= 16 * (size_t) n && offs < step) {
                const int16_t *p = src + offs - 16 * n;
                center = vld1q_s16(p + 8 * n);
                for (int i = n; i > 0; i--) {
                    int16_t c = param[i - 1];
                    int16x8_t a = vld1q_s16(p + 8 * (n - i));
                    int16x8_t b = vld1q_s16(p + 8 * (n + i));
                    lo = vmlal_n_s16(lo, vget_low_s16(a), c);
                    hi = vmlal_n_s16(hi, vget_high_s16(a), c);
                    lo = vmlal_n_s16(lo, vget_low_s16(b), c);
                    hi = vmlal_n_s16(hi, vget_high_s16(b), c);
                }
            } else {
                center = vld1q_s16(get_line(src, offs - n * 8, step));
                for (int i = n; i > 0; i--) {
                    int16_t c = param[i - 1];
                    int16x8_t a = vld1q_s16(get_line(src, offs - (n + i) * 8, step));
                    int16x8_t b = vld1q_s16(get_line(src, offs - (n - i) * 8, step));
                    lo = vmlal_n_s16(lo, vget_low_s16(a), c);
                    hi = vmlal_n_s16(hi, vget_high_s16(a), c);
                    lo = vmlal_n_s16(lo, vget_low_s16(b), c);
                    hi = vmlal_n_s16(hi, vget_high_s16(b), c);
                }
            }
            int32_t k = -2 * sum;
            lo = vmlaq_n_s32(lo, vmovl_s16(vget_low_s16(center)), k);
            hi = vmlaq_n_s32(hi, vmovl_s16(vget_high_s16(center)), k);
            vst1q_s16(dst, vaddq_s16(center, vcombine_s16(vshrn_n_s32(lo, 16),
                                                           vshrn_n_s32(hi, 16))));
            dst += 8;
            offs += 8;
        }
        src += step;
    }
}

#define BLUR(n) \
    void ass_blur ## n ## _horz16_neon(int16_t *dst, const int16_t *src, \
                                       size_t w, size_t h, const int16_t *param) \
    { \
        blur_horz(dst, src, w, h, param, n); \
    } \
    void ass_blur ## n ## _vert16_neon(int16_t *dst, const int16_t *src, \
                                       size_t w, size_t h, const int16_t *param) \
    { \
        blur_vert(dst, src, w, h, param, n); \
    }

BLUR(4)
BLUR(5)
BLUR(6)
BLUR(7)
BLUR(8)
