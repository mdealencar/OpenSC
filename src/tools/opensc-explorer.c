/*
 * opensc-explorer.c: A shell for accessing SmartCards with libopensc
 *
 * Copyright (C) 2001  Juha Yrj�l� <juha.yrjola@iki.fi>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <opensc.h>
#ifdef HAVE_READLINE_READLINE_H
#include <readline/readline.h>
#endif
#include "util.h"

int opt_reader = 0, opt_debug = 0;
const char *opt_driver = NULL;

struct sc_file *current_file = NULL;
struct sc_path current_path;
struct sc_context *ctx = NULL;
struct sc_card *card = NULL;

const struct option options[] = {
	{ "reader",		1, 0, 'r' },
	{ "card-driver",	1, 0, 'D' },
	{ "debug",		0, 0, 'd' },
	{ 0, 0, 0, 0 }
};
const char *option_help[] = {
	"Uses reader number <arg> [0]",
	"Forces the use of driver <arg> [auto-detect]",
	"Debug output -- maybe supplied several times",
};

struct command {
	const char *	name;
	int		(*func)(const char *, const char *);
	const char *	help;
};

void die(int ret)
{
	if (current_file != NULL)
		sc_file_free(current_file);
	if (card) {
		sc_unlock(card);
		sc_disconnect_card(card, 0);
	}
	if (ctx)
		sc_destroy_context(ctx);
	exit(ret);
}

static struct command *
ambiguous_match(struct command *table, const char *cmd)
{
	struct command *last_match = NULL;
	int matches = 0;

	for (; table->name; table++) {
                if (strncasecmp(cmd, table->name, strlen(cmd)) == 0) {
			last_match = table;
			matches++;
		}
	}
	if (matches > 1) {
		printf("Ambiguous command: %s\n", cmd);
		return NULL;
	}
	return last_match;
}

void check_ret(int r, int op, const char *err, const struct sc_file *file)
{
	fprintf(stderr, "%s: %s\n", err, sc_strerror(r));
	if (r == SC_ERROR_SECURITY_STATUS_NOT_SATISFIED)
		fprintf(stderr, "ACL for operation: %s\n", acl_to_str(sc_file_get_acl_entry(file, op)));
}

int arg_to_path(const char *arg, struct sc_path *path, int is_id)
{
	int buf[2];
	u8 cbuf[2];
	
	if (strlen(arg) != 4) {
		printf("Wrong ID length.\n");
		return -1;
	}
	if (sscanf(arg, "%02X%02X", &buf[0], &buf[1]) != 2) {
		printf("Invalid ID.\n");
		return -1;
	}
	cbuf[0] = buf[0];
	cbuf[1] = buf[1];
	if ((cbuf[0] == 0x3F && cbuf[1] == 0x00) || is_id) {
		path->len = 2;
		memcpy(path->value, cbuf, 2);
		if (is_id)
			path->type = SC_PATH_TYPE_FILE_ID;
		else
			path->type = SC_PATH_TYPE_PATH;
	} else {
		*path = current_path;
		sc_append_path_id(path, cbuf, 2);
	}

	return 0;	
}

void print_file(const struct sc_file *file)
{
	const char *st;

	if (file->type == SC_FILE_TYPE_DF)
		printf("[");
	else
                printf(" ");
	printf("%02X%02X", file->id >> 8, file->id & 0xFF);
	if (file->type == SC_FILE_TYPE_DF)
		printf("]");
	else
                printf(" ");
	switch (file->type) {
	case SC_FILE_TYPE_WORKING_EF:
		st = "wEF";
		break;
	case SC_FILE_TYPE_INTERNAL_EF:
		st = "iEF";
		break;
	case SC_FILE_TYPE_DF:
		st = "DF";
		break;
	default:
		st = "???";
		break;
	}
	printf("\t%4s", st);
        printf(" %5d", file->size);
	if (file->namelen) {
		printf("\tName: ");
		print_binary(stdout, file->name, file->namelen);
	}
        printf("\n");
	return;
}

int do_ls(const char *dummy, const char *dummy2)
{
	u8 buf[256], *cur = buf;
	int r, count;

	r = sc_list_files(card, buf, sizeof(buf));
	if (r < 0) {
		check_ret(r, SC_AC_OP_LIST_FILES, "unable to receive file listing", current_file);
		return -1;
	}
	count = r;
        printf("FileID\tType  Size\n");
	while (count >= 2) {
		struct sc_path path;
		struct sc_file *file = NULL;

		path = current_path;
		sc_append_path_id(&path, cur, 2);
		r = sc_select_file(card, &path, &file);
		if (r) {
			check_ret(r, SC_AC_OP_SELECT, "unable to select file", current_file);
			return -1;
		}
		file->id = (cur[0] << 8) | cur[1];
                cur += 2;
		count -= 2;
                print_file(file);
                sc_file_free(file);
		r = sc_select_file(card, &current_path, NULL);
		if (r) {
			printf("unable to select parent DF: %s\n", sc_strerror(r));
			die(1);
		}
	}
        return 0;
}

int do_cd(const char *arg, const char *dummy2)
{
	struct sc_path path;
	struct sc_file *file;
	int r;

	if (strcmp(arg, "..") == 0) {
		if (current_path.len < 4) {
			printf("unable to go up, already in MF.\n");
			return -1;
		}
                path = current_path;
		path.len -= 2;
		r = sc_select_file(card, &path, &file);
		if (r) {
			printf("unable to go up: %s\n", sc_strerror(r));
			return -1;
		}
		sc_file_free(current_file);
		current_file = file;
		current_path = path;
		return 0;
	}
	if (arg_to_path(arg, &path, 0) != 0) {
		printf("Usage: cd <file_id>\n");
		return -1;
	}
	r = sc_select_file(card, &path, &file);
	if (r) {
		check_ret(r, SC_AC_OP_SELECT, "unable to select DF", current_file);
		return -1;
	}
	if (file->type != SC_FILE_TYPE_DF) {
		printf("Error: file is not a DF.\n");
		sc_file_free(file);
		r = sc_select_file(card, &current_path, NULL);
		if (r) {
			printf("unable to select parent file: %s\n", sc_strerror(r));
			die(1);
		}
		return -1;
	}
	current_path = path;
	sc_file_free(current_file);
	current_file = file;

	return 0;
}

int read_and_print_binary_file(struct sc_file *file)
{
	unsigned int idx = 0;
	u8 buf[128];
	size_t count;
	int r;
	
	count = file->size;
	while (count) {
		int c = count > sizeof(buf) ? sizeof(buf) : count;

		r = sc_read_binary(card, idx, buf, c, 0);
		if (r < 0) {
			check_ret(r, SC_AC_OP_READ, "read failed", file);
			return -1;
		}
		if (r != c) {
			printf("expecting %d, got only %d bytes.\n", c, r);
			return -1;
		}
		hex_dump_asc(stdout, buf, c, idx);
		idx += c;
		count -= c;
	}
	return 0;
}

int read_and_print_record_file(struct sc_file *file)
{
	u8 buf[256];
	int rec, r;

	for (rec = 1; ; rec++) {
		r = sc_read_record(card, rec, buf, sizeof(buf), SC_RECORD_BY_REC_NR);
		if (r == SC_ERROR_RECORD_NOT_FOUND)
			return 0;
		if (r < 0) {
			check_ret(r, SC_AC_OP_READ, "read failed", file);
			return -1;
		}
		printf("Record %d:\n", rec);
		hex_dump_asc(stdout, buf, r, 0);
	}

	return 0;
}

int do_cat(const char *arg, const char *dummy2)
{
	int r, error = 0;
	struct sc_path path;
        struct sc_file *file;
	int not_current = 1;

	if (strlen(arg) == 0) {
		path = current_path;
		file = current_file;
		not_current = 0;
	} else {
		if (arg_to_path(arg, &path, 0) != 0) {
			printf("Usage: cat [file_id]\n");
			return -1;
		}
		r = sc_select_file(card, &path, &file);
		if (r) {
			check_ret(r, SC_AC_OP_SELECT, "unable to select file", current_file);
			return -1;
		}
	}
	if (file->type != SC_FILE_TYPE_WORKING_EF) {
		printf("only working EFs may be read\n");
		sc_file_free(file);
		return -1;
	}
	if (file->ef_structure == SC_FILE_EF_TRANSPARENT)
		read_and_print_binary_file(file);
	else
		read_and_print_record_file(file);
	if (not_current) {
		sc_file_free(file);
		r = sc_select_file(card, &current_path, NULL);
		if (r) {
			printf("unable to select parent file: %s\n", sc_strerror(r));
			die(1);
		}
	}
        return -error;
}

int do_info(const char *arg, const char *dummy2)
{
	struct sc_file *file;
	struct sc_path path;
	int r, i;
	const char *st;
	int not_current = 1;
	
	if (strlen(arg) == 0) {
		path = current_path;
		file = current_file;
		not_current = 0;
	} else {
		if (arg_to_path(arg, &path, 0) != 0) {
			printf("Usage: info [file_id]\n");
			return -1;
		}
		r = sc_select_file(card, &path, &file);
		if (r) {
			printf("unable to select file: %s\n", sc_strerror(r));
			return -1;
		}
	}
	switch (file->type) {
	case SC_FILE_TYPE_WORKING_EF:
	case SC_FILE_TYPE_INTERNAL_EF:
		st = "Elementary File";
		break;
	case SC_FILE_TYPE_DF:
		st = "Dedicated File";
		break;
	default:
		st = "Unknown File";
		break;
	}
	printf("\n%s  ID %04X\n\n", st, file->id);
	printf("%-15s", "File path:");
	for (i = 0; i < path.len; i++) {
		for (i = 0; i < path.len; i++) {
                        if ((i & 1) == 0 && i)
				printf("/");
			printf("%02X", path.value[i]);
		}
	}
	printf("\n%-15s%d bytes\n", "File size:", file->size);

	if (file->type == SC_FILE_TYPE_DF) {
		const char *ops[] = {
			"SELECT", "LOCK", "DELETE", "CREATE", "REHABILITATE",
			"INVALIDATE", "LIST FILES"
		};
		if (file->namelen) {
			printf("%-15s", "DF name:");
			print_binary(stdout, file->name, file->namelen);
			printf("\n");
		}
		for (i = 0; i < sizeof(ops)/sizeof(ops[0]); i++) {
			char buf[80];
			
			sprintf(buf, "ACL for %s:", ops[i]);
			printf("%-25s%s\n", buf, acl_to_str(sc_file_get_acl_entry(file, i)));
		}
	} else {
                const char *structs[] = {
                        "Unknown", "Transparent", "Linear fixed",
			"Linear fixed, SIMPLE-TLV", "Linear variable",
			"Cyclic", "Cyclic, SIMPLE-TLV",
                };
		const char *ops[] = {
			"READ", "UPDATE", "WRITE", "ERASE", "REHABILITATE",
			"INVALIDATE"
		};
		printf("%-15s%s\n", "EF structure:", structs[file->ef_structure]);
		for (i = 0; i < sizeof(ops)/sizeof(ops[0]); i++) {
			char buf[80];
			
			sprintf(buf, "ACL for %s:", ops[i]);
			printf("%-25s%s\n", buf, acl_to_str(sc_file_get_acl_entry(file, i)));
		}
	}	
	if (file->prop_attr_len) {
		printf("%-25s", "Proprietary attributes:");
		for (i = 0; i < file->prop_attr_len; i++)
			printf("%02X ", file->prop_attr[i]);
		printf("\n");
	}
	if (file->sec_attr_len) {
		printf("%-25s", "Security attributes:");
		for (i = 0; i < file->sec_attr_len; i++)
			printf("%02X ", file->sec_attr[i]);
		printf("\n");
	}
	printf("\n");
	if (not_current) {
		sc_file_free(file);
		r = sc_select_file(card, &current_path, NULL);
		if (r) {
			printf("unable to select parent file: %s\n", sc_strerror(r));
			die(1);
		}
	}
	return 0;
}

int create_file(struct sc_file *file)
{
	int r;
	
	r = sc_create_file(card, file);
	if (r) {
		check_ret(r, SC_AC_OP_CREATE, "CREATE FILE failed", current_file);
		return -1;
	}
	/* Make sure we're back in the parent directory, because on some cards
	 * CREATE FILE also selects the newly created file. */
	r = sc_select_file(card, &current_path, NULL);
	if (r) {
		printf("unable to select parent file: %s\n", sc_strerror(r));
		die(1);
	}
	return 0;
}

int do_create(const char *arg, const char *arg2)
{
	struct sc_path path;
	struct sc_file *file;
	unsigned int size;
	int r;

	if (arg_to_path(arg, &path, 1) != 0)
		goto usage;
	/* %z isn't supported everywhere */
	if (sscanf(arg2, "%d", &size) != 1)
		goto usage;
	file = sc_file_new();
	file->id = (path.value[0] << 8) | path.value[1];
	file->type = SC_FILE_TYPE_WORKING_EF;
	file->ef_structure = SC_FILE_EF_TRANSPARENT;
	file->size = (size_t) size;
	file->status = SC_FILE_STATUS_ACTIVATED;
	
	r = create_file(file);
	sc_file_free(file);
	return r;
usage:
	printf("Usage: create <file_id> <file_size>\n");
	return -1;
}

int do_mkdir(const char *arg, const char *arg2)
{
	struct sc_path path;
	struct sc_file *file;
	unsigned int size;
	int r;

	if (arg_to_path(arg, &path, 1) != 0)
		goto usage;
	if (sscanf(arg2, "%d", &size) != 1)
		goto usage;
	file = sc_file_new();
	file->id = (path.value[0] << 8) | path.value[1];
	file->type = SC_FILE_TYPE_DF;
	file->size = size;
	file->status = SC_FILE_STATUS_ACTIVATED;

	r = create_file(file);
	sc_file_free(file);
	return r;
usage:
	printf("Usage: mkdir <file_id> <df_size>\n");
	return -1;
}

int do_delete(const char *arg, const char *dummy2)
{
	struct sc_path path;
	int r;

	if (arg_to_path(arg, &path, 1) != 0)
		goto usage;
	if (path.len != 2)
		goto usage;
	path.type = SC_PATH_TYPE_FILE_ID;
	r = sc_delete_file(card, &path);
	if (r) {
		check_ret(r, SC_AC_OP_DELETE, "DELETE FILE failed", current_file);
		return -1;
	}
	return 0;
usage:
	printf("Usage: delete <file_id>\n");
	return -1;
}

int do_verify(const char *arg, const char *arg2)
{
	const char *types[] = {
		"CHV", "KEY", "PRO"
	};
	int i, type = -1, ref, r, tries_left = -1;
	u8 buf[30];
	size_t buflen = sizeof(buf);
	
	if (strlen(arg) == 0 || strlen(arg2) == 0)
		goto usage;
	for (i = 0; i < 3; i++)
		if (strncasecmp(arg, types[i], 3) == 0) {
			type = i;
			break;
		}
	if (type == -1) {
		printf("Invalid type.\n");
		goto usage;
	}
	if (sscanf(arg + 3, "%d", &ref) != 1) {
		printf("Invalid key reference.\n");
		goto usage;
	}
	if (arg2[0] == '"') {
		for (++arg2, i = 0; i < sizeof(buf) && arg2[i] != '"'; i++) 
			buf[i] = arg2[i];
		buflen = i;
	} else
	if (sc_hex_to_bin(arg2, buf, &buflen) != 0) {
		printf("Invalid key value.\n");
		goto usage;
	}
	switch (type) {
	case 0:
		type = SC_AC_CHV;
		break;
	case 1:
		type = SC_AC_AUT;
		break;
	case 2:
		type = SC_AC_PRO;
		break;
	}
	r = sc_verify(card, type, ref, buf, buflen, &tries_left);
	if (r) {
		if (r == SC_ERROR_PIN_CODE_INCORRECT) {
			if (tries_left >= 0) 
				printf("Incorrect code, %d tries left.\n", tries_left);
			else
				printf("Incorrect code.\n");
		}
		printf("Unable to verify PIN code: %s\n", sc_strerror(r));
		return -1;
	}
	printf("Code correct.\n");
	return 0;
usage:
	printf("Usage: verify <key type><key ref> <key in hex>\n");
	printf("Possible values of <key type>:\n");
	for (i = 0; i < sizeof(types)/sizeof(types[0]); i++)
		printf("\t%s\n", types[i]);
	printf("Example: verify CHV2 31:32:33:34:00:00:00:00\n");
	return -1;
}

int do_get(const char *arg, const char *arg2)
{
	u8 buf[256];
	int r, error = 0;
	size_t count = 0;
        unsigned int idx = 0;
	struct sc_path path;
        struct sc_file *file;
	const char *filename;
	FILE *outf = NULL;
	
	if (arg_to_path(arg, &path, 0) != 0)
		goto usage;
	if (strlen(arg2))
		filename = arg2;
	else {
		sprintf((char *) buf, "%02X%02X", path.value[0], path.value[1]);
		filename = (char *) buf;
	}
	outf = fopen(filename, "w");
	if (outf == NULL) {
		perror(filename);
		return -1;
	}
	r = sc_select_file(card, &path, &file);
	if (r) {
		check_ret(r, SC_AC_OP_SELECT, "unable to select file", current_file);
		return -1;
	}
	if (file->type != SC_FILE_TYPE_WORKING_EF) {
		printf("only working EFs may be read\n");
		sc_file_free(file);
		return -1;
	}
	count = file->size;
	while (count) {
		int c = count > sizeof(buf) ? sizeof(buf) : count;

		r = sc_read_binary(card, idx, buf, c, 0);
		if (r < 0) {
			check_ret(r, SC_AC_OP_READ, "read failed", file);
			error = 1;
                        goto err;
		}
		if (r != c) {
			printf("expecting %d, got only %d bytes.\n", c, r);
			error = 1;
                        goto err;
		}
		fwrite(buf, c, 1, outf);
		idx += c;
		count -= c;
	}
	printf("Total of %d bytes read.\n", idx);
err:
	sc_file_free(file);
	r = sc_select_file(card, &current_path, NULL);
	if (r) {
		printf("unable to select parent file: %s\n", sc_strerror(r));
		die(1);
	}
	if (outf)
		fclose(outf);
        return -error;
usage:
	printf("Usage: get <file id> [output file]\n");
	return -1;
}

int do_put(const char *arg, const char *arg2)
{
	u8 buf[256];
	int r, error = 0;
	size_t count = 0;
        unsigned int idx = 0;
	struct sc_path path;
        struct sc_file *file;
	const char *filename;
	FILE *outf = NULL;
	
	if (arg_to_path(arg, &path, 0) != 0)
		goto usage;
	if (strlen(arg2))
		filename = arg2;
	else {
		sprintf((char *) buf, "%02X%02X", path.value[0], path.value[1]);
		filename = (char *) buf;
	}
	outf = fopen(filename, "r");
	if (outf == NULL) {
		perror(filename);
		return -1;
	}
	r = sc_select_file(card, &path, &file);
	if (r) {
		check_ret(r, SC_AC_OP_SELECT, "unable to select file", current_file);
		return -1;
	}
	count = file->size;
	while (count) {
		int c = count > sizeof(buf) ? sizeof(buf) : count;

		r = fread(buf, 1, c, outf);
		if (r < 0) {
			perror("fread");
			error = 1;
			goto err;
		}
		if (r != c)
			count = c = r;
		r = sc_update_binary(card, idx, buf, c, 0);
		if (r < 0) {
			check_ret(r, SC_AC_OP_READ, "update failed", file);
			error = 1;
                        goto err;
		}
		if (r != c) {
			printf("expecting %d, wrote only %d bytes.\n", c, r);
			error = 1;
                        goto err;
		}
		idx += c;
		count -= c;
	}
	printf("Total of %d bytes written.\n", idx);
err:
	sc_file_free(file);
	r = sc_select_file(card, &current_path, NULL);
	if (r) {
		printf("unable to select parent file: %s\n", sc_strerror(r));
		die(1);
	}
	if (outf)
		fclose(outf);
        return -error;
usage:
	printf("Usage: put <file id> [output file]\n");
	return -1;
}

int do_debug(const char *arg, const char *dummy2)
{
	int	i;

	if (sscanf(arg, "%d", &i) != 1)
		return -1;
	printf("Debug level set to %d\n", i);
	ctx->debug = i;
	if (i) {
		ctx->error_file = stderr;
		ctx->debug_file = stdout;
	} else {
		ctx->error_file = NULL;
		ctx->debug_file = NULL;
	}
	return 0;
}

int do_quit(const char *dummy, const char *dummy2)
{
	die(0);
	return 0;
}

struct command		cmds[] = {
 { "ls",	do_ls,		"list all files in the current DF"	},
 { "cd",	do_cd,		"change to another DF"			},
 { "debug",	do_debug,	"set the debug level"			},
 { "cat",	do_cat,		"print the contents of an EF"		},
 { "info",	do_info,	"display attributes of card file"	},
 { "create",	do_create,	"create a new EF"			},
 { "delete",	do_delete,	"remove an EF/DF"			},
 { "verify",	do_verify,	"present a PIN or key to the card"	},
 { "put",	do_put,		"copy a local file to the card"		},
 { "get",	do_get,		"copy an EF to a local file"		},
 { "mkdir",	do_mkdir,	"create a DF"				},
 { "quit",	do_quit,	"quit this program"			},
 { 0, 0, 0 }
};

void usage()
{
	struct command	*cmd;

	printf("Supported commands:\n");
	for (cmd = cmds; cmd->name; cmd++)
		printf("  %-10s %s\n", cmd->name, cmd->help);
}

static int parse_line(char *in, char **argv)
{
	int	argc;

	for (argc = 0; argc < 3; argc++) {
		in += strspn(in, " \t\n");
		if (*in == '\0')
			return argc;
 		if (*in == '"') {
			/* Parse quoted string */
			argv[argc] = in++;
			in += strcspn(in, "\"");
			if (*in++ != '"')
				return 0;
		} else {
			/* White space delimited word */
 			argv[argc] = in;
			in += strcspn(in, " \t\n");
		}
		if (*in != '\0')
			*in++ = '\0';
 	}
	return argc;
}

#if !defined(HAVE_READLINE_READLINE_H)
char * readline(const char *prompt)
{
	static char buf[128];

	printf("%s", prompt);
	fflush(stdout);
	if (fgets(buf, sizeof(buf), stdin) == NULL)
		return NULL;
	if (strlen(buf) == 0)
		return NULL;
	if (buf[strlen(buf)-1] == '\n')
		buf[strlen(buf)-1] = '\0';
        return buf;
}
#endif

int main(int argc, char * const argv[])
{
	int r, c, long_optind = 0, err = 0;
	char *line, *cargv[3];

	printf("OpenSC Explorer version %s\n", sc_version);

	while (1) {
		c = getopt_long(argc, argv, "r:c:d", options, &long_optind);
		if (c == -1)
			break;
		if (c == '?')
			print_usage_and_die("opensc-explorer");
		switch (c) {
		case 'r':
			opt_reader = atoi(optarg);
			break;
		case 'c':
			opt_driver = optarg;
			break;
		case 'd':
			opt_debug++;
			break;
		}
	}
	r = sc_establish_context(&ctx);
	if (r) {
		fprintf(stderr, "Failed to establish context: %s\n", sc_strerror(r));
		return 1;
	}
	if (opt_debug) {
		ctx->error_file = stderr;
		ctx->debug_file = stdout;
		ctx->debug = opt_debug;
	}
	if (opt_reader >= ctx->reader_count || opt_reader < 0) {
		fprintf(stderr, "Illegal reader number. Only %d reader(s) configured.\n", ctx->reader_count);
		err = 1;
		goto end;
	}
	if (sc_detect_card_presence(ctx->reader[opt_reader], 0) != 1) {
		fprintf(stderr, "Card not present.\n");
		err = 3;
		goto end;
	}
	if (opt_driver != NULL) {
		err = sc_set_card_driver(ctx, opt_driver);
		if (err) {
			fprintf(stderr, "Driver '%s' not found!\n", opt_driver);
			err = 1;
			goto end;
		}
	}
	fprintf(stderr, "Connecting to card in reader %s...\n", ctx->reader[opt_reader]->name);
	r = sc_connect_card(ctx->reader[opt_reader], 0, &card);
	if (r) {
		fprintf(stderr, "Failed to connect to card: %s\n", sc_strerror(r));
		err = 1;
		goto end;
	}
	printf("Using card driver: %s\n", card->driver->name);
	r = sc_lock(card);
	if (r) {
		fprintf(stderr, "Unable to lock card: %s\n", sc_strerror(r));
		err = 1;
		goto end;
	}

        sc_format_path("3F00", &current_path);
	r = sc_select_file(card, &current_path, &current_file);
	if (r) {
		printf("unable to select MF: %s\n", sc_strerror(r));
		return 1;
	}
	while (1) {
		struct command *c;
		int i;
		char prompt[40];

		sprintf(prompt, "OpenSC [");
		for (i = 0; i < current_path.len; i++) {
                        if ((i & 1) == 0 && i)
				sprintf(prompt+strlen(prompt), "/");
			sprintf(prompt+strlen(prompt), "%02X", current_path.value[i]);
		}
                sprintf(prompt+strlen(prompt), "]> ");
		line = readline(prompt);
                if (line == NULL)
                	break;
                r = parse_line(line, cargv);
		if (r < 1)
			continue;
		while (r < 3)
			cargv[r++] = "";
		c = ambiguous_match(cmds, cargv[0]);
		if (c == NULL) {
			usage();
		} else {
			c->func(cargv[1], cargv[2]);
		}
	}
end:
	die(err);
	
	return 0; /* not reached */
}
