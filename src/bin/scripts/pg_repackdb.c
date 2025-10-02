/*-------------------------------------------------------------------------
 *
 * pg_repackdb
 *		An utility to run REPACK
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * FIXME: this is missing a way to specify the index to use to repack one
 * table, or whether to pass a WITH INDEX clause when multiple tables are
 * used.  Something like --index[=indexname].  Adding that bleeds into
 * vacuuming.c as well.
 *
 * src/bin/scripts/pg_repackdb.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <limits.h>

#include "common.h"
#include "common/logging.h"
#include "fe_utils/option_utils.h"
#include "vacuuming.h"

static void help(const char *progname);
static void check_objfilter(bits32 objfilter);

int
main(int argc, char *argv[])
{
	static struct option long_options[] = {
		{"host", required_argument, NULL, 'h'},
		{"port", required_argument, NULL, 'p'},
		{"username", required_argument, NULL, 'U'},
		{"no-password", no_argument, NULL, 'w'},
		{"password", no_argument, NULL, 'W'},
		{"echo", no_argument, NULL, 'e'},
		{"quiet", no_argument, NULL, 'q'},
		{"dbname", required_argument, NULL, 'd'},
		{"analyze", no_argument, NULL, 'z'},
		{"all", no_argument, NULL, 'a'},
		/* XXX this could be 'i', but optional_arg is messy */
		{"index", optional_argument, NULL, 1},
		{"table", required_argument, NULL, 't'},
		{"verbose", no_argument, NULL, 'v'},
		{"jobs", required_argument, NULL, 'j'},
		{"schema", required_argument, NULL, 'n'},
		{"exclude-schema", required_argument, NULL, 'N'},
		{"maintenance-db", required_argument, NULL, 2},
		{NULL, 0, NULL, 0}
	};

	const char *progname;
	int			optindex;
	int			c;
	const char *dbname = NULL;
	const char *maintenance_db = NULL;
	ConnParams	cparams;
	bool		echo = false;
	bool		quiet = false;
	vacuumingOptions vacopts;
	SimpleStringList objects = {NULL, NULL};
	int			concurrentCons = 1;
	int			tbl_count = 0;
	int			ret;

	/* initialize options */
	memset(&vacopts, 0, sizeof(vacopts));
	vacopts.mode = MODE_REPACK;

	/* the same for connection parameters */
	memset(&cparams, 0, sizeof(cparams));
	cparams.prompt_password = TRI_DEFAULT;

	pg_logging_init(argv[0]);
	progname = get_progname(argv[0]);
	set_pglocale_pgservice(argv[0], PG_TEXTDOMAIN("pgscripts"));

	handle_help_version_opts(argc, argv, progname, help);

	while ((c = getopt_long(argc, argv, "ad:eh:j:n:N:p:qt:U:vwWz",
							long_options, &optindex)) != -1)
	{
		switch (c)
		{
			case 'a':
				vacopts.objfilter |= OBJFILTER_ALL_DBS;
				break;
			case 'd':
				vacopts.objfilter |= OBJFILTER_DATABASE;
				dbname = pg_strdup(optarg);
				break;
			case 'e':
				echo = true;
				break;
			case 'h':
				cparams.pghost = pg_strdup(optarg);
				break;
			case 'j':
				if (!option_parse_int(optarg, "-j/--jobs", 1, INT_MAX,
									  &concurrentCons))
					exit(1);
				break;
			case 'n':
				vacopts.objfilter |= OBJFILTER_SCHEMA;
				simple_string_list_append(&objects, optarg);
				break;
			case 'N':
				vacopts.objfilter |= OBJFILTER_SCHEMA_EXCLUDE;
				simple_string_list_append(&objects, optarg);
				break;
			case 'p':
				cparams.pgport = pg_strdup(optarg);
				break;
			case 'q':
				quiet = true;
				break;
			case 't':
				vacopts.objfilter |= OBJFILTER_TABLE;
				simple_string_list_append(&objects, optarg);
				tbl_count++;
				break;
			case 'U':
				cparams.pguser = pg_strdup(optarg);
				break;
			case 'v':
				vacopts.verbose = true;
				break;
			case 'w':
				cparams.prompt_password = TRI_NO;
				break;
			case 'W':
				cparams.prompt_password = TRI_YES;
				break;
			case 'z':
				vacopts.and_analyze = true;
				break;
			case 1:
				vacopts.using_index = true;
				if (optarg)
					vacopts.indexname = pg_strdup(optarg);
				else
					vacopts.indexname = NULL;
				break;
			case 2:
				maintenance_db = pg_strdup(optarg);
				break;
			default:
				/* getopt_long already emitted a complaint */
				pg_log_error_hint("Try \"%s --help\" for more information.", progname);
				exit(1);
		}
	}

	/*
	 * Non-option argument specifies database name as long as it wasn't
	 * already specified with -d / --dbname
	 */
	if (optind < argc && dbname == NULL)
	{
		vacopts.objfilter |= OBJFILTER_DATABASE;
		dbname = argv[optind];
		optind++;
	}

	if (optind < argc)
	{
		pg_log_error("too many command-line arguments (first is \"%s\")",
					 argv[optind]);
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}

	/*
	 * Validate the combination of filters specified in the command-line
	 * options.
	 */
	check_objfilter(vacopts.objfilter);

	ret = vacuuming_main(&cparams, dbname, maintenance_db, &vacopts,
						 &objects, tbl_count, concurrentCons,
						 progname, echo, quiet);
	exit(ret);
}

/*
 * Verify that the filters used at command line are compatible.
 */
void
check_objfilter(bits32 objfilter)
{
	if ((objfilter & OBJFILTER_ALL_DBS) &&
		(objfilter & OBJFILTER_DATABASE))
		pg_fatal("cannot repack all databases and a specific one at the same time");

	if ((objfilter & OBJFILTER_TABLE) &&
		(objfilter & OBJFILTER_SCHEMA))
		pg_fatal("cannot repack all tables in schema(s) and specific table(s) at the same time");

	if ((objfilter & OBJFILTER_TABLE) &&
		(objfilter & OBJFILTER_SCHEMA_EXCLUDE))
		pg_fatal("cannot repack specific table(s) and exclude schema(s) at the same time");

	if ((objfilter & OBJFILTER_SCHEMA) &&
		(objfilter & OBJFILTER_SCHEMA_EXCLUDE))
		pg_fatal("cannot repack all tables in schema(s) and exclude schema(s) at the same time");
}

static void
help(const char *progname)
{
	printf(_("%s repacks a PostgreSQL database.\n\n"), progname);
	printf(_("Usage:\n"));
	printf(_("  %s [OPTION]... [DBNAME]\n"), progname);
	printf(_("\nOptions:\n"));
	printf(_("  -a, --all                       repack all databases\n"));
	printf(_("  -d, --dbname=DBNAME             database to repack\n"));
	printf(_("  -e, --echo                      show the commands being sent to the server\n"));
	printf(_("      --index[=INDEX]             repack following an index\n"));
	printf(_("  -j, --jobs=NUM                  use this many concurrent connections to repack\n"));
	printf(_("  -n, --schema=SCHEMA             repack tables in the specified schema(s) only\n"));
	printf(_("  -N, --exclude-schema=SCHEMA     do not repack tables in the specified schema(s)\n"));
	printf(_("  -q, --quiet                     don't write any messages\n"));
	printf(_("  -t, --table='TABLE'             repack specific table(s) only\n"));
	printf(_("  -v, --verbose                   write a lot of output\n"));
	printf(_("  -V, --version                   output version information, then exit\n"));
	printf(_("  -z, --analyze                   update optimizer statistics\n"));
	printf(_("  -?, --help                      show this help, then exit\n"));
	printf(_("\nConnection options:\n"));
	printf(_("  -h, --host=HOSTNAME       database server host or socket directory\n"));
	printf(_("  -p, --port=PORT           database server port\n"));
	printf(_("  -U, --username=USERNAME   user name to connect as\n"));
	printf(_("  -w, --no-password         never prompt for password\n"));
	printf(_("  -W, --password            force password prompt\n"));
	printf(_("  --maintenance-db=DBNAME   alternate maintenance database\n"));
	printf(_("\nRead the description of the SQL command REPACK for details.\n"));
	printf(_("\nReport bugs to <%s>.\n"), PACKAGE_BUGREPORT);
	printf(_("%s home page: <%s>\n"), PACKAGE_NAME, PACKAGE_URL);
}
