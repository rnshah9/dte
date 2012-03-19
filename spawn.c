#include "spawn.h"
#include "editor.h"
#include "buffer.h"
#include "regexp.h"
#include "error.h"
#include "gbuf.h"
#include "msg.h"
#include "term.h"
#include "fork.h"

static void handle_error_msg(struct compiler *c, char *str)
{
	int i, len;

	for (i = 0; str[i]; i++) {
		if (str[i] == '\n') {
			str[i] = 0;
			break;
		}
		if (str[i] == '\t')
			str[i] = ' ';
	}
	len = i;
	if (len == 0)
		return;

	for (i = 0; i < c->error_formats.count; i++) {
		const struct error_format *p = c->error_formats.ptrs[i];

		if (!regexp_match(p->pattern, str, len))
			continue;
		if (!p->ignore) {
			struct message *m = new_message(regexp_matches[p->msg_idx]);
			m->file = p->file_idx < 0 ? NULL : xstrdup(regexp_matches[p->file_idx]);
			m->u.location.line = p->line_idx < 0 ? 0 : atoi(regexp_matches[p->line_idx]);
			m->u.location.column = p->column_idx < 0 ? 0 : atoi(regexp_matches[p->column_idx]);
			add_message(m);
		}
		free_regexp_matches();
		return;
	}
	add_message(new_message(str));
}

static void read_errors(struct compiler *c, int fd, int quiet)
{
	FILE *f = fdopen(fd, "r");
	char line[4096];

	if (!f) {
		// should not happen
		return;
	}

	while (fgets(line, sizeof(line), f)) {
		if (!quiet)
			fputs(line, stderr);
		handle_error_msg(c, line);
	}
	fclose(f);
}

static void filter(int rfd, int wfd, struct filter_data *fdata)
{
	unsigned int wlen = 0;
	GBUF(buf);
	int rc;

	if (!fdata->in_len) {
		close(wfd);
		wfd = -1;
	}
	while (1) {
		fd_set rfds, wfds;
		fd_set *wfdsp = NULL;
		int fd_high = rfd;

		FD_ZERO(&rfds);
		FD_SET(rfd, &rfds);

		if (wfd >= 0) {
			FD_ZERO(&wfds);
			FD_SET(wfd, &wfds);
			wfdsp = &wfds;
		}
		if (wfd > fd_high)
			fd_high = wfd;

		rc = select(fd_high + 1, &rfds, wfdsp, NULL, NULL);
		if (rc < 0) {
			if (errno == EINTR)
				continue;
			error_msg("select: %s", strerror(errno));
			break;
		}

		if (FD_ISSET(rfd, &rfds)) {
			char data[8192];

			rc = read(rfd, data, sizeof(data));
			if (rc < 0) {
				error_msg("read: %s", strerror(errno));
				break;
			}
			if (!rc) {
				if (wlen < fdata->in_len)
					error_msg("Command did not read all data.");
				break;
			}
			gbuf_add_buf(&buf, data, rc);
		}
		if (wfdsp && FD_ISSET(wfd, &wfds)) {
			rc = write(wfd, fdata->in + wlen, fdata->in_len - wlen);
			if (rc < 0) {
				error_msg("write: %s", strerror(errno));
				break;
			}
			wlen += rc;
			if (wlen == fdata->in_len) {
				close(wfd);
				wfd = -1;
			}
		}
	}

	if (buf.len) {
		fdata->out_len = buf.len;
		fdata->out = gbuf_steal(&buf);
	} else {
		fdata->out_len = 0;
		fdata->out = NULL;
	}
}

static int open_dev_null(int flags)
{
	int fd = open("/dev/null", flags);
	if (fd < 0) {
		error_msg("Error opening /dev/null: %s", strerror(errno));
	} else {
		close_on_exec(fd);
	}
	return fd;
}

static int handle_child_error(int pid)
{
	int ret = wait_child(pid);

	if (ret < 0) {
		error_msg("waitpid: %s", strerror(errno));
	} else if (ret >= 256) {
		error_msg("Child received signal %d", ret >> 8);
	} else if (ret) {
		error_msg("Child returned %d", ret);
	}
	return ret;
}

int spawn_filter(char **argv, struct filter_data *data)
{
	int p0[2] = { -1, -1 };
	int p1[2] = { -1, -1 };
	int dev_null = -1;
	int fd[3], pid;

	data->out = NULL;
	data->out_len = 0;

	if (pipe_close_on_exec(p0) || pipe_close_on_exec(p1)) {
		error_msg("pipe: %s", strerror(errno));
		goto error;
	}
	dev_null = open_dev_null(O_WRONLY);
	if (dev_null < 0)
		goto error;

	fd[0] = p0[0];
	fd[1] = p1[1];
	fd[2] = dev_null;
	pid = fork_exec(argv, fd);
	if (pid < 0) {
		error_msg("Error: %s", strerror(errno));
		goto error;
	}

	close(dev_null);
	close(p0[0]);
	close(p1[1]);
	filter(p1[0], p0[1], data);
	close(p1[0]);
	close(p0[1]);

	if (handle_child_error(pid))
		return -1;
	return 0;
error:
	close(p0[0]);
	close(p0[1]);
	close(p1[0]);
	close(p1[1]);
	close(dev_null);
	return -1;
}

void spawn_compiler(char **args, unsigned int flags, struct compiler *c)
{
	int read_stdout = flags & SPAWN_READ_STDOUT;
	int prompt = flags & SPAWN_PROMPT;
	int quiet = flags & SPAWN_QUIET;
	int pid, dev_null, p[2], fd[3];

	dev_null = open_dev_null(O_WRONLY);
	if (dev_null < 0)
		return;
	if (pipe_close_on_exec(p)) {
		error_msg("pipe: %s", strerror(errno));
		close(dev_null);
		return;
	}

	fd[0] = dev_null;
	if (read_stdout) {
		fd[1] = p[1];
		fd[2] = quiet ? dev_null : 2;
	} else {
		fd[1] = quiet ? dev_null : 1;
		fd[2] = p[1];
	}

	if (!quiet) {
		child_controls_terminal = 1;
		ui_end();
	}

	pid = fork_exec(args, fd);
	if (pid < 0) {
		error_msg("Error: %s", strerror(errno));
		close(p[1]);
		prompt = 0;
	} else {
		// Must close write end of the pipe before read_errors() or
		// the read end never gets EOF!
		close(p[1]);
		read_errors(c, p[0], quiet);
		handle_child_error(pid);
	}
	if (!quiet) {
		term_raw();
		if (prompt)
			any_key();
		resize();
		child_controls_terminal = 0;
	}
	close(p[0]);
	close(dev_null);
}

void spawn(char **args, int fd[3], int prompt)
{
	int i, pid, quiet, redir_count = 0;
	int dev_null = -1;

	for (i = 0; i < 3; i++) {
		if (fd[i] >= 0)
			continue;

		if (dev_null < 0) {
			dev_null = open_dev_null(O_WRONLY);
			if (dev_null < 0)
				return;
		}
		fd[i] = dev_null;
		redir_count++;
	}
	quiet = redir_count == 3;

	if (!quiet) {
		child_controls_terminal = 1;
		ui_end();
	}

	pid = fork_exec(args, fd);
	if (pid < 0) {
		error_msg("Error: %s", strerror(errno));
		prompt = 0;
	} else {
		handle_child_error(pid);
	}
	if (!quiet) {
		term_raw();
		if (prompt)
			any_key();
		resize();
		child_controls_terminal = 0;
	}
	if (dev_null >= 0)
		close(dev_null);
}
