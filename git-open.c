#include "git-open.h"
#include "spawn.h"
#include "window.h"
#include "term.h"
#include "cmdline.h"
#include "editor.h"
#include "obuf.h"
#include "modes.h"
#include "screen.h"
#include "uchar.h"

struct git_open git_open;

static void git_open_clear(void)
{
	free(git_open.all_files);
	git_open.all_files = NULL;
	git_open.size = 0;
	git_open.files.count = 0;
	git_open.selected = 0;
	git_open.scroll = 0;
}

static char *cdup(void)
{
	static const char * const cmd[] = { "git", "rev-parse", "--show-cdup", NULL };
	struct filter_data data;
	int len;

	data.in = NULL;
	data.in_len = 0;
	if (spawn_filter((char **)cmd, &data))
		return NULL;

	len = data.out_len;
	if (len > 1 && data.out[len - 1] == '\n') {
		data.out[len - 1] = 0;
		return data.out;
	}
	free(data.out);
	return NULL;
}

static void git_open_load(void)
{
	static const char *cmd[] = { "git", "ls-files", "-z", NULL, NULL };
	struct filter_data data;
	char *dir = cdup();

	cmd[3] = dir;

	data.in = NULL;
	data.in_len = 0;
	if (spawn_filter((char **)cmd, &data) == 0) {
		git_open.all_files = data.out;
		git_open.size = data.out_len;
	} else {
		git_open.all_files = NULL;
		git_open.size = 0;
	}
	free(dir);
}

static int contains_upper(const char *str)
{
	unsigned int i = 0;

	while (str[i]) {
		if (u_is_upper(u_str_get_char(str, &i)))
			return 1;
	}
	return 0;
}

static void split(struct ptr_array *words, const char *str)
{
	int s, i = 0;

	while (str[i]) {
		while (isspace(str[i]))
			i++;
		if (!str[i])
			break;
		s = i++;
		while (str[i] && !isspace(str[i]))
			i++;
		ptr_array_add(words, xstrslice(str, s, i));
	}
}

static int words_match(const char *name, struct ptr_array *words)
{
	int i;

	for (i = 0; i < words->count; i++) {
		if (!strstr(name, words->ptrs[i]))
			return 0;
	}
	return 1;
}

static int words_match_icase(const char *name, struct ptr_array *words)
{
	int i;

	for (i = 0; i < words->count; i++) {
		if (u_str_index(name, words->ptrs[i]) < 0)
			return 0;
	}
	return 1;
}

static const char *selected_file(void)
{
	if (git_open.files.count == 0)
		return NULL;
	return git_open.files.ptrs[git_open.selected];
}

static void git_open_filter(void)
{
	char *str = cmdline.buf.buffer;
	char *ptr = git_open.all_files;
	char *end = git_open.all_files + git_open.size;
	int (*match)(const char *, struct ptr_array *) = words_match_icase;
	PTR_ARRAY(words);

	// NOTE: words_match_icase() requires str to be lowercase
	if (contains_upper(str))
		match = words_match;
	split(&words, str);

	git_open.files.count = 0;
	while (ptr < end) {
		char *zero = memchr(ptr, 0, end - ptr);
		if (zero == NULL)
			break;
		if (match(ptr, &words))
			ptr_array_add(&git_open.files, ptr);
		ptr = zero + 1;
	}
	ptr_array_free(&words);
	git_open.selected = 0;
	git_open.scroll = 0;
}

static void up(int count)
{
	git_open.selected -= count;
	if (git_open.selected < 0)
		git_open.selected = 0;
}

static void down(int count)
{
	git_open.selected += count;
	if (git_open.selected >= git_open.files.count)
		git_open.selected = git_open.files.count - 1;
}

static void open_selected(void)
{
	const char *sel = selected_file();
	struct view *v;

	if (sel == NULL)
		return;

	v = open_buffer(sel, 0, NULL);
	if (v) {
		struct view *old_view = view;
		set_view(v);
		prev_view = old_view;
	}
}

static void git_open_key(enum term_key_type type, unsigned int key)
{
	switch (type) {
	case KEY_NORMAL:
		switch (key) {
		case '\r':
			open_selected();
			cmdline_clear(&cmdline);
			input_mode = INPUT_NORMAL;
			break;
		case CTRL('O'):
			open_selected();
			down(1);
			break;
		}
		break;
	case KEY_META:
		switch (key) {
		case 'e':
			if (git_open.files.count > 0)
				git_open.selected = git_open.files.count - 1;
			break;
		case 't':
			git_open.selected = 0;
			break;
		}
		break;
	case KEY_SPECIAL:
		switch (key) {
		case SKEY_UP:
			up(1);
			break;
		case SKEY_DOWN:
			down(1);
			break;
		case SKEY_PAGE_UP:
			up(screen_h - 2);
			break;
		case SKEY_PAGE_DOWN:
			down(screen_h - 2);
			break;
		}
		break;
	case KEY_PASTE:
		break;
	}
}

void git_open_reload(void)
{
	git_open_clear();
	git_open_load();
	git_open_filter();
}

void git_open_keypress(enum term_key_type type, unsigned int key)
{
	switch (cmdline_handle_key(&cmdline, NULL, type, key)) {
	case CMDLINE_UNKNOWN_KEY:
		git_open_key(type, key);
		break;
	case CMDLINE_KEY_HANDLED:
		git_open_filter();
		break;
	case CMDLINE_CANCEL:
		input_mode = INPUT_NORMAL;
		break;
	}
	mark_everything_changed();
}

static void git_open_update(void)
{
	buf_hide_cursor();
	update_term_title();
	update_git_open();
	buf_move_cursor(cmdline_x, 0);
	buf_show_cursor();
	buf_flush();
}

const struct editor_mode_ops git_open_ops = {
	.keypress = git_open_keypress,
	.update = git_open_update,
};
