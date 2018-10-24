#include <stdio.h>
#include <termios.h>
#undef CTRL // macro from sys/ttydefaults.h clashes with the one in key.h
#include "ecma48.h"
#include "output.h"
#include "terminfo.h"
#include "xterm.h"
#include "../util/macros.h"
#include "../util/xsnprintf.h"

static struct termios termios_save;

void term_raw(void)
{
    // See termios(3)
    struct termios termios;

    tcgetattr(0, &termios);
    termios_save = termios;

    // Disable buffering
    // Disable echo
    // Disable generation of signals (free some control keys)
    termios.c_lflag &= ~(ICANON | ECHO | ISIG);

    // Disable CR to NL conversion (differentiate ^J from enter)
    // Disable flow control (free ^Q and ^S)
    termios.c_iflag &= ~(ICRNL | IXON | IXOFF);

    // Read at least 1 char on each read()
    termios.c_cc[VMIN] = 1;

    // Read blocks until there are MIN(VMIN, requested) bytes available
    termios.c_cc[VTIME] = 0;

    tcsetattr(0, 0, &termios);
}

void term_cooked(void)
{
    tcsetattr(0, 0, &termios_save);
}

void ecma48_clear_to_eol(void)
{
    buf_add_bytes("\033[K", 3);
}

void ecma48_move_cursor(int x, int y)
{
    if (x < 0 || x >= 999 || y < 0 || y >= 999) {
        return;
    }
    static char buf[16];
    const int len = xsnprintf (
        buf,
        11, // == strlen("\033[998;998H\0")
        "\033[%u;%uH",
        // x and y are zero-based
        ((unsigned int)y) + 1,
        ((unsigned int)x) + 1
    );
    buf_add_bytes(buf, (size_t)len);
}

void ecma48_set_color(const TermColor *const color)
{
    if (same_color(color, &obuf.color)) {
        return;
    }

    // Max 14 bytes ("E[0;1;7;30;40m")
    char buf[32] = "\033[0";
    size_t i = 3;
    TermColor c = *color;

    if (c.attr & ATTR_BOLD) {
        buf[i++] = ';';
        buf[i++] = '1';
    }
    if (c.attr & ATTR_REVERSE) {
        buf[i++] = ';';
        buf[i++] = '7';
    }

    if (c.fg >= 0 && c.fg < 8) {
        buf[i++] = ';';
        buf[i++] = '3';
        buf[i++] = '0' + (char) c.fg;
    }

    if (c.bg >= 0 && c.bg < 8) {
        buf[i++] = ';';
        buf[i++] = '4';
        buf[i++] = '0' + (char) c.bg;
    }

    buf[i++] = 'm';
    buf_add_bytes(buf, i);
    obuf.color = *color;
}

void ecma48_repeat_byte(char ch, size_t count)
{
    if (ch < ' ' || ch > '~' || count < 6 || count > 30000) {
        buf_repeat_byte(ch, count);
        return;
    }
    char buf[16];
    int n = xsnprintf(buf, sizeof buf, "%c\033[%zub", ch, count - 1);
    buf_add_bytes(buf, n);
}

static void no_op(void) {}
static void no_op_s(const char* UNUSED_ARG(s)) {}

TerminalInfo terminal = {
    .color_type = TERM_8_COLOR,
    .width = 80,
    .height = 24,
    .raw = &term_raw,
    .cooked = &term_cooked,
    .parse_key_sequence = &xterm_parse_key,
    .clear_to_eol = &ecma48_clear_to_eol,
    .set_color = &ecma48_set_color,
    .move_cursor = &ecma48_move_cursor,
    .repeat_byte = &buf_repeat_byte,
    .save_title = &no_op,
    .restore_title = &no_op,
    .set_title = &no_op_s,
    .control_codes = {
        .init = STRING_VIEW_INIT,
        .deinit = STRING_VIEW_INIT,
        .reset_colors = STRING_VIEW("\033[39;49m"),
        .reset_attrs = STRING_VIEW("\033[0m"),
        .keypad_off = STRING_VIEW_INIT,
        .keypad_on = STRING_VIEW_INIT,
        .cup_mode_off = STRING_VIEW_INIT,
        .cup_mode_on = STRING_VIEW_INIT,
        .show_cursor = STRING_VIEW_INIT,
        .hide_cursor = STRING_VIEW_INIT,
    }
};
