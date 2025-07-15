/*-------------------------------------------------------------------------
 *
 * proctype_list.h
 *
 * The list of process types is kept on its own source file for use by
 * automatic tools.  The exact representation of a process type is
 * determined by the PG_PROCTYPE macro, which is not defined in this
 * file; it can be defined by the caller for special purposes.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/include/postmaster/proctype_list.h
 *
 *-------------------------------------------------------------------------
 */

/* there is deliberately not an #ifndef PROCTYPE_LIST_H here */

/*
 * List of process types (symbol, description, Main function, shmem_attach)
 * entries.
 */

/* bktype, description, main_func, shmem_attach */
PG_PROCTYPE(B_ARCHIVER, "archiver", PgArchiverMain, true)
PG_PROCTYPE(B_AUTOVAC_LAUNCHER, "autovacuum launcher", AutoVacLauncherMain, true)
PG_PROCTYPE(B_AUTOVAC_WORKER, "autovacuum worker", AutoVacWorkerMain, true)
PG_PROCTYPE(B_BACKEND, "client backend", BackendMain, true)
PG_PROCTYPE(B_BG_WORKER, "background worker", BackgroundWorkerMain, true)
PG_PROCTYPE(B_BG_WRITER, "background writer", BackgroundWriterMain, true)
PG_PROCTYPE(B_CHECKPOINTER, "checkpointer", CheckpointerMain, true)
PG_PROCTYPE(B_DEAD_END_BACKEND, "dead-end client backend", BackendMain, true)
PG_PROCTYPE(B_INVALID, "unrecognized", NULL, false)
PG_PROCTYPE(B_IO_WORKER, "io worker", IoWorkerMain, true)
PG_PROCTYPE(B_LOGGER, "syslogger", SysLoggerMain, false)
PG_PROCTYPE(B_SLOTSYNC_WORKER, "slotsync worker", ReplSlotSyncWorkerMain, true)
PG_PROCTYPE(B_STANDALONE_BACKEND, "standalone backend", NULL, false)
PG_PROCTYPE(B_STARTUP, "startup", StartupProcessMain, true)
PG_PROCTYPE(B_WAL_RECEIVER, "walreceiver", WalReceiverMain, true)
PG_PROCTYPE(B_WAL_SENDER, "walsender", NULL, true)
PG_PROCTYPE(B_WAL_SUMMARIZER, "walsummarizer", WalSummarizerMain, true)
PG_PROCTYPE(B_WAL_WRITER, "walwriter", WalWriterMain, true)
