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

#ifndef LIBASS_FONT_H
#define LIBASS_FONT_H

#include <stdint.h>
#include <ft2build.h>
#include FT_FREETYPE_H

typedef struct ass_font ASS_Font;

#include "ass.h"
#include "ass_types.h"
#include "ass_fontselect.h"
#include "ass_cache.h"
#include "ass_outline.h"
#include "ass_arabic_charmap.h"
#include "ass_threading.h"

#define VERTICAL_LOWER_BOUND 0x02f1

#define ASS_FONT_MAX_FACES 10
#define DECO_UNDERLINE     1
#define DECO_STRIKETHROUGH 2
#define DECO_ROTATE        4

// Per-face direct-mapped cache for symbol -> FreeType glyph index lookups.
// Avoids re-querying FreeType's charmap (tt_cmap4_char_map_binary etc.) for
// the same codepoint on every frame. Must be a power of two.
#define ASS_GLYPH_INDEX_CACHE_SIZE 512

// The hit path is read LOCK-FREE: a single relaxed 64-bit atomic load returns
// a tear-free snapshot of the packed (symbol, index) pair, so the common warm-
// playback case no longer takes the font mutex. Safety:
//  - the (symbol -> index) mapping is fixed for a face's lifetime, so every
//    fully-stored tag is a correct answer (a hit is always right);
//  - the whole pair is one 64-bit atomic value, so no torn/half-updated state
//    is ever observed (a miss simply falls back to the locked path);
//  - the entry array is invalidated in place (never freed mid-life), so a
//    lock-free reader can never dereference freed memory.
// tag layout: high 32 bits = symbol, low 32 bits = index + 1 (0 == empty slot).
typedef struct {
    _Atomic uint64_t tag;
} GlyphIndexCacheEntry;

static inline uint64_t ass_glyph_index_pack(uint32_t symbol, uint32_t index)
{
    return ((uint64_t) symbol << 32) | (uint32_t) (index + 1);
}

// Enable the lock-free hit path only where 64-bit atomics are truly lock-free
// (all 64-bit targets: arm64, x86_64). On 32-bit targets the function still
// works, it just always takes the mutex (the tag is still accessed atomically,
// but under the lock, so no torn reads occur).
#if defined(ATOMIC_LLONG_LOCK_FREE) && ATOMIC_LLONG_LOCK_FREE == 2
#define ASS_GLYPH_INDEX_FAST_PATH 1
#else
#define ASS_GLYPH_INDEX_FAST_PATH 0
#endif

struct ass_font {
    ASS_FontDesc desc;
    ASS_Library *library;
    FT_Library ftlibrary;
    int faces_uid[ASS_FONT_MAX_FACES];
    FT_Face faces[ASS_FONT_MAX_FACES];
    struct hb_font_t *hb_fonts[ASS_FONT_MAX_FACES];
    GlyphIndexCacheEntry *_Atomic index_cache[ASS_FONT_MAX_FACES];
    _Atomic AtomicInt n_faces;

#if ENABLE_THREADS
    pthread_mutex_t mutex;
#endif
};

void ass_charmap_magic(ASS_Library *library, FT_Face face);
ASS_Font *ass_font_new(struct render_context *context, ASS_FontDesc *desc);
void ass_face_set_size(FT_Face face, double size);
int ass_face_get_weight(FT_Face face);
FT_Long ass_face_get_style_flags(FT_Face face);
bool ass_face_is_postscript(FT_Face face);
void ass_font_get_asc_desc(ASS_Font *font, int face_index,
                           int *asc, int *desc);
int ass_font_get_index(ASS_FontSelector *fontsel, ASS_Font *font,
                       uint32_t symbol, int *face_index, int *glyph_index);
uint32_t ass_font_index_magic(FT_Face face, uint32_t symbol);
uint32_t ass_font_get_char_index(ASS_Font *font, int face_index, uint32_t symbol);
bool ass_font_get_glyph(ASS_Font *font, int face_index, int index,
                        ASS_Hinting hinting);
void ass_font_clear(ASS_Font *font);

void ass_font_lock(ASS_Font *font);
void ass_font_unlock(ASS_Font *font);

bool ass_get_glyph_outline(ASS_Outline *outline, int32_t *advance,
                           FT_Face face, unsigned flags);

FT_Face ass_face_open(ASS_Library *lib, FT_Library ftlib, const char *path,
                      const char *postscript_name, int index);
FT_Face ass_face_stream(ASS_Library *lib, FT_Library ftlib, const char *name,
                        const ASS_FontStream *stream, int index);

#endif                          /* LIBASS_FONT_H */
