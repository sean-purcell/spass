#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "builtin.h"
#include "file_db.h"
#include "spass_util.h"

struct cmds {
	int (*fun)(dbfile_t*, int, char**);
	char* name;
	char* desc;
};

static struct cmds builtins[] = {
	{ &add, "add", "add a new password to the database" },
	{ &chpw, "chpw", "change the database password"},
	{ &get, "get", "retrieve a password from the database" },
	{ &gen, "gen", "generate a new random password and insert it in the database" },
	{ &ls, "ls", "list the names of the passwords in the database" },
	{ &rm, "rm", "removes a password from the database" }
};

static void cmd_unfound() {
	int i;
	puts("Commands:");
	for(i = 0; i < sizeof(builtins)/sizeof(*builtins); i++) {
		printf("%s: %s\n", builtins[i].name, builtins[i].desc);
	}
}

static void usage() {
	puts("usage: spass <command> [<args>]");
}

static int create_config() {
	char* expath = expand_tilde(cfgfile);
	char* dbfile = 0;
	printf("Creating config file in %s\n", expath);
	printf("Input location for database: ");
	
	size_t read = 0;
	read = getline(&dbfile, &read, stdin);

	if(read == -1) {
		goto err; 
	}

	dbfile[read-1] = '\0';

	cfg.dbfname = dbfile;

	free(expath);

	if(write_cfg() != SUCCESS) {
		goto err;
	}

	return SUCCESS;

err:
	free(expath);
	free(dbfile);

	return IO_ERR;
}

#define checkerr() if(rc != SUCCESS) goto err

char* errmessage = 0;

int main(int argv, char** argc) {
	if(argv < 2) {
		usage();
		return -1;
	}

	int (*cmd)(dbfile_t*, int, char**) = 0;
	int i, rc;
	dbfile_t dbf;

	rc = load_cfg();
	if(rc != SUCCESS && rc != NO_CFG) {
		goto err;
	}
	if(rc == NO_CFG) {
		rc = create_config();
		checkerr();
	}

	for(i = 0; i < sizeof(builtins)/sizeof(*builtins); i++) {
		if(strcmp(argc[1], builtins[i].name) == 0) {
			cmd = builtins[i].fun;
		}
	}

	if(cmd == 0) {
		cmd_unfound();
		return -1;
	}

	rc = load_database(&dbf);
	checkerr();

	rc = (*cmd)(&dbf, argv-1, argc+1);
	checkerr();

	if(dbf.modified) {
		rc = write_database(&dbf);
		if(rc != SUCCESS) {
			goto err;
		}
		if(isatty(STDOUT_FILENO)) {
			puts("Database written to file");
		}
	}

	clear_dbf_v00(&dbf);

	free(cfg.dbfname);

	return EXIT_SUCCESS;

err:
	fprintf(stderr, "%s", "Error: ");
	switch(rc) {
	case ALLOC_FAIL: fprintf(stderr, "%s\n", "Could not allocate memory, is the system out of memory?")             ; break;
	case PW_EXISTS:  fprintf(stderr, "%s\n", "A password already exists under this name in the database.")          ; break;
	case PW_NEXISTS: fprintf(stderr, "%s\n", "No password was found under this name in the database.")              ; break;
	case DB_FULL:    fprintf(stderr, "%s\n", "The database is full.")                                               ; break;
	case WRITE_ERR:  fprintf(stderr, "%s\n", "Could not write to file.  Is the given address valid?.")              ; break;
	case READ_ERR:   fprintf(stderr, "%s\n", "Could not read database/config from file.  Is the listed file valid?"); break;
	case INV_FILE:   fprintf(stderr, "%s\n", "Database file was not properly formatted or the password was wrong.") ; break;
	case NO_CFG:     fprintf(stderr, "%s\n", "No config file was found at $HOME/.spass.conf.")                      ; break;
	case INV_CFG:    fprintf(stderr, "%s\n", "Invalid config file, fix it or delete it to regenerate.")             ; break;
	case IO_ERR:     fprintf(stderr, "%s\n", "There was an error using stdin/out.")                                 ; break;
	case TOO_LONG:   fprintf(stderr, "%s\n", "Parameter was too large.")                                            ; break;
	case INV_ARG:    fprintf(stderr, "%s\n", "Invalid argument to function.")                                       ; break;
	}

	if(errmessage) {
		fprintf(stderr, "%s\n", errmessage);
	}

	return EXIT_FAILURE;
}

