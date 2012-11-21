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

/* These are always necessary for a bgworker */
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/shmem.h"

/* these headers are used by this particular worker's code */
#include "libpq/auth.h"
#include "utils/guc.h"
#include "utils/timestamp.h"

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
	int			save_errno = errno;

	terminate = true;
	if (MyProc)
		SetLatch(&MyProc->procLatch);

	errno = save_errno;
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
	terminate = false;

	/* We're now ready to receive signals */
	BackgroundWorkerUnblockSignals();

	/*
	 * Initialize counter variables
	 */
	LWLockAcquire(ac->lock, LW_EXCLUSIVE);
	ac->success = 0;
	ac->failure = 0;
	LWLockRelease(ac->lock);

	while (!terminate)
	{
		long	n_success;
		long	n_failed;
		int		rc;

		/*
		 * Background workers mustn't call sleep or any equivalent: instead,
		 * they wait on their process latch, which sleeps if necessary, but is
		 * awakened if postmaster dies.  That way the background process goes
		 * away immediately in an emergency.
		 */
		rc = WaitLatch(&MyProc->procLatch,
					   WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH,
					   auth_counter_interval * 1000L);
		ResetLatch(&MyProc->procLatch);

		/* emergency bailout if postmaster has died */
		if (rc & WL_POSTMASTER_DEATH)
			proc_exit(1);

		LWLockAcquire(ac->lock, LW_EXCLUSIVE);
		n_success = ac->success;
		n_failed  = ac->failure;
		LWLockRelease(ac->lock);

		elog(LOG, "%s (%d) %lu logins successful, %lu failed logins - %s",
			 WORKER_NAME, MyProcPid, n_success, n_failed,
			 timestamptz_to_str(GetCurrentTimestamp()));
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

	/* install our client auth hook */
	original_client_auth_hook = ClientAuthentication_hook;
	ClientAuthentication_hook = auth_counter_check;

	/* register the worker process */
	worker.bgw_name = WORKER_NAME;
	worker.bgw_flags = BGWORKER_SHMEM_ACCESS;
	worker.bgw_start_time = BgWorkerStart_ConsistentState;
	worker.bgw_main = auth_counter_main;
	worker.bgw_main_arg = NULL;
	worker.bgw_sighup = auth_counter_sigterm;
	worker.bgw_sigterm = auth_counter_sigterm;

	RegisterBackgroundWorker(&worker);
}
