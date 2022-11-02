#ifndef EDITOR_H
#define EDITOR_H

#include <regex.h>
#include <stdbool.h>
#include <stddef.h>
#include "bind.h"
#include "buffer.h"
#include "cmdline.h"
#include "command/macro.h"
#include "copy.h"
#include "encoding.h"
#include "file-history.h"
#include "frame.h"
#include "history.h"
#include "msg.h"
#include "options.h"
#include "regexp.h"
#include "search.h"
#include "syntax/color.h"
#include "tag.h"
#include "terminal/cursor.h"
#include "terminal/terminal.h"
#include "util/macros.h"
#include "util/ptr-array.h"
#include "util/string-view.h"
#include "view.h"

typedef enum {
    EDITOR_INITIALIZING,
    EDITOR_RUNNING,
    EDITOR_EXITING,
} EditorStatus;

typedef enum {
    INPUT_NORMAL,
    INPUT_COMMAND,
    INPUT_SEARCH,
} InputMode;

typedef struct EditorState {
    EditorStatus status;
    InputMode input_mode;
    CommandLine cmdline;
    SearchState search;
    GlobalOptions options;
    Terminal terminal;
    StringView home_dir;
    const char *user_config_dir;
    bool child_controls_terminal;
    bool everything_changed;
    bool cursor_style_changed;
    bool session_leader;
    int exit_code;
    size_t cmdline_x;
    KeyBindingGroup bindings[3];
    Clipboard clipboard;
    TagFile tagfile;
    HashMap compilers;
    HashMap syntaxes;
    ColorScheme colors;
    CommandMacroState macro;
    TermCursorStyle cursor_styles[NR_CURSOR_MODES];
    Frame *root_frame;
    struct Window *window;
    View *view;
    Buffer *buffer;
    PointerArray buffers;
    PointerArray filetypes;
    PointerArray file_options;
    PointerArray bookmarks;
    MessageArray messages;
    FileHistory file_history;
    History search_history;
    History command_history;
    RegexpWordBoundaryTokens regexp_word_tokens;
    const char *version;
} EditorState;

extern EditorState editor;

static inline void mark_everything_changed(EditorState *e)
{
    e->everything_changed = true;
}

static inline void set_input_mode(EditorState *e, InputMode mode)
{
    e->cursor_style_changed = true;
    e->input_mode = mode;
}

EditorState *init_editor_state(void);
int free_editor_state(EditorState *e);
char status_prompt(EditorState *e, const char *question, const char *choices) NONNULL_ARGS;
char dialog_prompt(EditorState *e, const char *question, const char *choices) NONNULL_ARGS;
void any_key(Terminal *term, unsigned int esc_timeout);
void normal_update(EditorState *e);
void main_loop(EditorState *e);
void ui_start(EditorState *e);
void ui_end(EditorState *e);
void handle_sigwinch(int signum);

#endif
