/* -------------------------------------------------------------------------
 *
 * auth_counter.c
 *
 * Copyright (C) 2012, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		contrib/auth_counter/auth_counter.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "libpq/auth.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "storage/smgr.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/resowner.h"
#include "utils/timestamp.h"
#include <unistd.h>

PG_MODULE_MAGIC;

#define WORKER_NAME "auth counter"

void	_PG_init(void);

/* GUC variable */
static int	auth_counter_interval;

/* Original hooks */
static ClientAuthentication_hook_type	original_client_auth_hook;
static shmem_startup_hook_type			shmem_startup_hook_next;

/* shared memory state */
typedef struct AuthCounterState
{
	LWLockId		lock;
	long			success;
	long			failure;
} AuthCounterState;

static AuthCounterState *ac;

/* State to be changed from signal handler */
static volatile bool	terminate;

static void
auth_counter_sigterm(SIGNAL_ARGS)
{
	terminate = true;
}

/*
 * auth_counter_main
 *
 * The main routine of this bgworker: logs the number of successful and failed
 * authentication for each intervals, until receiving a signal.
 */
static void
auth_counter_main(void *args)
{
	MemoryContext	auth_counter_context;

	terminate = false;

	/* We're now ready to receive signals */
	BackgroundWorkerUnblockSignals();
	/* Create a resource owner to keep track of our resources */
	CurrentResourceOwner = ResourceOwnerCreate(NULL, WORKER_NAME);

	/*
	 * Create a memory context that we will do all our work in.  We do this so
	 * that we can reset the context whenever it suit us, to avoid possible
	 * memory leaks.
	 */
	auth_counter_context = AllocSetContextCreate(TopMemoryContext,
												 WORKER_NAME,
												 ALLOCSET_DEFAULT_MINSIZE,
												 ALLOCSET_DEFAULT_INITSIZE,
												 ALLOCSET_DEFAULT_MAXSIZE);
	MemoryContextSwitchTo(auth_counter_context);


	/*
	 * Init counter variables
	 */
	LWLockAcquire(ac->lock, LW_EXCLUSIVE);
	ac->success = 0;
	ac->failure = 0;
	LWLockRelease(ac->lock);

	while (!terminate)
	{
		Datum	tstamp;
		long	n_success;
		long	n_failed;

		pg_usleep((long) auth_counter_interval * 1000000L);

		LWLockAcquire(ac->lock, LW_EXCLUSIVE);
		n_success = ac->success;
		n_failed  = ac->failure;
		LWLockRelease(ac->lock);

		tstamp = DirectFunctionCall1(timestamptz_out,
							TimestampTzGetDatum(GetCurrentTimestamp()));

		elog(LOG, "%s (%d) %lu logins successful, %lu failed logins - %s",
			 WORKER_NAME, MyProcPid, n_success, n_failed,
			 DatumGetCString(tstamp));

		/* clear temporary memory objects */
		MemoryContextReset(auth_counter_context);
	}

	proc_exit(0);
}


/*
 * auth_counter_check
 *
 * It increments the counter variable for each client authentication
 */
static void
auth_counter_check(Port *port, int status)
{
	if (original_client_auth_hook)
		original_client_auth_hook(port, status);

	LWLockAcquire(ac->lock, LW_EXCLUSIVE);
	if (status == STATUS_OK)
		ac->success++;
	else
		ac->failure++;
	LWLockRelease(ac->lock);
}

/*
 * Callback just after shared memory allocation
 */
static void
auth_counter_shmem_startup(void)
{
	bool	found;

	elog(LOG, "auth_counter shmem_startup");

	if (shmem_startup_hook_next)
		shmem_startup_hook_next();

	/* reset in case this is a restart within the postmaster */
	ac = NULL;

	/*
	 * Allocate (or reattach to) the shared memory we need.  Holding the
	 * AddinShmemInitLock is important for this, to avoid multiple processes
	 * from doing it concurrently.
	 */
	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);
	ac = ShmemInitStruct(WORKER_NAME,
							   2 * sizeof(long),
							   &found);

	if (!found)
	{
		/* first time through: allocate lwlock to protect counters */
		ac->lock = LWLockAssign();
	}

	/* Global shared initialization is complete; release this lock */
	LWLockRelease(AddinShmemInitLock);

	/*
	 * Now we can reset the module state, if appropriate.  This can be done
	 * holding only the module's own lock.
	 */
	if (!found)
	{
		LWLockAcquire(ac->lock, LW_EXCLUSIVE);

		/* reset counters */
		ac->success = 0;
		ac->failure = 0;

		LWLockRelease(ac->lock);
	}
}

/*
 * Entrypoint of this module
 */
void
_PG_init(void)
{
	BackgroundWorker		worker;

	DefineCustomIntVariable("auth_counter.interval",
							"Interval to display number of logins",
							NULL,
							&auth_counter_interval,
							10,				/* 1 minute (default) */
							5,				/* 5 seconds */
							24 * 60 * 60,	/* 1 day*/
							PGC_SIGHUP,
							GUC_UNIT_S,
							NULL, NULL, NULL);

	/* request shared memory and lock */
	RequestAddinShmemSpace(2 * sizeof(long));
	RequestAddinLWLocks(1);

	shmem_startup_hook_next = shmem_startup_hook;
	shmem_startup_hook = auth_counter_shmem_startup;

	/* install a hook */
	original_client_auth_hook = ClientAuthentication_hook;
	ClientAuthentication_hook = auth_counter_check;

	/* register the worker process */
	worker.bgw_name = WORKER_NAME;
	worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
	worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
	worker.bgw_main = auth_counter_main;
	worker.bgw_main_arg = NULL;
	worker.bgw_sighup = auth_counter_sigterm;
	worker.bgw_sigterm = auth_counter_sigterm;

	RegisterBackgroundWorker(&worker);
}
