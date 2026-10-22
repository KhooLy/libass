#ifdef CREATE_STRUCT_DEFINITIONS
#undef CREATE_STRUCT_DEFINITIONS
#define START(funcname, structname) \
    typedef struct structname {
#define GENERIC(type, member) \
        type member;
#define STRING(member) \
        ASS_StringView member;
#define VECTOR(member) \
        ASS_Vector member;
#define END(typedefnamename) \
    } typedefnamename;

#elif defined(CREATE_COMPARISON_FUNCTIONS)
#undef CREATE_COMPARISON_FUNCTIONS
#define START(funcname, structname) \
    static bool funcname##_compare(void *key1, void *key2) \
    { \
        struct structname *a = key1; \
        struct structname *b = key2; \
        return // conditions follow
#define GENERIC(type, member) \
            a->member == b->member &&
#define STRING(member) \
            ass_string_equal(a->member, b->member) &&
#define VECTOR(member) \
            a->member.x == b->member.x && a->member.y == b->member.y &&
#define END(typedefname) \
            true; \
    }

#elif defined(CREATE_HASH_FUNCTIONS)
#undef CREATE_HASH_FUNCTIONS
// Coalesce consecutive fixed-width (GENERIC/VECTOR) members into a single
// ass_hash_buf() call over their contiguous byte range. wyhash() pays a fixed
// setup cost plus ~2 128-bit multiplies per call, so hashing e.g. all 40 bytes
// of BitmapHashKey at once is far cheaper than the previous 5-9 per-member
// mixes. A STRING member (variable length, indirect storage) flushes the
// pending run; END flushes any trailing run.
//
// SAFETY: the coalesced range spans exactly [first_member, last_member_end);
// it never extends to sizeof(struct), so trailing padding is never hashed.
// All key structs are verified to have NO internal padding between adjacent
// fixed-width members (every adjacent pair is byte-contiguous), so no
// uninitialized padding bytes are ever included -> equal keys still hash equal.
#define START(funcname, structname) \
    static ass_hashcode funcname##_hash(void *buf, ass_hashcode hval) \
    { \
        struct structname *p = buf; \
        const char *run_beg = NULL, *run_end = NULL;
#define ASS_HASH_FLUSH_RUN \
        if (run_beg) { \
            hval = ass_hash_buf(run_beg, run_end - run_beg, hval); \
            run_beg = run_end = NULL; \
        }
#define ASS_HASH_ADD_RANGE(beg, end) \
        if (!run_beg) run_beg = (const char *) (beg); \
        run_end = (const char *) (end);
#define GENERIC(type, member) \
        ASS_HASH_ADD_RANGE(&p->member, (&p->member) + 1)
#define STRING(member) \
        ASS_HASH_FLUSH_RUN \
        hval = ass_hash_buf(p->member.str, p->member.len, hval);
#define VECTOR(member) \
        ASS_HASH_ADD_RANGE(&p->member, (&p->member) + 1)
#define END(typedefname) \
        ASS_HASH_FLUSH_RUN \
        return hval; \
    }

#else
#error missing defines
#endif



START(font, ass_font_desc )
    STRING(family)
    GENERIC(unsigned, bold)
    GENERIC(unsigned, italic)
    GENERIC(int, vertical)  // @font vertical layout
END(ASS_FontDesc)

// describes an outline bitmap
// outline is refed when inserted and unrefed when dropped
START(bitmap, bitmap_hash_key)
    GENERIC(OutlineHashValue *, outline)
    // quantized transform matrix
    VECTOR(offset)
    VECTOR(matrix_x)
    VECTOR(matrix_y)
    VECTOR(matrix_z)
END(BitmapHashKey)

// font is refed when inserted and unrefed when dropped
START(face_size_metrics, face_size_metrics_hash_key)
    GENERIC(ASS_Font *, font)
    GENERIC(double, size)
    GENERIC(int, face_index)
END(FaceSizeMetricsHashKey)

// font is refed when inserted and unrefed when dropped
START(glyph_metrics, glyph_metrics_hash_key)
    GENERIC(ASS_Font *, font)
    GENERIC(double, size)
    GENERIC(int, face_index)
    GENERIC(int, glyph_index)
END(GlyphMetricsHashKey)

// describes an outline glyph
// font is refed when inserted and unrefed when dropped
START(glyph, glyph_hash_key)
    GENERIC(ASS_Font *, font)
    GENERIC(double, size) // font size
    GENERIC(int, face_index)
    GENERIC(int, glyph_index)
    GENERIC(int, bold)
    GENERIC(int, italic)
    GENERIC(unsigned, flags) // glyph decoration flags
END(GlyphHashKey)

// describes an outline drawing
// on call to ass_cache_get(), text is a non-owning view;
// its content is duplicated when inserted; the copy is freed when dropped
START(drawing, drawing_hash_key)
    STRING(text)
END(DrawingHashKey)

// describes an offset outline
// outline is refed when inserted and unrefed when dropped
START(border, border_hash_key)
    GENERIC(OutlineHashValue *, outline)
    // outline is scaled by 2^scale_ord_x|y before stroking
    // to keep stoker error in allowable range
    GENERIC(int, scale_ord_x)
    GENERIC(int, scale_ord_y)
    VECTOR(border)  // border size in STROKER_ACCURACY units
END(BorderHashKey)

// describes post-combining effects
START(filter, filter_desc)
    GENERIC(int, flags)
    GENERIC(int, be)
    GENERIC(int, blur_x)
    GENERIC(int, blur_y)
    VECTOR(shadow)
END(FilterDesc)

// describes glyph bitmap reference
// bm and bm_o are refed when inserted and unrefed when dropped
START(bitmap_ref, bitmap_ref_key)
    GENERIC(Bitmap *, bm)
    GENERIC(Bitmap *, bm_o)
    VECTOR(pos)
    VECTOR(pos_o)
END(BitmapRef)

#undef START
#undef GENERIC
#undef STRING
#undef VECTOR
#undef END
#undef ASS_HASH_FLUSH_RUN
#undef ASS_HASH_ADD_RANGE
