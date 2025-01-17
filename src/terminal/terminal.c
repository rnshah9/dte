#include <stdint.h>
#include <string.h>
#include "terminal.h"
#include "linux.h"
#include "output.h"
#include "rxvt.h"
#include "util/array.h"
#include "util/bsearch.h"
#include "util/debug.h"
#include "util/log.h"
#include "util/str-util.h"
#include "xterm.h"

typedef struct {
    const char name[12];
    unsigned int name_len : 4;
    unsigned int color_type : 3; // TermColorCapabilityType
    unsigned int ncv_attrs : 5; // TermColor attributes (see "ncv" in terminfo(5))
    unsigned int features : 20; // TermFeatureFlags
} TermEntry;

enum {
    // Short aliases for TermFeatureFlags:
    BCE = TFLAG_BACK_COLOR_ERASE,
    REP = TFLAG_ECMA48_REPEAT,
    TITLE = TFLAG_SET_WINDOW_TITLE,
    RXVT = TFLAG_RXVT,
    LINUX = TFLAG_LINUX,
    OSC52 = TFLAG_OSC52_COPY,
    METAESC = TFLAG_META_ESC,
    KITTYKBD = TFLAG_KITTY_KEYBOARD,
    ITERM2 = TFLAG_ITERM2,
    CSYNC = TFLAG_SYNC_CSI,
    DSYNC = TFLAG_SYNC_DCS,
};

enum {
    // Short aliases for TermColor attributes:
    UL = ATTR_UNDERLINE,
    REV = ATTR_REVERSE,
    DIM = ATTR_DIM,
};

static const TermEntry terms[] = {
    {"Eterm", 5, TERM_8_COLOR, 0, BCE},
    {"alacritty", 9, TERM_TRUE_COLOR, 0, BCE | REP | OSC52 | DSYNC},
    {"ansi", 4, TERM_8_COLOR, UL, 0},
    {"ansiterm", 8, TERM_0_COLOR, 0, 0},
    {"aterm", 5, TERM_8_COLOR, 0, BCE},
    {"contour", 7, TERM_TRUE_COLOR, 0, BCE | REP | TITLE | OSC52 | CSYNC},
    {"cx", 2, TERM_8_COLOR, 0, 0},
    {"cx100", 5, TERM_8_COLOR, 0, 0},
    {"cygwin", 6, TERM_8_COLOR, 0, 0},
    {"cygwinB19", 9, TERM_8_COLOR, UL, 0},
    {"cygwinDBG", 9, TERM_8_COLOR, UL, 0},
    {"decansi", 7, TERM_8_COLOR, 0, 0},
    {"domterm", 7, TERM_8_COLOR, 0, BCE},
    {"dtterm", 6, TERM_8_COLOR, 0, 0},
    {"dvtm", 4, TERM_8_COLOR, 0, 0},
    {"fbterm", 6, TERM_256_COLOR, DIM | UL, BCE},
    {"foot", 4, TERM_TRUE_COLOR, 0, BCE | REP | TITLE | OSC52 | KITTYKBD | CSYNC},
    {"hurd", 4, TERM_8_COLOR, DIM | UL, BCE},
    {"iTerm.app", 9, TERM_256_COLOR, 0, BCE},
    {"iTerm2.app", 10, TERM_256_COLOR, 0, BCE | TITLE | OSC52 | ITERM2 | CSYNC},
    {"iterm", 5, TERM_256_COLOR, 0, BCE},
    {"iterm2", 6, TERM_256_COLOR, 0, BCE | TITLE | OSC52 | ITERM2 | CSYNC},
    {"jfbterm", 7, TERM_8_COLOR, DIM | UL, BCE},
    {"kitty", 5, TERM_TRUE_COLOR, 0, TITLE | OSC52 | KITTYKBD | CSYNC},
    {"kon", 3, TERM_8_COLOR, DIM | UL, BCE},
    {"kon2", 4, TERM_8_COLOR, DIM | UL, BCE},
    {"konsole", 7, TERM_8_COLOR, 0, BCE},
    {"kterm", 5, TERM_8_COLOR, 0, 0},
    {"linux", 5, TERM_8_COLOR, DIM | UL, LINUX | BCE},
    {"mgt", 3, TERM_8_COLOR, 0, BCE},
    {"mintty", 6, TERM_8_COLOR, 0, BCE | REP | TITLE | OSC52 | CSYNC},
    {"mlterm", 6, TERM_8_COLOR, 0, TITLE},
    {"mlterm2", 7, TERM_8_COLOR, 0, TITLE},
    {"mlterm3", 7, TERM_8_COLOR, 0, TITLE},
    {"mrxvt", 5, TERM_8_COLOR, 0, RXVT | BCE | TITLE | OSC52},
    {"pcansi", 6, TERM_8_COLOR, UL, 0},
    {"putty", 5, TERM_8_COLOR, DIM | REV | UL, BCE},
    {"rxvt", 4, TERM_8_COLOR, 0, RXVT | BCE | TITLE | OSC52},
    {"screen", 6, TERM_8_COLOR, 0, TITLE | OSC52},
    {"st", 2, TERM_8_COLOR, 0, BCE | OSC52},
    {"stterm", 6, TERM_8_COLOR, 0, BCE | OSC52},
    {"teken", 5, TERM_8_COLOR, DIM | REV, BCE},
    {"terminator", 10, TERM_256_COLOR, 0, BCE | TITLE},
    {"termite", 7, TERM_8_COLOR, 0, TITLE},
    {"tmux", 4, TERM_8_COLOR, 0, TITLE | OSC52},
    {"wezterm", 7, TERM_TRUE_COLOR, 0, BCE | REP | TITLE | OSC52 | CSYNC},
    {"xfce", 4, TERM_8_COLOR, 0, BCE | TITLE},
    {"xterm", 5, TERM_8_COLOR, 0, BCE | TITLE | OSC52 | METAESC},
    {"xterm.js", 8, TERM_8_COLOR, 0, BCE},
};

static const struct {
    const char suffix[10];
    uint8_t suffix_len;
    uint8_t color_type; // TermColorCapabilityType
} color_suffixes[] = {
    {"direct", 6, TERM_TRUE_COLOR},
    {"256color", 8, TERM_256_COLOR},
    {"16color", 7, TERM_16_COLOR},
    {"mono", 4, TERM_0_COLOR},
    {"m", 1, TERM_0_COLOR},
};

static int term_name_compare(const void *key, const void *elem)
{
    const StringView *prefix = key;
    const TermEntry *entry = elem;
    size_t cmplen = MIN(prefix->length, entry->name_len);
    int r = memcmp(prefix->data, entry->name, cmplen);
    return r ? r : (int)prefix->length - entry->name_len;
}

UNITTEST {
    CHECK_BSEARCH_ARRAY(terms, name, strcmp);
    CHECK_STRUCT_ARRAY(color_suffixes, suffix);
    StringView k = STRING_VIEW("xtermz");
    BUG_ON(BSEARCH(&k, terms, term_name_compare));
    k.length--;
    BUG_ON(!BSEARCH(&k, terms, term_name_compare));
    k.length--;
    BUG_ON(BSEARCH(&k, terms, term_name_compare));
}

void term_init(Terminal *term, const char *name, const char *colorterm)
{
    BUG_ON(name[0] == '\0');

    // Initialize defaults (without touching obuf or ibuf)
    term->color_type = TERM_8_COLOR;
    term->width = 80;
    term->height = 24;
    term->features = 0;
    term->ncv_attributes = 0;
    term->parse_input = xterm_parse_key;

    // Strip phony "xterm-" prefix used by certain terminals
    const char *real_name = name;
    if (str_has_prefix(name, "xterm-")) {
        const char *str = name + STRLEN("xterm-");
        if (str_has_prefix(str, "kitty") || str_has_prefix(str, "termite")) {
            real_name = str;
        }
    }

    // Extract the "root name" from $TERM, as defined by terminfo(5).
    // This is the initial part of the string up to the first hyphen.
    size_t pos = 0;
    size_t rnlen = strlen(real_name);
    StringView root_name = get_delim(real_name, &pos, rnlen, '-');

    // Look up the root name in the list of known terminals
    const TermEntry *entry = BSEARCH(&root_name, terms, term_name_compare);
    if (entry) {
        TermFeatureFlags features = entry->features;
        term->features = features;
        term->color_type = entry->color_type;
        term->ncv_attributes = entry->ncv_attrs;
        if (features & RXVT) {
            term->parse_input = rxvt_parse_key;
        } else if (features & LINUX) {
            term->parse_input = linux_parse_key;
        }
        const int n = (int)root_name.length;
        LOG_INFO("using built-in terminal support for '%.*s'", n, root_name.data);
    }

    const char *ct = colorterm;
    if (ct && (streq(ct, "truecolor") || streq(ct, "24bit"))) {
        term->color_type = TERM_TRUE_COLOR;
        LOG_INFO("24-bit color support detected (COLORTERM=%s)", ct);
    }

    if (term->color_type == TERM_TRUE_COLOR) {
        return;
    }

    while (pos < rnlen) {
        const StringView str = get_delim(real_name, &pos, rnlen, '-');
        for (size_t i = 0; i < ARRAYLEN(color_suffixes); i++) {
            const char *suffix = color_suffixes[i].suffix;
            size_t len = color_suffixes[i].suffix_len;
            if (strview_equal_strn(&str, suffix, len)) {
                term->color_type = color_suffixes[i].color_type;
                LOG_INFO("color type detected from $TERM suffix '-%s'", suffix);
                return;
            }
        }
    }
}

void term_enable_private_modes(Terminal *term)
{
    TermOutputBuffer *obuf = &term->obuf;
    TermFeatureFlags features = term->features;
    if (features & METAESC) {
        term_add_literal(obuf, "\033[?1036;1039s\033[?1036;1039h");
    }
    if (features & KITTYKBD) {
        term_add_literal(obuf, "\033[>5u");
    } else if (features & ITERM2) {
        term_add_literal(obuf, "\033[>1u");
    } else {
        // Try to use "modifyOtherKeys" mode
        term_add_literal(obuf, "\033[>4;1m");
    }

    // Try to enable bracketed paste mode. This is done unconditionally,
    // since it should be ignored by terminals that don't recognize it
    // and we really want to enable it for terminals that support it but
    // are spoofing $TERM for whatever reason.

    // TODO: fix term_read_bracketed_paste() to handle end delimiters
    // that get split between 2 reads before re-enabling this
    //term_add_literal(obuf, "\033[?2004s\033[?2004h");
}

void term_restore_private_modes(Terminal *term)
{
    TermOutputBuffer *obuf = &term->obuf;
    TermFeatureFlags features = term->features;
    if (features & METAESC) {
        term_add_literal(obuf, "\033[?1036;1039r");
    }
    if (features & (KITTYKBD | ITERM2)) {
        term_add_literal(obuf, "\033[<u");
    } else {
        term_add_literal(obuf, "\033[>4m");
    }
    //term_add_literal(obuf, "\033[?2004l\033[?2004r");
}

void term_restore_cursor_style(Terminal *term)
{
    // TODO: query the cursor style at startup and restore that
    // value instead of using CURSOR_DEFAULT (which basically
    // amounts to using the so-called "DECSCUSR 0 hack"
    static const TermCursorStyle reset = {
        .type = CURSOR_DEFAULT,
        .color = COLOR_DEFAULT,
    };

    term_set_cursor_style(term, reset);
}
