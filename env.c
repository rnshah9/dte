#include "env.h"
#include "gbuf.h"
#include "completion.h"
#include "window.h"
#include "selection.h"
#include "editor.h"

struct builtin_env {
	const char *name;
	void (*expand)(struct gbuf *buf);
};

static void expand_file(struct gbuf *buf)
{
	struct view *v = window->view;

	if (v->buffer->abs_filename)
		gbuf_add_str(buf, v->buffer->abs_filename);
}

static void expand_pkgdatadir(struct gbuf *buf)
{
	gbuf_add_str(buf, pkgdatadir);
}

static void expand_word(struct gbuf *buf)
{
	struct view *v = window->view;
	long size;
	char *str = view_get_selection(v, &size);

	if (str) {
		gbuf_add_buf(buf, str, size);
	} else {
		str = view_get_word_under_cursor(v);
		if (str)
			gbuf_add_str(buf, str);
	}
	free(str);
}

static const struct builtin_env builtin[] = {
	{ "FILE",	expand_file },
	{ "PKGDATADIR",	expand_pkgdatadir },
	{ "WORD",	expand_word },
};

void collect_builtin_env(const char *prefix, int len)
{
	int i;

	for (i = 0; i < ARRAY_COUNT(builtin); i++) {
		if (!strncmp(builtin[i].name, prefix, len))
			add_completion(xstrdup(builtin[i].name));
	}
}

bool expand_builtin_env(struct gbuf *buf, const char *name, int len)
{
	int i;

	for (i = 0; i < ARRAY_COUNT(builtin); i++) {
		const struct builtin_env *be = &builtin[i];

		if (len == strlen(be->name) && !memcmp(name, be->name, len)) {
			be->expand(buf);
			return true;
		}
	}
	return false;
}
