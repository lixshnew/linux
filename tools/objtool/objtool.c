// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2015 Josh Poimboeuf <jpoimboe@redhat.com>
 */

/*
 * objtool:
 *
 * The 'check' subcmd analyzes every .o file and ensures the validity of its
 * stack trace metadata.  It enforces a set of rules on asm code and C inline
 * assembly code so that stack traces can be reliable.
 *
 * For more information, see tools/objtool/Documentation/stack-validation.txt.
 */

#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <subcmd/exec-cmd.h>
#include <subcmd/pager.h>
#include <linux/kernel.h>

#include "builtin.h"
#include "objtool.h"
#include "warn.h"
#include "check.h"

struct cmd_struct {
	const char *name;
	int (*fn)(int, const char **);
	const char *help;
};

static const char objtool_usage_string[] =
	"objtool COMMAND [ARGS]";

static struct cmd_struct objtool_cmds[] = {
	{"check",	cmd_check,	"Perform stack metadata validation on an object file" },
	{"orc",		cmd_orc,	"Generate in-place ORC unwind tables for an object file" },
};

bool help;

const char *objname;
static struct objtool_file file;

struct objtool_file *objtool_open_read(const char *_objname)
{
	if (objname) {
		if (strcmp(objname, _objname)) {
			WARN("won't handle more than one file at a time");
			return NULL;
		}
		return &file;
	}
	objname = _objname;

	file.elf = elf_open_read(objname, O_RDWR);
	if (!file.elf)
		return NULL;

	INIT_LIST_HEAD(&file.insn_list);
	hash_init(file.insn_hash);
	INIT_LIST_HEAD(&file.static_call_list);
	file.c_file = !vmlinux && find_section_by_name(file.elf, ".comment");
	file.ignore_unreachables = no_unreachable;
	file.hints = false;

	return &file;
}

static void cmd_usage(void)
{
	unsigned int i, longest = 0;

	printf("\n usage: %s\n\n", objtool_usage_string);

	for (i = 0; i < ARRAY_SIZE(objtool_cmds); i++) {
		if (longest < strlen(objtool_cmds[i].name))
			longest = strlen(objtool_cmds[i].name);
	}

	puts(" Commands:");
	for (i = 0; i < ARRAY_SIZE(objtool_cmds); i++) {
		printf("   %-*s   ", longest, objtool_cmds[i].name);
		puts(objtool_cmds[i].help);
	}

	printf("\n");

	if (!help)
		exit(129);
	exit(0);
}

static void handle_options(int *argc, const char ***argv)
{
	while (*argc > 0) {
		const char *cmd = (*argv)[0];

		if (cmd[0] != '-')
			break;

		if (!strcmp(cmd, "--help") || !strcmp(cmd, "-h")) {
			help = true;
			break;
		} else {
			fprintf(stderr, "Unknown option: %s\n", cmd);
			cmd_usage();
		}

		(*argv)++;
		(*argc)--;
	}
}

static void handle_internal_command(int argc, const char **argv)
{
	const char *cmd = argv[0];
	unsigned int i, ret;

	for (i = 0; i < ARRAY_SIZE(objtool_cmds); i++) {
		struct cmd_struct *p = objtool_cmds+i;

		if (strcmp(p->name, cmd))
			continue;

		ret = p->fn(argc, argv);

		exit(ret);
	}

	cmd_usage();
}

int main(int argc, const char **argv)
{
	static const char *UNUSED = "OBJTOOL_NOT_IMPLEMENTED";

	/* libsubcmd init */
	exec_cmd_init("objtool", UNUSED, UNUSED, UNUSED);
	pager_init(UNUSED);

	argv++;
	argc--;
	handle_options(&argc, &argv);

	if (!argc || help)
		cmd_usage();

	handle_internal_command(argc, argv);

	return 0;
}
