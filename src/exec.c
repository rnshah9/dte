#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "exec.h"
#include "block-iter.h"
#include "command/macro.h"
#include "commands.h"
#include "config.h"
#include "ctags.h"
#include "error.h"
#include "misc.h"
#include "move.h"
#include "msg.h"
#include "selection.h"
#include "tag.h"
#include "util/debug.h"
#include "util/ptr-array.h"
#include "util/str-util.h"
#include "util/string-view.h"
#include "util/string.h"
#include "util/strtonum.h"
#include "util/xsnprintf.h"
#include "view.h"
#include "window.h"

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

    Tag tag;
    size_t pos = 0;
    const char *line = buf_next_line(str->buffer, &pos, str->len);
    if (pos == 0) {
        return;
    }

    if (!parse_ctags_line(&tag, line, pos - 1)) {
        // Treat line as simple tag name
        tag_lookup(line, e->buffer->abs_filename, &e->messages);
        goto activate;
    }

    char buf[8192];
    const char *cwd = getcwd(buf, sizeof buf);
    if (unlikely(!cwd)) {
        error_msg("getcwd() failed: %s", strerror(errno));
        return;
    }

    StringView dir = strview_from_cstring(cwd);
    clear_messages(&e->messages);
    add_message_for_tag(&e->messages, &tag, &dir);

activate:
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

static SpawnAction spawn_action_from_exec_action(ExecAction action)
{
    if (action == EXEC_NULL) {
        return SPAWN_NULL;
    } else if (action == EXEC_TTY) {
        return SPAWN_TTY;
    } else {
        return SPAWN_PIPE;
    }
}

ssize_t handle_exec (
    EditorState *e,
    const char **argv,
    ExecAction actions[3],
    SpawnFlags spawn_flags,
    bool strip_trailing_newline
) {
    View *view = e->view;
    BlockIter saved_cursor = view->cursor;
    bool output_to_buffer = (actions[STDOUT_FILENO] == EXEC_BUFFER);
    char *alloc = NULL;

    SpawnContext ctx = {
        .argv = argv,
        .outputs = {STRING_INIT, STRING_INIT},
        .flags = spawn_flags,
        .env = output_to_buffer ? lines_and_columns_env(e->window) : NULL,
        .actions = {
            spawn_action_from_exec_action(actions[0]),
            spawn_action_from_exec_action(actions[1]),
            spawn_action_from_exec_action(actions[2]),
        },
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
    case EXEC_WORD:
        if (view->selection) {
            ctx.input.length = prepare_selection(view);
        } else {
            size_t offset;
            StringView word = view_do_get_word_under_cursor(e->view, &offset);
            if (word.length == 0) {
                break;
            }
            // TODO: optimize this, so that the BlockIter moves by just the
            // minimal word offset instead of iterating to a line offset
            ctx.input.length = word.length;
            move_bol(view);
            view->cursor.offset += offset;
            BUG_ON(view->cursor.offset >= view->cursor.blk->size);
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
    // These can't be used as input actions and should be prevented by
    // the validity checks in cmd_exec():
    case EXEC_OPEN:
    case EXEC_TAG:
    case EXEC_EVAL:
    case EXEC_ERRMSG:
    default:
        BUG("unhandled action");
        return -1;
    }

    int err = spawn(&ctx);
    free(alloc);
    if (err != 0) {
        show_spawn_error_msg(&ctx.outputs[1], err);
        string_free(&ctx.outputs[0]);
        string_free(&ctx.outputs[1]);
        view->cursor = saved_cursor;
        return -1;
    }

    string_free(&ctx.outputs[1]);
    String *output = &ctx.outputs[0];
    if (
        strip_trailing_newline
        && output_to_buffer
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
    // These can't be used as output actions
    case EXEC_LINE:
    case EXEC_ERRMSG:
    case EXEC_WORD:
    default:
        BUG("unhandled action");
        return -1;
    }

    size_t output_len = output->len;
    string_free(output);
    return output_len;
}