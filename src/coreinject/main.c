/*
 * Copyright (c) 2012-2015 Ericsson AB
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>

/*
 * This program injects binary data dumped by the minicoredumper into a
 * core file. The required files generated by the minicoredumper are:
 *   - core file
 *   - symbol.map
 *   - binary dump files
 */

static void usage(const char *argv0)
{
	fprintf(stderr, "usage: %s <core> <symbol.map> <binary-dump>...\n",
		argv0);
}

struct symbol_data {
	const char *name;
	long dump_offset;
	long core_offset;
	long size;
};

static int write_core(FILE *f_core, FILE *f_dump, struct symbol_data *d,
		      int direct)
{
	char *buf = NULL;
	int err = -1;

	/* seek in core */
	if (fseek(f_core, d->core_offset, SEEK_SET) != 0) {
		fprintf(stderr, "error: failed to seek to position 0x%lx for "
				"symbol %s in core (%s)\n",
			d->core_offset, d->name, strerror(errno));
		goto out;
	}

	/* seek in dump */
	if (fseek(f_dump, d->dump_offset, SEEK_SET) != 0) {
		fprintf(stderr, "error: failed to seek to position 0x%lx for "
				"symbol %s in dump (%s)\n",
			d->dump_offset, d->name, strerror(errno));
		goto out;
	}

	/* alloc data buffer */
	buf = malloc(d->size);
	if (!buf) {
		fprintf(stderr, "error: out of memory allocating %ld bytes\n",
			d->size);
		goto out;
	}

	/* read from dump */
	if (fread(buf, d->size, 1, f_dump) != 1) {
		fprintf(stderr, "error: failed to read %ld bytes from "
				"dump (%s)\n", d->size, strerror(errno));
		goto out;
	}

	/* write to core */
	if (fwrite(buf, d->size, 1, f_core) != 1) {
		fprintf(stderr, "error: failed to write %ld bytes to "
				"core (%s)\n", d->size, strerror(errno));
		goto out;
	}

	printf("injected: %s, %ld bytes, %s\n", d->name, d->size,
	       direct ? "direct" : "indirect");

	err = 0;
out:
	if (buf)
		free(buf);

	return err;
}

static void strip_endline(char *str)
{
	char *p;

	p = strchr(str, '\r');
	if (p)
		*p = 0;
	p = strchr(str, '\n');
	if (p)
		*p = 0;
}

static int get_symbol_data(const char *symname, FILE *f_symbol,
			   struct symbol_data *direct,
			   struct symbol_data *indirect)
{
	struct symbol_data *d;
	unsigned long offset;
	unsigned long size;
	char line[128];
	char type;
	char *p;
	int i;

	memset(direct, 0, sizeof(*direct));
	memset(indirect, 0, sizeof(*indirect));

	/* Search the full symbol map to find the symbol information for
	 * the specified symbol. If the number of symbols in a symbol map
	 * become large and the number of dump files becomes large, then
	 * it would be more efficient to parse the system map once,
	 * allocating symbol information along the way. */

	rewind(f_symbol);

	while (fgets(line, sizeof(line), f_symbol)) {
		strip_endline(line);

		/* ignore invalid lines */
		if (sscanf(line, "%lx %ld %c ", &offset, &size, &type) != 3)
			continue;

		/* locate symbol name */
		p = line;
		for (i = 0; i < 3; i++) {
			p = strchr(p, ' ');
			if (!p)
				break;
			p++;
		}
		/* ignore invalid lines */
		if (i != 3)
			continue;

		/* check if this is the symbol we want */
		if (strcmp(symname, p) != 0)
			continue;

		if (type == 'D') {
			d = direct;
		} else if (type == 'I') {
			d = indirect;
		} else {
			/* ignore invalid lines */
			continue;
		}

		/* last entry wins in case of duplicates */
		d->core_offset = offset;
		d->size = size;
		d->name = symname;
	}

	/* If indirect data exists, it is at the head of the dump file.
	 * Adjust the direct data dump offset accordingly. */
	if (indirect->size && direct->size)
		direct->dump_offset += indirect->size;

	return 0;
}

static int inject_data(FILE *f_core, FILE *f_symbol, const char *b_fname)
{
	struct symbol_data indirect;
	struct symbol_data direct;
	const char *symname;
	FILE *f_dump;
	int err = 0;
	char *p;

	/* extract symbol name from file path */
	p = strrchr(b_fname, '/');
	if (p)
		symname = p + 1;
	else
		symname = b_fname;

	/* get offsets/sizes from symbol map */
	if (get_symbol_data(symname, f_symbol, &direct, &indirect) != 0) {
		fprintf(stderr, "error: unable to find symbol %s in map\n",
			symname);
		return -1;
	}

	/* open binary dump file for reading */
	f_dump = fopen(b_fname, "r");
	if (!f_dump) {
		fprintf(stderr, "error: failed to open %s (%s)\n", b_fname,
			strerror(errno));
		return -1;
	}

	/* write direct data (continuing on error) */
	if (direct.size > 0)
		err |= write_core(f_core, f_dump, &direct, 1);

	/* write indirect data */
	if (indirect.size > 0)
		err |= write_core(f_core, f_dump, &indirect, 0);

	fclose(f_dump);

	return err;
}

int main(int argc, char *argv[])
{
	FILE *f_core = NULL;
	FILE *f_symbols = NULL;
	struct stat s;
	int err = 1;
	int i;

	if (argc < 4) {
		usage(argv[0]);
		goto out;
	}

	/* check if the core file is present */
	if (stat(argv[1], &s) != 0) {
		fprintf(stderr, "error: failed to stat %s (%s)\n",
			argv[1], strerror(errno));
		goto out;
	}

	/* open the core file read-write */
	f_core = fopen(argv[1], "r+");
	if (!f_core) {
		fprintf(stderr, "error: failed to open %s for writing (%s)\n",
			argv[1], strerror(errno));
		goto out;
	}

	/* open the symbol map for reading */
	f_symbols = fopen(argv[2], "r");
	if (!f_symbols) {
		fprintf(stderr, "error: failed to open %s (%s)\n", argv[2],
			strerror(errno));
		goto out;
	}

	err = 0;

	/* try to add binary dumps (continuing on error) */
	for (i = 3; i < argc; i++) {
		if (inject_data(f_core, f_symbols, argv[i]) != 0)
			err |= 1;
	}
out:
	if (f_core)
		fclose(f_core);
	if (f_symbols)
		fclose(f_symbols);

	return err;
}
