/*-------------------------------------------------------------------------
 *
 * vacuuming.h
 *		Common declarations for vacuuming.c
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/scripts/vacuuming.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef VACUUMING_H
#define VACUUMING_H

#include "common.h"
#include "fe_utils/connect_utils.h"
#include "fe_utils/simple_list.h"

/* For analyze-in-stages mode */
#define ANALYZE_NO_STAGE	-1
#define ANALYZE_NUM_STAGES	3

/* vacuum options controlled by user flags */
typedef struct vacuumingOptions
{
	bool		analyze_only;
	bool		verbose;
	bool		and_analyze;
	bool		full;
	bool		freeze;
	bool		disable_page_skipping;
	bool		skip_locked;
	int			min_xid_age;
	int			min_mxid_age;
	int			parallel_workers;	/* >= 0 indicates user specified the
									 * parallel degree, otherwise -1 */
	bool		no_index_cleanup;
	bool		force_index_cleanup;
	bool		do_truncate;
	bool		process_main;
	bool		process_toast;
	bool		skip_database_stats;
	char	   *buffer_usage_limit;
	bool		missing_stats_only;
} vacuumingOptions;

/* object filter options */
typedef enum
{
	OBJFILTER_NONE = 0,			/* no filter used */
	OBJFILTER_ALL_DBS = (1 << 0),	/* -a | --all */
	OBJFILTER_DATABASE = (1 << 1),	/* -d | --dbname */
	OBJFILTER_TABLE = (1 << 2), /* -t | --table */
	OBJFILTER_SCHEMA = (1 << 3),	/* -n | --schema */
	OBJFILTER_SCHEMA_EXCLUDE = (1 << 4),	/* -N | --exclude-schema */
} VacObjFilter;

extern VacObjFilter objfilter;

extern void vacuuming_main(ConnParams *cparams, const char *dbname,
						   const char *maintenance_db, vacuumingOptions *vacopts,
						   SimpleStringList *objects, bool analyze_in_stages,
						   int tbl_count, int concurrentCons,
						   const char *progname, bool echo, bool quiet);

extern SimpleStringList *retrieve_objects(PGconn *conn,
										  vacuumingOptions *vacopts,
										  SimpleStringList *objects,
										  bool echo);

extern void vacuum_one_database(ConnParams *cparams,
								vacuumingOptions *vacopts,
								int stage,
								SimpleStringList *objects,
								SimpleStringList **found_objs,
								int concurrentCons,
								const char *progname, bool echo, bool quiet);

extern void vacuum_all_databases(ConnParams *cparams,
								 vacuumingOptions *vacopts,
								 bool analyze_in_stages,
								 SimpleStringList *objects,
								 int concurrentCons,
								 const char *progname, bool echo, bool quiet);

extern void prepare_vacuum_command(PQExpBuffer sql, int serverVersion,
								   vacuumingOptions *vacopts, const char *table);

extern void run_vacuum_command(PGconn *conn, const char *sql, bool echo,
							   const char *table);

extern char *escape_quotes(const char *src);

#endif							/* VACUUMING_H */
