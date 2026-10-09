#include "config.h"
#include "ass_compat.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ass_android.h"
#include "ass_utils.h"

#define FALLBACK_PREFIX "android-fallback:"

typedef struct AFontMatcher AFontMatcher;
typedef struct AFont AFont;

struct android_api {
    AFontMatcher *(*matcher_create)(void);
    void (*matcher_destroy)(AFontMatcher *);
    void (*matcher_set_style)(AFontMatcher *, uint16_t, bool);
    AFont *(*matcher_match)(const AFontMatcher *, const char *,
                            const uint16_t *, uint32_t, uint32_t *);
    const char *(*font_path)(const AFont *);
    size_t (*font_index)(const AFont *);
    uint16_t (*font_weight)(const AFont *);
    bool (*font_italic)(const AFont *);
    void (*font_close)(AFont *);
};

typedef struct {
    struct android_api api;
    FT_Library ftlib;
    char **added;
    size_t num_added;
} ProviderPrivate;

typedef struct {
    FT_Library ftlib;
    char *path;
    int index;
    FT_Face face;
    bool failed;
} FontPrivate;

static bool load_api(struct android_api *api)
{
    api->matcher_create = dlsym(RTLD_DEFAULT, "AFontMatcher_create");
    api->matcher_destroy = dlsym(RTLD_DEFAULT, "AFontMatcher_destroy");
    api->matcher_set_style = dlsym(RTLD_DEFAULT, "AFontMatcher_setStyle");
    api->matcher_match = dlsym(RTLD_DEFAULT, "AFontMatcher_match");
    api->font_path = dlsym(RTLD_DEFAULT, "AFont_getFontFilePath");
    api->font_index = dlsym(RTLD_DEFAULT, "AFont_getCollectionIndex");
    api->font_weight = dlsym(RTLD_DEFAULT, "AFont_getWeight");
    api->font_italic = dlsym(RTLD_DEFAULT, "AFont_isItalic");
    api->font_close = dlsym(RTLD_DEFAULT, "AFont_close");
    return api->matcher_create && api->matcher_destroy &&
           api->matcher_set_style && api->matcher_match && api->font_path &&
           api->font_index && api->font_weight && api->font_italic &&
           api->font_close;
}

static AFont *match(ProviderPrivate *p, const char *family, uint32_t code,
                    int weight, bool italic)
{
    uint16_t text[2];
    uint32_t len = 1;
    if (code >= 0x10000) {
        code -= 0x10000;
        text[0] = 0xD800 + (code >> 10);
        text[1] = 0xDC00 + (code & 0x3FF);
        len = 2;
    } else {
        text[0] = code ? code : 'a';
    }
    AFontMatcher *m = p->api.matcher_create();
    if (!m)
        return NULL;
    p->api.matcher_set_style(m, weight, italic);
    AFont *font = p->api.matcher_match(m, family, text, len, NULL);
    p->api.matcher_destroy(m);
    return font;
}

static bool check_glyph(void *data, uint32_t code)
{
    FontPrivate *f = data;
    if (!code)
        return true;
    if (!f->face && !f->failed)
        f->failed = FT_New_Face(f->ftlib, f->path, f->index, &f->face) != 0;
    return f->face && FT_Get_Char_Index(f->face, code);
}

static void destroy_font(void *data)
{
    FontPrivate *f = data;
    if (f->face)
        FT_Done_Face(f->face);
    free(f->path);
    free(f);
}

static void destroy_provider(void *data)
{
    ProviderPrivate *p = data;
    for (size_t n = 0; n < p->num_added; n++)
        free(p->added[n]);
    free(p->added);
    free(p);
}

static bool mark_added(ProviderPrivate *p, const char *key)
{
    for (size_t n = 0; n < p->num_added; n++) {
        if (!strcmp(p->added[n], key))
            return false;
    }
    char **added = realloc(p->added, (p->num_added + 1) * sizeof(*added));
    if (!added)
        return false;
    p->added = added;
    p->added[p->num_added] = strdup(key);
    if (!p->added[p->num_added])
        return false;
    p->num_added++;
    return true;
}

static void add_font(ProviderPrivate *p, ASS_FontProvider *provider,
                     AFont *font, char *name)
{
    const char *path = p->api.font_path(font);
    int index = p->api.font_index(font);
    char key[1024];
    snprintf(key, sizeof(key), "%s\n%s\n%d", name, path, index);
    if (!path || !mark_added(p, key))
        return;

    FontPrivate *f = calloc(1, sizeof(*f));
    if (!f)
        return;
    f->ftlib = p->ftlib;
    f->path = strdup(path);
    f->index = index;
    if (!f->path) {
        free(f);
        return;
    }

    ASS_FontProviderMetaData meta = {
        .families = &name,
        .n_family = 1,
        .fullnames = &name,
        .n_fullname = 1,
        .extended_family = name,
        .weight = p->api.font_weight(font),
        .style_flags = p->api.font_italic(font) ? FT_STYLE_FLAG_ITALIC : 0,
    };
    if (!ass_font_provider_add_font(provider, &meta, f->path, index, f))
        destroy_font(f);
}

static void match_fonts(void *priv, ASS_Library *lib,
                        ASS_FontProvider *provider, char *name)
{
    ProviderPrivate *p = priv;
    uint32_t code = 0;
    const char *family = name;
    if (!strncmp(name, FALLBACK_PREFIX, strlen(FALLBACK_PREFIX))) {
        char *end;
        code = strtoul(name + strlen(FALLBACK_PREFIX), &end, 16);
        family = *end == ':' ? end + 1 : "sans-serif";
    }
    static const int styles[][2] = {{400, 0}, {700, 0}, {400, 1}, {700, 1}};
    for (int n = 0; n < 4; n++) {
        AFont *font = match(p, family, code, styles[n][0], styles[n][1]);
        if (!font)
            continue;
        add_font(p, provider, font, name);
        p->api.font_close(font);
    }
}

static char *get_fallback(void *priv, ASS_Library *lib, const char *family,
                          uint32_t code)
{
    ProviderPrivate *p = priv;
    AFont *font = match(p, family, code, 400, false);
    if (!font)
        return NULL;
    p->api.font_close(font);
    size_t size = strlen(FALLBACK_PREFIX) + strlen(family) + 16;
    char *res = malloc(size);
    if (res)
        snprintf(res, size, FALLBACK_PREFIX "%x:%s", (unsigned)code, family);
    return res;
}

static ASS_FontProviderFuncs android_funcs = {
    .check_glyph = check_glyph,
    .destroy_font = destroy_font,
    .destroy_provider = destroy_provider,
    .match_fonts = match_fonts,
    .get_fallback = get_fallback,
};

ASS_FontProvider *
ass_android_add_provider(ASS_Library *lib, ASS_FontSelector *selector,
                         const char *config, FT_Library ftlib)
{
    ProviderPrivate *p = calloc(1, sizeof(*p));
    if (!p)
        return NULL;
    if (!load_api(&p->api)) {
        free(p);
        return NULL;
    }
    p->ftlib = ftlib;
    ASS_FontProvider *provider =
        ass_font_provider_new(selector, &android_funcs, p);
    if (!provider)
        free(p);
    return provider;
}
