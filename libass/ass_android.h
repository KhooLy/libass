#ifndef ASS_ANDROID_H
#define ASS_ANDROID_H

#include "ass_types.h"
#include "ass_fontselect.h"

#ifdef __ANDROID__

ASS_FontProvider *
ass_android_add_provider(ASS_Library *lib, ASS_FontSelector *selector,
                         const char *config, FT_Library ftlib);

#endif

#endif
