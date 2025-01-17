#ifndef TERMINAL_COLOR_H
#define TERMINAL_COLOR_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include "util/macros.h"
#include "util/ptr-array.h"

#define COLOR_RGB(x) (COLOR_FLAG_RGB | (x))

typedef enum {
    TERM_0_COLOR,
    TERM_8_COLOR,
    TERM_16_COLOR,
    TERM_256_COLOR,
    TERM_TRUE_COLOR
} TermColorCapabilityType;

enum {
    COLOR_INVALID = -3,
    COLOR_KEEP = -2,
    COLOR_DEFAULT = -1,
    COLOR_BLACK = 0,
    COLOR_RED = 1,
    COLOR_GREEN = 2,
    COLOR_YELLOW = 3,
    COLOR_BLUE = 4,
    COLOR_MAGENTA = 5,
    COLOR_CYAN = 6,
    COLOR_GRAY = 7,
    COLOR_DARKGRAY = 8,
    COLOR_LIGHTRED = 9,
    COLOR_LIGHTGREEN = 10,
    COLOR_LIGHTYELLOW = 11,
    COLOR_LIGHTBLUE = 12,
    COLOR_LIGHTMAGENTA = 13,
    COLOR_LIGHTCYAN = 14,
    COLOR_WHITE = 15,

    // This bit flag is used to allow 24-bit RGB colors to be differentiated
    // from basic colors (e.g. #000004 vs. COLOR_BLUE)
    COLOR_FLAG_RGB = INT32_C(1) << 24
};

enum {
    ATTR_KEEP = 0x01,
    ATTR_UNDERLINE = 0x02,
    ATTR_REVERSE = 0x04,
    ATTR_BLINK = 0x08,
    ATTR_DIM = 0x10,
    ATTR_BOLD = 0x20,
    ATTR_INVIS = 0x40,
    ATTR_ITALIC = 0x80,
    ATTR_STRIKETHROUGH = 0x100,
};

typedef struct {
    int32_t fg;
    int32_t bg;
    unsigned int attr;
} TermColor;

static inline void color_split_rgb(int32_t c, uint8_t *r, uint8_t *g, uint8_t *b)
{
    *r = (c >> 16) & 0xff;
    *g = (c >> 8) & 0xff;
    *b = c & 0xff;
}

static inline bool same_color(const TermColor *a, const TermColor *b)
{
    return a->fg == b->fg && a->bg == b->bg && a->attr == b->attr;
}

int32_t parse_rgb(const char *str, size_t len);
ssize_t parse_term_color(TermColor *color, char **strs, size_t nstrs) NONNULL_ARGS WARN_UNUSED_RESULT;
int32_t color_to_nearest(int32_t color, TermColorCapabilityType type, bool optimize);
const char *term_color_to_string(const TermColor *color) NONNULL_ARGS_AND_RETURN;
void collect_colors_and_attributes(PointerArray *a, const char *prefix) NONNULL_ARGS;

#endif
