#ifndef COMMANDS_H
#define COMMANDS_H

#include "command/run.h"
#include "util/macros.h"

extern const CommandSet normal_commands;

const Command *find_normal_command(const char *name) NONNULL_ARGS;
void collect_normal_commands(const char *prefix) NONNULL_ARGS;

#endif
