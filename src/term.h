#ifndef TERM_H
#define TERM_H

#include <stdbool.h>
#include <stddef.h>
#include "key.h"

enum {
    COLOR_DEFAULT = -1,
    COLOR_BLACK,
    COLOR_RED,
    COLOR_GREEN,
    COLOR_YELLOW,
    COLOR_BLUE,
    COLOR_MAGENTA,
    COLOR_CYAN,
    COLOR_GREY
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
};

typedef struct {
    short fg;
    short bg;
    unsigned short attr;
} TermColor;

void term_raw(void);
void term_cooked(void);

bool term_read_key(Key *key);
char *term_read_paste(size_t *size);
void term_discard_paste(void);

int term_get_size(int *w, int *h);

#endif
