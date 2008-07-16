#ifndef COMMANDS_H
#define COMMANDS_H

#include "term.h"

struct command {
	const char *name;
	const char *short_name;
	void (*cmd)(char **);
};

struct parsed_command {
	// can contain many commands. each terminated with NULL
	char **argv;

	// includes NULLs
	int count;
	int alloc;

	// for tab completion
	int comp_so;
	int comp_eo;
	int args_before_cursor;
};

extern const struct command commands[];
extern const struct command *current_command;

extern const char *config_file;
extern int config_line;

char *parse_command_arg(const char *cmd);
int parse_commands(struct parsed_command *pc, const char *cmd, int cursor_pos);
void free_commands(struct parsed_command *pc);
void handle_command(const char *cmd);
void handle_binding(enum term_key_type type, unsigned int key);
void read_config(const char *filename);

const struct command *find_command(const char *name);
void complete_command(void);
void reset_completion(void);
void add_completion(char *str);

#endif
