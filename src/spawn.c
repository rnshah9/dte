#include <errno.h>
#include <poll.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "spawn.h"
#include "editor.h"
#include "error.h"
#include "msg.h"
#include "regexp.h"
#include "terminal/mode.h"
#include "util/exec.h"
#include "util/macros.h"
#include "util/ptr-array.h"
#include "util/str-util.h"
#include "util/string.h"
#include "util/strtonum.h"
#include "util/xmalloc.h"
#include "util/xreadwrite.h"

static void handle_error_msg(const Compiler *c, MessageArray *msgs, char *str)
{
    if (str[0] == '\0' || str[0] == '\n') {
        return;
    }

    size_t str_len = str_replace_byte(str, '\t', ' ');
    if (str[str_len - 1] == '\n') {
        str[--str_len] = '\0';
    }

    for (size_t i = 0, n = c->error_formats.count; i < n; i++) {
        const ErrorFormat *p = c->error_formats.ptrs[i];
        regmatch_t m[ERRORFMT_CAPTURE_MAX];
        if (!regexp_exec(&p->re, str, str_len, ARRAYLEN(m), m, 0)) {
            continue;
        }
        if (p->ignore) {
            return;
        }

        int8_t mi = p->capture_index[ERRFMT_MESSAGE];
        if (m[mi].rm_so < 0) {
            mi = 0;
        }

        Message *msg = new_message(str + m[mi].rm_so, m[mi].rm_eo - m[mi].rm_so);
        msg->loc = xnew0(FileLocation, 1);

        int8_t fi = p->capture_index[ERRFMT_FILE];
        if (fi >= 0 && m[fi].rm_so >= 0) {
            msg->loc->filename = xstrslice(str, m[fi].rm_so, m[fi].rm_eo);

            unsigned long *const ptrs[] = {
                [ERRFMT_LINE] = &msg->loc->line,
                [ERRFMT_COLUMN] = &msg->loc->column,
            };

            static_assert(ARRAYLEN(ptrs) == 3);
            for (size_t j = ERRFMT_LINE; j < ARRAYLEN(ptrs); j++) {
                int8_t ci = p->capture_index[j];
                if (ci >= 0 && m[ci].rm_so >= 0) {
                    size_t len = m[ci].rm_eo - m[ci].rm_so;
                    unsigned long val;
                    if (len == buf_parse_ulong(str + m[ci].rm_so, len, &val)) {
                        *ptrs[j] = val;
                    }
                }
            }
        }

        add_message(msgs, msg);
        return;
    }

    add_message(msgs, new_message(str, str_len));
}

static void read_errors(const Compiler *c, MessageArray *msgs, int fd, bool quiet)
{
    FILE *f = fdopen(fd, "r");
    if (unlikely(!f)) {
        return;
    }
    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        if (!quiet) {
            fputs(line, stderr);
        }
        handle_error_msg(c, msgs, line);
    }
    fclose(f);
}

static void filter(int rfd, int wfd, SpawnContext *ctx)
{
    BUG_ON(rfd < 0 && wfd < 0);
    BUG_ON(rfd >= 0 && rfd <= 2);
    BUG_ON(wfd >= 0 && wfd <= 2);

    if (!ctx->input.length) {
        xclose(wfd);
        wfd = -1;
    }

    struct pollfd fds[] = {
        {.fd = rfd, .events = POLLIN},
        {.fd = wfd, .events = POLLOUT}
    };

    size_t wlen = 0;
    while (1) {
        if (unlikely(poll(fds, ARRAYLEN(fds), -1) < 0)) {
            if (errno == EINTR) {
                continue;
            }
            perror_msg("poll");
            break;
        }

        if (fds[0].revents & POLLIN) {
            size_t max_read = 8192;
            string_reserve_space(&ctx->output, max_read);
            char *buf = ctx->output.buffer + ctx->output.len;
            ssize_t rc = xread(fds[0].fd, buf, max_read);
            if (unlikely(rc < 0)) {
                perror_msg("read");
                break;
            }
            if (unlikely(rc == 0)) {
                if (wlen < ctx->input.length) {
                    error_msg("Command did not read all data");
                }
                break;
            }
            ctx->output.len += rc;
        }

        if (fds[1].revents & POLLOUT) {
            ssize_t rc = xwrite(fds[1].fd, ctx->input.data + wlen, ctx->input.length - wlen);
            if (unlikely(rc < 0)) {
                perror_msg("write");
                break;
            }
            wlen += (size_t) rc;
            if (wlen == ctx->input.length) {
                if (xclose(fds[1].fd)) {
                    perror_msg("close");
                    break;
                }
                fds[1].fd = -1;
            }
        }

        size_t active_fds = ARRAYLEN(fds);
        for (size_t i = 0; i < ARRAYLEN(fds); i++) {
            if (fds[i].fd < 0 || fds[i].revents & POLLNVAL) {
                fds[i].fd = -1;
                active_fds--;
                continue;
            }
            if (fds[i].revents & (POLLHUP | POLLERR)) {
                if (xclose(fds[i].fd)) {
                    perror_msg("close");
                }
                fds[i].fd = -1;
                active_fds--;
            }
        }
        if (active_fds == 0) {
            break;
        }
    }
}

static int open_dev_null(int flags)
{
    int fd = xopen("/dev/null", flags | O_CLOEXEC, 0);
    if (unlikely(fd < 0)) {
        error_msg("Error opening /dev/null: %s", strerror(errno));
    }
    return fd;
}

static int handle_child_error(pid_t pid)
{
    int ret = wait_child(pid);
    if (ret < 0) {
        perror_msg("waitpid");
    } else if (ret >= 256) {
        int sig = ret >> 8;
        const char *str = strsignal(sig);
        error_msg("Child received signal %d (%s)", sig, str ? str : "??");
    } else if (ret) {
        error_msg("Child returned %d", ret);
    }
    return ret;
}

static void yield_terminal(bool quiet)
{
    if (quiet) {
        term_raw_isig();
    } else {
        editor.child_controls_terminal = true;
        ui_end(&editor);
    }
}

static void resume_terminal(bool quiet, bool prompt)
{
    term_raw();
    if (!quiet && editor.child_controls_terminal) {
        if (prompt) {
            any_key(&editor);
        }
        ui_start(&editor);
        editor.child_controls_terminal = false;
    }
}

static void exec_error(const char *argv0)
{
    error_msg("Unable to exec '%s': %s", argv0, strerror(errno));
}

void spawn_compiler(char **args, SpawnFlags flags, const Compiler *c, MessageArray *msgs)
{
    int fd[3];
    fd[0] = open_dev_null(O_RDONLY);
    if (fd[0] < 0) {
        return;
    }

    int dev_null = open_dev_null(O_WRONLY);
    if (dev_null < 0) {
        xclose(fd[0]);
        return;
    }

    int p[2];
    if (!pipe_cloexec(p)) {
        perror_msg("pipe");
        xclose(dev_null);
        xclose(fd[0]);
        return;
    }

    bool read_stdout = !!(flags & SPAWN_READ_STDOUT);
    bool quiet = !!(flags & SPAWN_QUIET);
    bool prompt = !!(flags & SPAWN_PROMPT);
    if (read_stdout) {
        fd[1] = p[1];
        fd[2] = quiet ? dev_null : 2;
    } else {
        fd[1] = quiet ? dev_null : 1;
        fd[2] = p[1];
    }

    yield_terminal(quiet);
    pid_t pid = fork_exec(args, NULL, fd, quiet);
    if (pid < 0) {
        exec_error(args[0]);
        xclose(p[1]);
        prompt = false;
    } else {
        // Must close write end of the pipe before read_errors() or
        // the read end never gets EOF!
        xclose(p[1]);
        read_errors(c, msgs, p[0], quiet);
        handle_child_error(pid);
    }
    resume_terminal(quiet, prompt);

    xclose(p[0]);
    xclose(dev_null);
    xclose(fd[0]);
}

// Close fd only if valid (positive) and not stdin/stdout/stderr
static int safe_xclose(int fd)
{
    return (fd > STDERR_FILENO) ? xclose(fd) : 0;
}

bool spawn(SpawnContext *ctx, SpawnAction actions[3])
{
    int child_fds[3] = {-1, -1, -1};
    int parent_fds[3] = {-1, -1, -1};
    bool quiet = !!(ctx->flags & SPAWN_QUIET);

    // TODO: support stderr pipe
    BUG_ON(actions[STDERR_FILENO] == SPAWN_PIPE);

    for (size_t i = 0; i < ARRAYLEN(child_fds); i++) {
        switch (actions[i]) {
        case SPAWN_TTY:
            if (!quiet) {
                child_fds[i] = i;
                break;
            }
            // Fallthrough
        case SPAWN_NULL:
            child_fds[i] = open_dev_null(O_RDWR);
            if (child_fds[i] < 0) {
                goto error_close;
            }
            break;
        case SPAWN_PIPE: {
            int p[2];
            if (!pipe_cloexec(p)) {
                perror_msg("pipe");
                goto error_close;
            }
            BUG_ON(p[0] < 3);
            BUG_ON(p[1] < 3);
            child_fds[i] = i ? p[1] : p[0];
            parent_fds[i] = i ? p[0] : p[1];
            break;
        }
        default:
            BUG("unhandled action type");
            return false;
        }
    }

    yield_terminal(quiet);
    pid_t pid = fork_exec(ctx->argv, ctx->env, child_fds, quiet);
    if (pid < 0) {
        exec_error(ctx->argv[0]);
        goto error_resume;
    }

    safe_xclose(child_fds[0]);
    safe_xclose(child_fds[1]);
    safe_xclose(child_fds[1]);

    if (actions[0] == SPAWN_PIPE && actions[1] == SPAWN_PIPE) {
        filter(parent_fds[1], parent_fds[0], ctx);
    } else if (actions[0] == SPAWN_PIPE) {
        size_t input_len = ctx->input.length;
        if (input_len && xwrite_all(parent_fds[0], ctx->input.data, input_len) < 0) {
            perror_msg("write");
            goto error_resume;
        }
    } else if (actions[1] == SPAWN_PIPE) {
        while (1) {
            size_t max_read = 8192;
            string_reserve_space(&ctx->output, max_read);
            char *buf = ctx->output.buffer + ctx->output.len;
            ssize_t rc = xread_all(parent_fds[1], buf, max_read);
            if (unlikely(rc < 0)) {
                perror_msg("read");
                goto error_resume;
            }
            if (rc == 0) {
                break;
            }
            ctx->output.len += rc;
        }
    }

    safe_xclose(parent_fds[0]);
    safe_xclose(parent_fds[1]);
    safe_xclose(parent_fds[2]);

    int err = handle_child_error(pid);
    resume_terminal(quiet, !!(ctx->flags & SPAWN_PROMPT));
    if (err) {
        string_free(&ctx->output);
        return false;
    }
    return true;

error_resume:
    resume_terminal(quiet, false);
error_close:
    for (size_t i = 0; i < 3; i++) {
        safe_xclose(child_fds[i]);
        safe_xclose(parent_fds[i]);
    }
    return false;
}
