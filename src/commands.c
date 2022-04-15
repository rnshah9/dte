#include <errno.h>
#include <glob.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "commands.h"
#include "bind.h"
#include "change.h"
#include "cmdline.h"
#include "command/alias.h"
#include "command/args.h"
#include "command/macro.h"
#include "command/serialize.h"
#include "config.h"
#include "convert.h"
#include "copy.h"
#include "editor.h"
#include "encoding.h"
#include "error.h"
#include "file-option.h"
#include "filetype.h"
#include "frame.h"
#include "history.h"
#include "load-save.h"
#include "lock.h"
#include "misc.h"
#include "move.h"
#include "msg.h"
#include "regexp.h"
#include "search.h"
#include "selection.h"
#include "shift.h"
#include "show.h"
#include "spawn.h"
#include "syntax/state.h"
#include "syntax/syntax.h"
#include "tag.h"
#include "terminal/color.h"
#include "terminal/mode.h"
#include "terminal/osc52.h"
#include "terminal/terminal.h"
#include "util/arith.h"
#include "util/array.h"
#include "util/bit.h"
#include "util/bsearch.h"
#include "util/debug.h"
#include "util/hashmap.h"
#include "util/path.h"
#include "util/str-util.h"
#include "util/strtonum.h"
#include "util/xmalloc.h"
#include "util/xsnprintf.h"
#include "vars.h"
#include "view.h"
#include "window.h"

static void do_selection(View *view, SelectionType sel)
{
    if (sel == SELECT_NONE) {
        if (view->next_movement_cancels_selection) {
            view->next_movement_cancels_selection = false;
            unselect(view);
        }
        return;
    }

    view->next_movement_cancels_selection = true;

    if (view->selection) {
        view->selection = sel;
        mark_all_lines_changed(view->buffer);
        return;
    }

    view->sel_so = block_iter_get_offset(&view->cursor);
    view->sel_eo = UINT_MAX;
    view->selection = sel;

    // Need to mark current line changed because cursor might
    // move up or down before screen is updated
    view_update_cursor_y(view);
    buffer_mark_lines_changed(view->buffer, view->cy, view->cy);
}

static char last_flag_or_default(const CommandArgs *a, char def)
{
    size_t n = a->nr_flags;
    return n ? a->flags[n - 1] : def;
}

static char last_flag(const CommandArgs *a)
{
    return last_flag_or_default(a, 0);
}

static bool has_flag(const CommandArgs *a, unsigned char flag)
{
    return cmdargs_has_flag(a, flag);
}

static void handle_select_chars_flag(const CommandArgs *a)
{
    EditorState *e = a->userdata;
    do_selection(e->view, has_flag(a, 'c') ? SELECT_CHARS : SELECT_NONE);
}

static void handle_select_chars_or_lines_flags(const CommandArgs *a)
{
    EditorState *e = a->userdata;
    SelectionType sel = SELECT_NONE;
    if (has_flag(a, 'l')) {
        sel = SELECT_LINES;
    } else if (has_flag(a, 'c')) {
        sel = SELECT_CHARS;
    }
    do_selection(e->view, sel);
}

static void cmd_alias(const CommandArgs *a)
{
    const char *const name = a->args[0];
    const char *const cmd = a->args[1];

    if (unlikely(name[0] == '\0')) {
        error_msg("Empty alias name not allowed");
        return;
    }
    if (unlikely(name[0] == '-')) {
        // Disallowing this simplifies auto-completion for "alias "
        error_msg("Alias name cannot begin with '-'");
        return;
    }

    for (size_t i = 0; name[i]; i++) {
        unsigned char c = name[i];
        if (unlikely(!(is_word_byte(c) || c == '-' || c == '?' || c == '!'))) {
            error_msg("Invalid byte in alias name: %c (0x%02hhX)", c, c);
            return;
        }
    }

    if (unlikely(find_normal_command(name))) {
        error_msg("Can't replace existing command %s with an alias", name);
        return;
    }

    HashMap *aliases = &normal_commands.aliases;
    if (likely(cmd)) {
        add_alias(aliases, name, cmd);
    } else {
        remove_alias(aliases, name);
    }
}

static void cmd_bind(const CommandArgs *a)
{
    const char *keystr = a->args[0];
    const char *cmd = a->args[1];
    KeyCode key;
    if (unlikely(!parse_key_string(&key, keystr))) {
        if (cmd) {
            error_msg("invalid key string: %s", keystr);
        }
        return;
    }

    const bool modes[] = {
        [INPUT_NORMAL] = a->nr_flags == 0 || has_flag(a, 'n'),
        [INPUT_COMMAND] = has_flag(a, 'c'),
        [INPUT_SEARCH] = has_flag(a, 's'),
    };

    EditorState *e = a->userdata;
    static_assert(ARRAYLEN(modes) == ARRAYLEN(e->bindings));

    if (likely(cmd)) {
        for (InputMode mode = 0; mode < ARRAYLEN(modes); mode++) {
            if (modes[mode]) {
                add_binding(&e->bindings[mode], key, cmd);
            }
        }
    } else {
        for (InputMode mode = 0; mode < ARRAYLEN(modes); mode++) {
            if (modes[mode]) {
                remove_binding(&e->bindings[mode], key);
            }
        }
    }
}

static void cmd_bof(const CommandArgs *a)
{
    EditorState *e = a->userdata;
    do_selection(e->view, SELECT_NONE);
    move_bof(e->view);
}

static void cmd_bol(const CommandArgs *a)
{
    static const FlagMapping map[] = {
        {'s', BOL_SMART},
        {'t', BOL_SMART | BOL_SMART_TOGGLE},
    };

    EditorState *e = a->userdata;
    SmartBolFlags flags = cmdargs_convert_flags(a, map, ARRAYLEN(map), 0);
    handle_select_chars_flag(a);
    move_bol_smart(e->view, flags);
}

static void cmd_bolsf(const CommandArgs *a)
{
    EditorState *e = a->userdata;
    View *view = e->view;
    do_selection(view, SELECT_NONE);
    if (!block_iter_bol(&view->cursor)) {
        unsigned int margin = e->options.scroll_margin;
        long top = view->vy + window_get_scroll_margin(e->window, margin);
        if (view->cy > top) {
            move_up(view, view->cy - top);
        } else {
            block_iter_bof(&view->cursor);
        }
    }
    view_reset_preferred_x(view);
}

static void cmd_bookmark(const CommandArgs *a)
{
    EditorState *e = a->userdata;
    if (has_flag(a, 'r')) {
        bookmark_pop(&e->bookmarks);
        return;
    }

    bookmark_push(&e->bookmarks, get_current_file_location(e->view));
}

static void cmd_case(const CommandArgs *a)
{
    EditorState *e = a->userdata;
    change_case(e->view, last_flag_or_default(a, 't'));
}

static void mark_tabbar_changed(Window *w, void* UNUSED_ARG(data))
{
    w->update_tabbar = true;
}

static void cmd_cd(const CommandArgs *a)
{
    EditorState *e = a->userdata;
    const char *dir = a->args[0];
    if (unlikely(dir[0] == '\0')) {
        error_msg("directory argument cannot be empty");
        return;
    }

    if (streq(dir, "-")) {
        dir = getenv("OLDPWD");
        if (!dir || dir[0] == '\0') {
            error_msg("OLDPWD not set");
            return;
        }
    }

    char buf[8192];
    const char *cwd = getcwd(buf, sizeof(buf));
    if (chdir(dir) != 0) {
        error_msg("changing directory failed: %s", strerror(errno));
        return;
    }

    if (likely(cwd)) {
        int r = setenv("OLDPWD", cwd, 1);
        if (unlikely(r != 0)) {
            LOG_WARNING("failed to set OLDPWD: %s", strerror(errno));
        }
    }

    cwd = getcwd(buf, sizeof(buf));
    if (likely(cwd)) {
        int r = setenv("PWD", cwd, 1);
        if (unlikely(r != 0)) {
            LOG_WARNING("failed to set PWD: %s", strerror(errno));
        }
    }

    for (size_t i = 0, n = e->buffers.count; i < n; i++) {
        Buffer *b = e->buffers.ptrs[i];
        update_short_filename_cwd(b, cwd);
    }

    frame_for_each_window(e->root_frame, mark_tabbar_changed, NULL);
}

static void cmd_center_view(const CommandArgs *a)
{
    EditorState *e = a->userdata;
    e->view->force_center = true;
}

static void cmd_clear(const CommandArgs *a)
{
    EditorState *e = a->userdata;
    clear_lines(e->view);
}

static void cmd_close(const CommandArgs *a)
{
    EditorState *e = a->userdata;
    bool force = has_flag(a, 'f');
    bool prompt = has_flag(a, 'p');
    bool allow_quit = has_flag(a, 'q');
    bool allow_wclose = has_flag(a, 'w');

    if (!view_can_close(e->view) && !force) {
        if (prompt) {
            static const char str[] = "Close without saving changes? [y/N]";
            if (dialog_prompt(e, str, "ny") != 'y') {
                return;
            }
        } else {
            error_msg (
                "The buffer is modified. "
                "Save or run 'close -f' to close without saving."
            );
            return;
        }
    }

    if (allow_quit && e->buffers.count == 1 && e->root_frame->frames.count <= 1) {
        e->status = EDITOR_EXITING;
        return;
    }

    if (allow_wclose && e->window->views.count <= 1) {
        window_close_current(e);
        return;
    }

    window_close_current_view(e, e->window);
    set_view(e, e->window->view);
}

static void cmd_command(const CommandArgs *a)
{
    EditorState *e = a->userdata;
    const char *text = a->args[0];
    set_input_mode(e, INPUT_COMMAND);
    if (text) {
        cmdline_set_text(&e->cmdline, text);
    }
}

static void cmd_compile(const CommandArgs *a)
{
    static const FlagMapping map[] = {
        {'1', SPAWN_READ_STDOUT},
        {'p', SPAWN_PROMPT},
        {'s', SPAWN_QUIET},
    };

    EditorState *e = a->userdata;
    const char *name = a->args[0];
    Compiler *c = find_compiler(&e->compilers, name);
    if (unlikely(!c)) {
        error_msg("No such error parser %s", name);
        return;
    }

    SpawnFlags flags = cmdargs_convert_flags(a, map, ARRAYLEN(map), 0);
    clear_messages(&e->messages);
    spawn_compiler((const char **)a->args + 1, flags, c, &e->messages);
    if (e->messages.array.count) {
        activate_current_message_save(&e->messages, &e->bookmarks, e->view);
    }
}

static void cmd_copy(const CommandArgs *a)
{
    EditorState *e = a->userdata;
    View *view = e->view;
    const BlockIter save = view->cursor;
    size_t size;
    bool line_copy;
    if (view->selection) {
        size = prepare_selection(view);
        line_copy = (view->selection == SELECT_LINES);
    } else {
        block_iter_bol(&view->cursor);
        BlockIter tmp = view->cursor;
        size = block_iter_eat_line(&tmp);
        line_copy = true;
    }

    if (unlikely(size == 0)) {
        return;
    }

    bool internal = has_flag(a, 'i');
    bool clipboard = has_flag(a, 'b');
    bool primary = has_flag(a, 'p');
    if (!(internal || clipboard || primary)) {
        internal = true;
    }

    if (internal) {
        copy(&e->clipboard, view, size, line_copy);
    }

    Terminal *term = &e->terminal;
    if ((clipboard || primary) && term->features & TFLAG_OSC52_COPY) {
        if (internal) {
            view->cursor = save;
            if (view->selection) {
                size = prepare_selection(view);
            }
        }
        char *buf = block_iter_get_bytes(&view->cursor, size);
        if (!term_osc52_copy(&term->obuf, buf, size, clipboard, primary)) {
            error_msg("%s", strerror(errno));
        }
        free(buf);
    }

    if (!has_flag(a, 'k')) {
        unselect(view);
    }

    view->cursor = save;
}

static void cmd_cursor(const CommandArgs *a)
{
    EditorState *e = a->userdata;
    if (unlikely(a->nr_args == 0)) {
        // Reset all cursor styles
        for (CursorInputMode m = 0; m < ARRAYLEN(e->cursor_styles); m++) {
            e->cursor_styles[m] = get_default_cursor_style(m);
        }
        e->cursor_style_changed = true;
        return;
    }

    CursorInputMode mode = cursor_mode_from_str(a->args[0]);
    if (unlikely(mode >= NR_CURSOR_MODES)) {
        error_msg("invalid mode argument: %s", a->args[0]);
        return;
    }

    TermCursorStyle style = get_default_cursor_style(mode);
    if (a->nr_args >= 2) {
        style.type = cursor_type_from_str(a->args[1]);
        if (unlikely(style.type == CURSOR_INVALID)) {
            error_msg("invalid cursor type: %s", a->args[1]);
            return;
        }
    }

    if (a->nr_args >= 3) {
        style.color = cursor_color_from_str(a->args[2]);
        if (unlikely(style.color == COLOR_INVALID)) {
            error_msg("invalid cursor color: %s", a->args[2]);
            return;
        }
    }

    e->cursor_styles[mode] = style;
    e->cursor_style_changed = true;
}

static void cmd_cut(const CommandArgs *a)
{
    EditorState *e = a->userdata;
    View *view = e->view;
    const long x = view_get_preferred_x(view);
    if (view->selection) {
        bool is_lines = view->selection == SELECT_LINES;
        cut(&e->clipboard, view, prepare_selection(view), is_lines);
        if (view->selection == SELECT_LINES) {
            move_to_preferred_x(view, x);
        }
        unselect(view);
    } else {
        BlockIter tmp;
        block_iter_bol(&view->cursor);
        tmp = view->cursor;
        cut(&e->clipboard, view, block_iter_eat_line(&tmp), true);
        move_to_preferred_x(view, x);
    }
}

static void cmd_delete(const CommandArgs *a)
{
    EditorState *e = a->userdata;
    delete_ch(e->view);
}

static void cmd_delete_eol(const CommandArgs *a)
{
    EditorState *e = a->userdata;
    View *view = e->view;
    if (view->selection) {
        return;
    }

    bool delete_newline_if_at_eol = has_flag(a, 'n');
    BlockIter bi = view->cursor;
    if (delete_newline_if_at_eol) {
        CodePoint ch;
        if (block_iter_get_char(&view->cursor, &ch) == 1 && ch == '\n') {
            delete_ch(view);
            return;
        }
    }

    buffer_delete_bytes(view, block_iter_eol(&bi));
}

static void cmd_delete_line(const CommandArgs *a)
{
    EditorState *e = a->userdata;
    delete_lines(e->view);
}

static void cmd_delete_word(const CommandArgs *a)
{
    EditorState *e = a->userdata;
    bool skip_non_word = has_flag(a, 's');
    BlockIter bi = e->view->cursor;
    buffer_delete_bytes(e->view, word_fwd(&bi, skip_non_word));
}

static void cmd_down(const CommandArgs *a)
{
    EditorState *e = a->userdata;
    handle_select_chars_or_lines_flags(a);
    move_down(e->view, 1);
}

static void cmd_eof(const CommandArgs *a)
{
    EditorState *e = a->userdata;
    do_selection(e->view, SELECT_NONE);
    move_eof(e->view);
}

static void cmd_eol(const CommandArgs *a)
{
    EditorState *e = a->userdata;
    handle_select_chars_flag(a);
    move_eol(e->view);
}

static void cmd_eolsf(const CommandArgs *a)
{
    EditorState *e = a->userdata;
    Window *window = e->window;
    View *view = e->view;
    do_selection(view, SELECT_NONE);
    if (!block_iter_eol(&view->cursor)) {
        long margin = window_get_scroll_margin(window, e->options.scroll_margin);
        long bottom = view->vy + window->edit_h - 1 - margin;
        if (view->cy < bottom) {
            move_down(view, bottom - view->cy);
        } else {
            block_iter_eof(&view->cursor);
        }
    }
    view_reset_preferred_x(view);
}

static void cmd_erase(const CommandArgs *a)
{
    EditorState *e = a->userdata;
    erase(e->view);
}

static void cmd_erase_bol(const CommandArgs *a)
{
    EditorState *e = a->userdata;
    buffer_erase_bytes(e->view, block_iter_bol(&e->view->cursor));
}

static void cmd_erase_word(const CommandArgs *a)
{
    EditorState *e = a->userdata;
    View *view = e->view;
    bool skip_non_word = has_flag(a, 's');
    buffer_erase_bytes(view, word_bwd(&view->cursor, skip_non_word));
}

static void cmd_errorfmt(const CommandArgs *a)
{
    BUG_ON(a->nr_args == 0);
    EditorState *e = a->userdata;
    const char *name = a->args[0];
    if (a->nr_args == 1) {
        remove_compiler(&e->compilers, name);
        return;
    }

    bool ignore = has_flag(a, 'i');
    add_error_fmt(&e->compilers, name, ignore, a->args[1], a->args + 2);
}

static void open_files_from_string(EditorState *e, const String *str)
{
    PointerArray filenames = PTR_ARRAY_INIT;
    for (size_t pos = 0, size = str->len; pos < size; ) {
        char *filename = buf_next_line(str->buffer, &pos, size);
        if (filename[0] != '\0') {
            ptr_array_append(&filenames, filename);
        }
    }

    ptr_array_append(&filenames, NULL);
    window_open_files(e, e->window, (char**)filenames.ptrs, NULL);
    macro_command_hook("open", (char**)filenames.ptrs);
    ptr_array_free_array(&filenames);
}

static void parse_and_activate_message(EditorState *e, const String *str)
{
    size_t x;
    if (buf_parse_size(str->buffer, str->len, &x) > 0 && x > 0) {
        activate_message(&e->messages, x - 1);
    }
}

static void parse_and_goto_tag(EditorState *e, const String *str)
{
    if (unlikely(str->len == 0)) {
        error_msg("child produced no output");
        return;
    }

    size_t pos = 0;
    const char *tag = buf_next_line(str->buffer, &pos, str->len);
    tag_lookup(tag, e->buffer->abs_filename, &e->messages);
    activate_current_message_save(&e->messages, &e->bookmarks, e->view);
}

static const char **lines_and_columns_env(const Window *window)
{
    static char lines[DECIMAL_STR_MAX(window->edit_h)];
    static char columns[DECIMAL_STR_MAX(window->edit_w)];
    static const char *vars[] = {
        "LINES", lines,
        "COLUMNS", columns,
        NULL,
    };

    xsnprintf(lines, sizeof lines, "%d", window->edit_h);
    xsnprintf(columns, sizeof columns, "%d", window->edit_w);
    return vars;
}

static void show_spawn_error_msg(const String *errstr, int err)
{
    if (err <= 0) {
        return;
    }

    char msg[512];
    if (errstr->len) {
        size_t pos = 0;
        StringView line = buf_slice_next_line(errstr->buffer, &pos, errstr->len);
        BUG_ON(pos == 0);
        xsnprintf(msg, sizeof(msg), ": \"%.*s\"", (int)line.length, line.data);
    } else {
        msg[0] = '\0';
    }

    if (err >= 256) {
        int sig = err >> 8;
        const char *str = strsignal(sig);
        error_msg("Child received signal %d (%s)%s", sig, str ? str : "??", msg);
    } else if (err) {
        error_msg("Child returned %d%s", err, msg);
    }
}

typedef enum {
    // Note: these need to be kept sorted
    EXEC_BUFFER,
    EXEC_ERRMSG,
    EXEC_EVAL,
    EXEC_LINE,
    EXEC_MSG,
    EXEC_NULL,
    EXEC_OPEN,
    EXEC_TAG,
    EXEC_TTY,
} ExecAction;

typedef enum {
    IN = 1 << 0,
    OUT = 1 << 1,
    ERR = 1 << 2,
    ALL = IN | OUT | ERR,
} ExecFlags;

static const struct {
    char name[7];
    uint8_t spawn_action : 2; // SpawnAction
    uint8_t flags : 6; // ExecFlags
} exec_map[] = {
    [EXEC_BUFFER] = {"buffer", SPAWN_PIPE, IN | OUT},
    [EXEC_ERRMSG] = {"errmsg", SPAWN_PIPE, ERR},
    [EXEC_EVAL] = {"eval", SPAWN_PIPE, OUT},
    [EXEC_LINE] = {"line", SPAWN_PIPE, IN},
    [EXEC_MSG] = {"msg", SPAWN_PIPE, IN | OUT},
    [EXEC_NULL] = {"null", SPAWN_NULL, ALL},
    [EXEC_OPEN] = {"open", SPAWN_PIPE, OUT},
    [EXEC_TAG] = {"tag", SPAWN_PIPE, OUT},
    [EXEC_TTY] = {"tty", SPAWN_TTY, ALL},
};

UNITTEST {
    CHECK_BSEARCH_ARRAY(exec_map, name, strcmp);
}

static void cmd_exec(const CommandArgs *a)
{
    ExecAction actions[3] = {EXEC_TTY, EXEC_TTY, EXEC_TTY};
    SpawnAction spawn_actions[3] = {SPAWN_TTY, SPAWN_TTY, SPAWN_TTY};
    SpawnFlags spawn_flags = 0;
    bool lflag = false;
    bool move_after_insert = false;
    bool strip_trailing_newline = false;

    for (size_t i = 0, n = a->nr_flags, x = 0; i < n; i++) {
        size_t fd_idx;
        switch (a->flags[i]) {
            case 'e': fd_idx = STDERR_FILENO; break;
            case 'i': fd_idx = STDIN_FILENO; break;
            case 'o': fd_idx = STDOUT_FILENO; break;
            case 'p': spawn_flags |= SPAWN_PROMPT; continue;
            case 's': spawn_flags |= SPAWN_QUIET; continue;
            case 't': spawn_flags &= ~SPAWN_QUIET; continue;
            case 'l': lflag = true; continue;
            case 'm': move_after_insert = true; continue;
            case 'n': strip_trailing_newline = true; continue;
            default: BUG("unexpected flag"); return;
        }
        const char *flag_arg = a->args[x++];
        ssize_t action = BSEARCH_IDX(flag_arg, exec_map, (CompareFunction)strcmp);
        if (action < 0 || !(exec_map[action].flags & 1u << fd_idx)) {
            error_msg("invalid action for -%c: '%s'", a->flags[i], flag_arg);
            return;
        }
        actions[fd_idx] = action;
        spawn_actions[fd_idx] = exec_map[action].spawn_action;
    }

    EditorState *e = a->userdata;
    View *view = e->view;
    BlockIter saved_cursor = view->cursor;
    char *alloc = NULL;
    const char **env = NULL;
    if (actions[STDOUT_FILENO] == EXEC_BUFFER) {
        env = lines_and_columns_env(e->window);
    }
    if (move_after_insert && actions[STDOUT_FILENO] != EXEC_BUFFER) {
        move_after_insert = false;
    }
    if (lflag && actions[STDIN_FILENO] == EXEC_BUFFER) {
        // For compat. with old "filter" and "pipe-to" commands
        actions[STDIN_FILENO] = EXEC_LINE;
    }

    SpawnContext ctx = {
        .argv = (const char **)a->args + a->nr_flag_args,
        .outputs = {STRING_INIT, STRING_INIT},
        .flags = spawn_flags,
        .env = env,
    };

    switch (actions[STDIN_FILENO]) {
    case EXEC_LINE:
        if (view->selection) {
            ctx.input.length = prepare_selection(view);
        } else {
            StringView line;
            move_bol(view);
            fill_line_ref(&view->cursor, &line);
            ctx.input.length = line.length;
        }
        get_bytes:
        alloc = block_iter_get_bytes(&view->cursor, ctx.input.length);
        ctx.input.data = alloc;
        break;
    case EXEC_BUFFER:
        if (view->selection) {
            ctx.input.length = prepare_selection(view);
        } else {
            Block *blk;
            block_for_each(blk, &view->buffer->blocks) {
                ctx.input.length += blk->size;
            }
            move_bof(view);
        }
        goto get_bytes;
    case EXEC_MSG: {
        String messages = dump_messages(&e->messages);
        ctx.input = strview_from_string(&messages),
        alloc = messages.buffer;
        break;
    }
    case EXEC_NULL:
    case EXEC_TTY:
        break;
    // These are already handled by the validity check above:
    case EXEC_OPEN:
    case EXEC_TAG:
    case EXEC_EVAL:
    case EXEC_ERRMSG:
    default:
        BUG("unhandled action");
        return;
    }

    int err = spawn(&ctx, spawn_actions);
    free(alloc);
    if (err != 0) {
        show_spawn_error_msg(&ctx.outputs[1], err);
        string_free(&ctx.outputs[1]);
        view->cursor = saved_cursor;
        return;
    }

    string_free(&ctx.outputs[1]);
    String *output = &ctx.outputs[0];
    if (
        strip_trailing_newline
        && actions[STDOUT_FILENO] == EXEC_BUFFER
        && output->len > 0
        && output->buffer[output->len - 1] == '\n'
    ) {
        output->len--;
        if (output->len > 0 && output->buffer[output->len - 1] == '\r') {
            output->len--;
        }
    }

    switch (actions[STDOUT_FILENO]) {
    case EXEC_BUFFER: {
        size_t del_count = ctx.input.length;
        if (view->selection && del_count == 0) {
            del_count = prepare_selection(view);
        }
        buffer_replace_bytes(view, del_count, output->buffer, output->len);
        unselect(view);
        break;
    }
    case EXEC_MSG:
        parse_and_activate_message(e, output);
        break;
    case EXEC_OPEN:
        open_files_from_string(e, output);
        break;
    case EXEC_TAG:
        parse_and_goto_tag(e, output);
        break;
    case EXEC_EVAL:
        exec_config(&normal_commands, strview_from_string(output));
        break;
    case EXEC_NULL:
    case EXEC_TTY:
        break;
    case EXEC_LINE: // Already handled above
    case EXEC_ERRMSG:
    default:
        BUG("unhandled action");
        return;
    }

    if (move_after_insert) {
        block_iter_skip_bytes(&view->cursor, output->len);
    }
    string_free(output);
}

static void cmd_ft(const CommandArgs *a)
{
    char **args = a->args;
    const char *filetype = args[0];
    if (unlikely(!is_valid_filetype_name(filetype))) {
        error_msg("Invalid filetype name: '%s'", filetype);
    }

    FileDetectionType dt = FT_EXTENSION;
    switch (last_flag(a)) {
    case 'b':
        dt = FT_BASENAME;
        break;
    case 'c':
        dt = FT_CONTENT;
        break;
    case 'f':
        dt = FT_FILENAME;
        break;
    case 'i':
        dt = FT_INTERPRETER;
        break;
    }

    EditorState *e = a->userdata;
    for (size_t i = 1, n = a->nr_args; i < n; i++) {
        add_filetype(&e->filetypes, filetype, args[i], dt);
    }
}

static void cmd_hi(const CommandArgs *a)
{
    EditorState *e = a->userdata;
    if (unlikely(a->nr_args == 0)) {
        exec_builtin_color_reset(&e->colors, e->terminal.color_type);
        goto update;
    }

    char **strs = a->args + 1;
    size_t strs_len = a->nr_args - 1;
    TermColor color;
    ssize_t n = parse_term_color(&color, strs, strs_len);
    if (unlikely(n != strs_len)) {
        BUG_ON(n > strs_len);
        if (n < 0) {
            error_msg("too many colors");
        } else {
            error_msg("invalid color or attribute: '%s'", strs[n]);
        }
        return;
    }

    TermColorCapabilityType color_type = e->terminal.color_type;
    bool optimize = e->options.optimize_true_color;
    int32_t fg = color_to_nearest(color.fg, color_type, optimize);
    int32_t bg = color_to_nearest(color.bg, color_type, optimize);
    if (
        color_type != TERM_TRUE_COLOR
        && has_flag(a, 'c')
        && (fg != color.fg || bg != color.bg)
    ) {
        return;
    }

    color.fg = fg;
    color.bg = bg;
    set_highlight_color(&e->colors, a->args[0], &color);

update:
    // Don't call update_all_syntax_colors() needlessly.
    // It is called right after config has been loaded.
    if (e->status != EDITOR_INITIALIZING) {
        update_all_syntax_colors(&e->syntaxes);
        mark_everything_changed(e);
    }
}

static void cmd_include(const CommandArgs *a)
{
    ConfigFlags flags = has_flag(a, 'q') ? CFG_NOFLAGS : CFG_MUST_EXIST;
    if (has_flag(a, 'b')) {
        flags |= CFG_BUILTIN;
    }
    read_config(&normal_commands, a->args[0], flags);
}

static void cmd_insert(const CommandArgs *a)
{
    EditorState *e = a->userdata;
    const char *str = a->args[0];
    if (has_flag(a, 'k')) {
        for (size_t i = 0; str[i]; i++) {
            insert_ch(e->view, str[i]);
        }
        return;
    }

    bool move_after = has_flag(a, 'm');
    insert_text(e->view, str, strlen(str), move_after);
}

static void cmd_join(const CommandArgs *a)
{
    EditorState *e = a->userdata;
    join_lines(e->view);
}

static void cmd_left(const CommandArgs *a)
{
    EditorState *e = a->userdata;
    handle_select_chars_flag(a);
    move_cursor_left(e->view);
}

static void cmd_line(const CommandArgs *a)
{
    EditorState *e = a->userdata;
    View *view = e->view;
    const char *arg = a->args[0];
    const long x = view_get_preferred_x(view);
    size_t line;
    if (unlikely(!str_to_size(arg, &line) || line == 0)) {
        error_msg("Invalid line number: %s", arg);
        return;
    }

    do_selection(view, SELECT_NONE);
    move_to_line(view, line);
    move_to_preferred_x(view, x);
}

static void cmd_load_syntax(const CommandArgs *a)
{
    EditorState *e = a->userdata;
    const char *arg = a->args[0];
    const char *slash = strrchr(arg, '/');
    if (!slash) {
        const char *filetype = arg;
        if (!find_syntax(&e->syntaxes, filetype)) {
            load_syntax_by_filetype(filetype);
        }
        return;
    }

    const char *filetype = slash + 1;
    if (find_syntax(&e->syntaxes, filetype)) {
        error_msg("Syntax for filetype %s already loaded", filetype);
        return;
    }

    int err;
    load_syntax_file(arg, CFG_MUST_EXIST, &err);
}

static void cmd_macro(const CommandArgs *a)
{
    static const struct {
        const char name[8];
        void (*handler)(void);
    } actions[] = {
        {"record", macro_record},
        {"stop", macro_stop},
        {"toggle", macro_toggle},
        {"cancel", macro_cancel},
        {"play", macro_play},
        {"run", macro_play},
    };
    const char *action = a->args[0];
    for (size_t i = 0; i < ARRAYLEN(actions); i++) {
        if (streq(action, actions[i].name)) {
            actions[i].handler();
            return;
        }
    }
    error_msg("Unknown action '%s'", action);
}

static void cmd_match_bracket(const CommandArgs *a)
{
    EditorState *e = a->userdata;
    View *view = e->view;
    CodePoint cursor_char;
    if (!block_iter_get_char(&view->cursor, &cursor_char)) {
        error_msg("No character under cursor");
        return;
    }

    CodePoint target = cursor_char;
    BlockIter bi = view->cursor;
    size_t level = 0;
    CodePoint u = 0;

    switch (cursor_char) {
    case '<':
    case '[':
    case '{':
        target++;
        // Fallthrough
    case '(':
        target++;
        goto search_fwd;
    case '>':
    case ']':
    case '}':
        target--;
        // Fallthrough
    case ')':
        target--;
        goto search_bwd;
    default:
        error_msg("Character under cursor not matchable");
        return;
    }

search_fwd:
    block_iter_next_char(&bi, &u);
    BUG_ON(u != cursor_char);
    while (block_iter_next_char(&bi, &u)) {
        if (u == target) {
            if (level == 0) {
                block_iter_prev_char(&bi, &u);
                view->cursor = bi;
                return; // Found
            }
            level--;
        } else if (u == cursor_char) {
            level++;
        }
    }
    goto not_found;

search_bwd:
    while (block_iter_prev_char(&bi, &u)) {
        if (u == target) {
            if (level == 0) {
                view->cursor = bi;
                return; // Found
            }
            level--;
        } else if (u == cursor_char) {
            level++;
        }
    }

not_found:
    error_msg("No matching bracket found");
}

static void cmd_move_tab(const CommandArgs *a)
{
    EditorState *e = a->userdata;
    Window *window = e->window;
    const size_t ntabs = window->views.count;
    const char *str = a->args[0];
    size_t to, from = ptr_array_idx(&window->views, e->view);
    BUG_ON(from >= ntabs);
    if (streq(str, "left")) {
        to = size_decrement_wrapped(from, ntabs);
    } else if (streq(str, "right")) {
        to = size_increment_wrapped(from, ntabs);
    } else {
        if (!str_to_size(str, &to) || to == 0) {
            error_msg("Invalid tab position %s", str);
            return;
        }
        to--;
        if (to >= ntabs) {
            to = ntabs - 1;
        }
    }
    ptr_array_move(&window->views, from, to);
    window->update_tabbar = true;
}

static void cmd_msg(const CommandArgs *a)
{
    EditorState *e = a->userdata;
    const char *arg = a->args[0];
    if (!arg) {
        switch (last_flag(a)) {
        case 0:
            activate_current_message(&e->messages);
            break;
        case 'n':
            activate_next_message(&e->messages);
            break;
        case 'p':
            activate_prev_message(&e->messages);
            break;
        default:
            BUG("unexpected flag");
        }
        return;
    }

    if (a->nr_flags != 0) {
        error_msg("flags (-n|-p) and arguments are mutually exclusive");
        return;
    }

    size_t idx;
    if (!str_to_size(arg, &idx) || idx == 0) {
        error_msg("invalid message index: %s", arg);
        return;
    }

    activate_message(&e->messages, idx - 1);
}

static void cmd_new_line(const CommandArgs *a)
{
    EditorState *e = a->userdata;
    new_line(e->view, has_flag(a, 'a'));
}

static void cmd_next(const CommandArgs *a)
{
    EditorState *e = a->userdata;
    size_t i = ptr_array_idx(&e->window->views, e->view);
    size_t n = e->window->views.count;
    BUG_ON(i >= n);
    set_view(e, e->window->views.ptrs[size_increment_wrapped(i, n)]);
}

static bool xglob(char **args, glob_t *globbuf)
{
    BUG_ON(!args);
    BUG_ON(!args[0]);
    int err = glob(*args, GLOB_NOCHECK, NULL, globbuf);
    while (err == 0 && *++args) {
        err = glob(*args, GLOB_NOCHECK | GLOB_APPEND, NULL, globbuf);
    }

    if (likely(err == 0)) {
        BUG_ON(globbuf->gl_pathc == 0);
        BUG_ON(!globbuf->gl_pathv);
        BUG_ON(!globbuf->gl_pathv[0]);
        return true;
    }

    BUG_ON(err == GLOB_NOMATCH);
    error_msg("glob: %s", (err == GLOB_NOSPACE) ? strerror(ENOMEM) : "failed");
    globfree(globbuf);
    return false;
}

static void cmd_open(const CommandArgs *a)
{
    bool temporary = has_flag(a, 't');
    if (unlikely(temporary && a->nr_args > 0)) {
        error_msg("'open -t' can't be used with filename arguments");
        return;
    }

    const char *requested_encoding = NULL;
    char **args = a->args;
    if (unlikely(a->nr_flag_args > 0)) {
        // The "-e" flag is the only one that takes an argument, so the
        // above condition implies it was used
        BUG_ON(!has_flag(a, 'e'));
        requested_encoding = args[a->nr_flag_args - 1];
        args += a->nr_flag_args;
    }

    Encoding encoding = {.type = ENCODING_AUTODETECT};
    if (requested_encoding) {
        EncodingType enctype = lookup_encoding(requested_encoding);
        if (enctype == UTF8) {
            encoding = encoding_from_type(enctype);
        } else if (conversion_supported_by_iconv(requested_encoding, "UTF-8")) {
            encoding = encoding_from_name(requested_encoding);
        } else {
            if (errno == EINVAL) {
                error_msg("Unsupported encoding '%s'", requested_encoding);
            } else {
                error_msg (
                    "iconv conversion from '%s' failed: %s",
                    requested_encoding,
                    strerror(errno)
                );
            }
            return;
        }
    }

    EditorState *e = a->userdata;
    if (a->nr_args == 0) {
        View *v = window_open_new_file(e, e->window);
        v->buffer->temporary = temporary;
        if (requested_encoding) {
            buffer_set_encoding(v->buffer, encoding);
        }
        return;
    }

    char **paths = args;
    glob_t globbuf;
    bool use_glob = has_flag(a, 'g');
    if (use_glob) {
        if (!xglob(args, &globbuf)) {
            return;
        }
        paths = globbuf.gl_pathv;
    }

    if (!paths[1]) {
        // Previous view is remembered when opening single file
        window_open_file(e, e->window, paths[0], &encoding);
    } else {
        // It makes no sense to remember previous view when opening
        // multiple files
        window_open_files(e, e->window, paths, &encoding);
    }

    if (use_glob) {
        globfree(&globbuf);
    }
}

static void cmd_option(const CommandArgs *a)
{
    BUG_ON(a->nr_args < 3);
    size_t nstrs = a->nr_args - 1;
    if (unlikely(nstrs & 1)) {
        error_msg("Missing option value");
        return;
    }

    char **strs = a->args + 1;
    if (unlikely(!validate_local_options(strs))) {
        return;
    }

    EditorState *e = a->userdata;
    PointerArray *opts = &e->file_options;
    if (has_flag(a, 'r')) {
        const StringView pattern = strview_from_cstring(a->args[0]);
        add_file_options(opts, FILE_OPTIONS_FILENAME, pattern, strs, nstrs);
        return;
    }

    const char *ft_list = a->args[0];
    for (size_t pos = 0, len = strlen(ft_list); pos < len; ) {
        const StringView filetype = get_delim(ft_list, &pos, len, ',');
        add_file_options(opts, FILE_OPTIONS_FILETYPE, filetype, strs, nstrs);
    }
}

static void cmd_blkdown(const CommandArgs *a)
{
    handle_select_chars_or_lines_flags(a);

    // If current line is blank, skip past consecutive blank lines
    EditorState *e = a->userdata;
    View *view = e->view;
    StringView line;
    fetch_this_line(&view->cursor, &line);
    if (strview_isblank(&line)) {
        while (block_iter_next_line(&view->cursor)) {
            fill_line_ref(&view->cursor, &line);
            if (!strview_isblank(&line)) {
                break;
            }
        }
    }

    // Skip past non-blank lines
    while (block_iter_next_line(&view->cursor)) {
        fill_line_ref(&view->cursor, &line);
        if (strview_isblank(&line)) {
            break;
        }
    }

    // If we reach the last populated line in the buffer, move down one line
    BlockIter tmp = view->cursor;
    block_iter_eol(&tmp);
    block_iter_skip_bytes(&tmp, 1);
    if (block_iter_is_eof(&tmp)) {
        view->cursor = tmp;
    }
}

static void cmd_blkup(const CommandArgs *a)
{
    handle_select_chars_or_lines_flags(a);

    // If cursor is on the first line, just move to bol
    EditorState *e = a->userdata;
    View *view = e->view;
    if (view->cy == 0) {
        block_iter_bol(&view->cursor);
        return;
    }

    // If current line is blank, skip past consecutive blank lines
    StringView line;
    fetch_this_line(&view->cursor, &line);
    if (strview_isblank(&line)) {
        while (block_iter_prev_line(&view->cursor)) {
            fill_line_ref(&view->cursor, &line);
            if (!strview_isblank(&line)) {
                break;
            }
        }
    }

    // Skip past non-blank lines
    while (block_iter_prev_line(&view->cursor)) {
        fill_line_ref(&view->cursor, &line);
        if (strview_isblank(&line)) {
            break;
        }
    }
}

static void cmd_paste(const CommandArgs *a)
{
    EditorState *e = a->userdata;
    bool at_cursor = has_flag(a, 'c');
    paste(&e->clipboard, e->view, at_cursor);
}

static void cmd_pgdown(const CommandArgs *a)
{
    handle_select_chars_or_lines_flags(a);

    EditorState *e = a->userdata;
    Window *window = e->window;
    View *view = e->view;
    long margin = window_get_scroll_margin(window, e->options.scroll_margin);
    long bottom = view->vy + window->edit_h - 1 - margin;
    long count;

    if (view->cy < bottom) {
        count = bottom - view->cy;
    } else {
        count = window->edit_h - 1 - margin * 2;
    }
    move_down(view, count);
}

static void cmd_pgup(const CommandArgs *a)
{
    handle_select_chars_or_lines_flags(a);

    EditorState *e = a->userdata;
    Window *window = e->window;
    View *view = e->view;
    long margin = window_get_scroll_margin(window, e->options.scroll_margin);
    long top = view->vy + margin;
    long count;

    if (view->cy > top) {
        count = view->cy - top;
    } else {
        count = window->edit_h - 1 - margin * 2;
    }
    move_up(view, count);
}

static void cmd_prev(const CommandArgs *a)
{
    EditorState *e = a->userdata;
    size_t i = ptr_array_idx(&e->window->views, e->view);
    size_t n = e->window->views.count;
    BUG_ON(i >= n);
    set_view(e, e->window->views.ptrs[size_decrement_wrapped(i, n)]);
}

static void cmd_quit(const CommandArgs *a)
{
    EditorState *e = a->userdata;
    int exit_code = 0;
    if (a->nr_args) {
        if (!str_to_int(a->args[0], &exit_code)) {
            error_msg("Not a valid integer argument: '%s'", a->args[0]);
            return;
        }
        if (exit_code < 0 || exit_code > 125) {
            error_msg("Exit code should be between 0 and 125");
            return;
        }
    }

    if (has_flag(a, 'f')) {
        goto exit;
    }

    for (size_t i = 0, n = e->buffers.count; i < n; i++) {
        Buffer *b = e->buffers.ptrs[i];
        if (buffer_modified(b)) {
            // Activate modified buffer
            View *v = window_find_view(e->window, b);
            if (!v) {
                // Buffer isn't open in current window.
                // Activate first window of the buffer.
                v = b->views.ptrs[0];
                e->window = v->window;
                mark_everything_changed(e);
            }
            set_view(e, v);
            if (has_flag(a, 'p')) {
                static const char str[] = "Quit without saving changes? [y/N]";
                if (dialog_prompt(e, str, "ny") == 'y') {
                    goto exit;
                }
                return;
            } else {
                error_msg (
                    "Save modified files or run 'quit -f' to quit"
                    " without saving."
                );
                return;
            }
        }
    }

exit:
    e->status = EDITOR_EXITING;
    e->exit_code = exit_code;
}

static void cmd_redo(const CommandArgs *a)
{
    EditorState *e = a->userdata;
    char *arg = a->args[0];
    unsigned long change_id = 0;
    if (arg) {
        if (!str_to_ulong(arg, &change_id) || change_id == 0) {
            error_msg("Invalid change id: %s", arg);
            return;
        }
    }
    if (redo(e->view, change_id)) {
        unselect(e->view);
    }
}

static void cmd_refresh(const CommandArgs *a)
{
    EditorState *e = a->userdata;
    mark_everything_changed(e);
}

static void repeat_insert(EditorState *e, const char *str, unsigned int count, bool move_after)
{
    size_t str_len = strlen(str);
    size_t bufsize;
    if (unlikely(size_multiply_overflows(count, str_len, &bufsize))) {
        error_msg("Repeated insert would overflow");
        return;
    }
    if (unlikely(bufsize == 0)) {
        return;
    }
    char *buf = malloc(bufsize);
    if (unlikely(!buf)) {
        perror_msg("malloc");
        return;
    }

    char tmp[4096];
    if (str_len == 1) {
        memset(buf, str[0], bufsize);
        goto insert;
    } else if (bufsize < 2 * sizeof(tmp) || str_len > sizeof(tmp) / 8) {
        for (size_t i = 0; i < count; i++) {
            memcpy(buf + (i * str_len), str, str_len);
        }
        goto insert;
    }

    size_t strs_per_tmp = sizeof(tmp) / str_len;
    size_t tmp_len = strs_per_tmp * str_len;
    size_t tmps_per_buf = bufsize / tmp_len;
    size_t remainder = bufsize % tmp_len;

    // Create a block of text containing `strs_per_tmp` concatenated strs
    for (size_t i = 0; i < strs_per_tmp; i++) {
        memcpy(tmp + (i * str_len), str, str_len);
    }

    // Copy `tmps_per_buf` copies of `tmp` into `buf`
    for (size_t i = 0; i < tmps_per_buf; i++) {
        memcpy(buf + (i * tmp_len), tmp, tmp_len);
    }

    // Copy the remainder into `buf` (if any)
    if (remainder) {
        memcpy(buf + (tmps_per_buf * tmp_len), tmp, remainder);
    }

    LOG_DEBUG (
        "Optimized %u inserts of %zu bytes into %zu inserts of %zu bytes",
        count, str_len,
        tmps_per_buf, tmp_len
    );

insert:
    insert_text(e->view, buf, bufsize, move_after);
    free(buf);
}

static void cmd_repeat(const CommandArgs *a)
{
    unsigned int count;
    if (unlikely(!str_to_uint(a->args[0], &count))) {
        error_msg("Not a valid repeat count: %s", a->args[0]);
        return;
    }
    if (unlikely(count == 0)) {
        return;
    }

    const Command *cmd = find_normal_command(a->args[1]);
    if (unlikely(!cmd)) {
        error_msg("No such command: %s", a->args[1]);
        return;
    }

    EditorState *e = a->userdata;
    CommandArgs a2 = cmdargs_new(a->args + 2, e);
    current_command = cmd;
    bool ok = parse_args(cmd, &a2);
    current_command = NULL;
    if (unlikely(!ok)) {
        return;
    }

    void (*fn)(const CommandArgs *args) = cmd->cmd;
    if (fn == cmd_insert && !has_flag(&a2, 'k')) {
        // Use optimized implementation for repeated "insert"
        repeat_insert(e, a2.args[0], count, has_flag(&a2, 'm'));
        return;
    }

    while (count--) {
        fn(&a2);
    }
}

static void cmd_replace(const CommandArgs *a)
{
    static const FlagMapping map[] = {
        {'b', REPLACE_BASIC},
        {'c', REPLACE_CONFIRM},
        {'g', REPLACE_GLOBAL},
        {'i', REPLACE_IGNORE_CASE},
    };

    EditorState *e = a->userdata;
    ReplaceFlags flags = cmdargs_convert_flags(a, map, ARRAYLEN(map), 0);
    reg_replace(e, a->args[0], a->args[1], flags);
}

static void cmd_right(const CommandArgs *a)
{
    EditorState *e = a->userdata;
    handle_select_chars_flag(a);
    move_cursor_right(e->view);
}

static bool stat_changed(const Buffer *b, const struct stat *st)
{
    // Don't compare st_mode because we allow chmod 755 etc.
    return st->st_mtime != b->file.mtime
        || st->st_dev != b->file.dev
        || st->st_ino != b->file.ino;
}

static void cmd_save(const CommandArgs *a)
{
    EditorState *e = a->userdata;
    Buffer *buffer = e->buffer;
    if (unlikely(buffer->stdout_buffer)) {
        const char *f = buffer_filename(buffer);
        info_msg("%s can't be saved; it will be piped to stdout on exit", f);
        return;
    }

    bool dos_nl = has_flag(a, 'd');
    bool unix_nl = has_flag(a, 'u');
    bool crlf = buffer->crlf_newlines;
    if (unlikely(dos_nl && unix_nl)) {
        error_msg("flags -d and -u can't be used together");
        return;
    } else if (dos_nl) {
        crlf = true;
    } else if (unix_nl) {
        crlf = false;
    }

    const char *requested_encoding = NULL;
    char **args = a->args;
    if (unlikely(a->nr_flag_args > 0)) {
        BUG_ON(!has_flag(a, 'e'));
        requested_encoding = args[a->nr_flag_args - 1];
        args += a->nr_flag_args;
    }

    Encoding encoding = buffer->encoding;
    bool bom = buffer->bom;
    if (requested_encoding) {
        EncodingType et = lookup_encoding(requested_encoding);
        if (et == UTF8) {
            if (encoding.type != UTF8) {
                // Encoding changed
                encoding = encoding_from_type(et);
                bom = e->options.utf8_bom;
            }
        } else if (conversion_supported_by_iconv("UTF-8", requested_encoding)) {
            encoding = encoding_from_name(requested_encoding);
            if (encoding.name != buffer->encoding.name) {
                // Encoding changed
                bom = !!get_bom_for_encoding(encoding.type);
            }
        } else {
            if (errno == EINVAL) {
                error_msg("Unsupported encoding '%s'", requested_encoding);
            } else {
                error_msg (
                    "iconv conversion to '%s' failed: %s",
                    requested_encoding,
                    strerror(errno)
                );
            }
            return;
        }
    }

    bool b = has_flag(a, 'b');
    bool B = has_flag(a, 'B');
    if (unlikely(b && B)) {
        error_msg("flags -b and -B can't be used together");
        return;
    } else if (b) {
        bom = true;
    } else if (B) {
        bom = false;
    }

    char *absolute = buffer->abs_filename;
    bool force = has_flag(a, 'f');
    bool new_locked = false;
    if (a->nr_args > 0) {
        if (args[0][0] == '\0') {
            error_msg("Empty filename not allowed");
            goto error;
        }
        char *tmp = path_absolute(args[0]);
        if (!tmp) {
            error_msg("Failed to make absolute path: %s", strerror(errno));
            goto error;
        }
        if (absolute && streq(tmp, absolute)) {
            free(tmp);
        } else {
            absolute = tmp;
        }
    } else {
        if (!absolute) {
            bool prompt = has_flag(a, 'p');
            if (prompt) {
                set_input_mode(e, INPUT_COMMAND);
                cmdline_set_text(&e->cmdline, "save ");
                // This branch is not really an error, but we still return via
                // the "error" label because we need to clean up memory and
                // that's all it's used for currently.
                goto error;
            } else {
                error_msg("No filename");
                goto error;
            }
        }
        if (buffer->readonly && !force) {
            error_msg("Use -f to force saving read-only file");
            goto error;
        }
    }

    mode_t old_mode = buffer->file.mode;
    struct stat st;
    if (stat(absolute, &st)) {
        if (errno != ENOENT) {
            error_msg("stat failed for %s: %s", absolute, strerror(errno));
            goto error;
        }
        if (e->options.lock_files) {
            if (absolute == buffer->abs_filename) {
                if (!buffer->locked) {
                    if (!lock_file(absolute)) {
                        if (!force) {
                            error_msg("Can't lock file %s", absolute);
                            goto error;
                        }
                    } else {
                        buffer->locked = true;
                    }
                }
            } else {
                if (!lock_file(absolute)) {
                    if (!force) {
                        error_msg("Can't lock file %s", absolute);
                        goto error;
                    }
                } else {
                    new_locked = true;
                }
            }
        }
    } else {
        if (
            absolute == buffer->abs_filename
            && !force
            && stat_changed(buffer, &st)
        ) {
            error_msg (
                "File has been modified by another process."
                " Use 'save -f' to force overwrite."
            );
            goto error;
        }
        if (S_ISDIR(st.st_mode)) {
            error_msg("Will not overwrite directory %s", absolute);
            goto error;
        }
        if (e->options.lock_files) {
            if (absolute == buffer->abs_filename) {
                if (!buffer->locked) {
                    if (!lock_file(absolute)) {
                        if (!force) {
                            error_msg("Can't lock file %s", absolute);
                            goto error;
                        }
                    } else {
                        buffer->locked = true;
                    }
                }
            } else {
                if (!lock_file(absolute)) {
                    if (!force) {
                        error_msg("Can't lock file %s", absolute);
                        goto error;
                    }
                } else {
                    new_locked = true;
                }
            }
        }
        if (absolute != buffer->abs_filename && !force) {
            error_msg("Use -f to overwrite %s", absolute);
            goto error;
        }

        // Allow chmod 755 etc.
        buffer->file.mode = st.st_mode;
    }

    if (!save_buffer(buffer, absolute, &encoding, crlf, bom)) {
        goto error;
    }

    buffer->saved_change = buffer->cur_change;
    buffer->readonly = false;
    buffer->temporary = false;
    buffer->crlf_newlines = crlf;
    buffer->bom = bom;
    if (requested_encoding) {
        buffer->encoding = encoding;
    }

    if (absolute != buffer->abs_filename) {
        if (buffer->locked) {
            // Filename changes, release old file lock
            unlock_file(buffer->abs_filename);
        }
        buffer->locked = new_locked;

        free(buffer->abs_filename);
        buffer->abs_filename = absolute;
        update_short_filename(buffer);

        // Filename change is not detected (only buffer_modified() change)
        mark_buffer_tabbars_changed(buffer);
    }
    if (!old_mode && streq(buffer->options.filetype, "none")) {
        // New file and most likely user has not changed the filetype
        if (buffer_detect_filetype(buffer)) {
            set_file_options(&e->file_options, buffer);
            set_editorconfig_options(buffer);
            buffer_update_syntax(buffer);
        }
    }
    return;
error:
    if (new_locked) {
        unlock_file(absolute);
    }
    if (absolute != buffer->abs_filename) {
        free(absolute);
    }
}

static void cmd_scroll_down(const CommandArgs *a)
{
    EditorState *e = a->userdata;
    View *view = e->view;
    view->vy++;
    if (view->cy < view->vy) {
        move_down(view, 1);
    }
}

static void cmd_scroll_pgdown(const CommandArgs *a)
{
    EditorState *e = a->userdata;
    Window *window = e->window;
    View *view = e->view;
    long max = view->buffer->nl - window->edit_h + 1;
    if (view->vy < max && max > 0) {
        long count = window->edit_h - 1;
        if (view->vy + count > max) {
            count = max - view->vy;
        }
        view->vy += count;
        move_down(view, count);
    } else if (view->cy < view->buffer->nl) {
        move_down(view, view->buffer->nl - view->cy);
    }
}

static void cmd_scroll_pgup(const CommandArgs *a)
{
    EditorState *e = a->userdata;
    Window *window = e->window;
    View *view = e->view;
    if (view->vy > 0) {
        long count = window->edit_h - 1;
        if (count > view->vy) {
            count = view->vy;
        }
        view->vy -= count;
        move_up(view, count);
    } else if (view->cy > 0) {
        move_up(view, view->cy);
    }
}

static void cmd_scroll_up(const CommandArgs *a)
{
    EditorState *e = a->userdata;
    Window *window = e->window;
    View *view = e->view;
    if (view->vy) {
        view->vy--;
    }
    if (view->vy + window->edit_h <= view->cy) {
        move_up(view, 1);
    }
}

static uint_least64_t get_flagset_npw(void)
{
    // flagset mask for "-npw" flags
    return UINT64_C(517) << 40;
}

UNITTEST {
    uint_least64_t ref = 0;
    ref |= cmdargs_flagset_value('n');
    ref |= cmdargs_flagset_value('p');
    ref |= cmdargs_flagset_value('w');
    BUG_ON(get_flagset_npw() != ref);
}

static void cmd_search(const CommandArgs *a)
{
    const char *pattern = a->args[0];
    unsigned int npw_count = u64_popcount(a->flag_set & get_flagset_npw());
    if (npw_count >= 2) {
        error_msg("flags -n, -p and -w are mutually exclusive");
        return;
    }
    if (npw_count == 1 && pattern) {
        error_msg("flags [-npw] and pattern argument are mutually exclusive");
        return;
    }

    EditorState *e = a->userdata;
    View *view = e->view;
    char pattbuf[4096];
    bool use_word_under_cursor = has_flag(a, 'w');

    if (use_word_under_cursor) {
        StringView word = view_get_word_under_cursor(view);
        if (word.length == 0) {
            // Error message would not be very useful here
            return;
        }
        const RegexpWordBoundaryTokens *rwbt = &e->regexp_word_tokens;
        const size_t bmax = sizeof(rwbt->start);
        static_assert_compatible_types(rwbt->start, char[8]);
        if (unlikely(word.length >= sizeof(pattbuf) - (bmax * 2))) {
            error_msg("word under cursor too long");
            return;
        }
        char *ptr = stpncpy(pattbuf, rwbt->start, bmax);
        memcpy(ptr, word.data, word.length);
        memcpy(ptr + word.length, rwbt->end, bmax);
        pattern = pattbuf;
    }

    do_selection(view, SELECT_NONE);

    if (pattern) {
        e->search.direction = has_flag(a, 'r') ? SEARCH_BWD : SEARCH_FWD;
        search_set_regexp(&e->search, pattern);
        if (use_word_under_cursor) {
            search_next_word(e);
        } else {
            search_next(e);
        }
        if (!has_flag(a, 'H')) {
            history_add(&e->search_history, pattern);
        }
    } else if (has_flag(a, 'n')) {
        search_next(e);
    } else if (has_flag(a, 'p')) {
        search_prev(e);
    } else {
        set_input_mode(e, INPUT_SEARCH);
        e->search.direction = has_flag(a, 'r') ? SEARCH_BWD : SEARCH_FWD;
    }
}

static void cmd_select(const CommandArgs *a)
{
    EditorState *e = a->userdata;
    View *view = e->view;
    SelectionType sel = has_flag(a, 'l') ? SELECT_LINES : SELECT_CHARS;
    bool block = has_flag(a, 'b');
    bool keep = has_flag(a, 'k');
    view->next_movement_cancels_selection = false;

    if (block) {
        select_block(view);
        return;
    }

    if (view->selection) {
        if (!keep && view->selection == sel) {
            unselect(view);
            return;
        }
        view->selection = sel;
        mark_all_lines_changed(view->buffer);
        return;
    }

    view->sel_so = block_iter_get_offset(&view->cursor);
    view->sel_eo = UINT_MAX;
    view->selection = sel;

    // Need to mark current line changed because cursor might
    // move up or down before screen is updated
    view_update_cursor_y(view);
    buffer_mark_lines_changed(view->buffer, view->cy, view->cy);
}

static void cmd_set(const CommandArgs *a)
{
    EditorState *e = a->userdata;
    bool global = has_flag(a, 'g');
    bool local = has_flag(a, 'l');
    if (!e->buffer) {
        if (unlikely(local)) {
            error_msg("Flag -l makes no sense in config file");
            return;
        }
        global = true;
    }

    char **args = a->args;
    size_t count = a->nr_args;
    if (count == 1) {
        set_bool_option(args[0], local, global);
        return;
    } else if (count & 1) {
        error_msg("One or even number of arguments expected");
        return;
    }

    for (size_t i = 0; i < count; i += 2) {
        set_option(args[i], args[i + 1], local, global);
    }
}

static void cmd_setenv(const CommandArgs *a)
{
    const char *name = a->args[0];
    if (unlikely(streq(name, "DTE_VERSION"))) {
        error_msg("$DTE_VERSION cannot be changed");
        return;
    }

    const size_t nr_args = a->nr_args;
    int res;
    if (nr_args == 2) {
        res = setenv(name, a->args[1], true);
    } else {
        BUG_ON(nr_args != 1);
        res = unsetenv(name);
    }

    if (unlikely(res != 0)) {
        if (errno == EINVAL) {
            error_msg("Invalid environment variable name '%s'", name);
        } else {
            perror_msg(nr_args == 2 ? "setenv" : "unsetenv");
        }
    }
}

static void cmd_shift(const CommandArgs *a)
{
    const char *arg = a->args[0];
    int count;
    if (!str_to_int(arg, &count)) {
        error_msg("Invalid number: %s", arg);
        return;
    }
    if (count == 0) {
        error_msg("Count must be non-zero");
        return;
    }
    EditorState *e = a->userdata;
    shift_lines(e->view, count);
}

static void cmd_show(const CommandArgs *a)
{
    EditorState *e = a->userdata;
    bool write_to_cmdline = has_flag(a, 'c');
    if (write_to_cmdline && a->nr_args < 2) {
        error_msg("\"show -c\" requires 2 arguments");
        return;
    }
    show(e, a->args[0], a->args[1], write_to_cmdline);
}

static void cmd_suspend(const CommandArgs *a)
{
    EditorState *e = a->userdata;
    if (e->session_leader) {
        error_msg("Session leader can't suspend");
        return;
    }
    if (
        !e->child_controls_terminal
        && e->status != EDITOR_INITIALIZING
    ) {
        ui_end(e);
    }
    int r = kill(0, SIGSTOP);
    if (unlikely(r != 0)) {
        perror_msg("kill");
        term_raw();
        ui_start(e);
    }
}

static void cmd_tag(const CommandArgs *a)
{
    EditorState *e = a->userdata;
    if (has_flag(a, 'r')) {
        bookmark_pop(&e->bookmarks);
        return;
    }

    const char *name = a->args[0];
    char *word = NULL;
    if (!name) {
        StringView w = view_get_word_under_cursor(e->view);
        if (w.length == 0) {
            return;
        }
        word = xstrcut(w.data, w.length);
        name = word;
    }

    tag_lookup(name, e->buffer->abs_filename, &e->messages);
    activate_current_message_save(&e->messages, &e->bookmarks, e->view);
    free(word);
}

static void cmd_title(const CommandArgs *a)
{
    EditorState *e = a->userdata;
    Buffer *buffer = e->buffer;
    if (buffer->abs_filename) {
        error_msg("saved buffers can't be retitled");
        return;
    }
    set_display_filename(buffer, xstrdup(a->args[0]));
    mark_buffer_tabbars_changed(buffer);
}

static void cmd_toggle(const CommandArgs *a)
{
    bool global = has_flag(a, 'g');
    bool verbose = has_flag(a, 'v');
    const char *option_name = a->args[0];
    size_t nr_values = a->nr_args - 1;
    if (nr_values) {
        char **values = a->args + 1;
        toggle_option_values(option_name, global, verbose, values, nr_values);
    } else {
        toggle_option(option_name, global, verbose);
    }
}

static void cmd_undo(const CommandArgs *a)
{
    EditorState *e = a->userdata;
    if (undo(e->view)) {
        unselect(e->view);
    }
}

static void cmd_unselect(const CommandArgs *a)
{
    EditorState *e = a->userdata;
    unselect(e->view);
}

static void cmd_up(const CommandArgs *a)
{
    EditorState *e = a->userdata;
    handle_select_chars_or_lines_flags(a);
    move_up(e->view, 1);
}

static void cmd_view(const CommandArgs *a)
{
    EditorState *e = a->userdata;
    Window *window = e->window;
    BUG_ON(window->views.count == 0);
    const char *arg = a->args[0];
    size_t idx;
    if (streq(arg, "last")) {
        idx = window->views.count - 1;
    } else {
        if (!str_to_size(arg, &idx) || idx == 0) {
            error_msg("Invalid view index: %s", arg);
            return;
        }
        idx--;
        if (idx > window->views.count - 1) {
            idx = window->views.count - 1;
        }
    }
    set_view(e, window->views.ptrs[idx]);
}

static void cmd_wclose(const CommandArgs *a)
{
    EditorState *e = a->userdata;
    bool force = has_flag(a, 'f');
    bool prompt = has_flag(a, 'p');
    View *v = window_find_unclosable_view(e->window);
    if (v && !force) {
        set_view(e, v);
        if (prompt) {
            static const char str[] = "Close window without saving? [y/N]";
            if (dialog_prompt(e, str, "ny") != 'y') {
                return;
            }
        } else {
            error_msg (
                "Save modified files or run 'wclose -f' to close "
                "window without saving."
            );
            return;
        }
    }
    window_close_current(e);
}

static void cmd_wflip(const CommandArgs *a)
{
    EditorState *e = a->userdata;
    Frame *f = e->window->frame;
    if (!f->parent) {
        return;
    }
    f->parent->vertical ^= 1;
    mark_everything_changed(e);
}

static void cmd_wnext(const CommandArgs *a)
{
    EditorState *e = a->userdata;
    e->window = next_window(e, e->window);
    set_view(e, e->window->view);
    mark_everything_changed(e);
    debug_frame(e->root_frame);
}

static void cmd_word_bwd(const CommandArgs *a)
{
    EditorState *e = a->userdata;
    handle_select_chars_flag(a);
    bool skip_non_word = has_flag(a, 's');
    word_bwd(&e->view->cursor, skip_non_word);
    view_reset_preferred_x(e->view);
}

static void cmd_word_fwd(const CommandArgs *a)
{
    EditorState *e = a->userdata;
    handle_select_chars_flag(a);
    bool skip_non_word = has_flag(a, 's');
    word_fwd(&e->view->cursor, skip_non_word);
    view_reset_preferred_x(e->view);
}

static void cmd_wprev(const CommandArgs *a)
{
    EditorState *e = a->userdata;
    e->window = prev_window(e, e->window);
    set_view(e, e->window->view);
    mark_everything_changed(e);
    debug_frame(e->root_frame);
}

static void cmd_wrap_paragraph(const CommandArgs *a)
{
    EditorState *e = a->userdata;
    const char *arg = a->args[0];
    unsigned int width = e->buffer->options.text_width;
    if (arg) {
        if (!str_to_uint(arg, &width) || width < 1 || width > TEXT_WIDTH_MAX) {
            error_msg("invalid paragraph width: %s", arg);
            return;
        }
    }
    format_paragraph(e->view, width);
}

static void cmd_wresize(const CommandArgs *a)
{
    EditorState *e = a->userdata;
    Window *window = e->window;
    if (!window->frame->parent) {
        // Only window
        return;
    }

    ResizeDirection dir = RESIZE_DIRECTION_AUTO;
    switch (last_flag(a)) {
    case 'h':
        dir = RESIZE_DIRECTION_HORIZONTAL;
        break;
    case 'v':
        dir = RESIZE_DIRECTION_VERTICAL;
        break;
    }

    const char *arg = a->args[0];
    if (arg) {
        int n;
        if (!str_to_int(arg, &n)) {
            error_msg("Invalid resize value: %s", arg);
            return;
        }
        if (arg[0] == '+' || arg[0] == '-') {
            add_to_frame_size(window->frame, dir, n);
        } else {
            resize_frame(window->frame, dir, n);
        }
    } else {
        equalize_frame_sizes(window->frame->parent);
    }
    mark_everything_changed(e);
    debug_frame(e->root_frame);
}

static void cmd_wsplit(const CommandArgs *a)
{
    bool before = has_flag(a, 'b');
    bool use_glob = has_flag(a, 'g') && a->nr_args > 0;
    bool vertical = has_flag(a, 'h');
    bool root = has_flag(a, 'r');
    bool temporary = has_flag(a, 't');
    bool empty = temporary || has_flag(a, 'n');

    if (unlikely(empty && a->nr_args > 0)) {
        error_msg("flags -n and -t can't be used with filename arguments");
        return;
    }

    char **paths = a->args;
    glob_t globbuf;
    if (use_glob) {
        if (!xglob(a->args, &globbuf)) {
            return;
        }
        paths = globbuf.gl_pathv;
    }

    EditorState *e = a->userdata;
    Frame *f;
    if (root) {
        f = split_root(&e->root_frame, vertical, before);
    } else {
        f = split_frame(e->window, vertical, before);
    }

    View *save = e->view;
    e->window = f->window;
    e->view = NULL;
    e->buffer = NULL;
    mark_everything_changed(e);

    if (empty) {
        window_open_new_file(e, e->window);
        e->buffer->temporary = temporary;
    } else if (paths[0]) {
        window_open_files(e, e->window, paths, NULL);
    } else {
        View *new = window_add_buffer(e->window, save->buffer);
        new->cursor = save->cursor;
        set_view(e, new);
    }

    if (use_glob) {
        globfree(&globbuf);
    }

    if (e->window->views.count == 0) {
        // Open failed, remove new window
        remove_frame(e->window->frame);
        e->view = save;
        e->buffer = save->buffer;
        e->window = save->window;
    }

    debug_frame(e->root_frame);
}

static void cmd_wswap(const CommandArgs *a)
{
    EditorState *e = a->userdata;
    Frame *frame = e->window->frame;
    Frame *parent = frame->parent;
    if (!parent) {
        return;
    }

    size_t count = parent->frames.count;
    size_t current = ptr_array_idx(&parent->frames, frame);
    BUG_ON(current >= count);
    size_t next = size_increment_wrapped(current, count);

    void **ptrs = parent->frames.ptrs;
    Frame *tmp = ptrs[current];
    ptrs[current] = ptrs[next];
    ptrs[next] = tmp;
    mark_everything_changed(e);
}

static const Command cmds[] = {
    {"alias", "-", true, 1, 2, cmd_alias},
    {"bind", "-cns", true, 1, 2, cmd_bind},
    {"blkdown", "cl", false, 0, 0, cmd_blkdown},
    {"blkup", "cl", false, 0, 0, cmd_blkup},
    {"bof", "", false, 0, 0, cmd_bof},
    {"bol", "cst", false, 0, 0, cmd_bol},
    {"bolsf", "", false, 0, 0, cmd_bolsf},
    {"bookmark", "r", false, 0, 0, cmd_bookmark},
    {"case", "lu", false, 0, 0, cmd_case},
    {"cd", "", true, 1, 1, cmd_cd},
    {"center-view", "", false, 0, 0, cmd_center_view},
    {"clear", "", false, 0, 0, cmd_clear},
    {"close", "fpqw", false, 0, 0, cmd_close},
    {"command", "-", false, 0, 1, cmd_command},
    {"compile", "-1ps", false, 2, -1, cmd_compile},
    {"copy", "bikp", false, 0, 0, cmd_copy},
    {"cursor", "", true, 0, 3, cmd_cursor},
    {"cut", "", false, 0, 0, cmd_cut},
    {"delete", "", false, 0, 0, cmd_delete},
    {"delete-eol", "n", false, 0, 0, cmd_delete_eol},
    {"delete-line", "", false, 0, 0, cmd_delete_line},
    {"delete-word", "s", false, 0, 0, cmd_delete_word},
    {"down", "cl", false, 0, 0, cmd_down},
    {"eof", "", false, 0, 0, cmd_eof},
    {"eol", "c", false, 0, 0, cmd_eol},
    {"eolsf", "", false, 0, 0, cmd_eolsf},
    {"erase", "", false, 0, 0, cmd_erase},
    {"erase-bol", "", false, 0, 0, cmd_erase_bol},
    {"erase-word", "s", false, 0, 0, cmd_erase_word},
    {"errorfmt", "i", true, 1, 2 + ERRORFMT_CAPTURE_MAX, cmd_errorfmt},
    {"exec", "-e=i=o=lmnpst", false, 1, -1, cmd_exec},
    {"ft", "-bcfi", true, 2, -1, cmd_ft},
    {"hi", "-c", true, 0, -1, cmd_hi},
    {"include", "bq", true, 1, 1, cmd_include},
    {"insert", "km", false, 1, 1, cmd_insert},
    {"join", "", false, 0, 0, cmd_join},
    {"left", "c", false, 0, 0, cmd_left},
    {"line", "", false, 1, 1, cmd_line},
    {"load-syntax", "", true, 1, 1, cmd_load_syntax},
    {"macro", "", false, 1, 1, cmd_macro},
    {"match-bracket", "", false, 0, 0, cmd_match_bracket},
    {"move-tab", "", false, 1, 1, cmd_move_tab},
    {"msg", "np", false, 0, 1, cmd_msg},
    {"new-line", "a", false, 0, 0, cmd_new_line},
    {"next", "", false, 0, 0, cmd_next},
    {"open", "e=gt", false, 0, -1, cmd_open},
    {"option", "-r", true, 3, -1, cmd_option},
    {"paste", "c", false, 0, 0, cmd_paste},
    {"pgdown", "cl", false, 0, 0, cmd_pgdown},
    {"pgup", "cl", false, 0, 0, cmd_pgup},
    {"prev", "", false, 0, 0, cmd_prev},
    {"quit", "fp", false, 0, 1, cmd_quit},
    {"redo", "", false, 0, 1, cmd_redo},
    {"refresh", "", false, 0, 0, cmd_refresh},
    {"repeat", "-", false, 2, -1, cmd_repeat},
    {"replace", "bcgi", false, 2, 2, cmd_replace},
    {"right", "c", false, 0, 0, cmd_right},
    {"save", "Bbde=fpu", false, 0, 1, cmd_save},
    {"scroll-down", "", false, 0, 0, cmd_scroll_down},
    {"scroll-pgdown", "", false, 0, 0, cmd_scroll_pgdown},
    {"scroll-pgup", "", false, 0, 0, cmd_scroll_pgup},
    {"scroll-up", "", false, 0, 0, cmd_scroll_up},
    {"search", "Hnprw", false, 0, 1, cmd_search},
    {"select", "bkl", false, 0, 0, cmd_select},
    {"set", "gl", true, 1, -1, cmd_set},
    {"setenv", "", true, 1, 2, cmd_setenv},
    {"shift", "", false, 1, 1, cmd_shift},
    {"show", "c", false, 1, 2, cmd_show},
    {"suspend", "", false, 0, 0, cmd_suspend},
    {"tag", "r", false, 0, 1, cmd_tag},
    {"title", "", false, 1, 1, cmd_title},
    {"toggle", "gv", false, 1, -1, cmd_toggle},
    {"undo", "", false, 0, 0, cmd_undo},
    {"unselect", "", false, 0, 0, cmd_unselect},
    {"up", "cl", false, 0, 0, cmd_up},
    {"view", "", false, 1, 1, cmd_view},
    {"wclose", "fp", false, 0, 0, cmd_wclose},
    {"wflip", "", false, 0, 0, cmd_wflip},
    {"wnext", "", false, 0, 0, cmd_wnext},
    {"word-bwd", "cs", false, 0, 0, cmd_word_bwd},
    {"word-fwd", "cs", false, 0, 0, cmd_word_fwd},
    {"wprev", "", false, 0, 0, cmd_wprev},
    {"wrap-paragraph", "", false, 0, 1, cmd_wrap_paragraph},
    {"wresize", "hv", false, 0, 1, cmd_wresize},
    {"wsplit", "bghnrt", false, 0, -1, cmd_wsplit},
    {"wswap", "", false, 0, 0, cmd_wswap},
};

static bool allow_macro_recording(const Command *cmd, char **args, void *userdata)
{
    if (cmd->cmd == cmd_macro || cmd->cmd == cmd_command) {
        return false;
    }

    if (cmd->cmd == cmd_search) {
        char **args_copy = copy_string_array(args, string_array_length(args));
        CommandArgs a = cmdargs_new(args_copy, userdata);
        bool ret = true;
        if (do_parse_args(cmd, &a) == ARGERR_NONE) {
            if (a.nr_args == 0 && !(a.flag_set & get_flagset_npw())) {
                // If command is "search" with no pattern argument and without
                // flags -n, -p or -w, the command would put the editor into
                // search mode, which shouldn't be recorded.
                ret = false;
            }
        }
        free_string_array(args_copy);
        return ret;
    }

    if (cmd->cmd == cmd_exec) {
        // TODO: don't record -o with open/tag/eval/msg
    }

    return true;
}

const Command *find_normal_command(const char *name)
{
    return BSEARCH(name, cmds, command_cmp);
}

CommandSet normal_commands = {
    .lookup = find_normal_command,
    .allow_recording = allow_macro_recording,
    .expand_variable = expand_normal_var,
    .aliases = HASHMAP_INIT,
    .userdata = &editor,
};

void collect_normal_commands(PointerArray *a, const char *prefix)
{
    COLLECT_STRING_FIELDS(cmds, name, a, prefix);
}

UNITTEST {
    CHECK_BSEARCH_ARRAY(cmds, name, strcmp);

    for (size_t i = 0, n = ARRAYLEN(cmds); i < n; i++) {
        // Check that flags arrays is null-terminated within bounds
        const char *const flags = cmds[i].flags;
        BUG_ON(flags[ARRAYLEN(cmds[0].flags) - 1] != '\0');

        // Count number of real flags (i.e. not including '-' or '=')
        size_t nr_real_flags = 0;
        for (size_t j = (flags[0] == '-' ? 1 : 0); flags[j]; j++) {
            unsigned char flag = flags[j];
            if (ascii_isalnum(flag)) {
                nr_real_flags++;
            } else if (flag != '=') {
                BUG("invalid command flag: 0x%02hhX", flag);
            }
        }

        // Check that max. number of real flags fits in CommandArgs::flags
        // array (and also leaves 1 byte for null-terminator)
        CommandArgs a;
        BUG_ON(nr_real_flags >= ARRAYLEN(a.flags));
    }
}
