/*
 * Copyright (C) 2006 Evgeniy Stepanov <eugeni.stepanov@gmail.com>
 *
 * This file is part of libass.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "config.h"
#include "ass_compat.h"

#include <assert.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <string.h>
#include <stdbool.h>

#ifdef CONFIG_UNIBREAK
#include <linebreak.h>
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "ass.h"
#include "ass_outline.h"
#include "ass_render.h"
#include "ass_parse.h"
#include "ass_priv.h"
#include "ass_shaper.h"

#define MAX_GLYPHS_INITIAL 1024
#define MAX_LINES_INITIAL 64
#define MAX_BITMAPS_INITIAL 16
#define MAX_SUB_BITMAPS_INITIAL 64
#define MAX_LAYER_SORT_RANGE 64
#define SUBPIXEL_MASK 63
#define STROKER_PRECISION 16     // stroker error in integer units, unrelated to final accuracy
#define RASTERIZER_PRECISION 16  // rasterizer spline approximation error in 1/64 pixel units
#define POSITION_PRECISION 8.0   // rough estimate of transform error in 1/64 pixel units
#define MAX_PERSP_SCALE 16.0
#define SUBPIXEL_ORDER 3  // ~ log2(64 / POSITION_PRECISION)
#define BLUR_PRECISION (1.0 / 256)  // blur error as fraction of full input range
#define MOTION_CACHE_SUBPIXEL_STEP 2  // snap 1/8-pixel phases to a 1/4-pixel grid


static bool text_info_init(TextInfo* text_info)
{
    text_info->max_bitmaps = MAX_BITMAPS_INITIAL;
    text_info->max_glyphs = MAX_GLYPHS_INITIAL;
    text_info->max_lines = MAX_LINES_INITIAL;
    text_info->n_bitmaps = 0;
    text_info->combined_bitmaps = calloc(MAX_BITMAPS_INITIAL, sizeof(CombinedBitmapInfo));
    text_info->glyphs = calloc(MAX_GLYPHS_INITIAL, sizeof(GlyphInfo));
    text_info->event_text = calloc(MAX_GLYPHS_INITIAL, sizeof(FriBidiChar));
    text_info->breaks = malloc(MAX_GLYPHS_INITIAL);
    text_info->lines = calloc(MAX_LINES_INITIAL, sizeof(LineInfo));

    if (!text_info->combined_bitmaps || !text_info->glyphs || !text_info->lines ||
        !text_info->breaks || !text_info->event_text)
        return false;

    return true;
}

static ImagePool *image_pool_create(void);
static void image_pool_renderer_done(ImagePool *pool);
static bool event_has_hard_overrides(ASS_Renderer *render_priv,
                                     ASS_Event *event);

// Static-event layout cache — see ass_render.h:LayoutSnapshot.
static void layout_cache_flush(ASS_Renderer *priv);
static void layout_cache_prune(ASS_Renderer *priv, uint64_t oldest_frame);
static ASS_RenderPriv *get_render_priv(ASS_Renderer *render_priv, ASS_Event *event);

#if ENABLE_THREADS
static inline void layout_cache_lock(ASS_Renderer *priv)
{
    if (priv->layout_cache_mutex_inited)
        pthread_mutex_lock(&priv->layout_cache_mutex);
}
static inline void layout_cache_unlock(ASS_Renderer *priv)
{
    if (priv->layout_cache_mutex_inited)
        pthread_mutex_unlock(&priv->layout_cache_mutex);
}
#else
#define layout_cache_lock(priv) ((void) (priv))
#define layout_cache_unlock(priv) ((void) (priv))
#endif

static void text_info_done(TextInfo* text_info)
{
    if (text_info->combined_bitmaps)
        for (int i = 0; i < text_info->max_bitmaps; i++)
            free(text_info->combined_bitmaps[i].bitmaps);
    free(text_info->glyphs);
    free(text_info->event_text);
    free(text_info->breaks);
    free(text_info->lines);
    free(text_info->combined_bitmaps);
}

static bool render_context_init(RenderContext *state, ASS_Renderer *priv)
{
    state->renderer = priv;

    if (!text_info_init(&state->text_info))
        return false;

    state->cache_client = ass_cache_client_create(&priv->cache.client_set);
    if (!state->cache_client)
        return false;

    if (!(state->shaper = ass_shaper_new(priv->cache.metrics_cache, priv->cache.face_size_metrics_cache, state->cache_client)))
        return false;

    state->image_pool = image_pool_create();  // NULL => fall back to malloc

    return ass_rasterizer_init(&priv->engine, &state->rasterizer, RASTERIZER_PRECISION);
}

static void render_context_done(RenderContext *state)
{
    ass_rasterizer_done(&state->rasterizer);

    if (state->shaper)
        ass_shaper_free(state->shaper);

    text_info_done(&state->text_info);

    image_pool_renderer_done(state->image_pool);
    state->image_pool = NULL;
}

ASS_Renderer *ass_renderer_init(ASS_Library *library)
{
    int error;
    FT_Library ft;
    ASS_Renderer *priv = 0;
    int vmajor, vminor, vpatch;

    ass_msg(library, MSGL_INFO, "libass API version: 0x%X", LIBASS_VERSION);
    ass_msg(library, MSGL_INFO, "libass source: %s", CONFIG_SOURCEVERSION);

    error = FT_Init_FreeType(&ft);
    if (error) {
        ass_msg(library, MSGL_FATAL, "%s failed", "FT_Init_FreeType");
        goto fail;
    }

    FT_Library_Version(ft, &vmajor, &vminor, &vpatch);
    ass_msg(library, MSGL_V, "Raster: FreeType %d.%d.%d",
           vmajor, vminor, vpatch);

    priv = calloc(1, sizeof(ASS_Renderer));
    if (!priv) {
        FT_Done_FreeType(ft);
        goto fail;
    }

    priv->library = library;
    priv->ftlibrary = ft;
    // images_root and related stuff is zero-filled in calloc

    unsigned flags = ASS_CPU_FLAG_ALL;
#if CONFIG_LARGE_TILES
    flags |= ASS_FLAG_LARGE_TILES;
#endif
    priv->engine = ass_bitmap_engine_init(flags);

    priv->cache.font_cache = ass_font_cache_create();
    priv->cache.bitmap_cache = ass_bitmap_cache_create();
    priv->cache.composite_cache = ass_composite_cache_create();
    priv->cache.outline_cache = ass_outline_cache_create();
    priv->cache.face_size_metrics_cache = ass_face_size_metrics_cache_create();
    priv->cache.metrics_cache = ass_glyph_metrics_cache_create();
    if (!priv->cache.font_cache || !priv->cache.bitmap_cache ||
        !priv->cache.composite_cache || !priv->cache.outline_cache ||
        !priv->cache.face_size_metrics_cache || !priv->cache.metrics_cache)
        goto fail;

    if (!ass_cache_client_set_init(&priv->cache.client_set))
        goto fail;

    const char *glyph_str = getenv("LIBASS_GLYPH_CACHE_MAX");
    const char *bitmap_str = getenv("LIBASS_BITMAP_CACHE_MAX");
    int glyph_size = glyph_str ? atoi(glyph_str) : 0;
    int bitmap_size = bitmap_str ? atoi(bitmap_str) : 0;

    ass_set_cache_limits(priv, glyph_size, bitmap_size);

    if (!render_context_init(&priv->state, priv))
        goto fail;

    priv->user_override_style.Name = "OverrideStyle"; // name insignificant

    priv->settings.font_size_coeff = 1.;
    priv->settings.selective_style_overrides = ASS_OVERRIDE_BIT_SELECTIVE_FONT_SCALE;

    ass_shaper_info(library);
    priv->settings.shaper = ASS_SHAPING_COMPLEX;

#if ENABLE_THREADS
    priv->settings.threads = default_threads();

    if (pthread_mutex_init(&priv->mutex, NULL) != 0)
        goto thread_fail;

    priv->mutex_inited = true;

    // Must succeed before threads run: layout_cache_lock is a no-op while
    // uninited, so failing here without disabling threading would leave the
    // snapshot registry unsynchronized across workers. goto thread_fail forces
    // the serial path, where the no-op lock is safe.
    if (pthread_mutex_init(&priv->layout_cache_mutex, NULL) != 0)
        goto thread_fail;
    priv->layout_cache_mutex_inited = true;

    if (pthread_cond_init(&priv->main_cond, NULL) != 0)
        goto thread_fail;

    priv->main_cond_inited = true;

    if (pthread_cond_init(&priv->pool_cond, NULL) != 0)
        goto thread_fail;

    priv->pool_cond_inited = true;

    atomic_init(&priv->processing_eimgs, 0);
    atomic_init(&priv->sent_eimgs, 0);
    atomic_init(&priv->next_eimg, 0);

thread_fail:
    if (!priv->pool_cond_inited) {
        ass_msg(library, MSGL_WARN, "Failed to initialize threading functionality; disabling");
        priv->thread_start_failed = 1;
    }
#endif

    ass_msg(library, MSGL_V, "Initialized");

    return priv;

fail:
    ass_msg(library, MSGL_ERR, "Initialization failed");
    ass_renderer_done(priv);

    return NULL;
}

#if ENABLE_THREADS
static void join_threads(ASS_Renderer *render_priv)
{
    if (render_priv->threads) {
        pthread_mutex_lock(&render_priv->mutex);
        render_priv->shutting_down = 1;
        pthread_mutex_unlock(&render_priv->mutex);
        pthread_cond_broadcast(&render_priv->pool_cond);

        for (unsigned i = 0; i < render_priv->n_threads; i++)
            pthread_join(render_priv->threads[i], NULL);

        free(render_priv->threads);

        render_priv->threads = NULL;
        render_priv->n_threads = 0;
        render_priv->started_threads = 0;
    }
}
#endif

void ass_renderer_done(ASS_Renderer *render_priv)
{
    if (!render_priv)
        return;

#if ENABLE_THREADS
    join_threads(render_priv);

    if (render_priv->mutex_inited)
        pthread_mutex_destroy(&render_priv->mutex);
    if (render_priv->main_cond_inited)
        pthread_cond_destroy(&render_priv->main_cond);
    if (render_priv->pool_cond_inited)
        pthread_cond_destroy(&render_priv->pool_cond);
#endif

    ass_frame_unref(render_priv->images_root);
    ass_frame_unref(render_priv->prev_images_root);

    // Free all live layout snapshots (dec_ref'ing held outline/font refs) while
    // the caches are still alive, then drop the registry mutex.
    layout_cache_flush(render_priv);
#if ENABLE_THREADS
    if (render_priv->layout_cache_mutex_inited)
        pthread_mutex_destroy(&render_priv->layout_cache_mutex);
#endif

    render_context_done(&render_priv->state);

    ass_cache_done(render_priv->cache.composite_cache);
    ass_cache_done(render_priv->cache.bitmap_cache);
    ass_cache_done(render_priv->cache.outline_cache);
    ass_cache_done(render_priv->cache.face_size_metrics_cache);
    ass_cache_done(render_priv->cache.metrics_cache);
    ass_cache_done(render_priv->cache.font_cache);
    ass_cache_client_set_done(&render_priv->cache.client_set);

    if (render_priv->fontselect)
        ass_fontselect_free(render_priv->fontselect);
    if (render_priv->ftlibrary)
        FT_Done_FreeType(render_priv->ftlibrary);
    free(render_priv->eimg);
    free(render_priv->eimg_sorted);

    free(render_priv->settings.default_font);
    free(render_priv->settings.default_family);

    free(render_priv->user_override_style.FontName);

    free(render_priv);
}

/**
 * \brief Create a new ASS_Image
 * Parameters are the same as ASS_Image fields.
 */
static ImagePool *image_pool_create(void)
{
    ImagePool *pool = calloc(1, sizeof(ImagePool));
    if (pool) {
#if ENABLE_THREADS
        if (pthread_mutex_init(&pool->mutex, NULL) != 0) {
            free(pool);
            return NULL;
        }
#endif
        pool->renderer_alive = true;
    }
    return pool;
}

#if ENABLE_THREADS
static inline void image_pool_lock(ImagePool *pool)
{
    pthread_mutex_lock(&pool->mutex);
}

static inline void image_pool_unlock(ImagePool *pool)
{
    pthread_mutex_unlock(&pool->mutex);
}
#else
static inline void image_pool_lock(ImagePool *pool) { (void) pool; }
static inline void image_pool_unlock(ImagePool *pool) { (void) pool; }
#endif

// Called with the mutex held after dropping renderer or image ownership.
// Only the final owner can see no renderer or live images and claim destruction;
// callers must not access the pool after this releases the mutex.
static void image_pool_unlock_or_destroy(ImagePool *pool)
{
    if (pool->renderer_alive || pool->live) {
        image_pool_unlock(pool);
        return;
    }

    ASS_ImagePriv *cur = pool->free_list;
    pool->free_list = NULL;
    image_pool_unlock(pool);

#if ENABLE_THREADS
    pthread_mutex_destroy(&pool->mutex);
#endif

    while (cur) {
        ASS_ImagePriv *next = (ASS_ImagePriv *) cur->result.next;
        free(cur);
        cur = next;
    }
    free(pool);
}

static void image_pool_renderer_done(ImagePool *pool)
{
    if (!pool)
        return;

    image_pool_lock(pool);
    pool->renderer_alive = false;
    image_pool_unlock_or_destroy(pool);
}

// Obtain an ASS_ImagePriv shell, reusing a recycled one when available.
static ASS_ImagePriv *image_pool_alloc(ImagePool *pool)
{
    ASS_ImagePriv *img;

    if (pool) {
        image_pool_lock(pool);
        if (pool->free_list) {
            img = pool->free_list;
            pool->free_list = (ASS_ImagePriv *) img->result.next;
        } else {
            img = malloc(sizeof(ASS_ImagePriv));
            if (!img) {
                image_pool_unlock(pool);
                return NULL;
            }
        }
        pool->live++;
        image_pool_unlock(pool);
    } else {
        img = malloc(sizeof(ASS_ImagePriv));
        if (!img)
            return NULL;
    }

    img->pool = pool;
    img->shared = NULL;   // only clone_image_snapshot sets this; recycled shells may be stale
    return img;
}

static ASS_Image *my_draw_bitmap(ImagePool *pool, unsigned char *bitmap,
                                 int bitmap_w, int bitmap_h, int stride,
                                 int dst_x, int dst_y, uint32_t color,
                                 CompositeHashValue *source)
{
    ASS_ImagePriv *img = image_pool_alloc(pool);
    if (!img) {
        if (!source)
            ass_aligned_free(bitmap);
        return NULL;
    }

    img->result.w = bitmap_w;
    img->result.h = bitmap_h;
    img->result.stride = stride;
    img->result.bitmap = bitmap;
    img->result.color = color;
    img->result.dst_x = dst_x;
    img->result.dst_y = dst_y;

    img->source = source;
    ass_cache_inc_ref(source);
    img->buffer = source ? NULL : bitmap;
    img->ref_count = 0;

    return &img->result;
}

// Ref-counted read-only pixel buffer backing cloned owned-buffer images (vector
// clip / opaque-box background) in the layout cache. The capturing snapshot holds
// one ref; each clone holds one ref; the buffer (and this header) is freed only
// when the last ref drops. So a shared master always outlives every emitted image
// that points into it, even if the snapshot is freed (gen bump / teardown) while
// the user still holds a rendered frame — matching how emitted images already
// outlive their source composite. refs is atomic to match the multi-threaded
// render/free of pooled images.
struct shared_buffer {
    _Atomic AtomicInt refs;
    unsigned char *data;
};

static SharedBuffer *shared_buffer_new(unsigned align,
                                       const unsigned char *src, size_t size)
{
    SharedBuffer *sb = malloc(sizeof(*sb));
    if (!sb)
        return NULL;
    sb->data = ass_aligned_alloc(align, size, false);
    if (!sb->data) {
        free(sb);
        return NULL;
    }
    memcpy(sb->data, src, size);
    atomic_init(&sb->refs, 1);
    return sb;
}

static void shared_buffer_ref(SharedBuffer *sb)
{
    inc_ref(&sb->refs);
}

static void shared_buffer_unref(SharedBuffer *sb)
{
    if (dec_ref(&sb->refs) == 0) {
        ass_aligned_free(sb->data);
        free(sb);
    }
}

/**
 * \brief Mapping between script and screen coordinates
 */
static double x2scr_pos(ASS_Renderer *render_priv, double x)
{
    return x * render_priv->frame_content_width / render_priv->par_scale_x / render_priv->track->PlayResX +
        render_priv->settings.left_margin;
}
static double x2scr_left(RenderContext *state, double x)
{
    ASS_Renderer *render_priv = state->renderer;
    if (state->explicit || !render_priv->settings.use_margins)
        return x2scr_pos(render_priv, x);
    return x * render_priv->fit_width / render_priv->par_scale_x /
        render_priv->track->PlayResX;
}
static double x2scr_right(RenderContext *state, double x)
{
    ASS_Renderer *render_priv = state->renderer;
    if (state->explicit || !render_priv->settings.use_margins)
        return x2scr_pos(render_priv, x);
    return x * render_priv->fit_width / render_priv->par_scale_x /
        render_priv->track->PlayResX +
        (render_priv->width - render_priv->fit_width);
}
static double x2scr_pos_scaled(ASS_Renderer *render_priv, double x)
{
    return x * render_priv->frame_content_width / render_priv->track->PlayResX +
        render_priv->settings.left_margin;
}
/**
 * \brief Mapping between script and screen coordinates
 */
static double y2scr_pos(ASS_Renderer *render_priv, double y)
{
    return y * render_priv->frame_content_height / render_priv->track->PlayResY +
        render_priv->settings.top_margin;
}
static double y2scr(RenderContext *state, double y)
{
    ASS_Renderer *render_priv = state->renderer;
    if (state->explicit || !render_priv->settings.use_margins)
        return y2scr_pos(render_priv, y);
    return y * render_priv->fit_height /
        render_priv->track->PlayResY +
        (render_priv->height - render_priv->fit_height) * 0.5;
}

// the same for toptitles
static double y2scr_top(RenderContext *state, double y)
{
    ASS_Renderer *render_priv = state->renderer;
    if (state->explicit || !render_priv->settings.use_margins)
        return y2scr_pos(render_priv, y);
    return y * render_priv->fit_height /
        render_priv->track->PlayResY;
}
// the same for subtitles
static double y2scr_sub(RenderContext *state, double y)
{
    ASS_Renderer *render_priv = state->renderer;
    if (state->explicit || !render_priv->settings.use_margins)
        return y2scr_pos(render_priv, y);
    return y * render_priv->fit_height /
        render_priv->track->PlayResY +
        (render_priv->height - render_priv->fit_height);
}

/*
 * \brief Convert bitmap glyphs into ASS_Image list with inverse clipping
 *
 * Inverse clipping with the following strategy:
 * - find rectangle from (x0, y0) to (cx0, y1)
 * - find rectangle from (cx0, y0) to (cx1, cy0)
 * - find rectangle from (cx0, cy1) to (cx1, y1)
 * - find rectangle from (cx1, y0) to (x1, y1)
 * These rectangles can be invalid and in this case are discarded.
 * Afterwards, they are clipped against the screen coordinates.
 * In an additional pass, the rectangles need to be split up left/right for
 * karaoke effects.  This can result in a lot of bitmaps (6 to be exact).
 */
static ASS_Image **render_glyph_i(RenderContext *state,
                                  Bitmap *bm, int dst_x, int dst_y,
                                  uint32_t color, uint32_t color2, int brk,
                                  ASS_Image **tail, unsigned type,
                                  CompositeHashValue *source)
{
    ASS_Renderer *render_priv = state->renderer;
    int i, j, x0, y0, x1, y1, cx0, cy0, cx1, cy1, sx, sy, zx, zy;
    Rect r[4];
    ASS_Image *img;

    dst_x += bm->left;
    dst_y += bm->top;
    brk -= dst_x;

    // we still need to clip against screen boundaries
    zx = x2scr_pos_scaled(render_priv, 0);
    zy = y2scr_pos(render_priv, 0);
    sx = x2scr_pos_scaled(render_priv, render_priv->track->PlayResX);
    sy = y2scr_pos(render_priv, render_priv->track->PlayResY);

    x0 = 0;
    y0 = 0;
    x1 = bm->w;
    y1 = bm->h;
    cx0 = state->clip_x0 - dst_x;
    cy0 = state->clip_y0 - dst_y;
    cx1 = state->clip_x1 - dst_x;
    cy1 = state->clip_y1 - dst_y;

    // calculate rectangles and discard invalid ones while we're at it.
    i = 0;
    r[i].x0 = x0;
    r[i].y0 = y0;
    r[i].x1 = (cx0 > x1) ? x1 : cx0;
    r[i].y1 = y1;
    if (r[i].x1 > r[i].x0 && r[i].y1 > r[i].y0) i++;
    r[i].x0 = (cx0 < 0) ? x0 : cx0;
    r[i].y0 = y0;
    r[i].x1 = (cx1 > x1) ? x1 : cx1;
    r[i].y1 = (cy0 > y1) ? y1 : cy0;
    if (r[i].x1 > r[i].x0 && r[i].y1 > r[i].y0) i++;
    r[i].x0 = (cx0 < 0) ? x0 : cx0;
    r[i].y0 = (cy1 < 0) ? y0 : cy1;
    r[i].x1 = (cx1 > x1) ? x1 : cx1;
    r[i].y1 = y1;
    if (r[i].x1 > r[i].x0 && r[i].y1 > r[i].y0) i++;
    r[i].x0 = (cx1 < 0) ? x0 : cx1;
    r[i].y0 = y0;
    r[i].x1 = x1;
    r[i].y1 = y1;
    if (r[i].x1 > r[i].x0 && r[i].y1 > r[i].y0) i++;

    // clip each rectangle to screen coordinates
    for (j = 0; j < i; j++) {
        r[j].x0 = (r[j].x0 + dst_x < zx) ? zx - dst_x : r[j].x0;
        r[j].y0 = (r[j].y0 + dst_y < zy) ? zy - dst_y : r[j].y0;
        r[j].x1 = (r[j].x1 + dst_x > sx) ? sx - dst_x : r[j].x1;
        r[j].y1 = (r[j].y1 + dst_y > sy) ? sy - dst_y : r[j].y1;
    }

    // draw the rectangles
    for (j = 0; j < i; j++) {
        int lbrk = brk;
        // kick out rectangles that are invalid now
        if (r[j].x1 <= r[j].x0 || r[j].y1 <= r[j].y0)
            continue;
        // split up into left and right for karaoke, if needed
        if (lbrk > r[j].x0) {
            if (lbrk > r[j].x1) lbrk = r[j].x1;
            if (_a(color) != 0xFF) {
                img = my_draw_bitmap(state->image_pool, bm->buffer + r[j].y0 * bm->stride + r[j].x0,
                                     lbrk - r[j].x0, r[j].y1 - r[j].y0, bm->stride,
                                     dst_x + r[j].x0, dst_y + r[j].y0, color, source);
                if (!img) break;
                img->type = type;
                *tail = img;
                tail = &img->next;
            }
        }
        if (lbrk < r[j].x1) {
            if (lbrk < r[j].x0) lbrk = r[j].x0;
            if (_a(color2) != 0xFF) {
                img = my_draw_bitmap(state->image_pool, bm->buffer + r[j].y0 * bm->stride + lbrk,
                                     r[j].x1 - lbrk, r[j].y1 - r[j].y0, bm->stride,
                                     dst_x + lbrk, dst_y + r[j].y0, color2, source);
                if (!img) break;
                img->type = type;
                *tail = img;
                tail = &img->next;
            }
        }
    }

    return tail;
}

/**
 * \brief convert bitmap glyph into ASS_Image struct(s)
 * \param bit freetype bitmap glyph, FT_PIXEL_MODE_GRAY
 * \param dst_x bitmap x coordinate in video frame
 * \param dst_y bitmap y coordinate in video frame
 * \param color first color, RGBA
 * \param color2 second color, RGBA
 * \param brk x coordinate relative to glyph origin, color is used to the left of brk, color2 - to the right
 * \param tail pointer to the last image's next field, head of the generated list should be stored here
 * \return pointer to the new list tail
 * Performs clipping. Uses my_draw_bitmap for actual bitmap conversion.
 */
static ASS_Image **
render_glyph(RenderContext *state, Bitmap *bm, int dst_x, int dst_y,
             uint32_t color, uint32_t color2, int brk, ASS_Image **tail,
             unsigned type, CompositeHashValue *source)
{
    // Inverse clipping in use?
    if (state->clip_mode)
        return render_glyph_i(state, bm, dst_x, dst_y, color, color2,
                              brk, tail, type, source);

    // brk is absolute
    // color = color left of brk
    // color2 = color right of brk
    int b_x0, b_y0, b_x1, b_y1; // visible part of the bitmap
    int clip_x0, clip_y0, clip_x1, clip_y1;
    int tmp;
    ASS_Image *img;
    ASS_Renderer *render_priv = state->renderer;

    dst_x += bm->left;
    dst_y += bm->top;
    brk -= dst_x;

    // clipping
    clip_x0 = FFMINMAX(state->clip_x0, 0, render_priv->width);
    clip_y0 = FFMINMAX(state->clip_y0, 0, render_priv->height);
    clip_x1 = FFMINMAX(state->clip_x1, 0, render_priv->width);
    clip_y1 = FFMINMAX(state->clip_y1, 0, render_priv->height);
    b_x0 = 0;
    b_y0 = 0;
    b_x1 = bm->w;
    b_y1 = bm->h;

    tmp = dst_x - clip_x0;
    if (tmp < 0)
        b_x0 = -tmp;
    tmp = dst_y - clip_y0;
    if (tmp < 0)
        b_y0 = -tmp;
    tmp = clip_x1 - dst_x - bm->w;
    if (tmp < 0)
        b_x1 = bm->w + tmp;
    tmp = clip_y1 - dst_y - bm->h;
    if (tmp < 0)
        b_y1 = bm->h + tmp;

    if ((b_y0 >= b_y1) || (b_x0 >= b_x1))
        return tail;

    if (brk > b_x0) {           // draw left part
        if (brk > b_x1)
            brk = b_x1;
        if (_a(color) != 0xFF) {
            img = my_draw_bitmap(state->image_pool, bm->buffer + bm->stride * b_y0 + b_x0,
                                 brk - b_x0, b_y1 - b_y0, bm->stride,
                                 dst_x + b_x0, dst_y + b_y0, color, source);
            if (!img) return tail;
            img->type = type;
            *tail = img;
            tail = &img->next;
        }
    }
    if (brk < b_x1) {           // draw right part
        if (brk < b_x0)
            brk = b_x0;
        if (_a(color2) != 0xFF) {
            img = my_draw_bitmap(state->image_pool, bm->buffer + bm->stride * b_y0 + brk,
                                 b_x1 - brk, b_y1 - b_y0, bm->stride,
                                 dst_x + brk, dst_y + b_y0, color2, source);
            if (!img) return tail;
            img->type = type;
            *tail = img;
            tail = &img->next;
        }
    }
    return tail;
}

static bool quantize_transform(double m[3][3], ASS_Vector *pos,
                               ASS_DVector *offset, bool first,
                               BitmapHashKey *key)
{
    // Full transform:
    // x_out = (m_xx * x + m_xy * y + m_xz) / z,
    // y_out = (m_yx * x + m_yy * y + m_yz) / z,
    // z     =  m_zx * x + m_zy * y + m_zz.

    const double max_val = 1000000;

    const ASS_Rect *bbox = &key->outline->cbox;
    double x0 = (bbox->x_min + bbox->x_max) / 2.0;
    double y0 = (bbox->y_min + bbox->y_max) / 2.0;
    double dx = (bbox->x_max - bbox->x_min) / 2.0 + 64;
    double dy = (bbox->y_max - bbox->y_min) / 2.0 + 64;
    bool affine = m[2][0] == 0. && m[2][1] == 0.;

    // Change input coordinates' origin to (x0, y0),
    // after that transformation x:[-dx, dx], y:[-dy, dy],
    // max|x| = dx and max|y| = dy.
    for (int i = 0; i < 3; i++)
        m[i][2] += m[i][0] * x0 + m[i][1] * y0;

    if (m[2][2] <= 0)
        return false;

    double w = 1 / m[2][2];
    // Transformed center of bounding box
    double center[2] = { m[0][2] * w, m[1][2] * w };
    // Change output coordinates' origin to center,
    // m_xz and m_yz is skipped as it becomes 0 and no longer needed.
    if (!affine) {
        for (int i = 0; i < 2; i++)
            for (int j = 0; j < 2; j++)
                m[i][j] -= m[2][j] * center[i];
    }

    double delta[2] = {0};
    if (!first) {
        delta[0] = offset->x;
        delta[1] = offset->y;
    }

    int32_t qr[2];  // quantized center position
    for (int i = 0; i < 2; i++) {
        center[i] /= 64 >> SUBPIXEL_ORDER;
        center[i] -= delta[i];
        if (!(fabs(center[i]) < max_val))
            return false;
        qr[i] = ass_lrint(center[i]);
    }

    // Minimal bounding box z coordinate
    double z0 = affine ? m[2][2] :
        m[2][2] - fabs(m[2][0]) * dx - fabs(m[2][1]) * dy;
    // z0 clamped to z_center / MAX_PERSP_SCALE to mitigate problems with small z
    w = 1.0 / POSITION_PRECISION /
        (affine ? z0 : FFMAX(z0, m[2][2] / MAX_PERSP_SCALE));
    double mul[2] = { dx * w, dy * w };  // 1 / q_x, 1 / q_y

    // z0 = m_zz - |m_zx| * dx - |m_zy| * dy,
    // m_zz = z0 + |m_zx| * dx + |m_zy| * dy,
    // z = m_zx * x + m_zy * y + m_zz
    //  = m_zx * (x + sign(m_zx) * dx) + m_zy * (y + sign(m_zy) * dy) + z0.

    // Let D(f) denote the absolute error of a quantity f.
    // Our goal is to determine tolerable error for matrix coefficients,
    // so that the total error of the output x_out, y_out is still acceptable.
    // As glyph dimensions are usually larger than a couple of pixels, errors
    // will be relatively small and we can use first order approximation.

    // z0 is effectively a scale factor and can thus be treated as a constant.
    // Error of constants is obviously zero, so:  D(dx) = D(dy) = D(z0) = 0.
    // For arbitrary quantities A, B, C with C not zero, the following holds true:
    //   D(A * B) <= D(A) * max|B| + max|A| * D(B),
    //   D(1 / C) <= D(C) * max|1 / C^2|.
    // Write ~ for 'same magnitude' and ~= for 'approximately'.

    // D(x_out) = D((m_xx * x + m_xy * y) / z)
    //  <= D(m_xx * x + m_xy * y) * max|1 / z| + max|m_xx * x + m_xy * y| * D(1 / z)
    //  <= (D(m_xx) * dx + D(m_xy) * dy) / z0 + (|m_xx| * dx + |m_xy| * dy) * D(z) / z0^2,
    // D(y_out) = D((m_yx * x + m_yy * y) / z)
    //  <= D(m_yx * x + m_yy * y) * max|1 / z| + max|m_yx * x + m_yy * y| * D(1 / z)
    //  <= (D(m_yx) * dx + D(m_yy) * dy) / z0 + (|m_yx| * dx + |m_yy| * dy) * D(z) / z0^2,
    // |m_xx| * dx + |m_xy| * dy = x_lim,
    // |m_yx| * dx + |m_yy| * dy = y_lim,
    // D(z) <= 2 * (D(m_zx) * dx + D(m_zy) * dy),
    // D(x_out) <= (D(m_xx) * dx + D(m_xy) * dy) / z0
    //       + 2 * (D(m_zx) * dx + D(m_zy) * dy) * x_lim / z0^2,
    // D(y_out) <= (D(m_yx) * dx + D(m_yy) * dy) / z0
    //       + 2 * (D(m_zx) * dx + D(m_zy) * dy) * y_lim / z0^2.

    // To estimate acceptable error in a matrix coefficient, pick ACCURACY for this substep,
    // set error in all other coefficients to zero and solve the system
    // D(x_out) <= ACCURACY, D(y_out) <= ACCURACY for desired D(m_ij).
    // Note that ACCURACY isn't equal to total error.
    // Total error is larger than each ACCURACY, but still of the same magnitude.
    // Via our choice of ACCURACY, we get a total error of up to several POSITION_PRECISION.

    // Quantization steps (pick: ACCURACY = POSITION_PRECISION):
    // D(m_xx), D(m_yx) ~ q_x = POSITION_PRECISION * z0 / dx,
    // D(m_xy), D(m_yy) ~ q_y = POSITION_PRECISION * z0 / dy,
    // qm_xx = round(m_xx / q_x), qm_xy = round(m_xy / q_y),
    // qm_yx = round(m_yx / q_x), qm_yy = round(m_yy / q_y).

    int32_t qm[3][2];
    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 2; j++) {
            double val = m[i][j] * mul[j];
            if (!(fabs(val) < max_val))
                return false;
            qm[i][j] = ass_lrint(val);
        }

    // x_lim = |m_xx| * dx + |m_xy| * dy
    //  ~= |qm_xx| * q_x * dx + |qm_xy| * q_y * dy
    //  = (|qm_xx| + |qm_xy|) * POSITION_PRECISION * z0,
    // y_lim = |m_yx| * dx + |m_yy| * dy
    //  ~= |qm_yx| * q_x * dx + |qm_yy| * q_y * dy
    //  = (|qm_yx| + |qm_yy|) * POSITION_PRECISION * z0,
    // max(x_lim, y_lim) / z0 ~= w
    //  = max(|qm_xx| + |qm_xy|, |qm_yx| + |qm_yy|) * POSITION_PRECISION.

    // Quantization steps (pick: ACCURACY = 2 * POSITION_PRECISION):
    // D(m_zx) ~ POSITION_PRECISION * z0^2 / max(x_lim, y_lim) / dx ~= q_zx = q_x / w,
    // D(m_zy) ~ POSITION_PRECISION * z0^2 / max(x_lim, y_lim) / dy ~= q_zy = q_y / w,
    // qm_zx = round(m_zx / q_zx), qm_zy = round(m_zy / q_zy).

    int32_t qmx = abs(qm[0][0]) + abs(qm[0][1]);
    int32_t qmy = abs(qm[1][0]) + abs(qm[1][1]);
    w = POSITION_PRECISION * FFMAX(qmx, qmy);
    mul[0] *= w;
    mul[1] *= w;

    if (affine) {
        qm[2][0] = qm[2][1] = 0;
    } else {
        for (int j = 0; j < 2; j++) {
            double val = m[2][j] * mul[j];
            if (!(fabs(val) < max_val))
                return false;
            qm[2][j] = ass_lrint(val);
        }
    }

    if (first && offset) {
        offset->x = center[0] - qr[0];
        offset->y = center[1] - qr[1];
    }
    *pos = (ASS_Vector) {
        .x = qr[0] >> SUBPIXEL_ORDER,
        .y = qr[1] >> SUBPIXEL_ORDER,
    };
    key->offset.x = qr[0] & ((1 << SUBPIXEL_ORDER) - 1);
    key->offset.y = qr[1] & ((1 << SUBPIXEL_ORDER) - 1);
    key->matrix_x.x = qm[0][0];  key->matrix_x.y = qm[0][1];
    key->matrix_y.x = qm[1][0];  key->matrix_y.y = qm[1][1];
    key->matrix_z.x = qm[2][0];  key->matrix_z.y = qm[2][1];
    return true;
}

static inline double scale_by_pow2(double value, int exponent)
{
    // These small exponents cover normal subtitle scaling. Multiplication by
    // an exactly represented power of two has the same result as ldexp on a
    // binary floating-point implementation, but avoids an out-of-line libm
    // call on platforms without a scale opcode.
#if FLT_RADIX == 2
    switch (exponent) {
    case -4: return value * 0x1p-4;
    case -3: return value * 0x1p-3;
    case -2: return value * 0x1p-2;
    case -1: return value * 0x1p-1;
    case  0: return value;
    case  1: return value * 0x1p1;
    case  2: return value * 0x1p2;
    case  3: return value * 0x1p3;
    case  4: return value * 0x1p4;
    default: break;
    }
#endif
    return ldexp(value, exponent);
}

static int32_t round_to_step(int32_t value, int32_t step)
{
    if (step <= 1)
        return value;

    int64_t v = value;
    if (v >= 0)
        return (int32_t) (((v + step / 2) / step) * step);
    return (int32_t) (-(((-v + step / 2) / step) * step));
}

static void snap_subpixel_offset(int32_t *offset, int *pos)
{
    int32_t snapped = round_to_step(*offset, MOTION_CACHE_SUBPIXEL_STEP);
    if (snapped >= (1 << SUBPIXEL_ORDER)) {
        snapped -= 1 << SUBPIXEL_ORDER;
        (*pos)++;
    }
    *offset = snapped;
}

static void snap_motion_cache_key(BitmapHashKey *key, ASS_Vector *pos)
{
    snap_subpixel_offset(&key->offset.x, &pos->x);
    snap_subpixel_offset(&key->offset.y, &pos->y);
}

static bool motion_cache_enabled(RenderContext *state, GlyphInfo *info)
{
    return (state->renderer->track->parser_priv->feature_flags &
            FEATURE_MASK(ASS_FEATURE_MOTION_CACHE)) &&
           (info->blur > 0 || info->be);
}

static void restore_transform(double m[3][3], const BitmapHashKey *key)
{
    const ASS_Rect *bbox = &key->outline->cbox;
    double x0 = (bbox->x_min + bbox->x_max) / 2.0;
    double y0 = (bbox->y_min + bbox->y_max) / 2.0;
    double dx = (bbox->x_max - bbox->x_min) / 2.0 + 64;
    double dy = (bbox->y_max - bbox->y_min) / 2.0 + 64;

    // Arbitrary scale has chosen so that z0 = 1
    double q_x = POSITION_PRECISION / dx;
    double q_y = POSITION_PRECISION / dy;
    m[0][0] = key->matrix_x.x * q_x;
    m[0][1] = key->matrix_x.y * q_y;
    m[1][0] = key->matrix_y.x * q_x;
    m[1][1] = key->matrix_y.y * q_y;

    int32_t qmx = abs(key->matrix_x.x) + abs(key->matrix_x.y);
    int32_t qmy = abs(key->matrix_y.x) + abs(key->matrix_y.y);
    double scale_z = 1.0 / POSITION_PRECISION / FFMAX(qmx, qmy);
    m[2][0] = key->matrix_z.x * q_x * scale_z;  // qm_zx * q_zx
    m[2][1] = key->matrix_z.y * q_y * scale_z;  // qm_zy * q_zy

    m[0][2] = m[1][2] = 0;
    m[2][2] = 1 + fabs(m[2][0]) * dx + fabs(m[2][1]) * dy;
    m[2][2] = FFMIN(m[2][2], MAX_PERSP_SCALE);

    double center[2] = {
        key->offset.x * (64 >> SUBPIXEL_ORDER),
        key->offset.y * (64 >> SUBPIXEL_ORDER),
    };
    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 3; j++)
            m[i][j] += m[2][j] * center[i];

    for (int i = 0; i < 3; i++)
        m[i][2] -= m[i][0] * x0 + m[i][1] * y0;
}

// Calculate bitmap memory footprint
static inline size_t bitmap_size(const Bitmap *bm)
{
    return bm->stride * bm->h;
}

/**
 * Iterate through a list of bitmaps and blend with clip vector, if
 * applicable. The blended bitmaps are added to a free list which is freed
 * at the start of a new frame.
 */
static void blend_vector_clip(RenderContext *state, ASS_Image *head)
{
    if (!state->clip_drawing_text.str)
        return;

    ASS_Renderer *render_priv = state->renderer;

    OutlineHashKey ol_key;
    ol_key.type = OUTLINE_DRAWING;
    ol_key.u.drawing.text = state->clip_drawing_text;

    double m[3][3] = {{0}};
    int32_t scale_base = lshiftwrapi(1, state->clip_drawing_scale - 1);
    double w = scale_base > 0 ? (1.0 / scale_base) : 0;
    m[0][0] = state->screen_scale_x * w;
    m[1][1] = state->screen_scale_y * w;
    m[2][2] = 1;

    m[0][2] = int_to_d6(render_priv->settings.left_margin);
    m[1][2] = int_to_d6(render_priv->settings.top_margin);

    ASS_Vector pos;
    BitmapHashKey key;
    key.outline = ass_cache_get(render_priv->cache.outline_cache, state->cache_client, &ol_key, render_priv);
    if (!key.outline || !key.outline->valid ||
            !quantize_transform(m, &pos, NULL, true, &key))
        return;

    Bitmap *clip_bm = ass_cache_get(render_priv->cache.bitmap_cache, state->cache_client, &key, state);
    if (!clip_bm)
        return;

    // Iterate through bitmaps and blend/clip them
    for (ASS_Image *cur = head; cur; cur = cur->next) {
        int left, top, right, bottom, w, h;
        int ax, ay, aw, ah, as;
        int bx, by, bw, bh, bs;
        int aleft, atop, bleft, btop;
        unsigned char *abuffer, *bbuffer, *nbuffer;

        abuffer = cur->bitmap;
        bbuffer = clip_bm->buffer;
        ax = cur->dst_x;
        ay = cur->dst_y;
        aw = cur->w;
        ah = cur->h;
        as = cur->stride;
        bx = pos.x + clip_bm->left;
        by = pos.y + clip_bm->top;
        bw = clip_bm->w;
        bh = clip_bm->h;
        bs = clip_bm->stride;

        // Calculate overlap coordinates
        left = (ax > bx) ? ax : bx;
        top = (ay > by) ? ay : by;
        right = ((ax + aw) < (bx + bw)) ? (ax + aw) : (bx + bw);
        bottom = ((ay + ah) < (by + bh)) ? (ay + ah) : (by + bh);
        aleft = left - ax;
        atop = top - ay;
        w = right - left;
        h = bottom - top;
        bleft = left - bx;
        btop = top - by;

        unsigned align = 1 << render_priv->engine.align_order;
        if (state->clip_drawing_mode) {
            // Inverse clip
            if (ax + aw < bx || ay + ah < by || ax > bx + bw ||
                ay > by + bh || !h || !w) {
                continue;
            }

            // Allocate new buffer and add to free list
            nbuffer = ass_aligned_alloc(align, as * ah + align, false);
            if (!nbuffer)
                break;

            // Blend together
            memcpy(nbuffer, abuffer, ((ah - 1) * as) + aw);
            render_priv->engine.imul_bitmaps(nbuffer + atop * as + aleft, as,
                                             bbuffer + btop * bs + bleft, bs,
                                             w, h);
        } else {
            // Regular clip
            if (ax + aw < bx || ay + ah < by || ax > bx + bw ||
                ay > by + bh || !h || !w) {
                cur->w = cur->h = cur->stride = 0;
                continue;
            }

            // Allocate new buffer and add to free list
            unsigned ns = ass_align(align, w);
            nbuffer = ass_aligned_alloc(align, ns * h + align, false);
            if (!nbuffer)
                break;

            // Blend together
            render_priv->engine.mul_bitmaps(nbuffer, ns,
                                            abuffer + atop * as + aleft, as,
                                            bbuffer + btop * bs + bleft, bs,
                                            w, h);
            cur->dst_x += aleft;
            cur->dst_y += atop;
            cur->w = w;
            cur->h = h;
            cur->stride = ns;
        }

        ASS_ImagePriv *priv = (ASS_ImagePriv *) cur;
        priv->buffer = cur->bitmap = nbuffer;
        ass_cache_dec_ref(priv->source);
        priv->source = NULL;
    }
}

/**
 * \brief Convert TextInfo struct to ASS_Image list
 * Splits glyphs in halves when needed (for \kf karaoke).
 */
static ASS_Image *render_text(RenderContext *state)
{
    ASS_Image *head;
    ASS_Image **tail = &head;
    unsigned n_bitmaps = state->text_info.n_bitmaps;
    CombinedBitmapInfo *bitmaps = state->text_info.combined_bitmaps;

    for (unsigned i = 0; i < n_bitmaps; i++) {
        CombinedBitmapInfo *info = &bitmaps[i];
        if (!info->bm_s || state->border_style == 4)
            continue;

        tail =
            render_glyph(state, info->bm_s, info->x, info->y, info->c[3], 0,
                         1000000, tail, IMAGE_TYPE_SHADOW, info->image);
    }

    for (unsigned i = 0; i < n_bitmaps; i++) {
        CombinedBitmapInfo *info = &bitmaps[i];
        if (!info->bm_o)
            continue;

        if ((info->effect_type == EF_KARAOKE_KO)
                && (info->effect_timing <= 0)) {
            // do nothing
        } else {
            tail =
                render_glyph(state, info->bm_o, info->x, info->y, info->c[2],
                             0, 1000000, tail, IMAGE_TYPE_OUTLINE, info->image);
        }
    }

    for (unsigned i = 0; i < n_bitmaps; i++) {
        CombinedBitmapInfo *info = &bitmaps[i];
        if (!info->bm)
            continue;

        if ((info->effect_type == EF_KARAOKE)
                || (info->effect_type == EF_KARAOKE_KO)) {
            if (info->effect_timing > 0)
                tail =
                    render_glyph(state, info->bm, info->x, info->y,
                                 info->c[0], 0, 1000000, tail,
                                 IMAGE_TYPE_CHARACTER, info->image);
            else
                tail =
                    render_glyph(state, info->bm, info->x, info->y,
                                 info->c[1], 0, 1000000, tail,
                                 IMAGE_TYPE_CHARACTER, info->image);
        } else if (info->effect_type == EF_KARAOKE_KF) {
            tail =
                render_glyph(state, info->bm, info->x, info->y, info->c[0],
                             info->c[1], info->effect_timing, tail,
                             IMAGE_TYPE_CHARACTER, info->image);
        } else
            tail =
                render_glyph(state, info->bm, info->x, info->y, info->c[0],
                             0, 1000000, tail, IMAGE_TYPE_CHARACTER, info->image);
    }

    *tail = 0;
    blend_vector_clip(state, head);

    return head;
}

static void compute_string_bbox(TextInfo *text, ASS_DRect *bbox)
{
    if (text->length > 0) {
        bbox->x_min = +32000;
        bbox->x_max = -32000;
        bbox->y_min = -text->lines[0].asc;
        bbox->y_max = bbox->y_min + text->height;

        for (int i = 0; i < text->length; i++) {
            GlyphInfo *info = text->glyphs + i;
            if (info->skip) continue;
            double s = d6_to_double(info->pos.x);
            double e = s + d6_to_double(info->cluster_advance.x);
            bbox->x_min = FFMIN(bbox->x_min, s);
            bbox->x_max = FFMAX(bbox->x_max, e);
        }
    } else
        bbox->x_min = bbox->x_max = bbox->y_min = bbox->y_max = 0;
}

static ASS_Style *handle_selective_style_overrides(RenderContext *state,
                                                   ASS_Style *rstyle)
{
    // The script style is the one the event was declared with.
    ASS_Renderer *render_priv = state->renderer;
    ASS_Style *script = render_priv->track->styles +
                        state->event->Style;
    // The user style was set with ass_set_selective_style_override().
    ASS_Style *user = &render_priv->user_override_style;
    ASS_Style *new = &state->override_style_temp_storage;
    int explicit = state->explicit;
    int requested = render_priv->settings.selective_style_overrides;
    double scale;

    // Either the event's style, or the style forced with a \r tag.
    if (!rstyle)
        rstyle = script;

    // Create a new style that contains a mix of the original style and
    // user_style (the user's override style). Copy only fields from the
    // script's style that are deemed necessary.
    *new = *rstyle;

    state->apply_font_scale =
        !explicit || !(requested & ASS_OVERRIDE_BIT_SELECTIVE_FONT_SCALE);

    // On positioned events, do not apply most overrides.
    if (explicit)
        requested = 0;

    if (requested & ASS_OVERRIDE_BIT_STYLE)
        requested |= ASS_OVERRIDE_BIT_FONT_NAME |
                     ASS_OVERRIDE_BIT_FONT_SIZE_FIELDS |
                     ASS_OVERRIDE_BIT_COLORS |
                     ASS_OVERRIDE_BIT_BORDER |
                     ASS_OVERRIDE_BIT_ATTRIBUTES;

    // Copies fields even not covered by any of the other bits.
    if (requested & ASS_OVERRIDE_FULL_STYLE)
        *new = *user;

    // The user style is supposed to be independent of the script resolution.
    // Treat the user style's values as if they were specified for a script with
    // PlayResY=288, and rescale the values to the current script.
    scale = render_priv->track->PlayResY / 288.0;

    if (requested & ASS_OVERRIDE_BIT_FONT_SIZE_FIELDS) {
        new->FontSize = user->FontSize * scale;
        new->Spacing = user->Spacing * scale;
        new->ScaleX = user->ScaleX;
        new->ScaleY = user->ScaleY;
    }

    if (requested & ASS_OVERRIDE_BIT_FONT_NAME) {
        new->FontName = user->FontName;
        new->treat_fontname_as_pattern = user->treat_fontname_as_pattern;
    }

    if (requested & ASS_OVERRIDE_BIT_COLORS) {
        new->PrimaryColour = user->PrimaryColour;
        new->SecondaryColour = user->SecondaryColour;
        new->OutlineColour = user->OutlineColour;
        new->BackColour = user->BackColour;
    }

    if (requested & ASS_OVERRIDE_BIT_ATTRIBUTES) {
        new->Bold = user->Bold;
        new->Italic = user->Italic;
        new->Underline = user->Underline;
        new->StrikeOut = user->StrikeOut;
    }

    if (requested & ASS_OVERRIDE_BIT_BORDER) {
        new->BorderStyle = user->BorderStyle;
        new->Outline = user->Outline * scale;
        new->Shadow = user->Shadow * scale;
    }

    if (requested & ASS_OVERRIDE_BIT_BLUR)
        new->Blur = user->Blur * scale;

    if (requested & ASS_OVERRIDE_BIT_ALIGNMENT)
        new->Alignment = user->Alignment;

    if (requested & ASS_OVERRIDE_BIT_JUSTIFY)
        new->Justify = user->Justify;

    if (requested & ASS_OVERRIDE_BIT_MARGINS) {
        new->MarginL = user->MarginL;
        new->MarginR = user->MarginR;
        new->MarginV = user->MarginV;
    }

    if (!new->FontName)
        new->FontName = rstyle->FontName;

    state->style = new;
    state->overrides = requested;

    return new;
}

ASS_Vector ass_layout_res(ASS_Renderer *render_priv)
{
    ASS_Track *track = render_priv->track;
    if (track->LayoutResX > 0 && track->LayoutResY > 0)
        return (ASS_Vector) { track->LayoutResX, track->LayoutResY };

    ASS_Settings *settings = &render_priv->settings;
    if (settings->storage_width > 0 && settings->storage_height > 0)
        return (ASS_Vector) { settings->storage_width, settings->storage_height };

    if (settings->par <= 0 || settings->par == 1 ||
            !render_priv->frame_content_width || !render_priv->frame_content_height)
        return (ASS_Vector) { track->PlayResX, track->PlayResY };
    if (settings->par > 1)
        return (ASS_Vector) {
            FFMAX(1, lround(track->PlayResY * render_priv->frame_content_width
                    / render_priv->frame_content_height / settings->par)),
            track->PlayResY
        };
    else
        return (ASS_Vector) {
            track->PlayResX,
            FFMAX(1, lround(track->PlayResX * render_priv->frame_content_height
                    / render_priv->frame_content_width * settings->par))
        };
}

static void init_font_scale(RenderContext *state)
{
    ASS_Renderer *render_priv = state->renderer;
    ASS_Settings *settings_priv = &render_priv->settings;

    double font_scr_w = render_priv->frame_content_width;
    double font_scr_h = render_priv->frame_content_height;
    if (!state->explicit && render_priv->settings.use_margins) {
        font_scr_w = render_priv->fit_width;
        font_scr_h = render_priv->fit_height;
    }

    state->screen_scale_x = font_scr_w / render_priv->track->PlayResX;
    state->screen_scale_y = font_scr_h / render_priv->track->PlayResY;

    ASS_Vector layout_res = ass_layout_res(render_priv);
    state->blur_scale_x = font_scr_w / layout_res.x;
    state->blur_scale_y = font_scr_h / layout_res.y;
    if (render_priv->track->ScaledBorderAndShadow) {
        state->border_scale_x = state->screen_scale_x;
        state->border_scale_y = state->screen_scale_y;
    } else {
        state->border_scale_x = state->blur_scale_x;
        state->border_scale_y = state->blur_scale_y;
    }

    if (state->apply_font_scale) {
        state->screen_scale_x *= settings_priv->font_size_coeff;
        state->screen_scale_y *= settings_priv->font_size_coeff;
        state->border_scale_x *= settings_priv->font_size_coeff;
        state->border_scale_y *= settings_priv->font_size_coeff;
        state->blur_scale_x *= settings_priv->font_size_coeff;
        state->blur_scale_y *= settings_priv->font_size_coeff;
    }
}

/**
 * \brief partially reset render_context to style values
 * Works like {\r}: resets some style overrides
 */
void ass_reset_render_context(RenderContext *state, ASS_Style *style)
{
    style = handle_selective_style_overrides(state, style);

    init_font_scale(state);

    state->c[0] = style->PrimaryColour;
    state->c[1] = style->SecondaryColour;
    state->c[2] = style->OutlineColour;
    state->c[3] = style->BackColour;
    state->flags =
        (style->Underline ? DECO_UNDERLINE : 0) |
        (style->StrikeOut ? DECO_STRIKETHROUGH : 0);
    state->font_size = style->FontSize;

    state->family.str = style->FontName;
    state->family.len = strlen(style->FontName);
    state->treat_family_as_pattern = style->treat_fontname_as_pattern;
    state->bold = style->Bold;
    state->italic = style->Italic;
    ass_update_font(state);

    state->border_style = style->BorderStyle;
    state->border_x = style->Outline;
    state->border_y = style->Outline;
    state->scale_x = style->ScaleX;
    state->scale_y = style->ScaleY;
    state->hspacing = style->Spacing;
    state->be = 0;
    state->blur = style->Blur;
    state->shadow_x = style->Shadow;
    state->shadow_y = style->Shadow;
    state->frx = state->fry = 0.;
    state->frz = style->Angle;
    state->fax = state->fay = 0.;
    state->font_encoding = style->Encoding;
}

/**
 * \brief Start new event. Reset state.
 */
static void
init_render_context(RenderContext *state, ASS_Event *event)
{
    ASS_Renderer *render_priv = state->renderer;

    state->event = event;
    state->parsed_tags = 0;
    state->evt_type = EVENT_NORMAL;

    state->wrap_style = render_priv->track->WrapStyle;

    state->pos_x = 0;
    state->pos_y = 0;
    state->org_x = 0;
    state->org_y = 0;
    state->have_origin = 0;
    state->clip_x0 = 0;
    state->clip_y0 = 0;
    state->clip_x1 = render_priv->track->PlayResX;
    state->clip_y1 = render_priv->track->PlayResY;
    state->clip_mode = 0;
    state->detect_collisions = 1;
    state->fade = 0;
    state->drawing_scale = 0;
    state->pbo = 0;
    state->effect_type = EF_NONE;
    state->effect_timing = 0;
    state->effect_skip_timing = 0;
    state->reset_effect = false;

    ass_apply_transition_effects(state);
    state->explicit = state->evt_type != EVENT_NORMAL ||
                      event_has_hard_overrides(render_priv, event);

    ass_reset_render_context(state, NULL);
    state->alignment = state->style->Alignment;
    state->justify = state->style->Justify;
}

static void free_render_context(RenderContext *state)
{
    state->font = NULL;
    state->family.str = NULL;
    state->family.len = 0;
    state->clip_drawing_text.str = NULL;
    state->clip_drawing_text.len = 0;
    state->text_info.length = 0;
}

/**
 * \brief Get normal and outline (border) glyphs
 * \param info out: struct filled with extracted data
 * Tries to get both glyphs from cache.
 * If they can't be found, gets a glyph from font face, generates outline,
 * and add them to cache.
 */
static void
get_outline_glyph(RenderContext *state, GlyphInfo *info)
{
    ASS_Renderer *priv = state->renderer;
    OutlineHashValue *val;
    ASS_DVector scale, offset = {0};

    int32_t asc, desc;
    OutlineHashKey key;
    if (info->drawing_text.str) {
        key.type = OUTLINE_DRAWING;
        key.u.drawing.text = info->drawing_text;
        val = ass_cache_get(priv->cache.outline_cache, state->cache_client, &key, priv);
        if (!val || !val->valid)
            return;

        int32_t scale_base = lshiftwrapi(1, info->drawing_scale - 1);
        double w = scale_base > 0 ? (1.0 / scale_base) : 0;
        scale.x = info->scale_x * w * state->screen_scale_x / priv->par_scale_x;
        scale.y = info->scale_y * w * state->screen_scale_y;
        desc = 64 * info->drawing_pbo;
        asc = val->asc - desc;

        offset.y = -asc * scale.y;
    } else {
        key.type = OUTLINE_GLYPH;
        GlyphHashKey *k = &key.u.glyph;
        k->font = info->font;
        k->size = info->font_size;
        k->face_index = info->face_index;
        k->glyph_index = info->glyph_index;
        k->bold = info->bold;
        k->italic = info->italic;
        k->flags = info->flags;

        val = ass_cache_get(priv->cache.outline_cache, state->cache_client, &key, priv);
        if (!val || !val->valid)
            return;

        scale.x = info->scale_x;
        scale.y = info->scale_y;
        asc  = val->asc;
        desc = val->desc;
    }

    info->outline = val;
    info->transform.scale = scale;
    info->transform.offset = offset;

    info->bbox.x_min = ass_lrint(val->cbox.x_min * scale.x + offset.x);
    info->bbox.y_min = ass_lrint(val->cbox.y_min * scale.y + offset.y);
    info->bbox.x_max = ass_lrint(val->cbox.x_max * scale.x + offset.x);
    info->bbox.y_max = ass_lrint(val->cbox.y_max * scale.y + offset.y);

    if (info->drawing_text.str || priv->settings.shaper == ASS_SHAPING_SIMPLE) {
        info->cluster_advance.x = info->advance.x = ass_lrint(val->advance * scale.x);
        info->cluster_advance.y = info->advance.y = 0;
    }
    info->asc  = ass_lrint(asc  * scale.y);
    info->desc = ass_lrint(desc * scale.y);
}

size_t ass_outline_construct(void *key, void *value, void *priv)
{
    ASS_Renderer *render_priv = priv;
    OutlineHashKey *outline_key = key;
    OutlineHashValue *v = value;
    memset(v, 0, sizeof(*v));

    switch (outline_key->type) {
    case OUTLINE_GLYPH:
        {
            GlyphHashKey *k = &outline_key->u.glyph;
            ass_font_lock(k->font);
            ass_face_set_size(k->font->faces[k->face_index], k->size);
            if (!ass_font_get_glyph(k->font, k->face_index, k->glyph_index,
                                    render_priv->settings.hinting)) {
                ass_font_unlock(k->font);
                return 1;
            }
            if (!ass_get_glyph_outline(&v->outline[0], &v->advance,
                                       k->font->faces[k->face_index],
                                       k->flags)) {
                ass_font_unlock(k->font);
                return 1;
            }
            ass_font_get_asc_desc(k->font, k->face_index,
                                  &v->asc, &v->desc);
            ass_font_unlock(k->font);
            break;
        }
    case OUTLINE_DRAWING:
        {
            ASS_Rect bbox;
            const char *text = outline_key->u.drawing.text.str;  // always zero-terminated
            if (!ass_drawing_parse(&v->outline[0], &bbox, text, render_priv->library))
                return 1;

            v->advance = bbox.x_max - bbox.x_min;
            v->asc = bbox.y_max - bbox.y_min;
            v->desc = 0;
            break;
        }
    case OUTLINE_BORDER:
        {
            BorderHashKey *k = &outline_key->u.border;
            if (!k->border.x && !k->border.y)
                break;
            if (!k->outline->outline[0].n_points)
                break;

            ASS_Outline src;
            if (!ass_outline_scale_pow2(&src, &k->outline->outline[0],
                                        k->scale_ord_x, k->scale_ord_y))
                return 1;
            if (!ass_outline_stroke(&v->outline[0], &v->outline[1], &src,
                                    k->border.x * STROKER_PRECISION,
                                    k->border.y * STROKER_PRECISION,
                                    STROKER_PRECISION)) {
                ass_msg(render_priv->library, MSGL_WARN, "Cannot stroke outline");
                ass_outline_free(&v->outline[0]);
                ass_outline_free(&v->outline[1]);
                ass_outline_free(&src);
                return 1;
            }
            ass_outline_free(&src);
            break;
        }
    case OUTLINE_BOX:
        {
            ASS_Outline *ol = &v->outline[0];
            if (!ass_outline_alloc(ol, 4, 4))
                return 1;
            ol->points[0].x = ol->points[3].x = 0;
            ol->points[1].x = ol->points[2].x = 64;
            ol->points[0].y = ol->points[1].y = 0;
            ol->points[2].y = ol->points[3].y = 64;
            ol->segments[0] = OUTLINE_LINE_SEGMENT;
            ol->segments[1] = OUTLINE_LINE_SEGMENT;
            ol->segments[2] = OUTLINE_LINE_SEGMENT;
            ol->segments[3] = OUTLINE_LINE_SEGMENT | OUTLINE_CONTOUR_END;
            ol->n_points = ol->n_segments = 4;
            break;
        }
    default:
        return 1;
    }

    rectangle_reset(&v->cbox);
    ass_outline_update_cbox(&v->outline[0], &v->cbox);
    ass_outline_update_cbox(&v->outline[1], &v->cbox);
    if (v->cbox.x_min > v->cbox.x_max || v->cbox.y_min > v->cbox.y_max)
        v->cbox.x_min = v->cbox.y_min = v->cbox.x_max = v->cbox.y_max = 0;
    v->valid = true;
    return 1;
}

/**
 * \brief Calculate outline transformation matrix
 */
static void calc_transform_matrix(RenderContext *state,
                                  GlyphInfo *info, double m[3][3])
{
    ASS_Renderer *render_priv = state->renderer;

    double frx = ASS_PI / 180 * info->frx;
    double fry = ASS_PI / 180 * info->fry;
    double frz = ASS_PI / 180 * info->frz;

    // Most glyphs are unrotated, yet sin/cos of the (zero) angles are still
    // computed via libm on every glyph, before the bitmap cache lookup. Skip
    // the calls when the angle is exactly 0. Bit-exact: ASS_PI/180 * 0 == +0.0,
    // sin(+0.0) == +0.0, cos(+0.0) == 1.0, so -sin(0) == -0.0 etc.; we
    // substitute those exact values (including the negative zero).
    double sx, cx, sy, cy, sz, cz;
    if (frx == 0.) { sx = -0.0; cx = 1.0; }
    else { sx = -sin(frx); cx = cos(frx); }
    if (fry == 0.) { sy = 0.0;  cy = 1.0; }
    else { sy =  sin(fry); cy = cos(fry); }
    if (frz == 0.) { sz = -0.0; cz = 1.0; }
    else { sz = -sin(frz); cz = cos(frz); }

    double fax = info->fax * info->scale_x / info->scale_y;
    double fay = info->fay * info->scale_y / info->scale_x;
    double x1[3] = { 1, fax, info->shift.x + info->asc * fax };
    double y1[3] = { fay, 1, info->shift.y };

    double x2[3], y2[3];
    for (int i = 0; i < 3; i++) {
        x2[i] = x1[i] * cz - y1[i] * sz;
        y2[i] = x1[i] * sz + y1[i] * cz;
    }

    double y3[3], z3[3];
    for (int i = 0; i < 3; i++) {
        y3[i] = y2[i] * cx;
        z3[i] = y2[i] * sx;
    }

    double x4[3], z4[3];
    for (int i = 0; i < 3; i++) {
        x4[i] = x2[i] * cy - z3[i] * sy;
        z4[i] = x2[i] * sy + z3[i] * cy;
    }

    double dist = 20000 * state->blur_scale_y;
    z4[2] += dist;

    double scale_x = dist * render_priv->par_scale_x;
    double offs_x = info->pos.x - info->shift.x * render_priv->par_scale_x;
    double offs_y = info->pos.y - info->shift.y;
    for (int i = 0; i < 3; i++) {
        m[0][i] = z4[i] * offs_x + x4[i] * scale_x;
        m[1][i] = z4[i] * offs_y + y3[i] * dist;
        m[2][i] = z4[i];
    }
}

/**
 * \brief Get bitmaps for a glyph
 * \param info glyph info
 * Tries to get glyph bitmaps from bitmap cache.
 * If they can't be found, they are generated by rotating and rendering the glyph.
 * After that, bitmaps are added to the cache.
 * They are returned in info->bm (glyph), info->bm_o (outline).
 */
static void
get_bitmap_glyph(RenderContext *state, GlyphInfo *info,
                 int32_t *leftmost_x,
                 ASS_Vector *pos, ASS_Vector *pos_o,
                 ASS_DVector *offset, bool first, int flags)
{
    ASS_Renderer *render_priv = state->renderer;

    if (!info->outline || info->symbol == '\n' || info->symbol == 0 || info->skip)
        return;

    double m1[3][3], m2[3][3], m[3][3];
    const ASS_Transform *tr = &info->transform;
    calc_transform_matrix(state, info, m1);
    for (int i = 0; i < 3; i++) {
        m2[i][0] = m1[i][0] * tr->scale.x;
        m2[i][1] = m1[i][1] * tr->scale.y;
        m2[i][2] = m1[i][0] * tr->offset.x + m1[i][1] * tr->offset.y + m1[i][2];
    }
    memcpy(m, m2, sizeof(m));

    if (info->effect_type == EF_KARAOKE_KF)
        ass_outline_update_min_transformed_x(&info->outline->outline[0], m, leftmost_x);

    BitmapHashKey key;
    key.outline = info->outline;
    if (!quantize_transform(m, pos, offset, first, &key))
        return;
    if (motion_cache_enabled(state, info))
        snap_motion_cache_key(&key, pos);

    info->bm = ass_cache_get(render_priv->cache.bitmap_cache, state->cache_client, &key, state);
    if (!info->bm)
        state->alloc_error = true;
    if (!info->bm || !info->bm->buffer)
        info->bm = NULL;

    *pos_o = *pos;

    OutlineHashKey ol_key;
    if (flags & FILTER_BORDER_STYLE_3) {
        if (!(flags & (FILTER_NONZERO_BORDER | FILTER_NONZERO_SHADOW)))
            return;

        ol_key.type = OUTLINE_BOX;

        ASS_DVector bord = {
            64 * info->border_x * state->border_scale_x /
                render_priv->par_scale_x,
            64 * info->border_y * state->border_scale_y,
        };
        double width = info->hspacing_scaled + info->advance.x;
        double height = info->asc + info->desc;

        ASS_DVector orig_scale;
        orig_scale.x = info->scale_x * info->scale_fix;
        orig_scale.y = info->scale_y * info->scale_fix;

        // Emulate the WTFish behavior of VSFilter, i.e. double-scale
        // the sizes of the opaque box.
        bord.x *= orig_scale.x;
        bord.y *= orig_scale.y;
        width  *= orig_scale.x;
        height *= orig_scale.y;

        // to avoid gaps
        bord.x = FFMAX(64, bord.x);
        bord.y = FFMAX(64, bord.y);

        ASS_DVector scale = {
            (width  + 2 * bord.x) / 64,
            (height + 2 * bord.y) / 64,
        };
        ASS_DVector offset = { -bord.x, -bord.y - info->asc };
        for (int i = 0; i < 3; i++) {
            m[i][0] = m1[i][0] * scale.x;
            m[i][1] = m1[i][1] * scale.y;
            m[i][2] = m1[i][0] * offset.x + m1[i][1] * offset.y + m1[i][2];
        }
    } else {
        if (!(flags & FILTER_NONZERO_BORDER))
            return;

        ol_key.type = OUTLINE_BORDER;
        BorderHashKey *k = &ol_key.u.border;
        k->outline = info->outline;

        double bord_x =
            64 * state->border_scale_x * info->border_x / tr->scale.x /
                render_priv->par_scale_x;
        double bord_y =
            64 * state->border_scale_y * info->border_y / tr->scale.y;

        const ASS_Rect *bbox = &info->outline->cbox;
        // Estimate bounding box half size after stroking
        double dx = (bbox->x_max - bbox->x_min) / 2.0 + (bord_x + 64);
        double dy = (bbox->y_max - bbox->y_min) / 2.0 + (bord_y + 64);

        // Matrix after quantize_transform() has
        // input and output origin at bounding box center.
        double mxx = fabs(m[0][0]), mxy = fabs(m[0][1]);
        double myx = fabs(m[1][0]), myy = fabs(m[1][1]);
        double mzx = fabs(m[2][0]), mzy = fabs(m[2][1]);

        double z0 = m[2][2] - mzx * dx - mzy * dy;
        double w = 1 / FFMAX(z0, m[2][2] / MAX_PERSP_SCALE);

        // Notation from quantize_transform().
        // Note that goal here is to estimate acceptable error for stroking, i. e. D(x) and D(y).
        // Matrix coefficients are constants now, so D(m_ij) = 0 for all i, j from {x, y, z}.

        // D(z) <= |m_zx| * D(x) + |m_zy| * D(y),
        // D(x_out) = D((m_xx * x + m_xy * y) / z)
        //  <= (|m_xx| * D(x) + |m_xy| * D(y)) / z0 + x_lim * D(z) / z0^2
        //  <= (|m_xx| / z0 + |m_zx| * x_lim / z0^2) * D(x)
        //   + (|m_xy| / z0 + |m_zy| * x_lim / z0^2) * D(y),
        // D(y_out) = D((m_yx * x + m_yy * y) / z)
        //  <= (|m_yx| * D(x) + |m_yy| * D(y)) / z0 + y_lim * D(z) / z0^2
        //  <= (|m_yx| / z0 + |m_zx| * y_lim / z0^2) * D(x)
        //   + (|m_yy| / z0 + |m_zy| * y_lim / z0^2) * D(y).

        // Quantization steps (pick: ACCURACY = POSITION_PRECISION):
        // STROKER_PRECISION / 2^scale_ord_x ~ D(x) ~ POSITION_PRECISION /
        //   (max(|m_xx|, |m_yx|) / z0 + |m_zx| * max(x_lim, y_lim) / z0^2),
        // STROKER_PRECISION / 2^scale_ord_y ~ D(y) ~ POSITION_PRECISION /
        //   (max(|m_xy|, |m_yy|) / z0 + |m_zy| * max(x_lim, y_lim) / z0^2).

        double x_lim = mxx * dx + mxy * dy;
        double y_lim = myx * dx + myy * dy;
        double rz = FFMAX(x_lim, y_lim) * w;

        w *= STROKER_PRECISION / POSITION_PRECISION;
        frexp(w * (FFMAX(mxx, myx) + mzx * rz), &k->scale_ord_x);
        frexp(w * (FFMAX(mxy, myy) + mzy * rz), &k->scale_ord_y);
        bord_x = scale_by_pow2(bord_x, k->scale_ord_x);
        bord_y = scale_by_pow2(bord_y, k->scale_ord_y);
        if (!(bord_x < OUTLINE_MAX && bord_y < OUTLINE_MAX))
            return;
        k->border.x = ass_lrint(bord_x / STROKER_PRECISION);
        k->border.y = ass_lrint(bord_y / STROKER_PRECISION);
        if (!k->border.x && !k->border.y) {
            info->bm_o = info->bm;
            return;
        }

        for (int i = 0; i < 3; i++) {
            m[i][0] = scale_by_pow2(m2[i][0], -k->scale_ord_x);
            m[i][1] = scale_by_pow2(m2[i][1], -k->scale_ord_y);
            m[i][2] = m2[i][2];
        }
    }

    key.outline = ass_cache_get(render_priv->cache.outline_cache, state->cache_client, &ol_key, render_priv);
    if (!key.outline)
        state->alloc_error = true;
    if (!key.outline || !key.outline->valid ||
            !quantize_transform(m, pos_o, offset, false, &key))
        return;
    if (motion_cache_enabled(state, info))
        snap_motion_cache_key(&key, pos_o);

    info->bm_o = ass_cache_get(render_priv->cache.bitmap_cache, state->cache_client, &key, state);
    if (!info->bm_o)
        state->alloc_error = true;
    if (!info->bm_o || !info->bm_o->buffer) {
        info->bm_o = NULL;
        *pos_o = *pos;
    } else if (!info->bm)
        *pos = *pos_o;
}

static inline size_t outline_size(const ASS_Outline* outline)
{
    return sizeof(ASS_Vector) * outline->n_points + outline->n_segments;
}

size_t ass_bitmap_construct(void *key, void *value, void *priv)
{
    RenderContext *state = priv;
    BitmapHashKey *k = key;
    Bitmap *bm = value;

    double m[3][3];
    restore_transform(m, k);

    ASS_Outline outline[2];
    if (k->matrix_z.x || k->matrix_z.y) {
        ass_outline_transform_3d(&outline[0], &k->outline->outline[0], m);
        ass_outline_transform_3d(&outline[1], &k->outline->outline[1], m);
    } else {
        ass_outline_transform_2d(&outline[0], &k->outline->outline[0], m);
        ass_outline_transform_2d(&outline[1], &k->outline->outline[1], m);
    }

    if (!ass_outline_to_bitmap(state, bm, &outline[0], &outline[1]))
        memset(bm, 0, sizeof(*bm));
    ass_outline_free(&outline[0]);
    ass_outline_free(&outline[1]);

    return sizeof(BitmapHashKey) + sizeof(Bitmap) + bitmap_size(bm) +
           sizeof(OutlineHashValue) + outline_size(&k->outline->outline[0]) + outline_size(&k->outline->outline[1]);
}

static void measure_text_on_eol(RenderContext *state, double scale, int cur_line,
                                int max_asc, int max_desc,
                                double max_border_x, double max_border_y)
{
    TextInfo *text_info = &state->text_info;
    text_info->lines[cur_line].asc  = scale * max_asc;
    text_info->lines[cur_line].desc = scale * max_desc;
    text_info->height += scale * max_asc + scale * max_desc;
    // For *VSFilter compatibility do biased rounding on max_border*
    // https://github.com/Cyberbeing/xy-VSFilter/blob/xy_sub_filter_rc4@%7B2020-05-17%7D/src/subtitles/RTS.cpp#L1465
    text_info->border_bottom = (int) (state->border_scale_y * max_border_y + 0.5);
    if (cur_line == 0)
        text_info->border_top = text_info->border_bottom;
    // VSFilter takes max \bordx into account for collision, even if far from edge
    text_info->border_x = FFMAX(text_info->border_x,
            (int) (state->border_scale_x * max_border_x + 0.5));
}


/**
 * This function goes through text_info and calculates text parameters.
 * The following text_info fields are filled:
 *   height
 *   border_top
 *   border_bottom
 *   border_x
 *   lines[].asc
 *   lines[].desc
 */
static void measure_text(RenderContext *state)
{
    ASS_Renderer *render_priv = state->renderer;
    TextInfo *text_info = &state->text_info;
    text_info->height = 0;
    text_info->border_x = 0;

    int cur_line = 0;
    double scale = 0.5 / 64;
    int max_asc = 0, max_desc = 0;
    double max_border_y = 0, max_border_x = 0;
    bool empty_trimmed_line = true;
    for (int i = 0; i < text_info->length; i++) {
        if (text_info->glyphs[i].linebreak) {
            measure_text_on_eol(state, scale, cur_line,
                    max_asc, max_desc, max_border_x, max_border_y);
            empty_trimmed_line = true;
            max_asc = max_desc = 0;
            max_border_y = max_border_x = 0;
            scale = 0.5 / 64;
            cur_line++;
        }
        GlyphInfo *cur = text_info->glyphs + i;
        // VSFilter ignores metrics of line-leading/trailing (trimmed)
        // whitespace, except when the line becomes empty after trimming
        if (empty_trimmed_line && !cur->is_trimmed_whitespace) {
            empty_trimmed_line = false;
            // Forget metrics of line-leading whitespace
            max_asc = max_desc = 0;
            max_border_y = max_border_x = 0;
        } else if (!empty_trimmed_line && cur->is_trimmed_whitespace) {
            // Ignore metrics of line-trailing whitespace
            continue;
        }
        max_asc  = FFMAX(max_asc,  cur->asc);
        max_desc = FFMAX(max_desc, cur->desc);
        max_border_y = FFMAX(max_border_y, cur->border_y);
        max_border_x = FFMAX(max_border_x, cur->border_x);
        if (cur->symbol != '\n')
            scale = 1.0 / 64;
    }
    assert(cur_line == text_info->n_lines - 1);
    measure_text_on_eol(state, scale, cur_line,
            max_asc, max_desc, max_border_x, max_border_y);
    text_info->height += cur_line * render_priv->settings.line_spacing;
}

/**
 * Mark extra whitespace for later removal.
 */
#define IS_WHITESPACE(x) ((x->symbol == ' ' || x->symbol == '\n') \
                          && !x->linebreak)
static void trim_whitespace(RenderContext *state)
{
    int i, j;
    GlyphInfo *cur;
    TextInfo *ti = &state->text_info;

    if (!ti->length)
      return;

    // Mark trailing spaces
    i = ti->length - 1;
    cur = ti->glyphs + i;
    while (i && IS_WHITESPACE(cur)) {
        cur->skip = true;
        cur->is_trimmed_whitespace = true;
        cur = ti->glyphs + --i;
    }

    // Mark leading whitespace
    i = 0;
    cur = ti->glyphs;
    while (i < ti->length && IS_WHITESPACE(cur)) {
        cur->skip = true;
        cur->is_trimmed_whitespace = true;
        cur = ti->glyphs + ++i;
    }
    if (i < ti->length)
        cur->starts_new_run = true;

    // Mark all extraneous whitespace inbetween
    // XXX: should this really start at 0 again?
    for (i = 0; i < ti->length; ++i) {
        cur = ti->glyphs + i;
        if (cur->linebreak) {
            // Mark whitespace before
            j = i - 1;
            cur = ti->glyphs + j;
            // Use > instead of >= to avoid UB from moving the pointer outside valid range.
            // White space at j == 0 was already trimmed in the "leading" loop before anyway.
            while (j > 0 && IS_WHITESPACE(cur)) {
                cur->skip = true;
                cur->is_trimmed_whitespace = true;
                cur = ti->glyphs + --j;
            }

            // A break itself can contain a whitespace, too
            cur = ti->glyphs + i;
            if (cur->symbol == ' ' || cur->symbol == '\n') {
                cur->skip = true;
                cur->is_trimmed_whitespace = true;
                // Mark whitespace after
                j = i + 1;
                cur = ti->glyphs + j;
                while (j < ti->length && IS_WHITESPACE(cur)) {
                    cur->skip = true;
                    cur->is_trimmed_whitespace = true;
                    cur = ti->glyphs + ++j;
                }
                i = j - 1;
            }
            if (cur < ti->glyphs + ti->length)
                cur->starts_new_run = true;
        }
    }
}
#undef IS_WHITESPACE

#ifdef CONFIG_UNIBREAK
    #define ALLOWBREAK(glyph, index) (unibrks ? unibrks[index] == LINEBREAK_ALLOWBREAK : glyph == ' ')
    #define FORCEBREAK(glyph, index) (unibrks ? unibrks[index] == LINEBREAK_MUSTBREAK  : glyph == '\n')
#else
    #define ALLOWBREAK(glyph, index) (glyph == ' ')
    #define FORCEBREAK(glyph, index) (glyph == '\n')
#endif

/*
 * Starts a new line on the first breakable character after overflow
 */
static void
wrap_lines_naive(RenderContext *state, double max_text_width, char *unibrks)
{
    ASS_Renderer *render_priv = state->renderer;
    TextInfo *text_info = &state->text_info;
    GlyphInfo *s1  = text_info->glyphs; // current line start
    int last_breakable = -1;
    int break_type = 0;

    text_info->n_lines = 1;
    for (int i = 0; i < text_info->length; ++i) {
        GlyphInfo *cur = text_info->glyphs + i;
        int break_at = -1;
        double s_offset = d6_to_double(s1->bbox.x_min + s1->pos.x);
        double len = d6_to_double(cur->bbox.x_max + cur->pos.x) - s_offset;

        if (FORCEBREAK(cur->symbol, i)) {
            break_type = 2;
            break_at = i;
            ass_msg(render_priv->library, MSGL_DBG2,
                    "forced line break at %d", break_at);
        } else if (len >= max_text_width &&
                   cur->symbol != ' ' /* get trimmed */ &&
                   (state->wrap_style != 2)) {
            break_type = 1;
            break_at = last_breakable;
            if (break_at >= 0)
                ass_msg(render_priv->library, MSGL_DBG2, "line break at %d",
                        break_at);
        }
        if (ALLOWBREAK(cur->symbol, i)) {
            last_breakable = i;
        }

        if (break_at != -1 && break_at + 1 < text_info->length) {
            // need to use one more line
            if (text_info->n_lines >= text_info->max_lines) {
                // Try to raise the maximum number of lines
                bool success = false;
                if (text_info->max_lines <= INT_MAX / 2) {
                    text_info->max_lines *= 2;
                    success = ASS_REALLOC_ARRAY(text_info->lines, text_info->max_lines);
                }
                // If realloc fails it's screwed and due to error-info not propagating (FIXME),
                // the best we can do is to avoid UB by discarding the previous break
                if (!success) {
                    s1->linebreak = 0;
                    text_info->n_lines--;
                }
            }

            // marking break_at+1 as start of a new line
            int lead = break_at + 1; // the first symbol of the new line
            text_info->glyphs[lead].linebreak = break_type;
            last_breakable = -1;
            s1 = text_info->glyphs + lead;
            text_info->n_lines++;
        }
    }
}

/*
 * Rewind from a linestart position back to the first non-whitespace (0x20)
 * character. Trailing ASCII whitespace gets trimmed in rendering.
 * Assumes both arguments are part of the same array.
 * start2 is never dereferenced.
 */
static inline GlyphInfo *rewind_trailing_spaces(GlyphInfo *start1, GlyphInfo* start2)
{
    GlyphInfo *g = start2;
    do {
        --g;
    } while ((g > start1) && (g->symbol == ' '));
    return g;
}

/*
 * Shift soft linebreaks to balance out line lengths
 * Does not change the linebreak count
 * FIXME: implement style 0 and 3 correctly
 */
static void
wrap_lines_rebalance(RenderContext *state, double max_text_width, char *unibrks)
{
    TextInfo *text_info = &state->text_info;
    int exit = 0;

#define DIFF(x,y) (((x) < (y)) ? (y - x) : (x - y))
    while (!exit && state->wrap_style != 1) {
        exit = 1;
        GlyphInfo  *s1, *s2, *s3;
        s3 = text_info->glyphs;
        s1 = s2 = 0;
        for (int i = 0; i <= text_info->length; ++i) {
            GlyphInfo *cur = text_info->glyphs + i;
            if ((i == text_info->length) || cur->linebreak) {
                s1 = s2;
                s2 = s3;
                // WARNING: this may point one past the end and thus
                // must ONLY be used for pointer comparison; never dereferenced!
                s3 = cur;
                if (s1 && (s2->linebreak == 1)) {       // have at least 2 lines, and linebreak is 'soft'
                    double l1, l2, l1_new, l2_new;

                    // Find last word of line and trim surrounding whitespace before measuring
                    // (whitespace ' ' will also get trimmed in rendering)
                    GlyphInfo *w = rewind_trailing_spaces(s1, s2);
                    GlyphInfo *e1_old = w;
                    while ((w > s1) && (!ALLOWBREAK(w->symbol, w - text_info->glyphs))) {
                        --w;
                    }
                    GlyphInfo *e1 = w;
                    while ((e1 > s1) && (e1->symbol == ' ')) {
                        --e1;
                    }
                    if (w->symbol == ' ')
                        ++w;
                    if (w == s1)
                        continue; // Merging linebreaks is never beneficial

                    GlyphInfo *e2 = rewind_trailing_spaces(s2, s3);

                    l1 = d6_to_double(
                        (e1_old->bbox.x_max + e1_old->pos.x) -
                        (s1->bbox.x_min + s1->pos.x));
                    l2 = d6_to_double(
                        (e2->bbox.x_max + e2->pos.x) -
                        (s2->bbox.x_min + s2->pos.x));
                    l1_new = d6_to_double(
                        (e1->bbox.x_max + e1->pos.x) -
                        (s1->bbox.x_min + s1->pos.x));
                    l2_new = d6_to_double(
                        (e2->bbox.x_max + e2->pos.x) -
                        (w->bbox.x_min + w->pos.x));

                    if (DIFF(l1_new, l2_new) < DIFF(l1, l2)) {
                        w->linebreak = 1;
                        s2->linebreak = 0;
                        s2 = w;
                        exit = 0;
                    }
                }
            }
            if (i == text_info->length)
                break;
        }

    }
    assert(text_info->n_lines >= 1);
#undef DIFF
}

#undef ALLOWBREAK
#undef FORCEBREAK

/**
 * \brief rearrange text between lines
 * \param max_text_width maximal text line width in pixels
 * The algo is similar to the one in libvo/sub.c:
 * 1. Place text, wrapping it when current line is full
 * 2. Try moving words from the end of a line to the beginning of the next one while it reduces
 * the difference in lengths between this two lines.
 * The result may not be optimal, but usually is good enough.
 *
 * FIXME: implement style 0 and 3 correctly
 */
static void
wrap_lines_smart(RenderContext *state, double max_text_width)
{
    char *unibrks = NULL;

#ifdef CONFIG_UNIBREAK
    ASS_Renderer *render_priv = state->renderer;
    TextInfo *text_info = &state->text_info;
    if (render_priv->track->parser_priv->feature_flags & FEATURE_MASK(ASS_FEATURE_WRAP_UNICODE)) {
        unibrks = text_info->breaks;
        set_linebreaks_utf32(
            text_info->event_text, text_info->length,
            render_priv->track->Language, unibrks);
#if UNIBREAK_VERSION < 0x0500UL
        // Prior to 5.0 libunibreaks always ended text with LINE_BREAKMUSTBREAK, matching
        // Unicode spec, but messing with our text-overflow detection.
        // Thus reevaluate the last char in a different context.
        // (Later versions set either MUSTBREAK or the newly added INDETERMINATE)
        unibrks[text_info->length - 1] = is_line_breakable(
            text_info->event_text[text_info->length - 1],
            ' ',
            render_priv->track->Language
        );
#endif
    }
#endif

    wrap_lines_naive(state, max_text_width, unibrks);
    wrap_lines_rebalance(state, max_text_width, unibrks);

    trim_whitespace(state);
    measure_text(state);
}

/**
 * \brief Calculate base point for positioning and rotation
 * \param bbox text bbox
 * \param alignment alignment
 * \param bx, by out: base point coordinates
 */
static void get_base_point(ASS_DRect *bbox, int alignment, double *bx, double *by)
{
    const int halign = alignment & 3;
    const int valign = alignment & 12;
    if (bx)
        switch (halign) {
        case HALIGN_LEFT:
            *bx = bbox->x_min;
            break;
        case HALIGN_CENTER:
            *bx = (bbox->x_max + bbox->x_min) / 2.0;
            break;
        case HALIGN_RIGHT:
            *bx = bbox->x_max;
            break;
        }
    if (by)
        switch (valign) {
        case VALIGN_TOP:
            *by = bbox->y_min;
            break;
        case VALIGN_CENTER:
            *by = (bbox->y_max + bbox->y_min) / 2.0;
            break;
        case VALIGN_SUB:
            *by = bbox->y_max;
            break;
        }
}

/**
 * \brief Adjust the glyph's font size and scale factors to ensure smooth
 *  scaling and handle pathological font sizes. The main problem here is
 *  freetype's grid fitting, which destroys animations by font size, or will
 *  result in incorrect final text size if font sizes are very small and
 *  scale factors very large. See Google Code issue #46.
 * \param priv guess what
 * \param glyph the glyph to be modified
 */
static void
fix_glyph_scaling(ASS_Renderer *priv, GlyphInfo *glyph)
{
    double ft_size;
    if (priv->settings.hinting == ASS_HINTING_NONE) {
        // arbitrary, not too small to prevent grid fitting rounding effects
        // XXX: this is a rather crude hack
        ft_size = 256.0;
    } else {
        // If hinting is enabled, we want to pass the real font size
        // to freetype. Normalize scale_y to 1.0.
        ft_size = glyph->scale_y * glyph->font_size;
    }

    if (!ft_size || !glyph->font_size)
        return;

    double mul = glyph->font_size / ft_size;
    glyph->scale_fix = 1 / mul;
    glyph->scale_x *= mul;
    glyph->scale_y *= mul;
    glyph->font_size = ft_size;
}

// Initial run splitting based purely on the characters' styles
static void split_style_runs(RenderContext *state)
{
    TextInfo *text_info = &state->text_info;
    Effect last_effect_type = text_info->glyphs[0].effect_type;
    text_info->glyphs[0].starts_new_run = true;
    for (int i = 1; i < text_info->length; i++) {
        GlyphInfo *info = text_info->glyphs + i;
        GlyphInfo *last = text_info->glyphs + (i - 1);
        Effect effect_type = info->effect_type;
        info->starts_new_run =
            info->effect_timing ||  // but ignore effect_skip_timing
            (effect_type != EF_NONE && effect_type != last_effect_type) ||
            info->drawing_text.str ||
            last->drawing_text.str ||
            !ass_string_equal(last->font->desc.family, info->font->desc.family) ||
            last->font->desc.vertical != info->font->desc.vertical ||
            last->font_size != info->font_size ||
            last->c[0] != info->c[0] ||
            last->c[1] != info->c[1] ||
            last->c[2] != info->c[2] ||
            last->c[3] != info->c[3] ||
            last->be != info->be ||
            last->blur != info->blur ||
            last->shadow_x != info->shadow_x ||
            last->shadow_y != info->shadow_y ||
            last->frx != info->frx ||
            last->fry != info->fry ||
            last->frz != info->frz ||
            last->fax != info->fax ||
            last->fay != info->fay ||
            last->scale_x != info->scale_x ||
            last->scale_y != info->scale_y ||
            last->border_style != info->border_style ||
            last->border_x != info->border_x ||
            last->border_y != info->border_y ||
            last->hspacing != info->hspacing ||
            last->italic != info->italic ||
            last->bold != info->bold ||
            ((last->flags ^ info->flags) & ~DECO_ROTATE);
        if (effect_type != EF_NONE)
            last_effect_type = effect_type;
    }
}

// Parse event text.
// Fill render_priv->text_info.
static bool parse_events(RenderContext *state, ASS_Event *event)
{
    TextInfo *text_info = &state->text_info;
    ASS_Renderer *render_priv = state->renderer;

    char *p = event->Text, *q;

    // Event parsing.
    while (true) {
        ASS_StringView drawing_text = {NULL, 0};

        // get next char, executing style override
        // this affects render_context
        unsigned code = 0;
        while (*p) {
            if ((*p == '{') && (q = strchr(p, '}'))) {
                p = ass_parse_tags(state, p, q, 1., false);
                assert(*p == '}');
                p++;
            } else if (state->drawing_scale) {
                q = p;
                if (*p == '{')
                    q++;
                while ((*q != '{') && (*q != 0))
                    q++;
                drawing_text.str = p;
                drawing_text.len = q - p;
                code = 0xfffc; // object replacement character
                p = q;
                break;
            } else {
                code = ass_get_next_char(state, &p);
                break;
            }
        }

        if (code == 0)
            break;

        // face could have been changed in get_next_char
        if (!state->font)
            goto fail;

        if (text_info->length >= text_info->max_glyphs) {
            // Raise maximum number of glyphs
            int new_max = 2 * FFMIN(FFMAX(text_info->max_glyphs, text_info->length / 2 + 1),
                                    INT_MAX / 2);
            if (text_info->length >= new_max)
                goto fail;
            if (!ASS_REALLOC_ARRAY(text_info->glyphs, new_max) ||
                    !ASS_REALLOC_ARRAY(text_info->event_text, new_max) ||
                    !ASS_REALLOC_ARRAY(text_info->breaks, new_max))
                goto fail;
            text_info->max_glyphs = new_max;
        }

        GlyphInfo *info = &text_info->glyphs[text_info->length];

        // Clear current GlyphInfo
        memset(info, 0, sizeof(GlyphInfo));

        // Parse drawing
        if (drawing_text.str) {
            info->drawing_text = drawing_text;
            info->drawing_scale = state->drawing_scale;
            info->drawing_pbo = state->pbo;
        }

        // Fill glyph information
        info->symbol = code;
        info->font = state->font;
        for (int i = 0; i < 4; i++)
            info->c[i] = state->c[i];

        info->effect_type = state->effect_type;
        info->effect_timing = state->effect_timing;
        info->effect_skip_timing = state->effect_skip_timing;
        info->reset_effect = state->reset_effect;
        // VSFilter compatibility: font glyphs use PlayResY scaling in both dimensions
        info->font_size =
            fabs(state->font_size * state->screen_scale_y);
        info->be = state->be;
        info->blur = state->blur;
        info->shadow_x = state->shadow_x;
        info->shadow_y = state->shadow_y;
        info->scale_x = state->scale_x;
        info->scale_y = state->scale_y;
        info->border_style = state->border_style;
        info->border_x = state->border_x;
        info->border_y = state->border_y;
        info->hspacing = state->hspacing;
        info->bold = state->bold;
        info->italic = state->italic;
        info->flags = state->flags;
        if (info->font->desc.vertical && code >= VERTICAL_LOWER_BOUND)
            info->flags |= DECO_ROTATE;
        info->frx = state->frx;
        info->fry = state->fry;
        info->frz = state->frz;
        info->fax = state->fax;
        info->fay = state->fay;
        info->fade = state->fade;

        info->hspacing_scaled = 0;
        info->scale_fix = 1;

        if (!drawing_text.str) {
            info->hspacing_scaled = double_to_d6(info->hspacing *
                    state->screen_scale_x / render_priv->par_scale_x *
                    info->scale_x);
            fix_glyph_scaling(render_priv, info);
        }

        text_info->length++;

        state->effect_type = EF_NONE;
        state->effect_timing = 0;
        state->effect_skip_timing = 0;
        state->reset_effect = false;
    }

    return true;

fail:
    free_render_context(state);
    return false;
}

// Process render_priv->text_info and load glyph outlines.
static void retrieve_glyphs(RenderContext *state)
{
    GlyphInfo *glyphs = state->text_info.glyphs;
    int i;

    for (i = 0; i < state->text_info.length; i++) {
        GlyphInfo *info = glyphs + i;
        do {
            get_outline_glyph(state, info);
            info = info->next;
        } while (info);
        info = glyphs + i;

        // Add additional space after italic to non-italic style changes
        if (i && glyphs[i - 1].italic && !info->italic) {
            int back = i - 1;
            GlyphInfo *og = &glyphs[back];
            while (back && og->bbox.x_max - og->bbox.x_min == 0
                    && og->italic)
                og = &glyphs[--back];
            if (og->bbox.x_max > og->cluster_advance.x)
                og->cluster_advance.x = og->bbox.x_max;
        }

        // add horizontal letter spacing
        info->cluster_advance.x += info->hspacing_scaled;
    }
}

// Preliminary layout (for line wrapping)
static void preliminary_layout(RenderContext *state)
{
    ASS_Vector pen = { 0, 0 };
    for (int i = 0; i < state->text_info.length; i++) {
        GlyphInfo *info = state->text_info.glyphs + i;
        ASS_Vector cluster_pen = pen;
        do {
            info->pos.x = cluster_pen.x;
            info->pos.y = cluster_pen.y;

            cluster_pen.x += info->advance.x;
            cluster_pen.y += info->advance.y;

            info = info->next;
        } while (info);
        info = state->text_info.glyphs + i;
        pen.x += info->cluster_advance.x;
        pen.y += info->cluster_advance.y;
    }
}

// Reorder text into visual order
static void reorder_text(RenderContext *state)
{
    ASS_Renderer *render_priv = state->renderer;
    TextInfo *text_info = &state->text_info;
    FriBidiStrIndex *cmap = ass_shaper_reorder(state->shaper, text_info);
    if (!cmap) {
        ass_msg(render_priv->library, MSGL_ERR, "Failed to reorder text");
        ass_shaper_cleanup(state->shaper, text_info);
        free_render_context(state);
        return;
    }

    // Reposition according to the map
    ASS_Vector pen = { 0, 0 };
    int lineno = 1;
    for (int i = 0; i < text_info->length; i++) {
        GlyphInfo *info = text_info->glyphs + cmap[i];
        if (text_info->glyphs[i].linebreak) {
            pen.x = 0;
            pen.y += double_to_d6(text_info->lines[lineno-1].desc);
            pen.y += double_to_d6(text_info->lines[lineno].asc);
            pen.y += double_to_d6(render_priv->settings.line_spacing);
            lineno++;
        }
        if (info->skip)
            continue;
        ASS_Vector cluster_pen = pen;
        pen.x += info->cluster_advance.x;
        pen.y += info->cluster_advance.y;
        while (info) {
            info->pos.x = info->offset.x + cluster_pen.x;
            info->pos.y = info->offset.y + cluster_pen.y;
            cluster_pen.x += info->advance.x;
            cluster_pen.y += info->advance.y;
            info = info->next;
        }
    }
}

static void apply_baseline_shear(RenderContext *state)
{
    ASS_Renderer *render_priv = state->renderer;
    TextInfo *text_info = &state->text_info;
    FriBidiStrIndex *cmap = ass_shaper_get_reorder_map(state->shaper);
    int32_t shear = 0;
    bool whole_text_layout =
        render_priv->track->parser_priv->feature_flags &
        FEATURE_MASK(ASS_FEATURE_WHOLE_TEXT_LAYOUT);
    for (int i = 0; i < text_info->length; i++) {
        GlyphInfo *info = text_info->glyphs + cmap[i];
        if (text_info->glyphs[i].linebreak ||
            (!whole_text_layout && text_info->glyphs[i].starts_new_run))
            shear = 0;
        if (!info->scale_x || !info->scale_y)
            info->skip = true;
        if (info->skip)
            continue;
        double fay = info->fay / info->scale_x * info->scale_y;
        for (GlyphInfo *cur = info; cur; cur = cur->next) {
            cur->pos.y += shear + fay * cur->offset.x;
            shear += fay * cur->advance.x;
        }
    }
}

static void align_lines(RenderContext *state, double max_text_width)
{
    TextInfo *text_info = &state->text_info;
    GlyphInfo *glyphs = text_info->glyphs;
    int i, j;
    double width = 0;
    int last_break = -1;
    int halign = state->alignment & 3;
    int justify = state->justify;
    double max_width = 0;

    if (state->evt_type & EVENT_HSCROLL) {
        justify = halign;
        halign = HALIGN_LEFT;
    }

    for (i = 0; i <= text_info->length; ++i) {   // (text_info->length + 1) is the end of the last line
        if ((i == text_info->length) || glyphs[i].linebreak) {
            max_width = FFMAX(max_width,width);
            width = 0;
        }
        if (i < text_info->length && !glyphs[i].skip &&
                glyphs[i].symbol != '\n' && glyphs[i].symbol != 0) {
            width += d6_to_double(glyphs[i].cluster_advance.x);
        }
    }
    for (i = 0; i <= text_info->length; ++i) {   // (text_info->length + 1) is the end of the last line
        if ((i == text_info->length) || glyphs[i].linebreak) {
            double shift = 0;
            if (halign == HALIGN_LEFT) {    // left aligned, no action
                if (justify == ASS_JUSTIFY_RIGHT) {
                    shift = max_width - width;
                } else if (justify == ASS_JUSTIFY_CENTER) {
                    shift = (max_width - width) / 2.0;
                } else {
                    shift = 0;
                }
            } else if (halign == HALIGN_RIGHT) {    // right aligned
                if (justify == ASS_JUSTIFY_LEFT) {
                    shift = max_text_width - max_width;
                } else if (justify == ASS_JUSTIFY_CENTER) {
                    shift = max_text_width - max_width + (max_width - width) / 2.0;
                } else {
                    shift = max_text_width - width;
                }
            } else if (halign == HALIGN_CENTER) {   // centered
                if (justify == ASS_JUSTIFY_LEFT) {
                    shift = (max_text_width - max_width) / 2.0;
                } else if (justify == ASS_JUSTIFY_RIGHT) {
                    shift = (max_text_width - max_width) / 2.0 + max_width - width;
                } else {
                    shift = (max_text_width - width) / 2.0;
                }
            }
            for (j = last_break + 1; j < i; ++j) {
                GlyphInfo *info = glyphs + j;
                while (info) {
                    info->pos.x += double_to_d6(shift);
                    info = info->next;
                }
            }
            last_break = i - 1;
            width = 0;
        }
        if (i < text_info->length && !glyphs[i].skip &&
                glyphs[i].symbol != '\n' && glyphs[i].symbol != 0) {
            width += d6_to_double(glyphs[i].cluster_advance.x);
        }
    }
}

static void calculate_rotation_params(RenderContext *state, ASS_DRect *bbox,
                                      double device_x, double device_y)
{
    ASS_Renderer *render_priv = state->renderer;
    ASS_DVector center;
    if (state->have_origin) {
        center.x = x2scr_pos(render_priv, state->org_x);
        center.y = y2scr_pos(render_priv, state->org_y);
    } else {
        double bx = 0., by = 0.;
        get_base_point(bbox, state->alignment, &bx, &by);
        center.x = device_x + bx;
        center.y = device_y + by;
    }

    TextInfo *text_info = &state->text_info;
    for (int i = 0; i < text_info->length; i++) {
        GlyphInfo *info = text_info->glyphs + i;
        while (info) {
            info->shift.x = info->pos.x + double_to_d6(device_x - center.x +
                    info->shadow_x * state->border_scale_x /
                    render_priv->par_scale_x);
            info->shift.y = info->pos.y + double_to_d6(device_y - center.y +
                    info->shadow_y * state->border_scale_y);
            info = info->next;
        }
    }
}


static int quantize_blur(double radius, int32_t *shadow_mask)
{
    // Gaussian filter kernel (1D):
    // G(x, r2) = exp(-x^2 / (2 * r2)) / sqrt(2 * pi * r2),
    // position unit is 1/64th of pixel, r = 64 * radius, r2 = r^2.

    // Difference between kernels with different but near r2:
    // G(x, r2 + dr2) - G(x, r2) ~= dr2 * G(x, r2) * (x^2 - r2) / (2 * r2^2).
    // Maximal possible error relative to full pixel value is half of
    // integral (from -inf to +inf) of absolute value of that difference.
    // E_max ~= dr2 / 2 * integral(G(x, r2) * |x^2 - r2| / (2 * r2^2), x)
    //  = dr2 / (4 * r2) * integral(G(y, 1) * |y^2 - 1|, y)
    //  = dr2 / (4 * r2) * 4 / sqrt(2 * pi * e)
    //  ~ dr2 / (4 * r2) ~= dr / (2 * r).
    // E_max ~ BLUR_PRECISION / 2 as we have 2 dimensions.

    // To get discretized blur radius solve the following
    // differential equation (n--quantization index):
    // dr(n) / dn = BLUR_PRECISION * r + POSITION_PRECISION, r(0) = 0,
    // r(n) = (exp(BLUR_PRECISION * n) - 1) * POSITION_PRECISION / BLUR_PRECISION,
    // n = log(1 + r * BLUR_PRECISION / POSITION_PRECISION) / BLUR_PRECISION.

    // To get shadow offset quantization estimate difference of
    // G(x + dx, r2) - G(x, r2) ~= dx * G(x, r2) * (-x / r2).
    // E_max ~= dx / 2 * integral(G(x, r2) * |x| / r2, x)
    //  = dx / sqrt(2 * pi * r2) ~ dx / (2 * r).
    // 2^ord ~ dx ~ BLUR_PRECISION * r + POSITION_PRECISION.

    const double scale = 64 * BLUR_PRECISION / POSITION_PRECISION;
    radius *= scale;

    int ord;
    // ord = floor(log2(BLUR_PRECISION * r + POSITION_PRECISION))
    //     = floor(log2(64 * radius * BLUR_PRECISION + POSITION_PRECISION))
    //     = floor(log2((radius * scale + 1) * POSITION_PRECISION)),
    // floor(log2(x)) = frexp(x) - 1 = frexp(x / 2).
    frexp((1 + radius) * (POSITION_PRECISION / 2), &ord);
    *shadow_mask = ((uint32_t) 1 << ord) - 1;
    return ass_lrint(log1p(radius) / BLUR_PRECISION);
}

static double restore_blur(int qblur)
{
    const double scale = 64 * BLUR_PRECISION / POSITION_PRECISION;
    double sigma = expm1(BLUR_PRECISION * qblur) / scale;
    return sigma * sigma;
}

// Convert glyphs to bitmaps, combine them, apply blur, generate shadows.
static void render_and_combine_glyphs(RenderContext *state,
                                      double device_x, double device_y)
{
    ASS_Renderer *render_priv = state->renderer;
    TextInfo *text_info = &state->text_info;
    int left = render_priv->settings.left_margin;
    device_x = (device_x - left) * render_priv->par_scale_x + left;
    state->alloc_error = false;
    unsigned nb_bitmaps = 0;
    bool new_run = true;
    CombinedBitmapInfo *combined_info = text_info->combined_bitmaps;
    CombinedBitmapInfo *current_info = NULL;
    ASS_DVector offset;
    // Loop-invariant blur scale (depends only on the event render context).
    const double blur_radius_scale = 2 / sqrt(log(256));
    const double blur_scale_x = state->blur_scale_x * blur_radius_scale;
    const double blur_scale_y = state->blur_scale_y * blur_radius_scale;
    for (int i = 0; i < text_info->length; i++) {
        GlyphInfo *info = text_info->glyphs + i;
        if (info->starts_new_run) new_run = true;
        if (info->skip)
            continue;

        for (; info; info = info->next) {
            int flags = 0;
            if (render_priv->track->parser_priv->feature_flags &
                FEATURE_MASK(ASS_FEATURE_FAST_BLUR))
                flags |= FILTER_FAST_BLUR;
            if (info->border_style == 3)
                flags |= FILTER_BORDER_STYLE_3;
            if (info->border_x || info->border_y)
                flags |= FILTER_NONZERO_BORDER;
            if (info->shadow_x || info->shadow_y)
                flags |= FILTER_NONZERO_SHADOW;
            if (flags & FILTER_NONZERO_SHADOW &&
                (info->effect_type == EF_KARAOKE_KF ||
                 info->effect_type == EF_KARAOKE_KO ||
                 _a(info->c[0]) != 0xFF ||
                 info->border_style == 3))
                flags |= FILTER_FILL_IN_SHADOW;
            if (!(flags & FILTER_NONZERO_BORDER) &&
                !(flags & FILTER_FILL_IN_SHADOW))
                flags &= ~FILTER_NONZERO_SHADOW;
            if ((flags & FILTER_NONZERO_BORDER &&
                 _a(info->c[0]) == 0 &&
                 _a(info->c[1]) == 0 &&
                 info->fade == 0) ||
                info->border_style == 3)
                flags |= FILTER_FILL_IN_BORDER;

            if (new_run) {
                if (nb_bitmaps >= text_info->max_bitmaps) {
                    size_t old_size = text_info->max_bitmaps;
                    size_t new_size = 2 * old_size;
                    if (!ASS_REALLOC_ARRAY(text_info->combined_bitmaps, new_size)) {
                        state->alloc_error = true;
                        continue;
                    }

                    text_info->max_bitmaps = new_size;
                    combined_info = text_info->combined_bitmaps;
                    memset(combined_info + old_size, 0,
                           (new_size - old_size) * sizeof(*combined_info));
                }
                current_info = &combined_info[nb_bitmaps];

                memcpy(&current_info->c, &info->c, sizeof(info->c));
                for (int i = 0; i < 4; i++)
                    ass_apply_fade(&current_info->c[i], info->fade);

                current_info->effect_type = info->effect_type;
                current_info->effect_timing = info->effect_timing;
                current_info->leftmost_x = OUTLINE_MAX;

                FilterDesc *filter = &current_info->filter;
                filter->flags = flags;
                filter->be = info->be;

                int32_t shadow_mask_x, shadow_mask_y;
                filter->blur_x = quantize_blur(info->blur * blur_scale_x, &shadow_mask_x);
                filter->blur_y = quantize_blur(info->blur * blur_scale_y, &shadow_mask_y);
                if (flags & FILTER_NONZERO_SHADOW) {
                    int32_t x = double_to_d6(info->shadow_x * state->border_scale_x);
                    int32_t y = double_to_d6(info->shadow_y * state->border_scale_y);
                    filter->shadow.x = (x + (shadow_mask_x >> 1)) & ~shadow_mask_x;
                    filter->shadow.y = (y + (shadow_mask_y >> 1)) & ~shadow_mask_y;
                } else
                    filter->shadow.x = filter->shadow.y = 0;

                current_info->x = current_info->y = INT_MAX;
                current_info->bm = current_info->bm_o = current_info->bm_s = NULL;
                current_info->image = NULL;

                current_info->bitmap_count = 0;
                if (!current_info->bitmaps) {
                    current_info->bitmaps = malloc(MAX_SUB_BITMAPS_INITIAL * sizeof(BitmapRef));
                    if (!current_info->bitmaps) {
                        state->alloc_error = true;
                        continue;
                    }

                    current_info->max_bitmap_count = MAX_SUB_BITMAPS_INITIAL;
                }

                nb_bitmaps++;
                new_run = false;
            }
            assert(current_info);

            ASS_Vector pos, pos_o;
            info->pos.x = double_to_d6(device_x + d6_to_double(info->pos.x) * render_priv->par_scale_x);
            info->pos.y = double_to_d6(device_y) + info->pos.y;
            get_bitmap_glyph(state, info, &current_info->leftmost_x, &pos, &pos_o,
                             &offset, !current_info->bitmap_count, flags);

            if (!info->bm && !info->bm_o)
                continue;

            if (current_info->bitmap_count >= current_info->max_bitmap_count) {
                size_t new_size = 2 * current_info->max_bitmap_count;
                if (!ASS_REALLOC_ARRAY(current_info->bitmaps, new_size)) {
                    state->alloc_error = true;
                    continue;
                }

                current_info->max_bitmap_count = new_size;
            }
            current_info->bitmaps[current_info->bitmap_count].bm   = info->bm;
            current_info->bitmaps[current_info->bitmap_count].bm_o = info->bm_o;
            current_info->bitmaps[current_info->bitmap_count].pos   = pos;
            current_info->bitmaps[current_info->bitmap_count].pos_o = pos_o;
            current_info->bitmap_count++;

            current_info->x = FFMIN(current_info->x, pos.x);
            current_info->y = FFMIN(current_info->y, pos.y);
        }
    }

    for (int i = 0; i < nb_bitmaps; i++) {
        CombinedBitmapInfo *info = &combined_info[i];
        if (!info->bitmap_count)
            continue;

        if (info->effect_type == EF_KARAOKE_KF)
            info->effect_timing = lround(d6_to_double(info->leftmost_x) +
                d6_to_double(info->effect_timing) * render_priv->par_scale_x);

        for (int j = 0; j < info->bitmap_count; j++) {
            info->bitmaps[j].pos.x -= info->x;
            info->bitmaps[j].pos.y -= info->y;
            info->bitmaps[j].pos_o.x -= info->x;
            info->bitmaps[j].pos_o.y -= info->y;
        }

        CompositeHashKey key;
        key.filter = info->filter;
        key.bitmap_count = info->bitmap_count;
        key.bitmaps = info->bitmaps;
        bool owned_bitmaps = !info->reuse_bitmaps;
        key.owns_bitmaps = owned_bitmaps;
        CompositeHashValue *val = ass_cache_get(render_priv->cache.composite_cache,
                state->cache_client, &key, render_priv);
        if (owned_bitmaps)
            info->reuse_bitmaps = val != NULL;
        else if (!key.bitmaps)
            info->reuse_bitmaps = false;
        if (!key.bitmaps) {
            info->bitmaps = NULL;
            info->max_bitmap_count = 0;
        }
        if (!val) {
            state->alloc_error = true;
            continue;
        }

        if (val->bm.buffer)
            info->bm = &val->bm;
        if (val->bm_o.buffer)
            info->bm_o = &val->bm_o;
        if (val->bm_s.buffer)
            info->bm_s = &val->bm_s;
        info->image = val;
        continue;
    }

    text_info->n_bitmaps = nb_bitmaps;
}

static inline void rectangle_combine(ASS_Rect *rect, const Bitmap *bm, ASS_Vector pos)
{
    pos.x += bm->left;
    pos.y += bm->top;
    rectangle_update(rect, pos.x, pos.y, pos.x + bm->w, pos.y + bm->h);
}

/*
 * To find these values, simulate blur on the border between two
 * half-planes, one zero-filled (background) and the other filled
 * with the maximum supported value (foreground). Keep incrementing
 * the \be argument. The necessary padding is the distance by which
 * the blurred foreground image extends beyond the original border
 * and into the background. Initially it increases along with \be,
 * but very soon it grinds to a halt. At some point, the blurred
 * image actually reaches a stationary point and stays unchanged
 * forever after, simply _shifting_ by one pixel for each \be
 * step--moving in the direction of the non-zero half-plane and
 * thus decreasing the necessary padding (although the large
 * padding is still needed for intermediate results). In practice,
 * images are finite rather than infinite like half-planes, but
 * this can only decrease the required padding. Half-planes filled
 * with extreme values are the theoretical limit of the worst case.
 * Make sure to use the right pixel value range in the simulation!
 */
int ass_be_padding(int be)
{
    if (be <= 3)
        return be;
    if (be <= 7)
        return 4;
    return 5;
}


size_t ass_composite_construct(void *key, void *value, void *priv)
{
    ASS_Renderer *render_priv = priv;
    CompositeHashKey *k = key;
    CompositeHashValue *v = value;
    memset(v, 0, sizeof(*v));

    ASS_Rect rect, rect_o;
    rectangle_reset(&rect);
    rectangle_reset(&rect_o);

    size_t n_bm = 0, n_bm_o = 0;
    BitmapRef *last = NULL, *last_o = NULL;
    for (int i = 0; i < k->bitmap_count; i++) {
        BitmapRef *ref = &k->bitmaps[i];
        if (ref->bm) {
            rectangle_combine(&rect, ref->bm, ref->pos);
            last = ref;
            n_bm++;
        }
        if (ref->bm_o) {
            rectangle_combine(&rect_o, ref->bm_o, ref->pos_o);
            last_o = ref;
            n_bm_o++;
        }
    }

    int bord = ass_be_padding(k->filter.be);
    if (!bord && n_bm == 1) {
        ass_copy_bitmap(&render_priv->engine, &v->bm, last->bm);
        v->bm.left += last->pos.x;
        v->bm.top  += last->pos.y;
    } else if (n_bm && ass_alloc_bitmap(&render_priv->engine, &v->bm,
                                        rect.x_max - rect.x_min + 2 * bord,
                                        rect.y_max - rect.y_min + 2 * bord,
                                        true)) {
        Bitmap *dst = &v->bm;
        dst->left = rect.x_min - bord;
        dst->top  = rect.y_min - bord;
        for (int i = 0; i < k->bitmap_count; i++) {
            Bitmap *src = k->bitmaps[i].bm;
            if (!src)
                continue;
            int x = k->bitmaps[i].pos.x + src->left - dst->left;
            int y = k->bitmaps[i].pos.y + src->top  - dst->top;
            assert(x >= 0 && x + src->w <= dst->w);
            assert(y >= 0 && y + src->h <= dst->h);
            unsigned char *buf = dst->buffer + y * dst->stride + x;
            render_priv->engine.add_bitmaps(buf, dst->stride,
                                            src->buffer, src->stride,
                                            src->w, src->h);
        }
    }
    if (!bord && n_bm_o == 1) {
        ass_copy_bitmap(&render_priv->engine, &v->bm_o, last_o->bm_o);
        v->bm_o.left += last_o->pos_o.x;
        v->bm_o.top  += last_o->pos_o.y;
    } else if (n_bm_o && ass_alloc_bitmap(&render_priv->engine, &v->bm_o,
                                          rect_o.x_max - rect_o.x_min + 2 * bord,
                                          rect_o.y_max - rect_o.y_min + 2 * bord,
                                          true)) {
        Bitmap *dst = &v->bm_o;
        dst->left = rect_o.x_min - bord;
        dst->top  = rect_o.y_min - bord;
        for (int i = 0; i < k->bitmap_count; i++) {
            Bitmap *src = k->bitmaps[i].bm_o;
            if (!src)
                continue;
            int x = k->bitmaps[i].pos_o.x + src->left - dst->left;
            int y = k->bitmaps[i].pos_o.y + src->top  - dst->top;
            assert(x >= 0 && x + src->w <= dst->w);
            assert(y >= 0 && y + src->h <= dst->h);
            unsigned char *buf = dst->buffer + y * dst->stride + x;
            render_priv->engine.add_bitmaps(buf, dst->stride,
                                            src->buffer, src->stride,
                                            src->w, src->h);
        }
    }

    int flags = k->filter.flags;
    bool fast_blur = flags & FILTER_FAST_BLUR;
    double r2x = restore_blur(k->filter.blur_x);
    double r2y = restore_blur(k->filter.blur_y);
    if (!(flags & FILTER_NONZERO_BORDER) || (flags & FILTER_BORDER_STYLE_3))
        ass_synth_blur(&render_priv->engine, &v->bm, k->filter.be, fast_blur, r2x, r2y);
    ass_synth_blur(&render_priv->engine, &v->bm_o, k->filter.be, fast_blur, r2x, r2y);

    if (!(flags & FILTER_FILL_IN_BORDER) && !(flags & FILTER_FILL_IN_SHADOW))
        ass_fix_outline(&v->bm, &v->bm_o);

    if (flags & FILTER_NONZERO_SHADOW) {
        if (flags & FILTER_NONZERO_BORDER) {
            ass_copy_bitmap(&render_priv->engine, &v->bm_s, &v->bm_o);
            if ((flags & FILTER_FILL_IN_BORDER) && !(flags & FILTER_FILL_IN_SHADOW))
                ass_fix_outline(&v->bm, &v->bm_s);
        } else if (flags & FILTER_BORDER_STYLE_3) {
            v->bm_s = v->bm_o;
            memset(&v->bm_o, 0, sizeof(v->bm_o));
        } else {
            ass_copy_bitmap(&render_priv->engine, &v->bm_s, &v->bm);
        }

        // Works right even for negative offsets
        // '>>' rounds toward negative infinity, '&' returns correct remainder
        v->bm_s.left += k->filter.shadow.x >> 6;
        v->bm_s.top  += k->filter.shadow.y >> 6;
        ass_shift_bitmap(&v->bm_s, k->filter.shadow.x & SUBPIXEL_MASK, k->filter.shadow.y & SUBPIXEL_MASK);
    }

    if ((flags & FILTER_FILL_IN_SHADOW) && !(flags & FILTER_FILL_IN_BORDER))
        ass_fix_outline(&v->bm, &v->bm_o);

    return sizeof(CompositeHashKey) + sizeof(CompositeHashValue) +
        k->bitmap_count * sizeof(BitmapRef) +
        bitmap_size(&v->bm) + bitmap_size(&v->bm_o) + bitmap_size(&v->bm_s);
}

static void add_background(RenderContext *state, EventImages *event_images)
{
    ASS_Renderer *render_priv = state->renderer;
    int size_x = state->shadow_x > 0 ?
        lround(state->shadow_x * state->border_scale_x) : 0;
    int size_y = state->shadow_y > 0 ?
        lround(state->shadow_y * state->border_scale_y) : 0;
    int left    = event_images->left - size_x;
    int top     = event_images->top  - size_y;
    int right   = event_images->left + event_images->width  + size_x;
    int bottom  = event_images->top  + event_images->height + size_y;
    left        = FFMINMAX(left,   0, render_priv->width);
    top         = FFMINMAX(top,    0, render_priv->height);
    right       = FFMINMAX(right,  0, render_priv->width);
    bottom      = FFMINMAX(bottom, 0, render_priv->height);
    int w = right - left;
    int h = bottom - top;
    if (w < 1 || h < 1)
        return;
    uint32_t clr = state->c[3];
    ass_apply_fade(&clr, state->fade);
    if (_a(clr) == 0xFF)
        return;
    void *nbuffer = ass_aligned_alloc(1, w * h, false);
    if (!nbuffer)
        return;
    memset(nbuffer, 0xFF, w * h);
    ASS_Image *img = my_draw_bitmap(state->image_pool, nbuffer, w, h, w, left, top,
                                    clr, NULL);
    if (img) {
        img->next = event_images->imgs;
        event_images->imgs = img;
    }
}

static void setup_shaper(ASS_Shaper *shaper, ASS_Renderer *render_priv)
{
    ASS_Track *track = render_priv->track;

    ass_shaper_set_kerning(shaper, track->Kerning);
    ass_shaper_set_language(shaper, track->Language);
    ass_shaper_set_level(shaper, render_priv->settings.shaper);
#ifdef USE_FRIBIDI_EX_API
    ass_shaper_set_bidi_brackets(shaper,
            track->parser_priv->feature_flags & FEATURE_MASK(ASS_FEATURE_BIDI_BRACKETS));
#endif
    ass_shaper_set_whole_text_layout(shaper,
            track->parser_priv->feature_flags & FEATURE_MASK(ASS_FEATURE_WHOLE_TEXT_LAYOUT));
}

// ---- Static-event layout cache -------------------------------------------
//
// For an event with no time-dependent overrides (and no scrolling transition),
// the entire parse/shape/layout pipeline up to render_and_combine_glyphs is a
// pure function of (event, renderer config). We snapshot its result on the
// first render (a miss) and restore it on later frames (a hit), skipping the
// pipeline. See ass_render.h:LayoutSnapshot for the storage/lifetime model.

// Copy one laid-out glyph (cluster head or chain node) into snapshot/live
// storage: drop the per-render bitmap pointers and the drawing-text view (both
// unused past the cache boundary). Snapshot copies (take_ref) take a counted
// ref on outline/font so they survive LRU eviction; live restore copies borrow.
static void copy_layout_glyph(GlyphInfo *dst, const GlyphInfo *src, bool take_ref)
{
    *dst = *src;
    dst->bm = dst->bm_o = NULL;
    dst->drawing_text = (ASS_StringView) { NULL, 0 };
    dst->next = NULL;
    if (take_ref) {
        if (dst->outline)
            ass_cache_inc_ref(dst->outline);
        if (dst->font)
            ass_cache_inc_ref(dst->font);
    }
}

// Free a cluster next-chain (the head struct itself is array-owned, kept).
// Snapshot chains dec_ref their counted refs; live restore chains only borrow.
static void free_layout_chain(GlyphInfo *head, bool dec_ref)
{
    GlyphInfo *node = head->next;
    head->next = NULL;
    while (node) {
        GlyphInfo *next = node->next;
        if (dec_ref) {
            if (node->outline)
                ass_cache_dec_ref(node->outline);
            if (node->font)
                ass_cache_dec_ref(node->font);
        }
        free(node);
        node = next;
    }
}

static void layout_snapshot_free_contents(LayoutSnapshot *snap)
{
    for (int i = 0; i < snap->length; i++) {
        GlyphInfo *head = &snap->glyphs[i];
        free_layout_chain(head, true);
        if (head->outline)
            ass_cache_dec_ref(head->outline);
        if (head->font)
            ass_cache_dec_ref(head->font);
    }
    for (unsigned i = 0; i < snap->n_bitmaps; i++) {
        if (snap->combined_bitmaps[i].image)
            ass_cache_dec_ref(snap->combined_bitmaps[i].image);
    }
    // Drop the snapshot's ref on each shared master; source-backed cached images
    // borrow the composite pinned via combined_bitmaps above (no per-image ref).
    for (unsigned i = 0; i < snap->n_images; i++) {
        if (snap->images[i].shared)
            shared_buffer_unref(snap->images[i].shared);
    }
    free(snap->images);
    free(snap->combined_bitmaps);
    free(snap->glyphs);
    free(snap->lines);
    free(snap->clip_drawing_text);
    free(snap);
}

// Registry unlink; caller must hold layout_cache_mutex.
static void layout_registry_unlink(ASS_Renderer *priv, LayoutSnapshot *snap)
{
    if (snap->reg_prev)
        snap->reg_prev->reg_next = snap->reg_next;
    else if (priv->layout_cache == snap)
        priv->layout_cache = snap->reg_next;
    if (snap->reg_next)
        snap->reg_next->reg_prev = snap->reg_prev;
    snap->reg_prev = snap->reg_next = NULL;
}

// Evict one snapshot: unlink from the registry, detach from its event, dec_ref
// and free. dec_ref happens outside the lock.
static void free_layout_snapshot(LayoutSnapshot *snap)
{
    if (!snap)
        return;
    ASS_Renderer *priv = snap->owner;
    layout_cache_lock(priv);
    layout_registry_unlink(priv, snap);
    if (snap->back)
        snap->back->layout = NULL;
    layout_cache_unlock(priv);
    layout_snapshot_free_contents(snap);
}

// Drop every live snapshot (on generation change and at renderer teardown).
static void layout_cache_flush(ASS_Renderer *priv)
{
    layout_cache_lock(priv);
    LayoutSnapshot *snap = priv->layout_cache;
    priv->layout_cache = NULL;
    for (LayoutSnapshot *s = snap; s; s = s->reg_next)
        if (s->back)
            s->back->layout = NULL;
    layout_cache_unlock(priv);
    while (snap) {
        LayoutSnapshot *next = snap->reg_next;
        snap->reg_prev = snap->reg_next = NULL;
        layout_snapshot_free_contents(snap);
        snap = next;
    }
}

// In the default bounded mode, drop snapshots that were not used in the
// immediately preceding frame. Detach the stale entries under the registry
// lock, then release their cache references outside it.
static void layout_cache_prune(ASS_Renderer *priv, uint64_t oldest_frame)
{
    LayoutSnapshot *stale = NULL;

    layout_cache_lock(priv);
    for (LayoutSnapshot *snap = priv->layout_cache, *next; snap; snap = next) {
        next = snap->reg_next;
        if (snap->last_used_frame >= oldest_frame)
            continue;
        layout_registry_unlink(priv, snap);
        if (snap->back)
            snap->back->layout = NULL;
        snap->reg_next = stale;
        stale = snap;
    }
    layout_cache_unlock(priv);

    while (stale) {
        LayoutSnapshot *next = stale->reg_next;
        stale->reg_prev = stale->reg_next = NULL;
        layout_snapshot_free_contents(stale);
        stale = next;
    }
}

void ass_render_priv_done(RenderPriv *priv)
{
    if (!priv)
        return;
    if (priv->layout)
        free_layout_snapshot(priv->layout);
    free(priv);
}

// Memoized time-dependent-tag scan, keyed on the event's Text pointer (like
// has_hard_overrides). Combined with the evt_type scroll check, this is the
// layout-cache eligibility gate.
static bool event_has_time_dependent_tags(RenderPriv *rp, ASS_Event *event)
{
    if (!rp)
        return ass_event_has_time_dependent_tags(event->Text);
    if (rp->time_dep_text != event->Text) {
        rp->has_time_dep_tags = ass_event_has_time_dependent_tags(event->Text);
        rp->time_dep_text = event->Text;
    }
    return rp->has_time_dep_tags;
}

// Build a snapshot of the fully laid-out event (called on a cacheable miss,
// just before render_and_combine_glyphs). On any allocation failure the event
// is simply not cached (the render proceeds normally).
static void build_layout_snapshot(RenderContext *state, RenderPriv *rp,
                                  double device_x, double device_y,
                                  const ASS_DRect *bbox)
{
    ASS_Renderer *priv = state->renderer;
    TextInfo *ti = &state->text_info;
    int length = ti->length;

    // A stale snapshot should have been evicted at lookup; drop any survivor.
    if (rp->layout) {
        free_layout_snapshot(rp->layout);
        rp->layout = NULL;
    }

    LayoutSnapshot *snap = calloc(1, sizeof(*snap));
    if (!snap)
        return;
    snap->owner = priv;
    snap->back = rp;
    snap->gen = priv->layout_gen;
    snap->last_used_frame = priv->layout_frame;

    snap->glyphs = calloc(length, sizeof(GlyphInfo));
    if (length && !snap->glyphs) {
        free(snap);
        return;
    }
    for (int i = 0; i < length; i++) {
        GlyphInfo *head = &snap->glyphs[i];
        copy_layout_glyph(head, &ti->glyphs[i], true);
        GlyphInfo *src = ti->glyphs[i].next;
        GlyphInfo **tail = &head->next;
        while (src) {
            GlyphInfo *node = malloc(sizeof(GlyphInfo));
            if (!node)
                goto fail;
            copy_layout_glyph(node, src, true);
            *tail = node;
            tail = &node->next;
            src = src->next;
        }
        snap->length = i + 1;
    }

    snap->n_lines = ti->n_lines;
    if (ti->n_lines) {
        snap->lines = malloc(ti->n_lines * sizeof(LineInfo));
        if (!snap->lines)
            goto fail;
        memcpy(snap->lines, ti->lines, ti->n_lines * sizeof(LineInfo));
    }
    snap->height = ti->height;
    snap->border_top = ti->border_top;
    snap->border_bottom = ti->border_bottom;
    snap->border_x = ti->border_x;

    snap->blur_scale_x = state->blur_scale_x;
    snap->blur_scale_y = state->blur_scale_y;
    snap->border_scale_x = state->border_scale_x;
    snap->border_scale_y = state->border_scale_y;
    snap->screen_scale_x = state->screen_scale_x;
    snap->screen_scale_y = state->screen_scale_y;
    snap->border_style = state->border_style;
    snap->back_color = state->c[3];
    snap->fade = state->fade;
    snap->shadow_x = state->shadow_x;
    snap->shadow_y = state->shadow_y;
    snap->clip_x0 = state->clip_x0;
    snap->clip_y0 = state->clip_y0;
    snap->clip_x1 = state->clip_x1;
    snap->clip_y1 = state->clip_y1;
    snap->clip_mode = state->clip_mode;
    if (state->clip_drawing_text.str) {
        snap->clip_drawing_text = ass_copy_string(state->clip_drawing_text);
        if (!snap->clip_drawing_text)
            goto fail;
        snap->clip_drawing_len = state->clip_drawing_text.len;
    }
    snap->clip_drawing_scale = state->clip_drawing_scale;
    snap->clip_drawing_mode = state->clip_drawing_mode;
    snap->detect_collisions = state->detect_collisions;
    snap->alignment = state->alignment;

    snap->device_x = device_x;
    snap->device_y = device_y;
    snap->bbox = *bbox;

    layout_cache_lock(priv);
    snap->reg_prev = NULL;
    snap->reg_next = priv->layout_cache;
    if (priv->layout_cache)
        priv->layout_cache->reg_prev = snap;
    priv->layout_cache = snap;
    layout_cache_unlock(priv);
    rp->layout = snap;
    return;

fail:
    // A chain malloc failed mid-head: clean the partially-built head (not yet
    // counted in snap->length) before the generic free.
    if (snap->glyphs && snap->length < length) {
        GlyphInfo *head = &snap->glyphs[snap->length];
        free_layout_chain(head, true);
        if (head->outline)
            ass_cache_dec_ref(head->outline);
        if (head->font)
            ass_cache_dec_ref(head->font);
    }
    layout_snapshot_free_contents(snap);
}

// Restore a snapshot into the live (thread-local) RenderContext, ready to skip
// straight to render_and_combine_glyphs. Returns false on allocation failure,
// leaving text_info empty so the caller can fall back to the full pipeline.
static bool restore_layout_snapshot(RenderContext *state, const LayoutSnapshot *snap,
                                    ASS_Event *event, double *device_x,
                                    double *device_y, ASS_DRect *bbox)
{
    TextInfo *ti = &state->text_info;
    int length = snap->length;

    // The snapshot may have been built by another worker whose RenderContext
    // grew larger; grow glyphs/event_text/breaks together to preserve the
    // invariant the miss path relies on.
    if (length > ti->max_glyphs) {
        int new_max = length;
        if (!ASS_REALLOC_ARRAY(ti->glyphs, new_max) ||
                !ASS_REALLOC_ARRAY(ti->event_text, new_max) ||
                !ASS_REALLOC_ARRAY(ti->breaks, new_max))
            return false;
        ti->max_glyphs = new_max;
    }
    if (snap->n_lines > ti->max_lines) {
        int new_max = snap->n_lines;
        if (!ASS_REALLOC_ARRAY(ti->lines, new_max))
            return false;
        ti->max_lines = new_max;
    }

    for (int i = 0; i < length; i++) {
        copy_layout_glyph(&ti->glyphs[i], &snap->glyphs[i], false);
        GlyphInfo *src = snap->glyphs[i].next;
        GlyphInfo **tail = &ti->glyphs[i].next;
        while (src) {
            GlyphInfo *node = malloc(sizeof(GlyphInfo));
            if (!node) {
                for (int k = 0; k <= i; k++)
                    free_layout_chain(&ti->glyphs[k], false);
                ti->length = 0;
                return false;
            }
            copy_layout_glyph(node, src, false);
            *tail = node;
            tail = &node->next;
            src = src->next;
        }
    }
    ti->length = length;
    ti->n_lines = snap->n_lines;
    if (snap->n_lines)
        memcpy(ti->lines, snap->lines, snap->n_lines * sizeof(LineInfo));
    ti->height = snap->height;
    ti->border_top = snap->border_top;
    ti->border_bottom = snap->border_bottom;
    ti->border_x = snap->border_x;

    state->event = event;
    state->blur_scale_x = snap->blur_scale_x;
    state->blur_scale_y = snap->blur_scale_y;
    state->border_scale_x = snap->border_scale_x;
    state->border_scale_y = snap->border_scale_y;
    state->screen_scale_x = snap->screen_scale_x;
    state->screen_scale_y = snap->screen_scale_y;
    state->border_style = snap->border_style;
    state->c[3] = snap->back_color;
    state->fade = snap->fade;
    state->shadow_x = snap->shadow_x;
    state->shadow_y = snap->shadow_y;
    state->clip_x0 = snap->clip_x0;
    state->clip_y0 = snap->clip_y0;
    state->clip_x1 = snap->clip_x1;
    state->clip_y1 = snap->clip_y1;
    state->clip_mode = snap->clip_mode;
    state->clip_drawing_text.str = snap->clip_drawing_text;
    state->clip_drawing_text.len = snap->clip_drawing_len;
    state->clip_drawing_scale = snap->clip_drawing_scale;
    state->clip_drawing_mode = snap->clip_drawing_mode;
    state->detect_collisions = snap->detect_collisions;
    state->alignment = snap->alignment;

    *device_x = snap->device_x;
    *device_y = snap->device_y;
    *bbox = snap->bbox;
    return true;
}

static ASS_Image *ass_free_image(ASS_Image *img);

// Capture the emitted ASS_Image list of a static event (post render_text +
// add_background) so a later hit can skip render_text entirely. Runs on the miss
// frame, after the combined snapshot is built. Each image is either source-backed
// (borrows the composite pinned via combined_bitmaps) or owns a buffer (vector
// clip / opaque-box background) whose pixels are captured once into a ref-counted
// read-only SharedBuffer (no per-clone copy). On any allocation failure the
// partial capture is rolled back and images_cached stays false, so hits fall back
// to render_text.
static void build_image_snapshot(RenderContext *state, LayoutSnapshot *snap,
                                 ASS_Image *imgs)
{
    unsigned n = 0;
    for (ASS_Image *cur = imgs; cur; cur = cur->next)
        n++;
    if (!n) {
        snap->images_cached = true;   // trivial (empty) list is a valid cache
        return;
    }
    CachedImage *arr = malloc(n * sizeof(CachedImage));
    if (!arr)
        return;
    unsigned align = 1 << state->renderer->engine.align_order;
    unsigned i = 0;
    for (ASS_Image *cur = imgs; cur; cur = cur->next, i++) {
        ASS_ImagePriv *priv = (ASS_ImagePriv *) cur;
        CachedImage *ci = &arr[i];
        ci->result = *cur;            // struct fields incl. the bitmap pointer
        ci->result.next = NULL;
        ci->source = priv->source;    // NULL => owned-buffer; else pinned composite
        ci->shared = NULL;
        if (!priv->source) {
            // Owned-buffer image: bitmap == buffer (start of the aligned alloc),
            // valid extent stride*(h-1)+w per the ASS_Image contract.
            size_t size = cur->h > 0 ? (size_t) cur->stride * (cur->h - 1) + cur->w : 0;
            SharedBuffer *sb = size ? shared_buffer_new(align, cur->bitmap, size) : NULL;
            if (!sb) {                // OOM or degenerate 0-size owned buffer: don't cache
                for (unsigned k = 0; k < i; k++)
                    if (arr[k].shared)
                        shared_buffer_unref(arr[k].shared);
                free(arr);
                return;
            }
            ci->shared = sb;
            ci->result.bitmap = sb->data;   // clones point into the shared master
        }
        // source-backed: ci->result.bitmap keeps its pointer into the pinned
        // composite buffer (kept alive by the combined snapshot's inc_ref).
    }
    snap->images = arr;
    snap->n_images = n;
    snap->images_cached = true;
}

// Reconstruct the cached emitted ASS_Image list into fresh pool shells. Source-
// backed images inc_ref the pinned composite; owned-buffer images take a ref on
// the shared read-only master — so every emitted image independently keeps its
// bitmap alive (surviving snapshot free / gen bump), exactly as render_text's
// output does, with no per-clone pixel copy. Returns the list via *out and true;
// on pool-shell OOM frees the partial list and returns false (caller runs
// render_text instead).
static bool clone_image_snapshot(RenderContext *state, const LayoutSnapshot *snap,
                                 ASS_Image **out)
{
    ASS_Image *head = NULL, **tail = &head;
    for (unsigned i = 0; i < snap->n_images; i++) {
        const CachedImage *ci = &snap->images[i];
        ASS_ImagePriv *img = image_pool_alloc(state->image_pool);
        if (!img) {
            for (ASS_Image *cur = head; cur; )
                cur = ass_free_image(cur);
            *out = NULL;
            return false;
        }
        img->result = ci->result;     // incl. bitmap ptr into composite or master
        img->result.next = NULL;
        img->ref_count = 0;
        img->buffer = NULL;
        img->source = ci->source;
        img->shared = ci->shared;
        if (ci->source)
            ass_cache_inc_ref(ci->source);
        else
            shared_buffer_ref(ci->shared);
        *tail = &img->result;         // link before next iter so fail path frees it
        tail = &img->result.next;
    }
    *out = head;
    return true;
}

// Extend a freshly-built layout snapshot with the combined-bitmap output of
// render_and_combine_glyphs, so later frames skip that per-frame work too. Holds
// an inc_ref on each group's composite `image` (which owns bm/bm_o/bm_s), just as
// the snapshot holds outline/font refs. render_text() reads only bm/bm_o/bm_s,
// x/y, c[], effect_*, and image, so the per-group `bitmaps` key array (owned by
// the RenderContext) is not copied. Returns false on OOM (caller drops the
// whole snapshot, so a surviving snapshot always carries its combined bitmaps).
static bool build_combined_snapshot(LayoutSnapshot *snap, TextInfo *ti)
{
    unsigned n = ti->n_bitmaps;
    // A group that produced a composite (image != NULL) but whose bm/bm_o/bm_s are
    // all empty is an allocation-failure artifact: a successful composite of a
    // non-empty group (bitmap_count > 0, guaranteed here — empty groups are
    // skipped in render_and_combine_glyphs) always yields at least one non-empty
    // buffer. Refuse to snapshot such an event: pinning (inc_ref) the empty
    // composite would freeze the best-effort-degraded render on every later hit,
    // defeating the self-heal a re-render gives once memory frees up. Falling back
    // to running render_and_combine_glyphs each frame tracks the shared composite
    // cache exactly as the non-cached path does.
    for (unsigned i = 0; i < n; i++) {
        CombinedBitmapInfo *s = &ti->combined_bitmaps[i];
        if (s->image && !s->bm && !s->bm_o && !s->bm_s)
            return false;
    }
    if (n) {
        snap->combined_bitmaps = malloc(n * sizeof(CombinedBitmapInfo));
        if (!snap->combined_bitmaps)
            return false;
        for (unsigned i = 0; i < n; i++) {
            CombinedBitmapInfo *dst = &snap->combined_bitmaps[i];
            *dst = ti->combined_bitmaps[i];
            dst->bitmaps = NULL;          // key array belongs to the RenderContext
            dst->max_bitmap_count = 0;
            dst->bitmap_count = 0;
            dst->reuse_bitmaps = false;
            if (dst->image)
                ass_cache_inc_ref(dst->image);
        }
    }
    snap->n_bitmaps = n;
    return true;
}

// Restore cached combined bitmaps into the live RenderContext, ready to skip
// straight to render_text(). Copies only the fields render_text/add_background
// read, preserving the context's reusable per-group `bitmaps` key arrays. The
// composite `image` (and thus bm/bm_o/bm_s) is kept alive by the snapshot's ref.
// Returns false on OOM (caller falls back to running render_and_combine_glyphs).
static bool restore_combined_snapshot(RenderContext *state, const LayoutSnapshot *snap)
{
    TextInfo *ti = &state->text_info;
    unsigned n = snap->n_bitmaps;
    if (n > ti->max_bitmaps) {
        unsigned old_max = ti->max_bitmaps;
        unsigned new_max = n;
        if (!ASS_REALLOC_ARRAY(ti->combined_bitmaps, new_max))
            return false;
        memset(ti->combined_bitmaps + old_max, 0,
               (new_max - old_max) * sizeof(CombinedBitmapInfo));
        ti->max_bitmaps = new_max;
    }
    for (unsigned i = 0; i < n; i++) {
        CombinedBitmapInfo *dst = &ti->combined_bitmaps[i];
        const CombinedBitmapInfo *src = &snap->combined_bitmaps[i];
        dst->filter = src->filter;
        dst->c[0] = src->c[0]; dst->c[1] = src->c[1];
        dst->c[2] = src->c[2]; dst->c[3] = src->c[3];
        dst->effect_type = src->effect_type;
        dst->effect_timing = src->effect_timing;
        dst->leftmost_x = src->leftmost_x;
        dst->x = src->x;
        dst->y = src->y;
        dst->bm = src->bm;
        dst->bm_o = src->bm_o;
        dst->bm_s = src->bm_s;
        dst->image = src->image;
    }
    ti->n_bitmaps = n;
    return true;
}

/**
 * \brief Main ass rendering function, glues everything together
 * \param event event to render
 * \param event_images struct containing resulting images, will also be initialized
 * Process event, appending resulting ASS_Image's to images_root.
 */
static bool
ass_render_event(RenderContext *state, EventImages *event_images)
{
    ASS_Event *event = event_images->event;

    ASS_Renderer *render_priv = state->renderer;
    if (event->Style >= render_priv->track->n_styles) {
        ass_msg(render_priv->library, MSGL_WARN, "No style found");
        return false;
    }
    if (!event->Text) {
        ass_msg(render_priv->library, MSGL_WARN, "Empty event");
        return false;
    }

    TextInfo *text_info = &state->text_info;

    // Shared geometry, produced by either the layout-cache restore (hit) or the
    // full pipeline (miss) below, and consumed from the `render:` label onward.
    int valign = 0;
    double device_x = 0;
    double device_y = 0;
    ASS_DRect bbox;
    bool built_layout = false;   // this frame built a fresh snapshot (miss path)

    // On a static-layout-cache hit, restore the laid-out state and skip the
    // whole parse/shape/layout pipeline.
    RenderPriv *rp = get_render_priv(render_priv, event);
    // A track can be shared by several renderers; the single per-event snapshot
    // slot belongs to whichever renderer filled it. Only the owning renderer may
    // use it — its outline/font refs live in that renderer's caches, so a foreign
    // snapshot must be left untouched (this renderer just runs the full pipeline
    // without caching while the other renderer is alive).
    bool foreign_snapshot = rp && rp->layout && rp->layout->owner != render_priv;
    if (rp && rp->layout && !foreign_snapshot) {
        if (rp->layout->gen == render_priv->layout_gen &&
                restore_layout_snapshot(state, rp->layout, event,
                                        &device_x, &device_y, &bbox)) {
            rp->layout->last_used_frame = render_priv->layout_frame;
            valign = state->alignment & 12;
            // A live snapshot always carries its combined bitmaps: restore them
            // and skip render_and_combine_glyphs too. Only on an OOM growing the
            // combined array do we fall back to running render_and_combine.
            if (restore_combined_snapshot(state, rp->layout))
                goto render_combined;
            goto render;
        }
        // Own snapshot but stale generation, or a restore that ran out of
        // memory: drop it and fall back to the full pipeline.
        free_layout_snapshot(rp->layout);
    }

    {   // --- full layout pipeline (cache miss) ---
    setup_shaper(state->shaper, render_priv);

    free_render_context(state);
    init_render_context(state, event);

    if (!parse_events(state, event))
        return false;

    if (text_info->length == 0) {
        // no valid symbols in the event; this can be smth like {comment}
        free_render_context(state);
        return false;
    }

    split_style_runs(state);

    // Find shape runs and shape text
    ass_shaper_set_base_direction(state->shaper,
            ass_resolve_base_direction(state->font_encoding));
    ass_shaper_find_runs(state->shaper, render_priv, text_info->glyphs,
            text_info->length);
    if (!ass_shaper_shape(state->shaper, text_info)) {
        ass_msg(render_priv->library, MSGL_ERR, "Failed to shape text");
        free_render_context(state);
        return false;
    }

    retrieve_glyphs(state);

    preliminary_layout(state);

    valign = state->alignment & 12;

    int MarginL =
        (event->MarginL) ? event->MarginL : state->style->MarginL;
    int MarginR =
        (event->MarginR) ? event->MarginR : state->style->MarginR;
    int MarginV =
        (event->MarginV) ? event->MarginV : state->style->MarginV;

    // calculate max length of a line
    double max_text_width =
        x2scr_right(state, render_priv->track->PlayResX - MarginR) -
        x2scr_left(state, MarginL);

    // wrap lines
    wrap_lines_smart(state, max_text_width);

    // depends on glyph x coordinates being monotonous within runs, so it should be done before reorder
    ass_process_karaoke_effects(state);

    reorder_text(state);

    align_lines(state, max_text_width);

    // determine text bounding box
    compute_string_bbox(text_info, &bbox);

    apply_baseline_shear(state);

    // determine device coordinates for text
    device_x = 0;
    device_y = 0;

    // handle positioned events first: an event can be both positioned and
    // scrolling, and the scrolling effect overrides the position on one axis
    if (state->evt_type & EVENT_POSITIONED) {
        double base_x = 0;
        double base_y = 0;
        get_base_point(&bbox, state->alignment, &base_x, &base_y);
        device_x =
            x2scr_pos(render_priv, state->pos_x) - base_x;
        device_y =
            y2scr_pos(render_priv, state->pos_y) - base_y;
    }

    // x coordinate
    if (state->evt_type & EVENT_HSCROLL) {
        if (state->scroll_direction == SCROLL_RL)
            device_x =
                x2scr_pos(render_priv,
                      render_priv->track->PlayResX -
                      state->scroll_shift);
        else if (state->scroll_direction == SCROLL_LR)
            device_x =
                x2scr_pos(render_priv, state->scroll_shift) -
                (bbox.x_max - bbox.x_min);
    } else if (!(state->evt_type & EVENT_POSITIONED)) {
        device_x = x2scr_left(state, MarginL);
    }

    // y coordinate
    if (state->evt_type & EVENT_VSCROLL) {
        if (state->scroll_direction == SCROLL_TB)
            device_y =
                y2scr(state,
                      state->scroll_y0 +
                      state->scroll_shift) -
                bbox.y_max;
        else if (state->scroll_direction == SCROLL_BT)
            device_y =
                y2scr(state,
                      state->scroll_y1 -
                      state->scroll_shift) -
                bbox.y_min;
    } else if (!(state->evt_type & EVENT_POSITIONED)) {
        if (valign == VALIGN_TOP) {     // toptitle
            device_y =
                y2scr_top(state,
                          MarginV) + text_info->lines[0].asc;
        } else if (valign == VALIGN_CENTER) {   // midtitle
            double scr_y =
                y2scr(state, render_priv->track->PlayResY / 2.0);
            device_y = scr_y - (bbox.y_max + bbox.y_min) / 2.0;
        } else {                // subtitle
            double line_pos = state->explicit ?
                0 : render_priv->settings.line_position;
            double scr_top, scr_bottom, scr_y0;
            if (valign != VALIGN_SUB)
                ass_msg(render_priv->library, MSGL_V,
                       "Invalid valign, assuming 0 (subtitle)");
            scr_bottom =
                y2scr_sub(state,
                          render_priv->track->PlayResY - MarginV);
            scr_top = y2scr_top(state, 0); //xxx not always 0?
            device_y = scr_bottom + (scr_top - scr_bottom) * line_pos / 100.0;
            device_y -= text_info->height;
            device_y += text_info->lines[0].asc;
            // clip to top to avoid confusion if line_position is very high,
            // turning the subtitle into a toptitle
            // also, don't change behavior if line_position is not used
            scr_y0 = scr_top + text_info->lines[0].asc;
            if (device_y < scr_y0 && line_pos > 0) {
                device_y = scr_y0;
            }
        }
    }

    // fix clip coordinates
    if (state->explicit || !render_priv->settings.use_margins) {
        state->clip_x0 =
            lround(x2scr_pos_scaled(render_priv, state->clip_x0));
        state->clip_x1 =
            lround(x2scr_pos_scaled(render_priv, state->clip_x1));
        state->clip_y0 =
            lround(y2scr_pos(render_priv, state->clip_y0));
        state->clip_y1 =
            lround(y2scr_pos(render_priv, state->clip_y1));

        if (state->explicit) {
            // we still need to clip against screen boundaries
            int zx = render_priv->settings.left_margin;
            int zy = render_priv->settings.top_margin;
            int sx = zx + render_priv->frame_content_width;
            int sy = zy + render_priv->frame_content_height;

            state->clip_x0 = FFMAX(state->clip_x0, zx);
            state->clip_y0 = FFMAX(state->clip_y0, zy);
            state->clip_x1 = FFMIN(state->clip_x1, sx);
            state->clip_y1 = FFMIN(state->clip_y1, sy);
        }
    } else {
        // no \clip (explicit==0) and use_margins => only clip to screen with margins
        state->clip_x0 = 0;
        state->clip_y0 = 0;
        state->clip_x1 = render_priv->settings.frame_width;
        state->clip_y1 = render_priv->settings.frame_height;
    }

    if (state->evt_type & EVENT_VSCROLL) {
        int y0 = lround(y2scr_pos(render_priv, state->scroll_y0));
        int y1 = lround(y2scr_pos(render_priv, state->scroll_y1));

        state->clip_y0 = FFMAX(state->clip_y0, y0);
        state->clip_y1 = FFMIN(state->clip_y1, y1);
    }

    calculate_rotation_params(state, &bbox, device_x, device_y);

    // Cache this static event's laid-out state so later frames hit. Eligible =
    // no scrolling/banner transition and no time-dependent override tags. Skip
    // when another renderer owns the snapshot slot for this shared event.
    if (rp && !foreign_snapshot &&
            !(state->evt_type & (EVENT_HSCROLL | EVENT_VSCROLL)) &&
            !event_has_time_dependent_tags(rp, event)) {
        build_layout_snapshot(state, rp, device_x, device_y, &bbox);
        built_layout = rp->layout != NULL;
    }
    }   // --- end full layout pipeline (cache miss) ---

render:
    render_and_combine_glyphs(state, device_x, device_y);

    // Extend the fresh snapshot with the combined bitmaps just produced, so later
    // frames skip render_and_combine too. If render_and_combine hit an allocation
    // failure (its best-effort partial output would otherwise be frozen and the
    // dropped glyphs lost on every future frame), or the copy itself OOMs, drop
    // the whole snapshot so the next frame re-renders and self-heals. Either way
    // the current frame is unaffected (it renders from the live text_info below).
    if (built_layout &&
            (state->alloc_error || !build_combined_snapshot(rp->layout, text_info)))
        free_layout_snapshot(rp->layout);

render_combined:
    // VSFilter does *not* shift lines with a border > margin to be within the
    // frame, so negative values for top and left may occur
    event_images->top = device_y - text_info->lines[0].asc - text_info->border_top;
    event_images->height =
        text_info->height + text_info->border_bottom + text_info->border_top;
    event_images->left =
        (device_x + bbox.x_min) * render_priv->par_scale_x - text_info->border_x + 0.5;
    event_images->width =
        (bbox.x_max - bbox.x_min) * render_priv->par_scale_x
        + 2 * text_info->border_x + 0.5;
    event_images->detect_collisions = state->detect_collisions;
    event_images->shift_direction = (valign == VALIGN_SUB) ? -1 : 1;

    // Image-list cache: on a hit for a static event, clone the cached emitted
    // images (skipping render_text/add_background). Any per-frame collision shift
    // is applied by the frame assembly to this (pre-shift) clone exactly as it is
    // to render_text's output, so caching stays bit-exact.
    //
    // Honor the same ownership/generation invariant as the restore paths (3756,
    // 3956): only clone THIS renderer's own, current-generation snapshot. A
    // foreign snapshot (shared track rendered by another renderer) was never
    // restored into this frame's text_info — its images belong to a different
    // config — and is also mutable state guarded by the OTHER renderer's lock, so
    // cloning it would emit wrong output and race its free. Fall through to
    // render_text instead, exactly as the miss path does for a foreign event.
    if (!built_layout && rp && rp->layout && !foreign_snapshot &&
            rp->layout->gen == render_priv->layout_gen &&
            rp->layout->images_cached &&
            clone_image_snapshot(state, rp->layout, &event_images->imgs)) {
        // cached list already includes any border-style-4 background
    } else {
        event_images->imgs = render_text(state);

        if (state->border_style == 4)
            add_background(state, event_images);

        // On the miss frame, capture the emitted (pre-shift) list so later hits
        // can clone it — only when the snapshot survived (rp->layout != NULL, so
        // its inc_ref keeps every source-backed image's composite alive).
        if (built_layout && rp && rp->layout)
            build_image_snapshot(state, rp->layout, event_images->imgs);
    }

    ass_shaper_cleanup(state->shaper, text_info);
    free_render_context(state);

    return true;
}

#if ENABLE_THREADS
#define MIN_THREADED_EVENTS 2

static void *run_thread(void *ptr)
{
    ASS_Renderer *priv = ptr;

    RenderContext state = {0};

    thread_set_name("libass/render");

    bool success = render_context_init(&state, priv);

    pthread_mutex_lock(&priv->mutex);

    if (!success) {
        ass_msg(priv->library, MSGL_WARN, "Thread setup failed; disabling");
        priv->thread_start_failed = 1;
        pthread_cond_broadcast(&priv->main_cond);
        goto fail;
    }

    priv->started_threads++;

    pthread_cond_broadcast(&priv->main_cond);

    for (;;) {
        while (!priv->shutting_down &&
               (atomic_load_explicit(&priv->next_eimg, memory_order_relaxed) >=
                atomic_load_explicit(&priv->sent_eimgs, memory_order_relaxed)))
            pthread_cond_wait(&priv->pool_cond, &priv->mutex);

        if (priv->shutting_down)
            break;

        priv->active_threads++;

        pthread_mutex_unlock(&priv->mutex);

        uintptr_t got_eimg;
        bool cascaded = false;
        while ((got_eimg = atomic_fetch_add_explicit(&priv->next_eimg, 1, memory_order_relaxed)) < atomic_load_explicit(&priv->sent_eimgs, memory_order_relaxed)) {
            // Cascade: wake one sibling the first time we confirm there is more
            // work, so the pool ramps up to the real parallelism (at most one
            // signal per worker -> at most n_threads signals/frame, never per
            // event). Done before rendering so the sibling wakes during this
            // render. Frames the main thread can handle alone never reach here
            // (no slot grabbed -> no cascade), so they don't wake the pool.
            if (!cascaded && got_eimg + 1 < atomic_load_explicit(&priv->sent_eimgs, memory_order_relaxed)) {
                cascaded = true;
                pthread_mutex_lock(&priv->mutex);
                pthread_cond_signal(&priv->pool_cond);
                pthread_mutex_unlock(&priv->mutex);
            }

            EventImages *imgs = priv->eimg + got_eimg;

            ass_render_event(&state, imgs);

            if (atomic_fetch_sub_explicit(&priv->processing_eimgs, 1, memory_order_acq_rel) - 1 == 0)
                break;
        }

        pthread_mutex_lock(&priv->mutex);

        priv->active_threads--;

        pthread_cond_signal(&priv->main_cond);
    }

fail:

    pthread_mutex_unlock(&priv->mutex);

    render_context_done(&state);

    return NULL;
}
#endif

/**
 * \brief Check cache limits and reset cache if they are exceeded
 */
static void check_cache_limits(ASS_Renderer *priv, CacheStore *cache)
{
    ass_cache_cut(cache->composite_cache, cache->composite_max_size);
    ass_cache_cut(cache->bitmap_cache, cache->bitmap_max_size);
    ass_cache_cut(cache->outline_cache, cache->glyph_max);
}

/**
 * \brief Start a new frame
 */
static bool
ass_start_frame(ASS_Renderer *render_priv, ASS_Track *track,
                long long now)
{
    if (!render_priv->settings.frame_width
        && !render_priv->settings.frame_height)
        return false;               // library not initialized

    if (!render_priv->fontselect)
        return false;

    if (render_priv->library != track->library)
        return false;

    if (track->n_events == 0)
        return false;               // nothing to do

    render_priv->track = track;
    render_priv->time = now;

    ass_lazy_track_init(render_priv->library, render_priv->track);

    if (render_priv->library->num_fontdata != render_priv->num_emfonts) {
        assert(render_priv->library->num_fontdata > render_priv->num_emfonts);
        render_priv->num_emfonts = ass_update_embedded_fonts(
            render_priv->fontselect, render_priv->num_emfonts);
    }

    // PAR correction
    double par = render_priv->settings.par;
    bool lr_track = track->LayoutResX > 0 && track->LayoutResY > 0;
    if (par == 0. || lr_track) {
        if (render_priv->frame_content_width && render_priv->frame_content_height && (lr_track ||
                (render_priv->settings.storage_width && render_priv->settings.storage_height))) {
            double dar = ((double) render_priv->frame_content_width) /
                         render_priv->frame_content_height;
            ASS_Vector layout_res = ass_layout_res(render_priv);
            double sar = ((double) layout_res.x) / layout_res.y;
            par = dar / sar;
        } else
            par = 1.0;
    }
    render_priv->par_scale_x = par;

    render_priv->prev_images_root = render_priv->images_root;
    render_priv->images_root = NULL;

    // Layout cache invalidation: bump layout_gen on any track-side layout input
    // that has no setter funnel (renderer-config setters bump it directly). This
    // is the single per-frame funnel for track edits, embedded-font reload and
    // per-track feature toggles. Whenever the gen advances (here or via a
    // setter), flush the snapshot registry so stale entries stop pinning cache
    // entries.
    if (!render_priv->layout_seen.valid ||
        render_priv->layout_seen.track != track ||
        render_priv->layout_seen.play_res_x != track->PlayResX ||
        render_priv->layout_seen.play_res_y != track->PlayResY ||
        render_priv->layout_seen.layout_res_x != track->LayoutResX ||
        render_priv->layout_seen.layout_res_y != track->LayoutResY ||
        render_priv->layout_seen.wrap_style != track->WrapStyle ||
        render_priv->layout_seen.kerning != track->Kerning ||
        render_priv->layout_seen.scaled_border_and_shadow != track->ScaledBorderAndShadow ||
        render_priv->layout_seen.feature_flags != track->parser_priv->feature_flags ||
        render_priv->layout_seen.num_emfonts != render_priv->num_emfonts) {
        render_priv->layout_gen++;
        render_priv->layout_seen.valid = true;
        render_priv->layout_seen.track = track;
        render_priv->layout_seen.play_res_x = track->PlayResX;
        render_priv->layout_seen.play_res_y = track->PlayResY;
        render_priv->layout_seen.layout_res_x = track->LayoutResX;
        render_priv->layout_seen.layout_res_y = track->LayoutResY;
        render_priv->layout_seen.wrap_style = track->WrapStyle;
        render_priv->layout_seen.kerning = track->Kerning;
        render_priv->layout_seen.scaled_border_and_shadow = track->ScaledBorderAndShadow;
        render_priv->layout_seen.feature_flags = track->parser_priv->feature_flags;
        render_priv->layout_seen.num_emfonts = render_priv->num_emfonts;
    }
    if (render_priv->layout_gen != render_priv->layout_gen_flushed) {
        layout_cache_flush(render_priv);
        render_priv->layout_gen_flushed = render_priv->layout_gen;
    }

    // By default, snapshots cover runs of consecutive active frames only. The
    // opt-in feature keeps the old persistent behavior for seek/replay-heavy
    // clients. Pruning happens before workers start, after the previous frame is
    // complete, so registry entries cannot be in use here.
    render_priv->layout_frame++;
    if (!(track->parser_priv->feature_flags &
            FEATURE_MASK(ASS_FEATURE_CACHE_LAYOUT)) &&
            render_priv->layout_frame > 1 && render_priv->layout_cache)
        layout_cache_prune(render_priv, render_priv->layout_frame - 1);

    ass_cache_promote(&render_priv->cache.client_set);
    check_cache_limits(render_priv, &render_priv->cache);

    return true;
}

static int cmp_event_layer(const void *p1, const void *p2)
{
    ASS_Event *e1 = ((EventImages *) p1)->event;
    ASS_Event *e2 = ((EventImages *) p2)->event;
    if (!e1 && !e2)
        return 0;
    if (e1 && !e2)
        return -1;
    if (!e1 && e2)
        return 1;
    if (e1->Layer < e2->Layer)
        return -1;
    if (e1->Layer > e2->Layer)
        return 1;
    if (e1->ReadOrder < e2->ReadOrder)
        return -1;
    if (e1->ReadOrder > e2->ReadOrder)
        return 1;
    return 0;
}

static bool stable_sort_events_by_layer(ASS_Renderer *render_priv,
                                        EventImages *events, int cnt,
                                        int min_layer, int max_layer)
{
    long long layer_range_ll = (long long) max_layer - min_layer + 1;
    if (layer_range_ll <= 0 || layer_range_ll > MAX_LAYER_SORT_RANGE)
        return false;

    if (cnt > render_priv->eimg_sorted_size) {
        EventImages *sorted =
            ass_realloc_array(render_priv->eimg_sorted, cnt, sizeof(*sorted));
        if (!sorted)
            return false;
        render_priv->eimg_sorted = sorted;
        render_priv->eimg_sorted_size = cnt;
    }

    int layer_range = (int) layer_range_ll;
    int counts[MAX_LAYER_SORT_RANGE] = { 0 };
    int offsets[MAX_LAYER_SORT_RANGE];
    for (int i = 0; i < cnt; i++)
        counts[(int) ((long long) events[i].event->Layer - min_layer)]++;

    int pos = 0;
    for (int i = 0; i < layer_range; i++) {
        offsets[i] = pos;
        pos += counts[i];
    }

    for (int i = 0; i < cnt; i++) {
        int layer = (int) ((long long) events[i].event->Layer - min_layer);
        render_priv->eimg_sorted[offsets[layer]++] = events[i];
    }

    memcpy(events, render_priv->eimg_sorted, cnt * sizeof(*events));
    return true;
}

static ASS_RenderPriv *get_render_priv(ASS_Renderer *render_priv,
                                       ASS_Event *event)
{
    if (!event->render_priv) {
        event->render_priv = calloc(1, sizeof(ASS_RenderPriv));
        if (!event->render_priv)
            return NULL;
    }
    if (render_priv->render_id != event->render_priv->render_id) {
        // The layout cache is normally flushed in ass_start_frame before any
        // event is rendered, so ->layout is already NULL here; free defensively
        // to avoid orphaning a snapshot if it somehow survives.
        if (event->render_priv->layout)
            free_layout_snapshot(event->render_priv->layout);
        memset(event->render_priv, 0, sizeof(ASS_RenderPriv));
        event->render_priv->render_id = render_priv->render_id;
    }

    return event->render_priv;
}

static bool event_has_hard_overrides(ASS_Renderer *render_priv,
                                     ASS_Event *event)
{
    ASS_RenderPriv *priv = get_render_priv(render_priv, event);
    if (!priv)
        return ass_event_has_hard_overrides(event->Text);

    if (priv->hard_overrides_text != event->Text) {
        priv->has_hard_overrides = ass_event_has_hard_overrides(event->Text);
        priv->hard_overrides_text = event->Text;
    }
    return priv->has_hard_overrides;
}

static int overlap(Rect *s1, Rect *s2)
{
    if (s1->y0 >= s2->y1 || s2->y0 >= s1->y1 ||
        s1->x0 >= s2->x1 || s2->x0 >= s1->x1)
        return 0;
    return 1;
}

static int cmp_rect_y0(const void *p1, const void *p2)
{
    return ((Rect *) p1)->y0 - ((Rect *) p2)->y0;
}

static void
shift_event(ASS_Renderer *render_priv, EventImages *ei, int shift)
{
    ASS_Image *cur = ei->imgs;
    while (cur) {
        cur->dst_y += shift;
        // clip top and bottom
        if (cur->dst_y < 0) {
            int clip = -cur->dst_y;
            cur->h -= clip;
            cur->bitmap += clip * cur->stride;
            cur->dst_y = 0;
        }
        if (cur->dst_y + cur->h >= render_priv->height) {
            int clip = cur->dst_y + cur->h - render_priv->height;
            cur->h -= clip;
        }
        if (cur->h <= 0) {
            cur->h = 0;
            cur->dst_y = 0;
        }
        cur = cur->next;
    }
    ei->top += shift;
}

// dir: 1 - move down
//      -1 - move up
static int fit_rect(Rect *s, Rect *fixed, int *cnt, int dir)
{
    int i;
    int shift = 0;

    if (dir == 1)               // move down
        for (i = 0; i < *cnt; ++i) {
            if (s->y1 + shift <= fixed[i].y0 || s->y0 + shift >= fixed[i].y1 ||
                s->x1 <= fixed[i].x0 || s->x0 >= fixed[i].x1)
                continue;
            shift = fixed[i].y1 - s->y0;
    } else                      // dir == -1, move up
        for (i = *cnt - 1; i >= 0; --i) {
            if (s->y1 + shift <= fixed[i].y0 || s->y0 + shift >= fixed[i].y1 ||
                s->x1 <= fixed[i].x0 || s->x0 >= fixed[i].x1)
                continue;
            shift = fixed[i].y0 - s->y1;
        }

    fixed[*cnt].y0 = s->y0 + shift;
    fixed[*cnt].y1 = s->y1 + shift;
    fixed[*cnt].x0 = s->x0;
    fixed[*cnt].x1 = s->x1;
    (*cnt)++;
    qsort(fixed, *cnt, sizeof(*fixed), cmp_rect_y0);

    return shift;
}

static void
fix_collisions(ASS_Renderer *render_priv, EventImages *imgs, int cnt)
{
    Rect *used = ass_realloc_array(NULL, cnt, sizeof(*used));
    int cnt_used = 0;
    int i, j;

    if (!used)
        return;

    // fill used[] with fixed events
    for (i = 0; i < cnt; ++i) {
        ASS_RenderPriv *priv;
        // VSFilter considers events colliding if their intersections area is non-zero,
        // zero-area events are therefore effectively fixed as well
        if (!imgs[i].detect_collisions || !imgs[i].height  || !imgs[i].width)
            continue;
        priv = get_render_priv(render_priv, imgs[i].event);
        if (priv && priv->height > 0) { // it's a fixed event
            Rect s;
            s.y0 = priv->top;
            s.y1 = priv->top + priv->height;
            s.x0 = priv->left;
            s.x1 = priv->left + priv->width;
            if (priv->height != imgs[i].height) {       // no, it's not
                ass_msg(render_priv->library, MSGL_WARN,
                        "Event height has changed");
                priv->top = 0;
                priv->height = 0;
                priv->left = 0;
                priv->width = 0;
            }
            for (j = 0; j < cnt_used; ++j)
                if (overlap(&s, used + j)) {    // no, it's not
                    priv->top = 0;
                    priv->height = 0;
                    priv->left = 0;
                    priv->width = 0;
                }
            if (priv->height > 0) {     // still a fixed event
                used[cnt_used].y0 = priv->top;
                used[cnt_used].y1 = priv->top + priv->height;
                used[cnt_used].x0 = priv->left;
                used[cnt_used].x1 = priv->left + priv->width;
                cnt_used++;
                shift_event(render_priv, imgs + i, priv->top - imgs[i].top);
            }
        }
    }
    qsort(used, cnt_used, sizeof(*used), cmp_rect_y0);

    // try to fit other events in free spaces
    for (i = 0; i < cnt; ++i) {
        ASS_RenderPriv *priv;
        if (!imgs[i].detect_collisions || !imgs[i].height  || !imgs[i].width)
            continue;
        priv = get_render_priv(render_priv, imgs[i].event);
        if (priv && priv->height == 0) {        // not a fixed event
            int shift;
            Rect s;
            s.y0 = imgs[i].top;
            s.y1 = imgs[i].top + imgs[i].height;
            s.x0 = imgs[i].left;
            s.x1 = imgs[i].left + imgs[i].width;
            shift = fit_rect(&s, used, &cnt_used, imgs[i].shift_direction);
            if (shift)
                shift_event(render_priv, imgs + i, shift);
            // make it fixed
            priv->top = imgs[i].top;
            priv->height = imgs[i].height;
            priv->left = imgs[i].left;
            priv->width = imgs[i].width;
        }

    }

    free(used);
}

/**
 * \brief compare two images
 * \param i1 first image
 * \param i2 second image
 * \return 0 if identical, 1 if different positions, 2 if different content
 */
static int ass_image_compare(ASS_Image *i1, ASS_Image *i2)
{
    if (i1->w != i2->w)
        return 2;
    if (i1->h != i2->h)
        return 2;
    if (i1->stride != i2->stride)
        return 2;
    if (i1->color != i2->color)
        return 2;
    if (i1->bitmap != i2->bitmap)
        return 2;
    if (i1->dst_x != i2->dst_x)
        return 1;
    if (i1->dst_y != i2->dst_y)
        return 1;
    return 0;
}

/**
 * \brief compare current and previous image list
 * \param priv library handle
 * \return 0 if identical, 1 if different positions, 2 if different content
 */
static int ass_detect_change(ASS_Renderer *priv)
{
    ASS_Image *img, *img2;
    int diff;

    img = priv->prev_images_root;
    img2 = priv->images_root;
    diff = 0;
    while (img && diff < 2) {
        ASS_Image *next, *next2;
        next = img->next;
        if (img2) {
            int d = ass_image_compare(img, img2);
            if (d > diff)
                diff = d;
            next2 = img2->next;
        } else {
            // previous list is shorter
            diff = 2;
            break;
        }
        img = next;
        img2 = next2;
    }

    // is the previous list longer?
    if (img2)
        diff = 2;

    return diff;
}

#if ENABLE_THREADS
static bool start_threads(ASS_Renderer *priv)
{
    // The calling thread renders from the same queue, so the pool only needs
    // N - 1 workers to honor ass_set_threads(N).
    unsigned workers = priv->settings.threads > 1 ?
        priv->settings.threads - 1 : 0;

    if (priv->n_threads == workers || priv->thread_start_failed)
        return true;

    join_threads(priv);

    priv->shutting_down = 0;

    if (!workers)
        return true;

    if (!(priv->threads = calloc(workers, sizeof(pthread_t)))) {
        ass_msg(priv->library, MSGL_ERR, "Allocation failure");
        return false;
    }

    for (priv->n_threads = 0; priv->n_threads < workers; priv->n_threads++) {
        if (pthread_create(&priv->threads[priv->n_threads], NULL, run_thread, priv) != 0) {
            pthread_mutex_lock(&priv->mutex);
            ass_msg(priv->library, MSGL_WARN, "Thread startup failure");
            priv->thread_start_failed = 1;
            pthread_mutex_unlock(&priv->mutex);
            return true;
        }
    }

    pthread_mutex_lock(&priv->mutex);
    while (priv->started_threads < priv->n_threads && !priv->thread_start_failed)
        pthread_cond_wait(&priv->main_cond, &priv->mutex);
    pthread_mutex_unlock(&priv->mutex);

    return true;
}
#endif

/**
 * \brief free a single image.
 * \param img image as returned by ass_render_frame().
 *        Only img will be freed, not img->next.
 * \return img->next
 * Should not be called outside of ass_frame_unref
 * once the image list's refcounting is set up.
 */
static ASS_Image *ass_free_image(ASS_Image *img) {
    ASS_Image *next = img->next;

    ASS_ImagePriv *priv = (ASS_ImagePriv *) img;
    ass_cache_dec_ref(priv->source);
    if (priv->shared)
        shared_buffer_unref(priv->shared);   // cloned owned-buffer image (buffer is NULL)
    else
        ass_aligned_free(priv->buffer);
    ImagePool *pool = priv->pool;
    if (pool) {
        // Recycle the shell instead of returning it to the heap.
        image_pool_lock(pool);
        priv->result.next = (ASS_Image *) pool->free_list;
        pool->free_list = priv;
        pool->live--;
        image_pool_unlock_or_destroy(pool);
    } else {
        free(priv);
    }

    return next;
}

/**
 * \brief render a frame
 * \param priv library handle
 * \param track track
 * \param now current video timestamp (ms)
 * \param detect_change a value describing how the new images differ from the previous ones will be written here:
 *        0 if identical, 1 if different positions, 2 if different content.
 *        Can be NULL, in that case no detection is performed.
 */
ASS_Image *ass_render_frame(ASS_Renderer *priv, ASS_Track *track,
                            long long now, int *detect_change)
{
    // init frame
    if (!ass_start_frame(priv, track, now)) {
        if (detect_change)
            *detect_change = 2;
        return NULL;
    }

    // render events separately
    int cnt = 0;
    bool readorder_sorted = true;
    int last_read_order = INT_MIN;
    int min_layer = INT_MAX;
    int max_layer = INT_MIN;
    for (int i = 0; i < track->n_events; i++) {
        ASS_Event *event = track->events + i;
        if ((event->Start <= now)
            && (now < (event->Start + event->Duration))) {
            if (cnt >= priv->eimg_size) {
                priv->eimg_size += 100;
                priv->eimg =
                    realloc(priv->eimg,
                            priv->eimg_size * sizeof(EventImages));
            }
            if (cnt > 0 && event->ReadOrder <= last_read_order)
                readorder_sorted = false;
            last_read_order = event->ReadOrder;
            if (event->Layer < min_layer)
                min_layer = event->Layer;
            if (event->Layer > max_layer)
                max_layer = event->Layer;
            priv->eimg[cnt++] = (EventImages){
                .event = event,
            };
        }
    }

#if ENABLE_THREADS
    bool parallel = cnt >= MIN_THREADED_EVENTS;

    // Do not allocate and initialize the worker pool until a frame can actually
    // use it. Once a pool exists, still run start_threads on light frames so a
    // changed ass_set_threads setting tears down/reconfigures it promptly.
    if ((parallel || priv->n_threads) && !start_threads(priv))
        return NULL;

    // Waking the worker pool is disproportionately expensive for tiny active
    // event sets; the serial path is byte-identical and much cheaper there.
    if (parallel && priv->n_threads > 0 && !priv->thread_start_failed) {
        pthread_mutex_lock(&priv->mutex);

        while (priv->active_threads)
            pthread_cond_wait(&priv->main_cond, &priv->mutex);

        atomic_store_explicit(&priv->processing_eimgs, cnt, memory_order_release);
        atomic_store_explicit(&priv->sent_eimgs, cnt, memory_order_release);
        atomic_store_explicit(&priv->next_eimg, 0, memory_order_release);

        // Wake ONE worker (not all of them). Workers cascade: each one that
        // actually grabs an event wakes one more, but only while work remains,
        // so the number of woken workers tracks the real parallelism. A frame
        // the main thread can handle alone (warm/light) thus wakes ~1 worker
        // instead of the whole pool -- no thundering herd -- while heavy frames
        // still ramp up to full parallelism (the wake overlaps the first render).
        pthread_cond_signal(&priv->pool_cond);

        // Release the lock so workers can start stealing immediately, and let
        // the main thread steal events from the same atomic queue instead of
        // blocking idle until they finish. This makes the threaded path never
        // slower than serial (the main thread does at least as much work as it
        // would serially) and adds one more renderer for large event sets.
        // Output is independent of which thread renders which event.
        pthread_mutex_unlock(&priv->mutex);

        bool main_done_all = false;
        uintptr_t got_eimg;
        while ((got_eimg = atomic_fetch_add_explicit(&priv->next_eimg, 1,
                        memory_order_relaxed)) <
                atomic_load_explicit(&priv->sent_eimgs, memory_order_relaxed)) {
            ass_render_event(&priv->state, priv->eimg + got_eimg);

            if (atomic_fetch_sub_explicit(&priv->processing_eimgs, 1,
                        memory_order_acq_rel) - 1 == 0) {
                main_done_all = true;  // main rendered the last event
                break;
            }
        }

        // Main exhausted the queue; wait for workers to finish any remaining
        // events. (Re-checks the predicate under the lock, so a worker signal
        // that races ahead of this lock is not lost.)
        if (!main_done_all) {
            pthread_mutex_lock(&priv->mutex);
            while (atomic_load_explicit(&priv->processing_eimgs, memory_order_acquire))
                pthread_cond_wait(&priv->main_cond, &priv->mutex);
            pthread_mutex_unlock(&priv->mutex);
        }
    } else
#endif
    {
        for (int i = 0; i < cnt; i++) {
            ass_render_event(&priv->state, priv->eimg + i);
        }
    }

    // sort by layer; cmp_event_layer is a total order (ReadOrder is unique),
    // so the active set is very often already in sorted order (events are
    // scanned in array order). Skip the qsort in that common case — same result.
    if (cnt > 0) {
        bool sorted = true;
        for (int i = 1; i < cnt; i++) {
            ASS_Event *a = priv->eimg[i - 1].event, *b = priv->eimg[i].event;
            if (a->Layer > b->Layer ||
                (a->Layer == b->Layer && a->ReadOrder >= b->ReadOrder)) {
                sorted = false;
                break;
            }
        }
        if (!sorted &&
                (!readorder_sorted ||
                 !stable_sort_events_by_layer(priv, priv->eimg, cnt,
                                              min_layer, max_layer)))
            qsort(priv->eimg, cnt, sizeof(EventImages), cmp_event_layer);
    }

    // call fix_collisions for each group of events with the same layer
    EventImages *last = priv->eimg;
    for (int i = 1; i < cnt; i++)
        if (last->event->Layer != priv->eimg[i].event->Layer) {
            fix_collisions(priv, last, priv->eimg + i - last);
            last = priv->eimg + i;
        }
    if (cnt > 0)
        fix_collisions(priv, last, priv->eimg + cnt - last);

    // concat lists, removing fully transparent bitmaps
    ASS_Image **tail = &priv->images_root;
    for (int i = 0; i < cnt; i++) {
        ASS_Image *cur = priv->eimg[i].imgs;
        while (cur) {
            if (_a(cur->color) == 0xFF) {
                cur = ass_free_image(cur);
                continue;
            }

            *tail = cur;
            tail = &cur->next;
            cur = cur->next;
        }
    }

    // If the last image was skipped in the above loop, *tail may not be NULL and needs to be set to NULL.
    *tail = NULL;

    ass_frame_ref(priv->images_root);

    if (detect_change)
        *detect_change = ass_detect_change(priv);

    // free the previous image list
    ass_frame_unref(priv->prev_images_root);
    priv->prev_images_root = NULL;

    if (track->parser_priv->prune_delay >= 0)
        ass_prune_events(track, now - track->parser_priv->prune_delay);

    return priv->images_root;
}

/**
 * \brief Add reference to a frame image list.
 * \param image_list image list returned by ass_render_frame()
 */
void ass_frame_ref(ASS_Image *img)
{
    if (!img)
        return;
    ((ASS_ImagePriv *) img)->ref_count++;
}

/**
 * \brief Release reference to a frame image list.
 * \param image_list image list returned by ass_render_frame()
 */
void ass_frame_unref(ASS_Image *img)
{
    if (!img || --((ASS_ImagePriv *) img)->ref_count)
        return;
    do {
        img = ass_free_image(img);
    } while (img);
}
