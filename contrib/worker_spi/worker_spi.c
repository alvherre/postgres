/* -------------------------------------------------------------------------
 *
 * worker_spi.c
 *
 * Copyright (C) 2012, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		contrib/worker_spi/worker_spi.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>

#include "access/xact.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "libpq/pqsignal.h"
#include "postmaster/bgworker.h"
#include "storage/ipc.h"
#include "utils/snapmgr.h"

PG_MODULE_MAGIC;

void	_PG_init(void);

static bool	got_sigterm = false;

static void
worker_spi_sigterm(SIGNAL_ARGS)
{
	got_sigterm = true;
}

static void
worker_spi_sighup(SIGNAL_ARGS)
{
	elog(LOG, "got sighup!");
}

static void
initialize_worker_spi(char *tabname)
{
	int		ret;
	int		ntup;
	bool	isnull;
	StringInfoData	buf;

	StartTransactionCommand();
	SPI_connect();
	PushActiveSnapshot(GetTransactionSnapshot());

	initStringInfo(&buf);
	appendStringInfo(&buf, "select count(*) from pg_namespace where nspname = '%s'", tabname);

	ret = SPI_execute(buf.data, true, 0);
	if (ret != SPI_OK_SELECT)
		elog(FATAL, "SPI_execute failed: error code %d", ret);

	if (SPI_processed != 1)
		elog(FATAL, "not a singleton result");

	ntup = DatumGetInt32(SPI_getbinval(SPI_tuptable->vals[0],
									   SPI_tuptable->tupdesc,
									   1, &isnull));
	if (isnull)
		elog(FATAL, "null result");

	elog(LOG, "pg_namespace has %d tuples for nspname = '%s'", ntup, tabname);

	if (ntup == 0)
	{
		resetStringInfo(&buf);
		appendStringInfo(&buf,
						 "create schema \"%s\" "
						 "create table \"%s\" (type	text, "
						 "value	int)", tabname, tabname);

		ret = SPI_execute(buf.data, false, 0);

		if (ret != SPI_OK_UTILITY)
			elog(FATAL, "failed to create my schema");
	}

	SPI_finish();
	PopActiveSnapshot();
	CommitTransactionCommand();
}

static void
worker_spi_main(void *main_arg)
{
	char		   *tabname;
	StringInfoData		buf;

	tabname = (char *) main_arg;

	/* Unblock signals (they were blocked when the postmaster forked us) */
    PG_SETMASK(&UnBlockSig);

	BackgroundWorkerInitializeConnection("alvherre", NULL);

	initialize_worker_spi(tabname);

	initStringInfo(&buf);
	appendStringInfo(&buf,
					 "WITH deleted AS (DELETE "
		"FROM %s.%s "
		"WHERE type = 'delta' RETURNING value), "
					 "total AS (SELECT coalesce(sum(value), 0) as sum "
							   "FROM deleted) "
					 "UPDATE %s.%s "
					 "SET value = %s.value + total.sum "
					 "FROM total WHERE type = 'total' "
					 "RETURNING %s.value", tabname, tabname, tabname, tabname, tabname, tabname);

	while (!got_sigterm)
	{
		int		ret;

		pg_usleep(1000 * 1000 * 10);	/* 10s */

		StartTransactionCommand();
		SPI_connect();
		PushActiveSnapshot(GetTransactionSnapshot());

		ret = SPI_execute(buf.data, false, 0);

		if (ret != SPI_OK_UPDATE_RETURNING)
			elog(FATAL, "cannot select from table %s: error code %d", tabname, ret);

		if (SPI_processed > 0)
		{
			bool	isnull;
			int32	val;

			val = DatumGetInt32(SPI_getbinval(SPI_tuptable->vals[0],
											   SPI_tuptable->tupdesc,
											   1, &isnull));
			if (!isnull)
				elog(LOG, "count is now %d", val);
		}

		SPI_finish();
		PopActiveSnapshot();
		CommitTransactionCommand();
	}

	proc_exit(0);
}

/*
 * Entrypoint of this module
 */
void
_PG_init(void)
{
	BackgroundWorker		worker;

	/* register the worker process */
	worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
	worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
	worker.bgw_main = worker_spi_main;
	worker.bgw_sighup = worker_spi_sighup;
	worker.bgw_sigterm = worker_spi_sigterm;

	worker.bgw_name = "SPI worker 1";
	worker.bgw_restart_time = BGW_NEVER_RESTART;
	worker.bgw_main_arg = "table1";
	RegisterBackgroundWorker(&worker);

	worker.bgw_name = "SPI worker 2";
	worker.bgw_restart_time = INT_MIN;
	worker.bgw_main_arg = "table2";
	RegisterBackgroundWorker(&worker);
}
