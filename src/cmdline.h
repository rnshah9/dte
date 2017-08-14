#ifndef CMDLINE_H
#define CMDLINE_H

#include "ptr-array.h"
#include "strbuf.h"
#include "term.h"

typedef struct {
    StringBuffer buf;
    long pos;
    int search_pos;
    char *search_text;
} CommandLine;

enum {
    CMDLINE_UNKNOWN_KEY,
    CMDLINE_KEY_HANDLED,
    CMDLINE_CANCEL,
};

#define CMDLINE(name) CommandLine name = {STRBUF_INIT, 0, -1, NULL}

void cmdline_clear(CommandLine *c);
void cmdline_set_text(CommandLine *c, const char *text);
int cmdline_handle_key(CommandLine *c, PointerArray *history, int key);

#endif
