/*-------------------------------------------------------------------------
 *
 * cluster.c
 *	  CLUSTER a table on an index.  This is now also used for VACUUM FULL.
 *
 * There is hardly anything left of Paul Brown's original implementation...
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994-5, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/cluster.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/amapi.h"
#include "access/heapam.h"
#include "access/multixact.h"
#include "access/relscan.h"
#include "access/tableam.h"
#include "access/toast_internals.h"
#include "access/transam.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "access/xlog_internal.h"
#include "access/xloginsert.h"
#include "access/xlogutils.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/heap.h"
#include "catalog/index.h"
#include "catalog/namespace.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_am.h"
#include "catalog/pg_control.h"
#include "catalog/pg_inherits.h"
#include "catalog/toasting.h"
#include "commands/cluster.h"
#include "commands/defrem.h"
#include "commands/progress.h"
#include "commands/tablecmds.h"
#include "commands/vacuum.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "optimizer/optimizer.h"
#include "pgstat.h"
#include "replication/decode.h"
#include "replication/logical.h"
#include "replication/snapbuild.h"
#include "storage/bufmgr.h"
#include "storage/ipc.h"
#include "storage/lmgr.h"
#include "storage/predicate.h"
#include "utils/acl.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/pg_rusage.h"
#include "utils/relmapper.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

/*
 * This struct is used to pass around the information on tables to be
 * clustered. We need this so we can make a list of them when invoked without
 * a specific table/index pair.
 */
typedef struct
{
	Oid			tableOid;
	Oid			indexOid;
} RelToCluster;

/*
 * The following definitions are used for concurrent processing.
 */

/*
 * The locators are used to avoid logical decoding of data that we do not need
 * for our table.
 */
RelFileLocator repacked_rel_locator = {.relNumber = InvalidOid};
RelFileLocator repacked_rel_toast_locator = {.relNumber = InvalidOid};

/*
 * Everything we need to call ExecInsertIndexTuples().
 */
typedef struct IndexInsertState
{
	ResultRelInfo *rri;
	EState	   *estate;

	Relation	ident_index;
} IndexInsertState;

/* The WAL segment being decoded. */
static XLogSegNo repack_current_segment = 0;


static bool cluster_rel_recheck(RepackCommand cmd, Relation OldHeap,
								Oid indexOid, Oid userid, LOCKMODE lmode,
								int options);
static void check_repack_concurrently_requirements(Relation rel);
static void rebuild_relation(RepackCommand cmd, bool usingindex,
							 Relation OldHeap, Relation index, Oid userid,
							 bool concurrent, bool verbose);
static void copy_table_data(Relation NewHeap, Relation OldHeap, Relation OldIndex,
							Snapshot snapshot, LogicalDecodingContext *decoding_ctx,
							bool verbose,
							bool *pSwapToastByContent,
							TransactionId *pFreezeXid,
							MultiXactId *pCutoffMulti);
static List *get_tables_to_repack(RepackCommand cmd, bool usingindex,
								  MemoryContext permcxt);
static List *get_tables_to_repack_partitioned(RepackCommand cmd,
											  MemoryContext cluster_context,
											  Oid relid, bool rel_is_index);
static bool cluster_is_permitted_for_relation(RepackCommand cmd,
											  Oid relid, Oid userid);

static void begin_concurrent_repack(Relation rel);
static void end_concurrent_repack(void);
static LogicalDecodingContext *setup_logical_decoding(Oid relid,
													  const char *slotname,
													  TupleDesc tupdesc);
static HeapTuple get_changed_tuple(char *change);
static void apply_concurrent_changes(RepackDecodingState *dstate,
									 Relation rel, ScanKey key, int nkeys,
									 IndexInsertState *iistate);
static void apply_concurrent_insert(Relation rel, ConcurrentChange *change,
									HeapTuple tup, IndexInsertState *iistate,
									TupleTableSlot *index_slot);
static void apply_concurrent_update(Relation rel, HeapTuple tup,
									HeapTuple tup_target,
									ConcurrentChange *change,
									IndexInsertState *iistate,
									TupleTableSlot *index_slot);
static void apply_concurrent_delete(Relation rel, HeapTuple tup_target,
									ConcurrentChange *change);
static HeapTuple find_target_tuple(Relation rel, ScanKey key, int nkeys,
								   HeapTuple tup_key,
								   IndexInsertState *iistate,
								   TupleTableSlot *ident_slot,
								   IndexScanDesc *scan_p);
static void process_concurrent_changes(LogicalDecodingContext *ctx,
									   XLogRecPtr end_of_wal,
									   Relation rel_dst,
									   Relation rel_src,
									   ScanKey ident_key,
									   int ident_key_nentries,
									   IndexInsertState *iistate);
static IndexInsertState *get_index_insert_state(Relation relation,
												Oid ident_index_id);
static ScanKey build_identity_key(Oid ident_idx_oid, Relation rel_src,
								  int *nentries);
static void free_index_insert_state(IndexInsertState *iistate);
static void cleanup_logical_decoding(LogicalDecodingContext *ctx);
static void rebuild_relation_finish_concurrent(Relation NewHeap, Relation OldHeap,
											   Relation cl_index,
											   LogicalDecodingContext *ctx,
											   bool swap_toast_by_content,
											   TransactionId frozenXid,
											   MultiXactId cutoffMulti);
static List *build_new_indexes(Relation NewHeap, Relation OldHeap, List *OldIndexes);
static Relation process_single_relation(RepackStmt *stmt,
										LOCKMODE lockmode,
										bool isTopLevel,
										ClusterParams *params);
static Oid	determine_clustered_index(Relation rel, bool usingindex,
									  const char *indexname);


#define REPL_PLUGIN_NAME   "pgoutput_repack"

static const char *
RepackCommandAsString(RepackCommand cmd)
{
	switch (cmd)
	{
		case REPACK_COMMAND_REPACK:
			return "REPACK";
		case REPACK_COMMAND_VACUUMFULL:
			return "VACUUM";
		case REPACK_COMMAND_CLUSTER:
			return "VACUUM";
	}
	return "???";
}

/*---------------------------------------------------------------------------
 * This cluster code allows for clustering multiple tables at once. Because
 * of this, we cannot just run everything on a single transaction, or we
 * would be forced to acquire exclusive locks on all the tables being
 * clustered, simultaneously --- very likely leading to deadlock.
 *
 * To solve this we follow a similar strategy to VACUUM code,
 * clustering each relation in a separate transaction. For this to work,
 * we need to:
 *	- provide a separate memory context so that we can pass information in
 *	  a way that survives across transactions
 *	- start a new transaction every time a new relation is clustered
 *	- check for validity of the information on to-be-clustered relations,
 *	  as someone might have deleted a relation behind our back, or
 *	  clustered one on a different index
 *	- end the transaction
 *
 * The single-relation case does not have any such overhead.
 *
 * We also allow a relation to be specified without index.  In that case,
 * the indisclustered bit will be looked up, and an ERROR will be thrown
 * if there is no index with the bit set.
 *---------------------------------------------------------------------------
 */
void
ExecRepack(ParseState *pstate, RepackStmt *stmt, bool isTopLevel)
{
	ClusterParams params = {0};
	Relation	rel = NULL;
	MemoryContext repack_context;
	LOCKMODE	lockmode;
	List	   *rtcs;

	/* Parse option list */
	foreach_node(DefElem, opt, stmt->params)
	{
		if (strcmp(opt->defname, "verbose") == 0 &&
			defGetBoolean(opt))
			params.options |= CLUOPT_VERBOSE;
		else if (strcmp(opt->defname, "concurrently") == 0 &&
				 defGetBoolean(opt))
		{
			if (stmt->command != REPACK_COMMAND_REPACK)
				elog(ERROR, "not repack, oops");	/* FIXME reachable? reword */
			params.options |= CLUOPT_CONCURRENT;
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("unrecognized %s option \"%s\"",
							RepackCommandAsString(stmt->command),
							opt->defname),
					 parser_errposition(pstate, opt->location)));
	}

	/*
	 * Determine the lock mode expected by cluster_rel().
	 *
	 * In the exclusive case, we obtain AccessExclusiveLock right away to
	 * avoid lock-upgrade hazard in the single-transaction case. In the
	 * CONCURRENTLY case, the AccessExclusiveLock will only be used at the end
	 * of processing, supposedly for very short time. Until then, we'll have
	 * to unlock the relation temporarily, so there's no lock-upgrade hazard.
	 */
	lockmode = (params.options & CLUOPT_CONCURRENT) == 0 ?
		AccessExclusiveLock : ShareUpdateExclusiveLock;

	/*
	 * If a single relation is specified, process it and we're done ... unless
	 * the relation is a partitioned table, in which case we fall through.
	 */
	if (stmt->relation != NULL)
	{
		rel = process_single_relation(stmt, AccessExclusiveLock, isTopLevel, &params);
		if (rel == NULL)
			return;
	}

	/*
	 * By here, we know we are in a multi-table situation.
	 *
	 * Concurrent processing is currently considered rather special (e.g. in
	 * terms of resources consumed) so it is not performed in bulk.
	 */
	if (params.options & CLUOPT_CONCURRENT)
	{
		if (rel != NULL)
		{
			Assert(rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE);
			ereport(ERROR,
					errmsg("REPACK CONCURRENTLY not supported for partitioned tables"),
					errhint("Consider running the command for individual partitions."));
		}
		else
			ereport(ERROR,
					errmsg("REPACK CONCURRENTLY requires explicit table name"));
	}

	/*
	 * In order to avoid holding locks for too long, we want to process each
	 * table in its own transaction.  This forces us to disallow running
	 * inside a user transaction block.
	 */
	PreventInTransactionBlock(isTopLevel, RepackCommandAsString(stmt->command));

	/* Also, we need a memory context to hold our list of relations */
	repack_context = AllocSetContextCreate(PortalContext,
										   "Repack",
										   ALLOCSET_DEFAULT_SIZES);

	params.options |= CLUOPT_RECHECK;

	/*
	 * If we don't have a relation yet, determine a relation list.  If we do,
	 * then it must be a partitioned table, and we want to process its
	 * partitions.
	 */
	if (rel == NULL)
	{
		Assert(stmt->indexname == NULL);
		rtcs = get_tables_to_repack(stmt->command, stmt->usingindex,
									repack_context);
	}
	else
	{
		Oid			relid;
		bool		rel_is_index;

		Assert(rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE);

		/*
		 * If an index name was specified, resolve it now and pass it down.
		 */
		if (stmt->usingindex)
		{
			/*
			 * XXX how should this behave?  Passing no index to a partitioned
			 * table could be useful to have certain partitions clustered by
			 * some index, and other partitions by a different index.
			 */
			if (!stmt->indexname)
				ereport(ERROR,
						errmsg("there is no previously clustered index for table \"%s\"",
							   RelationGetRelationName(rel)));

			relid = determine_clustered_index(rel, true, stmt->indexname);
			/* XXX is this the right place for this? */
			check_index_is_clusterable(rel, relid, AccessExclusiveLock);
			rel_is_index = true;
		}
		else
		{
			relid = RelationGetRelid(rel);
			rel_is_index = false;
		}

		rtcs = get_tables_to_repack_partitioned(stmt->command, repack_context,
												relid, rel_is_index);

		/* close parent relation, releasing lock on it */
		table_close(rel, AccessExclusiveLock);
		rel = NULL;
	}

	/* Commit to get out of starting transaction */
	PopActiveSnapshot();
	CommitTransactionCommand();

	/* Cluster the tables, each in a separate transaction */
	Assert(rel == NULL);
	foreach_ptr(RelToCluster, rtc, rtcs)
	{
		/* Start a new transaction for each relation. */
		StartTransactionCommand();

		/* functions in indexes may want a snapshot set */
		PushActiveSnapshot(GetTransactionSnapshot());

		/*
		 * Open the target table, coping with the case where it has been
		 * dropped.
		 */
		rel = try_table_open(rtc->tableOid, lockmode);
		if (rel == NULL)
			continue;

		/* Process this table */
		cluster_rel(stmt->command, stmt->usingindex,
					rel, rtc->indexOid, &params, isTopLevel);
		/* cluster_rel closes the relation, but keeps lock */

		PopActiveSnapshot();
		CommitTransactionCommand();
	}

	/* Start a new transaction for the cleanup work. */
	StartTransactionCommand();

	/* Clean up working storage */
	MemoryContextDelete(repack_context);
}

/*
 * cluster_rel
 *
 * This clusters the table by creating a new, clustered table and
 * swapping the relfilenumbers of the new table and the old table, so
 * the OID of the original table is preserved.  Thus we do not lose
 * GRANT, inheritance nor references to this table.
 *
 * Indexes are rebuilt too, via REINDEX. Since we are effectively bulk-loading
 * the new table, it's better to create the indexes afterwards than to fill
 * them incrementally while we load the table.
 *
 * If indexOid is InvalidOid, the table will be rewritten in physical order
 * instead of index order.
 *
 * Note that, in the concurrent case, the function releases the lock at some
 * point, in order to get AccessExclusiveLock for the final steps (i.e. to
 * swap the relation files). To make things simpler, the caller should expect
 * OldHeap to be closed on return, regardless CLUOPT_CONCURRENT. (The
 * AccessExclusiveLock is kept till the end of the transaction.)
 *
 * 'cmd' indicates which command is being executed, to be used for error
 * messages.
 */
void
cluster_rel(RepackCommand cmd, bool usingindex,
			Relation OldHeap, Oid indexOid, ClusterParams *params,
			bool isTopLevel)
{
	Oid			tableOid = RelationGetRelid(OldHeap);
	Oid			save_userid;
	int			save_sec_context;
	int			save_nestlevel;
	bool		verbose = ((params->options & CLUOPT_VERBOSE) != 0);
	bool		recheck = ((params->options & CLUOPT_RECHECK) != 0);
	Relation	index;
	bool		concurrent = ((params->options & CLUOPT_CONCURRENT) != 0);
	LOCKMODE	lmode;

	/*
	 * Check that the correct lock is held. The lock mode is
	 * AccessExclusiveLock for normal processing and ShareUpdateExclusiveLock
	 * for concurrent processing (so that SELECT, INSERT, UPDATE and DELETE
	 * commands work, but cluster_rel() cannot be called concurrently for the
	 * same relation).
	 */
	lmode = !concurrent ? AccessExclusiveLock : ShareUpdateExclusiveLock;

	/* There are specific requirements on concurrent processing. */
	if (concurrent)
	{
		/*
		 * Make sure we have no XID assigned, otherwise call of
		 * setup_logical_decoding() can cause a deadlock.
		 *
		 * The existence of transaction block actually does not imply that XID
		 * was already assigned, but it very likely is. We might want to check
		 * the result of GetCurrentTransactionIdIfAny() instead, but that
		 * would be less clear from user's perspective.
		 */
		PreventInTransactionBlock(isTopLevel, "REPACK CONCURRENTLY");

		check_repack_concurrently_requirements(OldHeap);
	}

	/* Check for user-requested abort. */
	CHECK_FOR_INTERRUPTS();

	if (cmd == REPACK_COMMAND_REPACK)
		pgstat_progress_start_command(PROGRESS_COMMAND_REPACK, tableOid);
	else
		pgstat_progress_start_command(PROGRESS_COMMAND_CLUSTER, tableOid);

	if (cmd == REPACK_COMMAND_REPACK)
		pgstat_progress_update_param(PROGRESS_REPACK_COMMAND,
									 PROGRESS_REPACK_COMMAND_REPACK);
	else if (cmd == REPACK_COMMAND_CLUSTER)
	{
		pgstat_progress_update_param(PROGRESS_REPACK_COMMAND,
									 PROGRESS_CLUSTER_COMMAND_CLUSTER);
	}
	else
	{
		Assert(cmd == REPACK_COMMAND_VACUUMFULL);
		pgstat_progress_update_param(PROGRESS_REPACK_COMMAND,
									 PROGRESS_CLUSTER_COMMAND_VACUUM_FULL);
	}

	/*
	 * Switch to the table owner's userid, so that any index functions are run
	 * as that user.  Also lock down security-restricted operations and
	 * arrange to make GUC variable changes local to this command.
	 */
	GetUserIdAndSecContext(&save_userid, &save_sec_context);
	SetUserIdAndSecContext(OldHeap->rd_rel->relowner,
						   save_sec_context | SECURITY_RESTRICTED_OPERATION);
	save_nestlevel = NewGUCNestLevel();
	RestrictSearchPath();

	/*
	 * Since we may open a new transaction for each relation, we have to check
	 * that the relation still is what we think it is.
	 *
	 * If this is a single-transaction CLUSTER, we can skip these tests. We
	 * *must* skip the one on indisclustered since it would reject an attempt
	 * to cluster a not-previously-clustered index.
	 */
	if (recheck)
		if (!cluster_rel_recheck(cmd, OldHeap, indexOid, save_userid,
								 lmode, params->options))
			goto out;

	/*
	 * We allow repacking shared catalogs only when not using an index. It
	 * would work to use an index in most respects, but the index would only
	 * get marked as indisclustered in the current database, leading to
	 * unexpected behavior if CLUSTER were later invoked in another database.
	 */
	if (usingindex && OldHeap->rd_rel->relisshared)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot run \"%s\" on a shared catalog",
						RepackCommandAsString(cmd))));

	/*
	 * The CONCURRENTLY case should have been rejected earlier because it does
	 * not support system catalogs.
	 */
	Assert(!(OldHeap->rd_rel->relisshared && concurrent));

	/*
	 * Don't process temp tables of other backends ... their local buffer
	 * manager is not going to cope.
	 */
	if (RELATION_IS_OTHER_TEMP(OldHeap))
	{
		if (cmd == REPACK_COMMAND_CLUSTER)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot cluster temporary tables of other sessions")));
		else if (cmd == REPACK_COMMAND_REPACK)
		{
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot repack temporary tables of other sessions")));
		}
		else
		{
			Assert(cmd == REPACK_COMMAND_VACUUMFULL);
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot vacuum temporary tables of other sessions")));
		}
	}

	/*
	 * Also check for active uses of the relation in the current transaction,
	 * including open scans and pending AFTER trigger events.
	 */
	CheckTableNotInUse(OldHeap, RepackCommandAsString(cmd));

	/* Check heap and index are valid to cluster on */
	if (OidIsValid(indexOid))
	{
		/* verify the index is good and lock it */
		check_index_is_clusterable(OldHeap, indexOid, lmode);
		/* also open it */
		index = index_open(indexOid, NoLock);
	}
	else
		index = NULL;

	/*
	 * Quietly ignore the request if this is a materialized view which has not
	 * been populated from its query. No harm is done because there is no data
	 * to deal with, and we don't want to throw an error if this is part of a
	 * multi-relation request -- for example, CLUSTER was run on the entire
	 * database.
	 */
	if (OldHeap->rd_rel->relkind == RELKIND_MATVIEW &&
		!RelationIsPopulated(OldHeap))
	{
		if (index)
			index_close(index, lmode);
		relation_close(OldHeap, lmode);
		goto out;
	}

	Assert(OldHeap->rd_rel->relkind == RELKIND_RELATION ||
		   OldHeap->rd_rel->relkind == RELKIND_MATVIEW ||
		   OldHeap->rd_rel->relkind == RELKIND_TOASTVALUE);

	/*
	 * All predicate locks on the tuples or pages are about to be made
	 * invalid, because we move tuples around.  Promote them to relation
	 * locks.  Predicate locks on indexes will be promoted when they are
	 * reindexed.
	 *
	 * During concurrent processing, the heap as well as its indexes stay in
	 * operation, so we postpone this step until they are locked using
	 * AccessExclusiveLock near the end of the processing.
	 */
	if (!concurrent)
		TransferPredicateLocksToHeapRelation(OldHeap);

	/* rebuild_relation does all the dirty work */
	PG_TRY();
	{
		/*
		 * For concurrent processing, make sure that our logical decoding
		 * ignores data changes of other tables than the one we are
		 * processing.
		 */
		if (concurrent)
			begin_concurrent_repack(OldHeap);

		rebuild_relation(cmd, usingindex, OldHeap, index, save_userid,
						 verbose, concurrent);
	}
	PG_FINALLY();
	{
		if (concurrent)
			end_concurrent_repack();
	}
	PG_END_TRY();

	/* rebuild_relation closes OldHeap, and index if valid */

out:
	/* Roll back any GUC changes executed by index functions */
	AtEOXact_GUC(false, save_nestlevel);

	/* Restore userid and security context */
	SetUserIdAndSecContext(save_userid, save_sec_context);

	pgstat_progress_end_command();
}

/*
 * Check if the table (and its index) still meets the requirements of
 * cluster_rel().
 */
static bool
cluster_rel_recheck(RepackCommand cmd, Relation OldHeap, Oid indexOid,
					Oid userid, LOCKMODE lmode, int options)
{
	Oid			tableOid = RelationGetRelid(OldHeap);

	/* Check that the user still has privileges for the relation */
	if (!cluster_is_permitted_for_relation(cmd, tableOid, userid))
	{
		relation_close(OldHeap, lmode);
		return false;
	}

	/*
	 * Silently skip a temp table for a remote session.  Only doing this check
	 * in the "recheck" case is appropriate (which currently means somebody is
	 * executing a database-wide CLUSTER or on a partitioned table), because
	 * there is another check in cluster() which will stop any attempt to
	 * cluster remote temp tables by name.  There is another check in
	 * cluster_rel which is redundant, but we leave it for extra safety.
	 */
	if (RELATION_IS_OTHER_TEMP(OldHeap))
	{
		relation_close(OldHeap, lmode);
		return false;
	}

	if (OidIsValid(indexOid))
	{
		/*
		 * Check that the index still exists
		 */
		if (!SearchSysCacheExists1(RELOID, ObjectIdGetDatum(indexOid)))
		{
			relation_close(OldHeap, lmode);
			return false;
		}

		/*
		 * Check that the index is still the one with indisclustered set, if
		 * needed.
		 */
		if ((options & CLUOPT_RECHECK_ISCLUSTERED) != 0 &&
			!get_index_isclustered(indexOid))
		{
			relation_close(OldHeap, lmode);
			return false;
		}
	}

	return true;
}

/*
 * Verify that the specified heap and index are valid to cluster on
 *
 * Side effect: obtains lock on the index.  The caller may
 * in some cases already have AccessExclusiveLock on the table, but
 * not in all cases so we can't rely on the table-level lock for
 * protection here.
 */
void
check_index_is_clusterable(Relation OldHeap, Oid indexOid, LOCKMODE lockmode)
{
	Relation	OldIndex;

	OldIndex = index_open(indexOid, lockmode);

	/*
	 * Check that index is in fact an index on the given relation
	 */
	if (OldIndex->rd_index == NULL ||
		OldIndex->rd_index->indrelid != RelationGetRelid(OldHeap))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not an index for table \"%s\"",
						RelationGetRelationName(OldIndex),
						RelationGetRelationName(OldHeap))));

	/* Index AM must allow clustering */
	if (!OldIndex->rd_indam->amclusterable)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot cluster on index \"%s\" because access method does not support clustering",
						RelationGetRelationName(OldIndex))));

	/*
	 * Disallow clustering on incomplete indexes (those that might not index
	 * every row of the relation).  We could relax this by making a separate
	 * seqscan pass over the table to copy the missing rows, but that seems
	 * expensive and tedious.
	 */
	if (!heap_attisnull(OldIndex->rd_indextuple, Anum_pg_index_indpred, NULL))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot cluster on partial index \"%s\"",
						RelationGetRelationName(OldIndex))));

	/*
	 * Disallow if index is left over from a failed CREATE INDEX CONCURRENTLY;
	 * it might well not contain entries for every heap row, or might not even
	 * be internally consistent.  (But note that we don't check indcheckxmin;
	 * the worst consequence of following broken HOT chains would be that we
	 * might put recently-dead tuples out-of-order in the new table, and there
	 * is little harm in that.)
	 */
	if (!OldIndex->rd_index->indisvalid)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot cluster on invalid index \"%s\"",
						RelationGetRelationName(OldIndex))));

	/* Drop relcache refcnt on OldIndex, but keep lock */
	index_close(OldIndex, NoLock);
}

/*
 * mark_index_clustered: mark the specified index as the one clustered on
 *
 * With indexOid == InvalidOid, will mark all indexes of rel not-clustered.
 */
void
mark_index_clustered(Relation rel, Oid indexOid, bool is_internal)
{
	HeapTuple	indexTuple;
	Form_pg_index indexForm;
	Relation	pg_index;
	ListCell   *index;

	/* Disallow applying to a partitioned table */
	if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot mark index clustered in partitioned table")));

	/*
	 * If the index is already marked clustered, no need to do anything.
	 */
	if (OidIsValid(indexOid))
	{
		if (get_index_isclustered(indexOid))
			return;
	}

	/*
	 * Check each index of the relation and set/clear the bit as needed.
	 */
	pg_index = table_open(IndexRelationId, RowExclusiveLock);

	foreach(index, RelationGetIndexList(rel))
	{
		Oid			thisIndexOid = lfirst_oid(index);

		indexTuple = SearchSysCacheCopy1(INDEXRELID,
										 ObjectIdGetDatum(thisIndexOid));
		if (!HeapTupleIsValid(indexTuple))
			elog(ERROR, "cache lookup failed for index %u", thisIndexOid);
		indexForm = (Form_pg_index) GETSTRUCT(indexTuple);

		/*
		 * Unset the bit if set.  We know it's wrong because we checked this
		 * earlier.
		 */
		if (indexForm->indisclustered)
		{
			indexForm->indisclustered = false;
			CatalogTupleUpdate(pg_index, &indexTuple->t_self, indexTuple);
		}
		else if (thisIndexOid == indexOid)
		{
			/* this was checked earlier, but let's be real sure */
			if (!indexForm->indisvalid)
				elog(ERROR, "cannot cluster on invalid index %u", indexOid);
			indexForm->indisclustered = true;
			CatalogTupleUpdate(pg_index, &indexTuple->t_self, indexTuple);
		}

		InvokeObjectPostAlterHookArg(IndexRelationId, thisIndexOid, 0,
									 InvalidOid, is_internal);

		heap_freetuple(indexTuple);
	}

	table_close(pg_index, RowExclusiveLock);
}

/*
 * Check if the CONCURRENTLY option is legal for the relation.
 */
static void
check_repack_concurrently_requirements(Relation rel)
{
	char		relpersistence,
				replident;
	Oid			ident_idx;

	/* Data changes in system relations are not logically decoded. */
	if (IsCatalogRelation(rel))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot repack relation \"%s\"",
						RelationGetRelationName(rel)),
				 errhint("REPACK CONCURRENTLY is not supported for catalog relations.")));

	/*
	 * reorderbuffer.c does not seem to handle processing of TOAST relation
	 * alone.
	 */
	if (IsToastRelation(rel))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot repack relation \"%s\"",
						RelationGetRelationName(rel)),
				 errhint("REPACK CONCURRENTLY is not supported for TOAST relations, unless the main relation is repacked too.")));

	relpersistence = rel->rd_rel->relpersistence;
	if (relpersistence != RELPERSISTENCE_PERMANENT)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("cannot repack relation \"%s\"",
						RelationGetRelationName(rel)),
				 errhint("REPACK CONCURRENTLY is only allowed for permanent relations.")));

	/* With NOTHING, WAL does not contain the old tuple. */
	replident = rel->rd_rel->relreplident;
	if (replident == REPLICA_IDENTITY_NOTHING)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("cannot repack relation \"%s\"",
						RelationGetRelationName(rel)),
				 errhint("Relation \"%s\" has insufficient replication identity.",
						 RelationGetRelationName(rel))));

	/*
	 * Identity index is not set if the replica identity is FULL, but PK might
	 * exist in such a case.
	 */
	ident_idx = RelationGetReplicaIndex(rel);
	if (!OidIsValid(ident_idx) && OidIsValid(rel->rd_pkindex))
		ident_idx = rel->rd_pkindex;
	if (!OidIsValid(ident_idx))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("cannot process relation \"%s\"",
						RelationGetRelationName(rel)),
				 (errhint("Relation \"%s\" has no identity index.",
						  RelationGetRelationName(rel)))));
}


/*
 * rebuild_relation: rebuild an existing relation in index or physical order
 *
 * OldHeap: table to rebuild.  See cluster_rel() for comments on the required
 * lock strength.
 *
 * index: index to cluster by, or NULL to rewrite in physical order.
 *
 * On entry, heap and index (if one is given) must be open, and the
 * appropriate lock held on them -- AccessExclusiveLock for exclusive
 * processing and ShareUpdateExclusiveLock for concurrent processing.
 *
 * On exit, they are closed, but still locked with AccessExclusiveLock.  (The
 * function handles the lock upgrade if 'concurrent' is true.)
 */
static void
rebuild_relation(RepackCommand cmd, bool usingindex,
				 Relation OldHeap, Relation index, Oid userid,
				 bool concurrent, bool verbose)
{
	Oid			tableOid = RelationGetRelid(OldHeap);
	Oid			accessMethod = OldHeap->rd_rel->relam;
	Oid			tableSpace = OldHeap->rd_rel->reltablespace;
	Oid			OIDNewHeap;
	Relation	NewHeap;
	char		relpersistence;
	bool		swap_toast_by_content;
	TransactionId frozenXid;
	MultiXactId cutoffMulti;
	NameData	slotname;
	LogicalDecodingContext *ctx = NULL;
	Snapshot	snapshot = NULL;
#if USE_ASSERT_CHECKING
	LOCKMODE	lmode;

	lmode = !concurrent ? AccessExclusiveLock : ShareUpdateExclusiveLock;

	Assert(CheckRelationLockedByMe(OldHeap, lmode, false) &&
		   (index == NULL || CheckRelationLockedByMe(index, lmode, false)));
#endif

	if (concurrent)
	{
		TupleDesc	tupdesc;

		/*
		 * REPACK CONCURRENTLY is not allowed in a transaction block, so this
		 * should never fire.
		 */
		Assert(GetTopTransactionIdIfAny() == InvalidTransactionId);

		/*
		 * A single backend should not execute multiple REPACK commands at a
		 * time, so use PID to make the slot unique.
		 */
		snprintf(NameStr(slotname), NAMEDATALEN, "repack_%d", MyProcPid);

		tupdesc = CreateTupleDescCopy(RelationGetDescr(OldHeap));

		/*
		 * Prepare to capture the concurrent data changes.
		 *
		 * Note that this call waits for all transactions with XID already
		 * assigned to finish. If some of those transactions is waiting for a
		 * lock conflicting with ShareUpdateExclusiveLock on our table (e.g.
		 * it runs CREATE INDEX), we can end up in a deadlock. Not sure this
		 * risk is worth unlocking/locking the table (and its clustering
		 * index) and checking again if its still eligible for REPACK
		 * CONCURRENTLY.
		 */
		ctx = setup_logical_decoding(tableOid, NameStr(slotname), tupdesc);

		snapshot = SnapBuildInitialSnapshotForRepack(ctx->snapshot_builder);
		PushActiveSnapshot(snapshot);
	}

	/* for CLUSTER or REPACK USING INDEX, mark the index as the one to use */
	if (usingindex)
		mark_index_clustered(OldHeap, RelationGetRelid(index), true);

	/* Remember info about rel before closing OldHeap */
	relpersistence = OldHeap->rd_rel->relpersistence;

	/*
	 * Create the transient table that will receive the re-ordered data.
	 *
	 * OldHeap is already locked, so no need to lock it again.  make_new_heap
	 * obtains AccessExclusiveLock on the new heap and its toast table.
	 */
	OIDNewHeap = make_new_heap(tableOid, tableSpace,
							   accessMethod,
							   relpersistence,
							   NoLock);
	Assert(CheckRelationOidLockedByMe(OIDNewHeap, AccessExclusiveLock, false));
	NewHeap = table_open(OIDNewHeap, NoLock);

	/* Copy the heap data into the new table in the desired order */
	copy_table_data(NewHeap, OldHeap, index, snapshot, ctx, verbose,
					&swap_toast_by_content, &frozenXid, &cutoffMulti);

	/* The historic snapshot won't be needed anymore. */
	if (snapshot)
		PopActiveSnapshot();

	if (concurrent)
	{
		/*
		 * Push a snapshot that we will use to find old versions of rows when
		 * processing concurrent UPDATE and DELETE commands. (That snapshot
		 * should also be used by index expressions.)
		 */
		PushActiveSnapshot(GetTransactionSnapshot());

		/*
		 * Make sure we can find the tuples just inserted when applying DML
		 * commands on top of those.
		 */
		CommandCounterIncrement();
		UpdateActiveSnapshotCommandId();

		rebuild_relation_finish_concurrent(NewHeap, OldHeap, index,
										   ctx, swap_toast_by_content,
										   frozenXid, cutoffMulti);
		PopActiveSnapshot();

		pgstat_progress_update_param(PROGRESS_REPACK_PHASE,
									 PROGRESS_REPACK_PHASE_FINAL_CLEANUP);

		/* Done with decoding. */
		cleanup_logical_decoding(ctx);
		ReplicationSlotRelease();
		ReplicationSlotDrop(NameStr(slotname), false);
	}
	else
	{
		bool		is_system_catalog = IsSystemRelation(OldHeap);

		/* Close relcache entries, but keep lock until transaction commit */
		table_close(OldHeap, NoLock);
		if (index)
			index_close(index, NoLock);

		/*
		 * Close the new relation so it can be dropped as soon as the storage
		 * is swapped. The relation is not visible to others, so no need to
		 * unlock it explicitly.
		 */
		table_close(NewHeap, NoLock);

		/*
		 * Swap the physical files of the target and transient tables, then
		 * rebuild the target's indexes and throw away the transient table.
		 */
		finish_heap_swap(tableOid, OIDNewHeap, is_system_catalog,
						 swap_toast_by_content, false, true, true,
						 frozenXid, cutoffMulti,
						 relpersistence);
	}
}


/*
 * Create the transient table that will be filled with new data during
 * CLUSTER, ALTER TABLE, and similar operations.  The transient table
 * duplicates the logical structure of the OldHeap; but will have the
 * specified physical storage properties NewTableSpace, NewAccessMethod, and
 * relpersistence.
 *
 * After this, the caller should load the new heap with transferred/modified
 * data, then call finish_heap_swap to complete the operation.
 */
Oid
make_new_heap(Oid OIDOldHeap, Oid NewTableSpace, Oid NewAccessMethod,
			  char relpersistence, LOCKMODE lockmode)
{
	TupleDesc	OldHeapDesc;
	char		NewHeapName[NAMEDATALEN];
	Oid			OIDNewHeap;
	Oid			toastid;
	Relation	OldHeap;
	HeapTuple	tuple;
	Datum		reloptions;
	bool		isNull;
	Oid			namespaceid;

	OldHeap = table_open(OIDOldHeap, lockmode);
	OldHeapDesc = RelationGetDescr(OldHeap);

	/*
	 * Note that the NewHeap will not receive any of the defaults or
	 * constraints associated with the OldHeap; we don't need 'em, and there's
	 * no reason to spend cycles inserting them into the catalogs only to
	 * delete them.
	 */

	/*
	 * But we do want to use reloptions of the old heap for new heap.
	 */
	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(OIDOldHeap));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u", OIDOldHeap);
	reloptions = SysCacheGetAttr(RELOID, tuple, Anum_pg_class_reloptions,
								 &isNull);
	if (isNull)
		reloptions = (Datum) 0;

	if (relpersistence == RELPERSISTENCE_TEMP)
		namespaceid = LookupCreationNamespace("pg_temp");
	else
		namespaceid = RelationGetNamespace(OldHeap);

	/*
	 * Create the new heap, using a temporary name in the same namespace as
	 * the existing table.  NOTE: there is some risk of collision with user
	 * relnames.  Working around this seems more trouble than it's worth; in
	 * particular, we can't create the new heap in a different namespace from
	 * the old, or we will have problems with the TEMP status of temp tables.
	 *
	 * Note: the new heap is not a shared relation, even if we are rebuilding
	 * a shared rel.  However, we do make the new heap mapped if the source is
	 * mapped.  This simplifies swap_relation_files, and is absolutely
	 * necessary for rebuilding pg_class, for reasons explained there.
	 */
	snprintf(NewHeapName, sizeof(NewHeapName), "pg_temp_%u", OIDOldHeap);

	OIDNewHeap = heap_create_with_catalog(NewHeapName,
										  namespaceid,
										  NewTableSpace,
										  InvalidOid,
										  InvalidOid,
										  InvalidOid,
										  OldHeap->rd_rel->relowner,
										  NewAccessMethod,
										  OldHeapDesc,
										  NIL,
										  RELKIND_RELATION,
										  relpersistence,
										  false,
										  RelationIsMapped(OldHeap),
										  ONCOMMIT_NOOP,
										  reloptions,
										  false,
										  true,
										  true,
										  OIDOldHeap,
										  NULL);
	Assert(OIDNewHeap != InvalidOid);

	ReleaseSysCache(tuple);

	/*
	 * Advance command counter so that the newly-created relation's catalog
	 * tuples will be visible to table_open.
	 */
	CommandCounterIncrement();

	/*
	 * If necessary, create a TOAST table for the new relation.
	 *
	 * If the relation doesn't have a TOAST table already, we can't need one
	 * for the new relation.  The other way around is possible though: if some
	 * wide columns have been dropped, NewHeapCreateToastTable can decide that
	 * no TOAST table is needed for the new table.
	 *
	 * Note that NewHeapCreateToastTable ends with CommandCounterIncrement, so
	 * that the TOAST table will be visible for insertion.
	 */
	toastid = OldHeap->rd_rel->reltoastrelid;
	if (OidIsValid(toastid))
	{
		/* keep the existing toast table's reloptions, if any */
		tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(toastid));
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for relation %u", toastid);
		reloptions = SysCacheGetAttr(RELOID, tuple, Anum_pg_class_reloptions,
									 &isNull);
		if (isNull)
			reloptions = (Datum) 0;

		NewHeapCreateToastTable(OIDNewHeap, reloptions, lockmode, toastid);

		ReleaseSysCache(tuple);
	}

	table_close(OldHeap, NoLock);

	return OIDNewHeap;
}

/*
 * Do the physical copying of table data.
 *
 * 'snapshot' and 'decoding_ctx': see table_relation_copy_for_cluster(). Pass
 * iff concurrent processing is required.
 *
 * There are three output parameters:
 * *pSwapToastByContent is set true if toast tables must be swapped by content.
 * *pFreezeXid receives the TransactionId used as freeze cutoff point.
 * *pCutoffMulti receives the MultiXactId used as a cutoff point.
 */
static void
copy_table_data(Relation NewHeap, Relation OldHeap, Relation OldIndex,
				Snapshot snapshot, LogicalDecodingContext *decoding_ctx,
				bool verbose, bool *pSwapToastByContent,
				TransactionId *pFreezeXid, MultiXactId *pCutoffMulti)
{
	Relation	relRelation;
	HeapTuple	reltup;
	Form_pg_class relform;
	TupleDesc	oldTupDesc PG_USED_FOR_ASSERTS_ONLY;
	TupleDesc	newTupDesc PG_USED_FOR_ASSERTS_ONLY;
	VacuumParams params;
	struct VacuumCutoffs cutoffs;
	bool		use_sort;
	double		num_tuples = 0,
				tups_vacuumed = 0,
				tups_recently_dead = 0;
	BlockNumber num_pages;
	int			elevel = verbose ? INFO : DEBUG2;
	PGRUsage	ru0;
	char	   *nspname;

	bool		concurrent = snapshot != NULL;

	pg_rusage_init(&ru0);

	/* Store a copy of the namespace name for logging purposes */
	nspname = get_namespace_name(RelationGetNamespace(OldHeap));

	/*
	 * Their tuple descriptors should be exactly alike, but here we only need
	 * assume that they have the same number of columns.
	 */
	oldTupDesc = RelationGetDescr(OldHeap);
	newTupDesc = RelationGetDescr(NewHeap);
	Assert(newTupDesc->natts == oldTupDesc->natts);

	/*
	 * If the OldHeap has a toast table, get lock on the toast table to keep
	 * it from being vacuumed.  This is needed because autovacuum processes
	 * toast tables independently of their main tables, with no lock on the
	 * latter.  If an autovacuum were to start on the toast table after we
	 * compute our OldestXmin below, it would use a later OldestXmin, and then
	 * possibly remove as DEAD toast tuples belonging to main tuples we think
	 * are only RECENTLY_DEAD.  Then we'd fail while trying to copy those
	 * tuples.
	 *
	 * We don't need to open the toast relation here, just lock it.  The lock
	 * will be held till end of transaction.
	 */
	if (OldHeap->rd_rel->reltoastrelid)
		LockRelationOid(OldHeap->rd_rel->reltoastrelid, AccessExclusiveLock);

	/*
	 * If both tables have TOAST tables, perform toast swap by content.  It is
	 * possible that the old table has a toast table but the new one doesn't,
	 * if toastable columns have been dropped.  In that case we have to do
	 * swap by links.  This is okay because swap by content is only essential
	 * for system catalogs, and we don't support schema changes for them.
	 */
	if (OldHeap->rd_rel->reltoastrelid && NewHeap->rd_rel->reltoastrelid)
	{
		*pSwapToastByContent = true;

		/*
		 * When doing swap by content, any toast pointers written into NewHeap
		 * must use the old toast table's OID, because that's where the toast
		 * data will eventually be found.  Set this up by setting rd_toastoid.
		 * This also tells toast_save_datum() to preserve the toast value
		 * OIDs, which we want so as not to invalidate toast pointers in
		 * system catalog caches, and to avoid making multiple copies of a
		 * single toast value.
		 *
		 * Note that we must hold NewHeap open until we are done writing data,
		 * since the relcache will not guarantee to remember this setting once
		 * the relation is closed.  Also, this technique depends on the fact
		 * that no one will try to read from the NewHeap until after we've
		 * finished writing it and swapping the rels --- otherwise they could
		 * follow the toast pointers to the wrong place.  (It would actually
		 * work for values copied over from the old toast table, but not for
		 * any values that we toast which were previously not toasted.)
		 */
		NewHeap->rd_toastoid = OldHeap->rd_rel->reltoastrelid;
	}
	else
		*pSwapToastByContent = false;

	/*
	 * Compute xids used to freeze and weed out dead tuples and multixacts.
	 * Since we're going to rewrite the whole table anyway, there's no reason
	 * not to be aggressive about this.
	 */
	memset(&params, 0, sizeof(VacuumParams));
	vacuum_get_cutoffs(OldHeap, params, &cutoffs);

	/*
	 * FreezeXid will become the table's new relfrozenxid, and that mustn't go
	 * backwards, so take the max.
	 */
	{
		TransactionId relfrozenxid = OldHeap->rd_rel->relfrozenxid;

		if (TransactionIdIsValid(relfrozenxid) &&
			TransactionIdPrecedes(cutoffs.FreezeLimit, relfrozenxid))
			cutoffs.FreezeLimit = relfrozenxid;
	}

	/*
	 * MultiXactCutoff, similarly, shouldn't go backwards either.
	 */
	{
		MultiXactId relminmxid = OldHeap->rd_rel->relminmxid;

		if (MultiXactIdIsValid(relminmxid) &&
			MultiXactIdPrecedes(cutoffs.MultiXactCutoff, relminmxid))
			cutoffs.MultiXactCutoff = relminmxid;
	}

	/*
	 * Decide whether to use an indexscan or seqscan-and-optional-sort to scan
	 * the OldHeap.  We know how to use a sort to duplicate the ordering of a
	 * btree index, and will use seqscan-and-sort for that case if the planner
	 * tells us it's cheaper.  Otherwise, always indexscan if an index is
	 * provided, else plain seqscan.
	 */
	if (OldIndex != NULL && OldIndex->rd_rel->relam == BTREE_AM_OID)
	{
		ResourceOwner oldowner = NULL;
		ResourceOwner resowner = NULL;

		/*
		 * In the CONCURRENT case, use a dedicated resource owner so we don't
		 * leave any additional locks behind us that we cannot release easily.
		 */
		if (concurrent)
		{
			Assert(CheckRelationLockedByMe(OldHeap, ShareUpdateExclusiveLock,
										   false));
			Assert(CheckRelationLockedByMe(OldIndex, ShareUpdateExclusiveLock,
										   false));

			resowner = ResourceOwnerCreate(CurrentResourceOwner,
										   "plan_cluster_use_sort");
			oldowner = CurrentResourceOwner;
			CurrentResourceOwner = resowner;
		}

		use_sort = plan_cluster_use_sort(RelationGetRelid(OldHeap),
										 RelationGetRelid(OldIndex));

		if (concurrent)
		{
			CurrentResourceOwner = oldowner;

			/*
			 * We are primarily concerned about locks, but if the planner
			 * happened to allocate any other resources, we should release
			 * them too because we're going to delete the whole resowner.
			 */
			ResourceOwnerRelease(resowner, RESOURCE_RELEASE_BEFORE_LOCKS,
								 false, false);
			ResourceOwnerRelease(resowner, RESOURCE_RELEASE_LOCKS,
								 false, false);
			ResourceOwnerRelease(resowner, RESOURCE_RELEASE_AFTER_LOCKS,
								 false, false);
			ResourceOwnerDelete(resowner);
		}
	}
	else
		use_sort = false;

	/* Log what we're doing */
	if (OldIndex != NULL && !use_sort)
		ereport(elevel,
				(errmsg("clustering \"%s.%s\" using index scan on \"%s\"",
						nspname,
						RelationGetRelationName(OldHeap),
						RelationGetRelationName(OldIndex))));
	else if (use_sort)
		ereport(elevel,
				(errmsg("clustering \"%s.%s\" using sequential scan and sort",
						nspname,
						RelationGetRelationName(OldHeap))));
	else
		ereport(elevel,
				(errmsg("vacuuming \"%s.%s\"",
						nspname,
						RelationGetRelationName(OldHeap))));

	/*
	 * Hand off the actual copying to AM specific function, the generic code
	 * cannot know how to deal with visibility across AMs. Note that this
	 * routine is allowed to set FreezeXid / MultiXactCutoff to different
	 * values (e.g. because the AM doesn't use freezing).
	 */
	table_relation_copy_for_cluster(OldHeap, NewHeap, OldIndex, use_sort,
									cutoffs.OldestXmin, snapshot,
									decoding_ctx,
									&cutoffs.FreezeLimit,
									&cutoffs.MultiXactCutoff,
									&num_tuples, &tups_vacuumed,
									&tups_recently_dead);

	/* return selected values to caller, get set as relfrozenxid/minmxid */
	*pFreezeXid = cutoffs.FreezeLimit;
	*pCutoffMulti = cutoffs.MultiXactCutoff;

	/*
	 * Reset rd_toastoid just to be tidy --- it shouldn't be looked at again.
	 * In the CONCURRENTLY case, we need to set it again before applying the
	 * concurrent changes.
	 */
	NewHeap->rd_toastoid = InvalidOid;

	num_pages = RelationGetNumberOfBlocks(NewHeap);

	/* Log what we did */
	ereport(elevel,
			(errmsg("\"%s.%s\": found %.0f removable, %.0f nonremovable row versions in %u pages",
					nspname,
					RelationGetRelationName(OldHeap),
					tups_vacuumed, num_tuples,
					RelationGetNumberOfBlocks(OldHeap)),
			 errdetail("%.0f dead row versions cannot be removed yet.\n"
					   "%s.",
					   tups_recently_dead,
					   pg_rusage_show(&ru0))));

	/* Update pg_class to reflect the correct values of pages and tuples. */
	relRelation = table_open(RelationRelationId, RowExclusiveLock);

	reltup = SearchSysCacheCopy1(RELOID,
								 ObjectIdGetDatum(RelationGetRelid(NewHeap)));
	if (!HeapTupleIsValid(reltup))
		elog(ERROR, "cache lookup failed for relation %u",
			 RelationGetRelid(NewHeap));
	relform = (Form_pg_class) GETSTRUCT(reltup);

	relform->relpages = num_pages;
	relform->reltuples = num_tuples;

	/* Don't update the stats for pg_class.  See swap_relation_files. */
	if (RelationGetRelid(OldHeap) != RelationRelationId)
		CatalogTupleUpdate(relRelation, &reltup->t_self, reltup);
	else
		CacheInvalidateRelcacheByTuple(reltup);

	/* Clean up. */
	heap_freetuple(reltup);
	table_close(relRelation, RowExclusiveLock);

	/* Make the update visible */
	CommandCounterIncrement();
}

/*
 * Swap the physical files of two given relations.
 *
 * We swap the physical identity (reltablespace, relfilenumber) while keeping
 * the same logical identities of the two relations.  relpersistence is also
 * swapped, which is critical since it determines where buffers live for each
 * relation.
 *
 * We can swap associated TOAST data in either of two ways: recursively swap
 * the physical content of the toast tables (and their indexes), or swap the
 * TOAST links in the given relations' pg_class entries.  The former is needed
 * to manage rewrites of shared catalogs (where we cannot change the pg_class
 * links) while the latter is the only way to handle cases in which a toast
 * table is added or removed altogether.
 *
 * Additionally, the first relation is marked with relfrozenxid set to
 * frozenXid.  It seems a bit ugly to have this here, but the caller would
 * have to do it anyway, so having it here saves a heap_update.  Note: in
 * the swap-toast-links case, we assume we don't need to change the toast
 * table's relfrozenxid: the new version of the toast table should already
 * have relfrozenxid set to RecentXmin, which is good enough.
 *
 * Lastly, if r2 and its toast table and toast index (if any) are mapped,
 * their OIDs are emitted into mapped_tables[].  This is hacky but beats
 * having to look the information up again later in finish_heap_swap.
 */
static void
swap_relation_files(Oid r1, Oid r2, bool target_is_pg_class,
					bool swap_toast_by_content,
					bool is_internal,
					TransactionId frozenXid,
					MultiXactId cutoffMulti,
					Oid *mapped_tables)
{
	Relation	relRelation;
	HeapTuple	reltup1,
				reltup2;
	Form_pg_class relform1,
				relform2;
	RelFileNumber relfilenumber1,
				relfilenumber2;
	RelFileNumber swaptemp;
	char		swptmpchr;
	Oid			relam1,
				relam2;

	/* We need writable copies of both pg_class tuples. */
	relRelation = table_open(RelationRelationId, RowExclusiveLock);

	reltup1 = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(r1));
	if (!HeapTupleIsValid(reltup1))
		elog(ERROR, "cache lookup failed for relation %u", r1);
	relform1 = (Form_pg_class) GETSTRUCT(reltup1);

	reltup2 = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(r2));
	if (!HeapTupleIsValid(reltup2))
		elog(ERROR, "cache lookup failed for relation %u", r2);
	relform2 = (Form_pg_class) GETSTRUCT(reltup2);

	relfilenumber1 = relform1->relfilenode;
	relfilenumber2 = relform2->relfilenode;
	relam1 = relform1->relam;
	relam2 = relform2->relam;

	if (RelFileNumberIsValid(relfilenumber1) &&
		RelFileNumberIsValid(relfilenumber2))
	{
		/*
		 * Normal non-mapped relations: swap relfilenumbers, reltablespaces,
		 * relpersistence
		 */
		Assert(!target_is_pg_class);

		swaptemp = relform1->relfilenode;
		relform1->relfilenode = relform2->relfilenode;
		relform2->relfilenode = swaptemp;

		swaptemp = relform1->reltablespace;
		relform1->reltablespace = relform2->reltablespace;
		relform2->reltablespace = swaptemp;

		swaptemp = relform1->relam;
		relform1->relam = relform2->relam;
		relform2->relam = swaptemp;

		swptmpchr = relform1->relpersistence;
		relform1->relpersistence = relform2->relpersistence;
		relform2->relpersistence = swptmpchr;

		/* Also swap toast links, if we're swapping by links */
		if (!swap_toast_by_content)
		{
			swaptemp = relform1->reltoastrelid;
			relform1->reltoastrelid = relform2->reltoastrelid;
			relform2->reltoastrelid = swaptemp;
		}
	}
	else
	{
		/*
		 * Mapped-relation case.  Here we have to swap the relation mappings
		 * instead of modifying the pg_class columns.  Both must be mapped.
		 */
		if (RelFileNumberIsValid(relfilenumber1) ||
			RelFileNumberIsValid(relfilenumber2))
			elog(ERROR, "cannot swap mapped relation \"%s\" with non-mapped relation",
				 NameStr(relform1->relname));

		/*
		 * We can't change the tablespace nor persistence of a mapped rel, and
		 * we can't handle toast link swapping for one either, because we must
		 * not apply any critical changes to its pg_class row.  These cases
		 * should be prevented by upstream permissions tests, so these checks
		 * are non-user-facing emergency backstop.
		 */
		if (relform1->reltablespace != relform2->reltablespace)
			elog(ERROR, "cannot change tablespace of mapped relation \"%s\"",
				 NameStr(relform1->relname));
		if (relform1->relpersistence != relform2->relpersistence)
			elog(ERROR, "cannot change persistence of mapped relation \"%s\"",
				 NameStr(relform1->relname));
		if (relform1->relam != relform2->relam)
			elog(ERROR, "cannot change access method of mapped relation \"%s\"",
				 NameStr(relform1->relname));
		if (!swap_toast_by_content &&
			(relform1->reltoastrelid || relform2->reltoastrelid))
			elog(ERROR, "cannot swap toast by links for mapped relation \"%s\"",
				 NameStr(relform1->relname));

		/*
		 * Fetch the mappings --- shouldn't fail, but be paranoid
		 */
		relfilenumber1 = RelationMapOidToFilenumber(r1, relform1->relisshared);
		if (!RelFileNumberIsValid(relfilenumber1))
			elog(ERROR, "could not find relation mapping for relation \"%s\", OID %u",
				 NameStr(relform1->relname), r1);
		relfilenumber2 = RelationMapOidToFilenumber(r2, relform2->relisshared);
		if (!RelFileNumberIsValid(relfilenumber2))
			elog(ERROR, "could not find relation mapping for relation \"%s\", OID %u",
				 NameStr(relform2->relname), r2);

		/*
		 * Send replacement mappings to relmapper.  Note these won't actually
		 * take effect until CommandCounterIncrement.
		 */
		RelationMapUpdateMap(r1, relfilenumber2, relform1->relisshared, false);
		RelationMapUpdateMap(r2, relfilenumber1, relform2->relisshared, false);

		/* Pass OIDs of mapped r2 tables back to caller */
		*mapped_tables++ = r2;
	}

	/*
	 * Recognize that rel1's relfilenumber (swapped from rel2) is new in this
	 * subtransaction. The rel2 storage (swapped from rel1) may or may not be
	 * new.
	 */
	{
		Relation	rel1,
					rel2;

		rel1 = relation_open(r1, NoLock);
		rel2 = relation_open(r2, NoLock);
		rel2->rd_createSubid = rel1->rd_createSubid;
		rel2->rd_newRelfilelocatorSubid = rel1->rd_newRelfilelocatorSubid;
		rel2->rd_firstRelfilelocatorSubid = rel1->rd_firstRelfilelocatorSubid;
		RelationAssumeNewRelfilelocator(rel1);
		relation_close(rel1, NoLock);
		relation_close(rel2, NoLock);
	}

	/*
	 * In the case of a shared catalog, these next few steps will only affect
	 * our own database's pg_class row; but that's okay, because they are all
	 * noncritical updates.  That's also an important fact for the case of a
	 * mapped catalog, because it's possible that we'll commit the map change
	 * and then fail to commit the pg_class update.
	 */

	/* set rel1's frozen Xid and minimum MultiXid */
	if (relform1->relkind != RELKIND_INDEX)
	{
		Assert(!TransactionIdIsValid(frozenXid) ||
			   TransactionIdIsNormal(frozenXid));
		relform1->relfrozenxid = frozenXid;
		relform1->relminmxid = cutoffMulti;
	}

	/* swap size statistics too, since new rel has freshly-updated stats */
	{
		int32		swap_pages;
		float4		swap_tuples;
		int32		swap_allvisible;
		int32		swap_allfrozen;

		swap_pages = relform1->relpages;
		relform1->relpages = relform2->relpages;
		relform2->relpages = swap_pages;

		swap_tuples = relform1->reltuples;
		relform1->reltuples = relform2->reltuples;
		relform2->reltuples = swap_tuples;

		swap_allvisible = relform1->relallvisible;
		relform1->relallvisible = relform2->relallvisible;
		relform2->relallvisible = swap_allvisible;

		swap_allfrozen = relform1->relallfrozen;
		relform1->relallfrozen = relform2->relallfrozen;
		relform2->relallfrozen = swap_allfrozen;
	}

	/*
	 * Update the tuples in pg_class --- unless the target relation of the
	 * swap is pg_class itself.  In that case, there is zero point in making
	 * changes because we'd be updating the old data that we're about to throw
	 * away.  Because the real work being done here for a mapped relation is
	 * just to change the relation map settings, it's all right to not update
	 * the pg_class rows in this case. The most important changes will instead
	 * performed later, in finish_heap_swap() itself.
	 */
	if (!target_is_pg_class)
	{
		CatalogIndexState indstate;

		indstate = CatalogOpenIndexes(relRelation);
		CatalogTupleUpdateWithInfo(relRelation, &reltup1->t_self, reltup1,
								   indstate);
		CatalogTupleUpdateWithInfo(relRelation, &reltup2->t_self, reltup2,
								   indstate);
		CatalogCloseIndexes(indstate);
	}
	else
	{
		/* no update ... but we do still need relcache inval */
		CacheInvalidateRelcacheByTuple(reltup1);
		CacheInvalidateRelcacheByTuple(reltup2);
	}

	/*
	 * Now that pg_class has been updated with its relevant information for
	 * the swap, update the dependency of the relations to point to their new
	 * table AM, if it has changed.
	 */
	if (relam1 != relam2)
	{
		if (changeDependencyFor(RelationRelationId,
								r1,
								AccessMethodRelationId,
								relam1,
								relam2) != 1)
			elog(ERROR, "could not change access method dependency for relation \"%s.%s\"",
				 get_namespace_name(get_rel_namespace(r1)),
				 get_rel_name(r1));
		if (changeDependencyFor(RelationRelationId,
								r2,
								AccessMethodRelationId,
								relam2,
								relam1) != 1)
			elog(ERROR, "could not change access method dependency for relation \"%s.%s\"",
				 get_namespace_name(get_rel_namespace(r2)),
				 get_rel_name(r2));
	}

	/*
	 * Post alter hook for modified relations. The change to r2 is always
	 * internal, but r1 depends on the invocation context.
	 */
	InvokeObjectPostAlterHookArg(RelationRelationId, r1, 0,
								 InvalidOid, is_internal);
	InvokeObjectPostAlterHookArg(RelationRelationId, r2, 0,
								 InvalidOid, true);

	/*
	 * If we have toast tables associated with the relations being swapped,
	 * deal with them too.
	 */
	if (relform1->reltoastrelid || relform2->reltoastrelid)
	{
		if (swap_toast_by_content)
		{
			if (relform1->reltoastrelid && relform2->reltoastrelid)
			{
				/* Recursively swap the contents of the toast tables */
				swap_relation_files(relform1->reltoastrelid,
									relform2->reltoastrelid,
									target_is_pg_class,
									swap_toast_by_content,
									is_internal,
									frozenXid,
									cutoffMulti,
									mapped_tables);
			}
			else
			{
				/* caller messed up */
				elog(ERROR, "cannot swap toast files by content when there's only one");
			}
		}
		else
		{
			/*
			 * We swapped the ownership links, so we need to change dependency
			 * data to match.
			 *
			 * NOTE: it is possible that only one table has a toast table.
			 *
			 * NOTE: at present, a TOAST table's only dependency is the one on
			 * its owning table.  If more are ever created, we'd need to use
			 * something more selective than deleteDependencyRecordsFor() to
			 * get rid of just the link we want.
			 */
			ObjectAddress baseobject,
						toastobject;
			long		count;

			/*
			 * We disallow this case for system catalogs, to avoid the
			 * possibility that the catalog we're rebuilding is one of the
			 * ones the dependency changes would change.  It's too late to be
			 * making any data changes to the target catalog.
			 */
			if (IsSystemClass(r1, relform1))
				elog(ERROR, "cannot swap toast files by links for system catalogs");

			/* Delete old dependencies */
			if (relform1->reltoastrelid)
			{
				count = deleteDependencyRecordsFor(RelationRelationId,
												   relform1->reltoastrelid,
												   false);
				if (count != 1)
					elog(ERROR, "expected one dependency record for TOAST table, found %ld",
						 count);
			}
			if (relform2->reltoastrelid)
			{
				count = deleteDependencyRecordsFor(RelationRelationId,
												   relform2->reltoastrelid,
												   false);
				if (count != 1)
					elog(ERROR, "expected one dependency record for TOAST table, found %ld",
						 count);
			}

			/* Register new dependencies */
			baseobject.classId = RelationRelationId;
			baseobject.objectSubId = 0;
			toastobject.classId = RelationRelationId;
			toastobject.objectSubId = 0;

			if (relform1->reltoastrelid)
			{
				baseobject.objectId = r1;
				toastobject.objectId = relform1->reltoastrelid;
				recordDependencyOn(&toastobject, &baseobject,
								   DEPENDENCY_INTERNAL);
			}

			if (relform2->reltoastrelid)
			{
				baseobject.objectId = r2;
				toastobject.objectId = relform2->reltoastrelid;
				recordDependencyOn(&toastobject, &baseobject,
								   DEPENDENCY_INTERNAL);
			}
		}
	}

	/*
	 * If we're swapping two toast tables by content, do the same for their
	 * valid index. The swap can actually be safely done only if the relations
	 * have indexes.
	 */
	if (swap_toast_by_content &&
		relform1->relkind == RELKIND_TOASTVALUE &&
		relform2->relkind == RELKIND_TOASTVALUE)
	{
		Oid			toastIndex1,
					toastIndex2;

		/* Get valid index for each relation */
		toastIndex1 = toast_get_valid_index(r1,
											AccessExclusiveLock);
		toastIndex2 = toast_get_valid_index(r2,
											AccessExclusiveLock);

		swap_relation_files(toastIndex1,
							toastIndex2,
							target_is_pg_class,
							swap_toast_by_content,
							is_internal,
							InvalidTransactionId,
							InvalidMultiXactId,
							mapped_tables);
	}

	/* Clean up. */
	heap_freetuple(reltup1);
	heap_freetuple(reltup2);

	table_close(relRelation, RowExclusiveLock);
}

/*
 * Remove the transient table that was built by make_new_heap, and finish
 * cleaning up (including rebuilding all indexes on the old heap).
 */
void
finish_heap_swap(Oid OIDOldHeap, Oid OIDNewHeap,
				 bool is_system_catalog,
				 bool swap_toast_by_content,
				 bool check_constraints,
				 bool is_internal,
				 bool reindex,
				 TransactionId frozenXid,
				 MultiXactId cutoffMulti,
				 char newrelpersistence)
{
	ObjectAddress object;
	Oid			mapped_tables[4];
	int			i;

	/* Report that we are now swapping relation files */
	pgstat_progress_update_param(PROGRESS_REPACK_PHASE,
								 PROGRESS_REPACK_PHASE_SWAP_REL_FILES);

	/* Zero out possible results from swapped_relation_files */
	memset(mapped_tables, 0, sizeof(mapped_tables));

	/*
	 * Swap the contents of the heap relations (including any toast tables).
	 * Also set old heap's relfrozenxid to frozenXid.
	 */
	swap_relation_files(OIDOldHeap, OIDNewHeap,
						(OIDOldHeap == RelationRelationId),
						swap_toast_by_content, is_internal,
						frozenXid, cutoffMulti, mapped_tables);

	/*
	 * If it's a system catalog, queue a sinval message to flush all catcaches
	 * on the catalog when we reach CommandCounterIncrement.
	 */
	if (is_system_catalog)
		CacheInvalidateCatalog(OIDOldHeap);

	if (reindex)
	{
		int			reindex_flags;
		ReindexParams reindex_params = {0};

		/*
		 * Rebuild each index on the relation (but not the toast table, which
		 * is all-new at this point).  It is important to do this before the
		 * DROP step because if we are processing a system catalog that will
		 * be used during DROP, we want to have its indexes available.  There
		 * is no advantage to the other order anyway because this is all
		 * transactional, so no chance to reclaim disk space before commit. We
		 * do not need a final CommandCounterIncrement() because
		 * reindex_relation does it.
		 *
		 * Note: because index_build is called via reindex_relation, it will
		 * never set indcheckxmin true for the indexes.  This is OK even
		 * though in some sense we are building new indexes rather than
		 * rebuilding existing ones, because the new heap won't contain any
		 * HOT chains at all, let alone broken ones, so it can't be necessary
		 * to set indcheckxmin.
		 */
		reindex_flags = REINDEX_REL_SUPPRESS_INDEX_USE;
		if (check_constraints)
			reindex_flags |= REINDEX_REL_CHECK_CONSTRAINTS;

		/*
		 * Ensure that the indexes have the same persistence as the parent
		 * relation.
		 */
		if (newrelpersistence == RELPERSISTENCE_UNLOGGED)
			reindex_flags |= REINDEX_REL_FORCE_INDEXES_UNLOGGED;
		else if (newrelpersistence == RELPERSISTENCE_PERMANENT)
			reindex_flags |= REINDEX_REL_FORCE_INDEXES_PERMANENT;

		/* Report that we are now reindexing relations */
		pgstat_progress_update_param(PROGRESS_REPACK_PHASE,
									 PROGRESS_REPACK_PHASE_REBUILD_INDEX);

		reindex_relation(NULL, OIDOldHeap, reindex_flags, &reindex_params);
	}

	/* Report that we are now doing clean up */
	pgstat_progress_update_param(PROGRESS_REPACK_PHASE,
								 PROGRESS_REPACK_PHASE_FINAL_CLEANUP);

	/*
	 * If the relation being rebuilt is pg_class, swap_relation_files()
	 * couldn't update pg_class's own pg_class entry (check comments in
	 * swap_relation_files()), thus relfrozenxid was not updated. That's
	 * annoying because a potential reason for doing a VACUUM FULL is a
	 * imminent or actual anti-wraparound shutdown.  So, now that we can
	 * access the new relation using its indices, update relfrozenxid.
	 * pg_class doesn't have a toast relation, so we don't need to update the
	 * corresponding toast relation. Not that there's little point moving all
	 * relfrozenxid updates here since swap_relation_files() needs to write to
	 * pg_class for non-mapped relations anyway.
	 */
	if (OIDOldHeap == RelationRelationId)
	{
		Relation	relRelation;
		HeapTuple	reltup;
		Form_pg_class relform;

		relRelation = table_open(RelationRelationId, RowExclusiveLock);

		reltup = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(OIDOldHeap));
		if (!HeapTupleIsValid(reltup))
			elog(ERROR, "cache lookup failed for relation %u", OIDOldHeap);
		relform = (Form_pg_class) GETSTRUCT(reltup);

		relform->relfrozenxid = frozenXid;
		relform->relminmxid = cutoffMulti;

		CatalogTupleUpdate(relRelation, &reltup->t_self, reltup);

		table_close(relRelation, RowExclusiveLock);
	}

	/* Destroy new heap with old filenumber */
	object.classId = RelationRelationId;
	object.objectId = OIDNewHeap;
	object.objectSubId = 0;

	/*
	 * The new relation is local to our transaction and we know nothing
	 * depends on it, so DROP_RESTRICT should be OK.
	 */
	performDeletion(&object, DROP_RESTRICT, PERFORM_DELETION_INTERNAL);

	/* performDeletion does CommandCounterIncrement at end */

	/*
	 * Now we must remove any relation mapping entries that we set up for the
	 * transient table, as well as its toast table and toast index if any. If
	 * we fail to do this before commit, the relmapper will complain about new
	 * permanent map entries being added post-bootstrap.
	 */
	for (i = 0; OidIsValid(mapped_tables[i]); i++)
		RelationMapRemoveMapping(mapped_tables[i]);

	/*
	 * At this point, everything is kosher except that, if we did toast swap
	 * by links, the toast table's name corresponds to the transient table.
	 * The name is irrelevant to the backend because it's referenced by OID,
	 * but users looking at the catalogs could be confused.  Rename it to
	 * prevent this problem.
	 *
	 * Note no lock required on the relation, because we already hold an
	 * exclusive lock on it.
	 */
	if (!swap_toast_by_content)
	{
		Relation	newrel;

		newrel = table_open(OIDOldHeap, NoLock);
		if (OidIsValid(newrel->rd_rel->reltoastrelid))
		{
			Oid			toastidx;
			char		NewToastName[NAMEDATALEN];

			/* Get the associated valid index to be renamed */
			toastidx = toast_get_valid_index(newrel->rd_rel->reltoastrelid,
											 NoLock);

			/* rename the toast table ... */
			snprintf(NewToastName, NAMEDATALEN, "pg_toast_%u",
					 OIDOldHeap);
			RenameRelationInternal(newrel->rd_rel->reltoastrelid,
								   NewToastName, true, false);

			/* ... and its valid index too. */
			snprintf(NewToastName, NAMEDATALEN, "pg_toast_%u_index",
					 OIDOldHeap);

			RenameRelationInternal(toastidx,
								   NewToastName, true, true);

			/*
			 * Reset the relrewrite for the toast. The command-counter
			 * increment is required here as we are about to update the tuple
			 * that is updated as part of RenameRelationInternal.
			 */
			CommandCounterIncrement();
			ResetRelRewrite(newrel->rd_rel->reltoastrelid);
		}
		relation_close(newrel, NoLock);
	}

	/* if it's not a catalog table, clear any missing attribute settings */
	if (!is_system_catalog)
	{
		Relation	newrel;

		newrel = table_open(OIDOldHeap, NoLock);
		RelationClearMissing(newrel);
		relation_close(newrel, NoLock);
	}
}

/*
 * Determine which relations to process, when REPACK/CLUSTER is called
 * without specifying a table name.  The exact process depends on whether
 * USING INDEX was given or not, and in any case we only return tables and
 * materialized views that the current user has privileges to repack/cluster.
 *
 * If USING INDEX was given, we scan pg_index to find those that have
 * indisclustered set; if it was not given, scan pg_class and return all
 * tables.
 *
 * Return it as a list of RelToCluster in the given memory context.
 */
static List *
get_tables_to_repack(RepackCommand command, bool usingindex,
					 MemoryContext permcxt)
{
	Relation	catalog;
	TableScanDesc scan;
	HeapTuple	tuple;
	MemoryContext old_context;
	List	   *rtcs = NIL;

	if (usingindex)
	{
		ScanKeyData entry;

		catalog = table_open(IndexRelationId, AccessShareLock);
		ScanKeyInit(&entry,
					Anum_pg_index_indisclustered,
					BTEqualStrategyNumber, F_BOOLEQ,
					BoolGetDatum(true));
		scan = table_beginscan_catalog(catalog, 1, &entry);
		while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
		{
			RelToCluster *rtc;
			Form_pg_index index;

			index = (Form_pg_index) GETSTRUCT(tuple);

			/*
			 * XXX I think the only reason there's no test failure here is
			 * that we seldom have clustered indexes that would be affected by
			 * concurrency.  Maybe we should also do the
			 * ConditionalLockRelationOid+SearchSysCacheExists dance that we
			 * do below.
			 */
			if (!cluster_is_permitted_for_relation(command, index->indrelid,
												   GetUserId()))
				continue;

			/* Use a permanent memory context for the result list */
			old_context = MemoryContextSwitchTo(permcxt);

			rtc = (RelToCluster *) palloc(sizeof(RelToCluster));
			rtc->tableOid = index->indrelid;
			rtc->indexOid = index->indexrelid;
			rtcs = lappend(rtcs, rtc);

			MemoryContextSwitchTo(old_context);
		}
	}
	else
	{
		catalog = table_open(RelationRelationId, AccessShareLock);
		scan = table_beginscan_catalog(catalog, 0, NULL);

		while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
		{
			RelToCluster *rtc;
			Form_pg_class class;

			class = (Form_pg_class) GETSTRUCT(tuple);

			/*
			 * Try to obtain a light lock on the table, to ensure it doesn't
			 * go away while we collect the list.  If we cannot, just
			 * disregard the table.  XXX we could release at the bottom of the
			 * loop, but for now just hold it until this transaction is
			 * finished.
			 */
			if (!ConditionalLockRelationOid(class->oid, AccessShareLock))
				continue;

			/* Verify that the table still exists. */
			if (!SearchSysCacheExists1(RELOID, ObjectIdGetDatum(class->oid)))
			{
				/* Release useless lock */
				UnlockRelationOid(class->oid, AccessShareLock);
				continue;
			}

			/* Can only process plain tables and matviews */
			if (class->relkind != RELKIND_RELATION &&
				class->relkind != RELKIND_MATVIEW)
				continue;

			if (!cluster_is_permitted_for_relation(command, class->oid,
												   GetUserId()))
				continue;

			/* Use a permanent memory context for the result list */
			old_context = MemoryContextSwitchTo(permcxt);

			rtc = (RelToCluster *) palloc(sizeof(RelToCluster));
			rtc->tableOid = class->oid;
			rtc->indexOid = InvalidOid;
			rtcs = lappend(rtcs, rtc);

			MemoryContextSwitchTo(old_context);
		}
	}

	table_endscan(scan);
	relation_close(catalog, AccessShareLock);

	return rtcs;
}

/*
 * Given a partitioned table or its index, return a list of RelToCluster for
 * all the children leaves tables/indexes.
 *
 * Like expand_vacuum_rel, but here caller must hold AccessExclusiveLock
 * on the table containing the index.
 *
 * 'rel_is_index' tells whether 'relid' is that of an index (true) or of the
 * owning relation.
 */
static List *
get_tables_to_repack_partitioned(RepackCommand cmd, MemoryContext cluster_context,
								 Oid relid, bool rel_is_index)
{
	List	   *inhoids;
	ListCell   *lc;
	List	   *rtcs = NIL;
	MemoryContext old_context;

	/* Do not lock the children until they're processed */
	inhoids = find_all_inheritors(relid, NoLock, NULL);

	foreach(lc, inhoids)
	{
		Oid			inhoid = lfirst_oid(lc);
		Oid			inhrelid,
					inhindid;
		RelToCluster *rtc;

		if (rel_is_index)
		{
			/* consider only leaf indexes */
			if (get_rel_relkind(inhoid) != RELKIND_INDEX)
				continue;

			inhrelid = IndexGetRelation(inhoid, false);
			inhindid = inhoid;
		}
		else
		{
			/* consider only leaf relations */
			if (get_rel_relkind(inhoid) != RELKIND_RELATION)
				continue;

			inhrelid = inhoid;
			inhindid = InvalidOid;
		}

		/*
		 * It's possible that the user does not have privileges to CLUSTER the
		 * leaf partition despite having such privileges on the partitioned
		 * table.  We skip any partitions which the user is not permitted to
		 * CLUSTER.
		 */
		if (!cluster_is_permitted_for_relation(cmd, inhrelid, GetUserId()))
			continue;

		/* Use a permanent memory context for the result list */
		old_context = MemoryContextSwitchTo(cluster_context);

		rtc = (RelToCluster *) palloc(sizeof(RelToCluster));
		rtc->tableOid = inhrelid;
		rtc->indexOid = inhindid;
		rtcs = lappend(rtcs, rtc);

		MemoryContextSwitchTo(old_context);
	}

	return rtcs;
}

/*
 * Return whether userid has privileges to CLUSTER relid.  If not, this
 * function emits a WARNING.
 */
static bool
cluster_is_permitted_for_relation(RepackCommand cmd, Oid relid, Oid userid)
{
	if (pg_class_aclcheck(relid, userid, ACL_MAINTAIN) == ACLCHECK_OK)
		return true;

	Assert(cmd == REPACK_COMMAND_CLUSTER || cmd == REPACK_COMMAND_REPACK);
	ereport(WARNING,
			errmsg("permission denied to execute %s on \"%s\", skipping it",
				   cmd == REPACK_COMMAND_CLUSTER ? "CLUSTER" : "REPACK",
				   get_rel_name(relid)));

	return false;
}


/*
 * Given a RepackStmt with an indicated relation name, resolve the relation
 * name, obtain lock on it, then determine what to do based on the relation
 * type: if it's not a partitioned table, repack it as indicated (using an
 * existing clustered index, or following the indicated index), and return
 * NULL.
 *
 * On the other hand, if the table is partitioned, do nothing further and
 * instead return the opened relcache entry, so that caller can process the
 * partitions using the multiple-table handling code.  The index name is not
 * resolve in this case.
 */
static Relation
process_single_relation(RepackStmt *stmt, LOCKMODE lockmode, bool isTopLevel,
						ClusterParams *params)
{
	Relation	rel;
	Oid			tableOid;

	Assert(stmt->relation != NULL);
	Assert(stmt->command == REPACK_COMMAND_CLUSTER ||
		   stmt->command == REPACK_COMMAND_REPACK);

	/* Find, lock, and check permissions on the table. */
	tableOid = RangeVarGetRelidExtended(stmt->relation,
										lockmode,
										0,
										RangeVarCallbackMaintainsTable,
										NULL);
	rel = table_open(tableOid, NoLock);

	/*
	 * Reject clustering a remote temp table ... their local buffer manager is
	 * not going to cope.
	 */
	if (RELATION_IS_OTHER_TEMP(rel))
	{
		ereport(ERROR,
				errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("cannot execute %s on temporary tables of other sessions",
					   RepackCommandAsString(stmt->command)));
	}

	/*
	 * For partitioned tables, let caller handle this.  Otherwise, process it
	 * here and we're done.
	 */
	if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		return rel;
	else
	{
		Oid			indexOid = InvalidOid;

		if (stmt->usingindex)
		{
			indexOid = determine_clustered_index(rel, stmt->usingindex,
												 stmt->indexname);
			check_index_is_clusterable(rel, indexOid, AccessExclusiveLock);
		}

		cluster_rel(stmt->command, stmt->usingindex, rel, indexOid,
					params, isTopLevel);
		return NULL;
	}
}

/*
 * Given a relation and the usingindex/indexname options in a
 * REPACK USING INDEX or CLUSTER command, return the OID of the index to use
 * for clustering the table.
 *
 * Caller must hold lock on the relation so that the set of indexes doesn't
 * change, and must call check_index_is_clusterable.
 */
static Oid
determine_clustered_index(Relation rel, bool usingindex, const char *indexname)
{
	Oid			indexOid;

	if (indexname == NULL && usingindex)
	{
		ListCell   *lc;

		/* Find an index with indisclustered set, or report error */
		foreach(lc, RelationGetIndexList(rel))
		{
			indexOid = lfirst_oid(lc);

			if (get_index_isclustered(indexOid))
				break;
			indexOid = InvalidOid;
		}

		if (!OidIsValid(indexOid))
			ereport(ERROR,
					errcode(ERRCODE_UNDEFINED_OBJECT),
					errmsg("there is no previously clustered index for table \"%s\"",
						   RelationGetRelationName(rel)));
	}
	else if (indexname != NULL)
	{
		/*
		 * An index was specified; figure out its name.  It must be in the
		 * same namespace as the relation.
		 */
		indexOid = get_relname_relid(indexname,
									 rel->rd_rel->relnamespace);
		if (!OidIsValid(indexOid))
			ereport(ERROR,
					errcode(ERRCODE_UNDEFINED_OBJECT),
					errmsg("index \"%s\" for table \"%s\" does not exist",
						   indexname, RelationGetRelationName(rel)));
	}
	else
		indexOid = InvalidOid;

	return indexOid;
}


/*
 * Call this function before REPACK CONCURRENTLY starts to setup logical
 * decoding. It makes sure that other users of the table put enough
 * information into WAL.
 *
 * The point is that at various places we expect that the table we're
 * processing is treated like a system catalog. For example, we need to be
 * able to scan it using a "historic snapshot" anytime during the processing
 * (as opposed to scanning only at the start point of the decoding, as logical
 * replication does during initial table synchronization), in order to apply
 * concurrent UPDATE / DELETE commands.
 *
 * Note that TOAST table needs no attention here as it's not scanned using
 * historic snapshot.
 */
static void
begin_concurrent_repack(Relation rel)
{
	Oid			toastrelid;

	/* Avoid logical decoding of other relations by this backend. */
	repacked_rel_locator = rel->rd_locator;
	toastrelid = rel->rd_rel->reltoastrelid;
	if (OidIsValid(toastrelid))
	{
		Relation	toastrel;

		/* Avoid logical decoding of other TOAST relations. */
		toastrel = table_open(toastrelid, AccessShareLock);
		repacked_rel_toast_locator = toastrel->rd_locator;
		table_close(toastrel, AccessShareLock);
	}
}

/*
 * Call this when done with REPACK CONCURRENTLY.
 */
static void
end_concurrent_repack(void)
{
	/*
	 * Restore normal function of (future) logical decoding for this backend.
	 */
	repacked_rel_locator.relNumber = InvalidOid;
	repacked_rel_toast_locator.relNumber = InvalidOid;
}

/*
 * This function is much like pg_create_logical_replication_slot() except that
 * the new slot is neither released (if anyone else could read changes from
 * our slot, we could miss changes other backends do while we copy the
 * existing data into temporary table), nor persisted (it's easier to handle
 * crash by restarting all the work from scratch).
 */
static LogicalDecodingContext *
setup_logical_decoding(Oid relid, const char *slotname, TupleDesc tupdesc)
{
	LogicalDecodingContext *ctx;
	RepackDecodingState *dstate;

	/*
	 * Check if we can use logical decoding.
	 */
	CheckSlotPermissions();
	CheckLogicalDecodingRequirements();

	/* RS_TEMPORARY so that the slot gets cleaned up on ERROR. */
	ReplicationSlotCreate(slotname, true, RS_TEMPORARY, false, false, false);

	/*
	 * Neither prepare_write nor do_write callback nor update_progress is
	 * useful for us.
	 *
	 * Regarding the value of need_full_snapshot, we pass false because the
	 * table we are processing is present in RepackedRelsHash and therefore,
	 * regarding logical decoding, treated like a catalog.
	 */
	ctx = CreateInitDecodingContext(REPL_PLUGIN_NAME,
									NIL,
									false,
									InvalidXLogRecPtr,
									XL_ROUTINE(.page_read = read_local_xlog_page,
											   .segment_open = wal_segment_open,
											   .segment_close = wal_segment_close),
									NULL, NULL, NULL);

	/*
	 * We don't have control on setting fast_forward, so at least check it.
	 */
	Assert(!ctx->fast_forward);

	DecodingContextFindStartpoint(ctx);

	/* Some WAL records should have been read. */
	Assert(ctx->reader->EndRecPtr != InvalidXLogRecPtr);

	XLByteToSeg(ctx->reader->EndRecPtr, repack_current_segment,
				wal_segment_size);

	/*
	 * Setup structures to store decoded changes.
	 */
	dstate = palloc0(sizeof(RepackDecodingState));
	dstate->relid = relid;
	dstate->tstore = tuplestore_begin_heap(false, false,
										   maintenance_work_mem);

	dstate->tupdesc = tupdesc;

	/* Initialize the descriptor to store the changes ... */
	dstate->tupdesc_change = CreateTemplateTupleDesc(1);

	TupleDescInitEntry(dstate->tupdesc_change, 1, NULL, BYTEAOID, -1, 0);
	/* ... as well as the corresponding slot. */
	dstate->tsslot = MakeSingleTupleTableSlot(dstate->tupdesc_change,
											  &TTSOpsMinimalTuple);

	dstate->resowner = ResourceOwnerCreate(CurrentResourceOwner,
										   "logical decoding");

	ctx->output_writer_private = dstate;
	return ctx;
}

/*
 * Retrieve tuple from ConcurrentChange structure.
 *
 * The input data starts with the structure but it might not be appropriately
 * aligned.
 */
static HeapTuple
get_changed_tuple(char *change)
{
	HeapTupleData tup_data;
	HeapTuple	result;
	char	   *src;

	/*
	 * Ensure alignment before accessing the fields. (This is why we can't use
	 * heap_copytuple() instead of this function.)
	 */
	src = change + offsetof(ConcurrentChange, tup_data);
	memcpy(&tup_data, src, sizeof(HeapTupleData));

	result = (HeapTuple) palloc(HEAPTUPLESIZE + tup_data.t_len);
	memcpy(result, &tup_data, sizeof(HeapTupleData));
	result->t_data = (HeapTupleHeader) ((char *) result + HEAPTUPLESIZE);
	src = change + SizeOfConcurrentChange;
	memcpy(result->t_data, src, result->t_len);

	return result;
}

/*
 * Decode logical changes from the WAL sequence up to end_of_wal.
 */
void
repack_decode_concurrent_changes(LogicalDecodingContext *ctx,
								 XLogRecPtr end_of_wal)
{
	RepackDecodingState *dstate;
	ResourceOwner resowner_old;

	/*
	 * Invalidate the "present" cache before moving to "(recent) history".
	 */
	InvalidateSystemCaches();

	dstate = (RepackDecodingState *) ctx->output_writer_private;
	resowner_old = CurrentResourceOwner;
	CurrentResourceOwner = dstate->resowner;

	PG_TRY();
	{
		while (ctx->reader->EndRecPtr < end_of_wal)
		{
			XLogRecord *record;
			XLogSegNo	segno_new;
			char	   *errm = NULL;
			XLogRecPtr	end_lsn;

			record = XLogReadRecord(ctx->reader, &errm);
			if (errm)
				elog(ERROR, "%s", errm);

			if (record != NULL)
				LogicalDecodingProcessRecord(ctx, ctx->reader);

			/*
			 * If WAL segment boundary has been crossed, inform the decoding
			 * system that the catalog_xmin can advance. (We can confirm more
			 * often, but a filling a single WAL segment should not take much
			 * time.)
			 */
			end_lsn = ctx->reader->EndRecPtr;
			XLByteToSeg(end_lsn, segno_new, wal_segment_size);
			if (segno_new != repack_current_segment)
			{
				LogicalConfirmReceivedLocation(end_lsn);
				elog(DEBUG1, "REPACK: confirmed receive location %X/%X",
					 (uint32) (end_lsn >> 32), (uint32) end_lsn);
				repack_current_segment = segno_new;
			}

			CHECK_FOR_INTERRUPTS();
		}
		InvalidateSystemCaches();
		CurrentResourceOwner = resowner_old;
	}
	PG_CATCH();
	{
		/* clear all timetravel entries */
		InvalidateSystemCaches();
		CurrentResourceOwner = resowner_old;
		PG_RE_THROW();
	}
	PG_END_TRY();
}

/*
 * Apply changes that happened during the initial load.
 *
 * Scan key is passed by caller, so it does not have to be constructed
 * multiple times. Key entries have all fields initialized, except for
 * sk_argument.
 */
static void
apply_concurrent_changes(RepackDecodingState *dstate, Relation rel,
						 ScanKey key, int nkeys, IndexInsertState *iistate)
{
	TupleTableSlot *index_slot,
			   *ident_slot;
	HeapTuple	tup_old = NULL;

	if (dstate->nchanges == 0)
		return;

	/* TupleTableSlot is needed to pass the tuple to ExecInsertIndexTuples(). */
	index_slot = MakeSingleTupleTableSlot(dstate->tupdesc, &TTSOpsHeapTuple);

	/* A slot to fetch tuples from identity index. */
	ident_slot = table_slot_create(rel, NULL);

	while (tuplestore_gettupleslot(dstate->tstore, true, false,
								   dstate->tsslot))
	{
		bool		shouldFree;
		HeapTuple	tup_change,
					tup,
					tup_exist;
		char	   *change_raw,
				   *src;
		ConcurrentChange change;
		bool		isnull[1];
		Datum		values[1];

		CHECK_FOR_INTERRUPTS();

		/* Get the change from the single-column tuple. */
		tup_change = ExecFetchSlotHeapTuple(dstate->tsslot, false, &shouldFree);
		heap_deform_tuple(tup_change, dstate->tupdesc_change, values, isnull);
		Assert(!isnull[0]);

		/* Make sure we access aligned data. */
		change_raw = (char *) DatumGetByteaP(values[0]);
		src = (char *) VARDATA(change_raw);
		memcpy(&change, src, SizeOfConcurrentChange);

		/* TRUNCATE change contains no tuple, so process it separately. */
		if (change.kind == CHANGE_TRUNCATE)
		{
			/*
			 * All the things that ExecuteTruncateGuts() does (such as firing
			 * triggers or handling the DROP_CASCADE behavior) should have
			 * taken place on the source relation. Thus we only do the actual
			 * truncation of the new relation (and its indexes).
			 */
			heap_truncate_one_rel(rel);

			pfree(tup_change);
			continue;
		}

		/*
		 * Extract the tuple from the change. The tuple is copied here because
		 * it might be assigned to 'tup_old', in which case it needs to
		 * survive into the next iteration.
		 */
		tup = get_changed_tuple(src);

		if (change.kind == CHANGE_UPDATE_OLD)
		{
			Assert(tup_old == NULL);
			tup_old = tup;
		}
		else if (change.kind == CHANGE_INSERT)
		{
			Assert(tup_old == NULL);

			apply_concurrent_insert(rel, &change, tup, iistate, index_slot);

			pfree(tup);
		}
		else if (change.kind == CHANGE_UPDATE_NEW ||
				 change.kind == CHANGE_DELETE)
		{
			IndexScanDesc ind_scan = NULL;
			HeapTuple	tup_key;

			if (change.kind == CHANGE_UPDATE_NEW)
			{
				tup_key = tup_old != NULL ? tup_old : tup;
			}
			else
			{
				Assert(tup_old == NULL);
				tup_key = tup;
			}

			/*
			 * Find the tuple to be updated or deleted.
			 */
			tup_exist = find_target_tuple(rel, key, nkeys, tup_key,
										  iistate, ident_slot, &ind_scan);
			if (tup_exist == NULL)
				elog(ERROR, "Failed to find target tuple");

			if (change.kind == CHANGE_UPDATE_NEW)
				apply_concurrent_update(rel, tup, tup_exist, &change, iistate,
										index_slot);
			else
				apply_concurrent_delete(rel, tup_exist, &change);

			if (tup_old != NULL)
			{
				pfree(tup_old);
				tup_old = NULL;
			}

			pfree(tup);
			index_endscan(ind_scan);
		}
		else
			elog(ERROR, "Unrecognized kind of change: %d", change.kind);

		/*
		 * If a change was applied now, increment CID for next writes and
		 * update the snapshot so it sees the changes we've applied so far.
		 */
		if (change.kind != CHANGE_UPDATE_OLD)
		{
			CommandCounterIncrement();
			UpdateActiveSnapshotCommandId();
		}

		/* TTSOpsMinimalTuple has .get_heap_tuple==NULL. */
		Assert(shouldFree);
		pfree(tup_change);
	}

	tuplestore_clear(dstate->tstore);
	dstate->nchanges = 0;

	/* Cleanup. */
	ExecDropSingleTupleTableSlot(index_slot);
	ExecDropSingleTupleTableSlot(ident_slot);
}

static void
apply_concurrent_insert(Relation rel, ConcurrentChange *change, HeapTuple tup,
						IndexInsertState *iistate, TupleTableSlot *index_slot)
{
	List	   *recheck;


	/*
	 * Like simple_heap_insert(), but make sure that the INSERT is not
	 * logically decoded - see reform_and_rewrite_tuple() for more
	 * information.
	 */
	heap_insert(rel, tup, GetCurrentCommandId(true), HEAP_INSERT_NO_LOGICAL,
				NULL);

	/*
	 * Update indexes.
	 *
	 * In case functions in the index need the active snapshot and caller
	 * hasn't set one.
	 */
	ExecStoreHeapTuple(tup, index_slot, false);
	recheck = ExecInsertIndexTuples(iistate->rri,
									index_slot,
									iistate->estate,
									false,	/* update */
									false,	/* noDupErr */
									NULL,	/* specConflict */
									NIL,	/* arbiterIndexes */
									false	/* onlySummarizing */
		);

	/*
	 * If recheck is required, it must have been preformed on the source
	 * relation by now. (All the logical changes we process here are already
	 * committed.)
	 */
	list_free(recheck);

	pgstat_progress_incr_param(PROGRESS_REPACK_HEAP_TUPLES_INSERTED, 1);
}

static void
apply_concurrent_update(Relation rel, HeapTuple tup, HeapTuple tup_target,
						ConcurrentChange *change, IndexInsertState *iistate,
						TupleTableSlot *index_slot)
{
	LockTupleMode lockmode;
	TM_FailureData tmfd;
	TU_UpdateIndexes update_indexes;
	TM_Result	res;
	List	   *recheck;

	/*
	 * Write the new tuple into the new heap. ('tup' gets the TID assigned
	 * here.)
	 *
	 * Do it like in simple_heap_update(), except for 'wal_logical' (and
	 * except for 'wait').
	 */
	res = heap_update(rel, &tup_target->t_self, tup,
					  GetCurrentCommandId(true),
					  InvalidSnapshot,
					  false,	/* no wait - only we are doing changes */
					  &tmfd, &lockmode, &update_indexes,
					  false /* wal_logical */ );
	if (res != TM_Ok)
		ereport(ERROR, (errmsg("failed to apply concurrent UPDATE")));

	ExecStoreHeapTuple(tup, index_slot, false);

	if (update_indexes != TU_None)
	{
		recheck = ExecInsertIndexTuples(iistate->rri,
										index_slot,
										iistate->estate,
										true,	/* update */
										false,	/* noDupErr */
										NULL,	/* specConflict */
										NIL,	/* arbiterIndexes */
		/* onlySummarizing */
										update_indexes == TU_Summarizing);
		list_free(recheck);
	}

	pgstat_progress_incr_param(PROGRESS_REPACK_HEAP_TUPLES_UPDATED, 1);
}

static void
apply_concurrent_delete(Relation rel, HeapTuple tup_target,
						ConcurrentChange *change)
{
	TM_Result	res;
	TM_FailureData tmfd;

	/*
	 * Delete tuple from the new heap.
	 *
	 * Do it like in simple_heap_delete(), except for 'wal_logical' (and
	 * except for 'wait').
	 */
	res = heap_delete(rel, &tup_target->t_self, GetCurrentCommandId(true),
					  InvalidSnapshot, false,
					  &tmfd,
					  false,	/* no wait - only we are doing changes */
					  false /* wal_logical */ );

	if (res != TM_Ok)
		ereport(ERROR, (errmsg("failed to apply concurrent DELETE")));

	pgstat_progress_incr_param(PROGRESS_REPACK_HEAP_TUPLES_DELETED, 1);
}

/*
 * Find the tuple to be updated or deleted.
 *
 * 'key' is a pre-initialized scan key, into which the function will put the
 * key values.
 *
 * 'tup_key' is a tuple containing the key values for the scan.
 *
 * On exit,'*scan_p' contains the scan descriptor used. The caller must close
 * it when he no longer needs the tuple returned.
 */
static HeapTuple
find_target_tuple(Relation rel, ScanKey key, int nkeys, HeapTuple tup_key,
				  IndexInsertState *iistate,
				  TupleTableSlot *ident_slot, IndexScanDesc *scan_p)
{
	IndexScanDesc scan;
	Form_pg_index ident_form;
	int2vector *ident_indkey;
	HeapTuple	result = NULL;

	/* XXX no instrumentation for now */
	scan = index_beginscan(rel, iistate->ident_index, GetActiveSnapshot(),
						   NULL, nkeys, 0);
	*scan_p = scan;
	index_rescan(scan, key, nkeys, NULL, 0);

	/* Info needed to retrieve key values from heap tuple. */
	ident_form = iistate->ident_index->rd_index;
	ident_indkey = &ident_form->indkey;

	/* Use the incoming tuple to finalize the scan key. */
	for (int i = 0; i < scan->numberOfKeys; i++)
	{
		ScanKey		entry;
		bool		isnull;
		int16		attno_heap;

		entry = &scan->keyData[i];
		attno_heap = ident_indkey->values[i];
		entry->sk_argument = heap_getattr(tup_key,
										  attno_heap,
										  rel->rd_att,
										  &isnull);
		Assert(!isnull);
	}
	if (index_getnext_slot(scan, ForwardScanDirection, ident_slot))
	{
		bool		shouldFree;

		result = ExecFetchSlotHeapTuple(ident_slot, false, &shouldFree);
		/* TTSOpsBufferHeapTuple has .get_heap_tuple != NULL. */
		Assert(!shouldFree);
	}

	return result;
}

/*
 * Decode and apply concurrent changes.
 *
 * Pass rel_src iff its reltoastrelid is needed.
 */
static void
process_concurrent_changes(LogicalDecodingContext *ctx, XLogRecPtr end_of_wal,
						   Relation rel_dst, Relation rel_src, ScanKey ident_key,
						   int ident_key_nentries, IndexInsertState *iistate)
{
	RepackDecodingState *dstate;

	pgstat_progress_update_param(PROGRESS_REPACK_PHASE,
								 PROGRESS_REPACK_PHASE_CATCH_UP);

	dstate = (RepackDecodingState *) ctx->output_writer_private;

	repack_decode_concurrent_changes(ctx, end_of_wal);

	if (dstate->nchanges == 0)
		return;

	PG_TRY();
	{
		/*
		 * Make sure that TOAST values can eventually be accessed via the old
		 * relation - see comment in copy_table_data().
		 */
		if (rel_src)
			rel_dst->rd_toastoid = rel_src->rd_rel->reltoastrelid;

		apply_concurrent_changes(dstate, rel_dst, ident_key,
								 ident_key_nentries, iistate);
	}
	PG_FINALLY();
	{
		if (rel_src)
			rel_dst->rd_toastoid = InvalidOid;
	}
	PG_END_TRY();
}

static IndexInsertState *
get_index_insert_state(Relation relation, Oid ident_index_id)
{
	EState	   *estate;
	int			i;
	IndexInsertState *result;

	result = (IndexInsertState *) palloc0(sizeof(IndexInsertState));
	estate = CreateExecutorState();

	result->rri = (ResultRelInfo *) palloc(sizeof(ResultRelInfo));
	InitResultRelInfo(result->rri, relation, 0, 0, 0);
	ExecOpenIndices(result->rri, false);

	/*
	 * Find the relcache entry of the identity index so that we spend no extra
	 * effort to open / close it.
	 */
	for (i = 0; i < result->rri->ri_NumIndices; i++)
	{
		Relation	ind_rel;

		ind_rel = result->rri->ri_IndexRelationDescs[i];
		if (ind_rel->rd_id == ident_index_id)
			result->ident_index = ind_rel;
	}
	if (result->ident_index == NULL)
		elog(ERROR, "Failed to open identity index");

	/* Only initialize fields needed by ExecInsertIndexTuples(). */
	result->estate = estate;

	return result;
}

/*
 * Build scan key to process logical changes.
 */
static ScanKey
build_identity_key(Oid ident_idx_oid, Relation rel_src, int *nentries)
{
	Relation	ident_idx_rel;
	Form_pg_index ident_idx;
	int			n,
				i;
	ScanKey		result;

	Assert(OidIsValid(ident_idx_oid));
	ident_idx_rel = index_open(ident_idx_oid, AccessShareLock);
	ident_idx = ident_idx_rel->rd_index;
	n = ident_idx->indnatts;
	result = (ScanKey) palloc(sizeof(ScanKeyData) * n);
	for (i = 0; i < n; i++)
	{
		ScanKey		entry;
		int16		relattno;
		Form_pg_attribute att;
		Oid			opfamily,
					opcintype,
					opno,
					opcode;

		entry = &result[i];
		relattno = ident_idx->indkey.values[i];
		if (relattno >= 1)
		{
			TupleDesc	desc;

			desc = rel_src->rd_att;
			att = TupleDescAttr(desc, relattno - 1);
		}
		else
			elog(ERROR, "Unexpected attribute number %d in index", relattno);

		opfamily = ident_idx_rel->rd_opfamily[i];
		opcintype = ident_idx_rel->rd_opcintype[i];
		opno = get_opfamily_member(opfamily, opcintype, opcintype,
								   BTEqualStrategyNumber);

		if (!OidIsValid(opno))
			elog(ERROR, "Failed to find = operator for type %u", opcintype);

		opcode = get_opcode(opno);
		if (!OidIsValid(opcode))
			elog(ERROR, "Failed to find = operator for operator %u", opno);

		/* Initialize everything but argument. */
		ScanKeyInit(entry,
					i + 1,
					BTEqualStrategyNumber, opcode,
					(Datum) NULL);
		entry->sk_collation = att->attcollation;
	}
	index_close(ident_idx_rel, AccessShareLock);

	*nentries = n;
	return result;
}

static void
free_index_insert_state(IndexInsertState *iistate)
{
	ExecCloseIndices(iistate->rri);
	FreeExecutorState(iistate->estate);
	pfree(iistate->rri);
	pfree(iistate);
}

static void
cleanup_logical_decoding(LogicalDecodingContext *ctx)
{
	RepackDecodingState *dstate;

	dstate = (RepackDecodingState *) ctx->output_writer_private;

	ExecDropSingleTupleTableSlot(dstate->tsslot);
	FreeTupleDesc(dstate->tupdesc_change);
	FreeTupleDesc(dstate->tupdesc);
	tuplestore_end(dstate->tstore);

	FreeDecodingContext(ctx);
}

/*
 * The final steps of rebuild_relation() for concurrent processing.
 *
 * On entry, NewHeap is locked in AccessExclusiveLock mode. OldHeap and its
 * clustering index (if one is passed) are still locked in a mode that allows
 * concurrent data changes. On exit, both tables and their indexes are closed,
 * but locked in AccessExclusiveLock mode.
 */
static void
rebuild_relation_finish_concurrent(Relation NewHeap, Relation OldHeap,
								   Relation cl_index,
								   LogicalDecodingContext *ctx,
								   bool swap_toast_by_content,
								   TransactionId frozenXid,
								   MultiXactId cutoffMulti)
{
	LOCKMODE	lockmode_old PG_USED_FOR_ASSERTS_ONLY;
	List	   *ind_oids_new;
	Oid			old_table_oid = RelationGetRelid(OldHeap);
	Oid			new_table_oid = RelationGetRelid(NewHeap);
	List	   *ind_oids_old = RelationGetIndexList(OldHeap);
	ListCell   *lc,
			   *lc2;
	char		relpersistence;
	bool		is_system_catalog;
	Oid			ident_idx_old,
				ident_idx_new;
	IndexInsertState *iistate;
	ScanKey		ident_key;
	int			ident_key_nentries;
	XLogRecPtr	wal_insert_ptr,
				end_of_wal;
	char		dummy_rec_data = '\0';
	Relation   *ind_refs,
			   *ind_refs_p;
	int			nind;

	/* Like in cluster_rel(). */
	lockmode_old = ShareUpdateExclusiveLock;
	Assert(CheckRelationLockedByMe(OldHeap, lockmode_old, false));
	Assert(cl_index == NULL ||
		   CheckRelationLockedByMe(cl_index, lockmode_old, false));
	/* This is expected from the caller. */
	Assert(CheckRelationLockedByMe(NewHeap, AccessExclusiveLock, false));

	ident_idx_old = RelationGetReplicaIndex(OldHeap);

	/*
	 * Unlike the exclusive case, we build new indexes for the new relation
	 * rather than swapping the storage and reindexing the old relation. The
	 * point is that the index build can take some time, so we do it before we
	 * get AccessExclusiveLock on the old heap and therefore we cannot swap
	 * the heap storage yet.
	 *
	 * index_create() will lock the new indexes using AccessExclusiveLock - no
	 * need to change that.
	 *
	 * We assume that ShareUpdateExclusiveLock on the table prevents anyone
	 * from dropping the existing indexes or adding new ones, so the lists of
	 * old and new indexes should match at the swap time. On the other hand we
	 * do not block ALTER INDEX commands that do not require table lock (e.g.
	 * ALTER INDEX ... SET ...).
	 *
	 * XXX Should we check a the end of our work if another transaction
	 * executed such a command and issue a NOTICE that we might have discarded
	 * its effects? (For example, someone changes storage parameter after we
	 * have created the new index, the new value of that parameter is lost.)
	 * Alternatively, we can lock all the indexes now in a mode that blocks
	 * all the ALTER INDEX commands (ShareUpdateExclusiveLock ?), and keep
	 * them locked till the end of the transactions. That might increase the
	 * risk of deadlock during the lock upgrade below, however SELECT / DML
	 * queries should not be involved in such a deadlock.
	 */
	ind_oids_new = build_new_indexes(NewHeap, OldHeap, ind_oids_old);

	/*
	 * Processing shouldn't start w/o valid identity index.
	 */
	Assert(OidIsValid(ident_idx_old));

	/* Find "identity index" on the new relation. */
	ident_idx_new = InvalidOid;
	forboth(lc, ind_oids_old, lc2, ind_oids_new)
	{
		Oid			ind_old = lfirst_oid(lc);
		Oid			ind_new = lfirst_oid(lc2);

		if (ident_idx_old == ind_old)
		{
			ident_idx_new = ind_new;
			break;
		}
	}
	if (!OidIsValid(ident_idx_new))

		/*
		 * Should not happen, given our lock on the old relation.
		 */
		ereport(ERROR,
				(errmsg("Identity index missing on the new relation")));

	/* Executor state to update indexes. */
	iistate = get_index_insert_state(NewHeap, ident_idx_new);

	/*
	 * Build scan key that we'll use to look for rows to be updated / deleted
	 * during logical decoding.
	 */
	ident_key = build_identity_key(ident_idx_new, OldHeap, &ident_key_nentries);

	/*
	 * Flush all WAL records inserted so far (possibly except for the last
	 * incomplete page, see GetInsertRecPtr), to minimize the amount of data
	 * we need to flush while holding exclusive lock on the source table.
	 */
	wal_insert_ptr = GetInsertRecPtr();
	XLogFlush(wal_insert_ptr);
	end_of_wal = GetFlushRecPtr(NULL);

	/*
	 * Apply concurrent changes first time, to minimize the time we need to
	 * hold AccessExclusiveLock. (Quite some amount of WAL could have been
	 * written during the data copying and index creation.)
	 */
	process_concurrent_changes(ctx, end_of_wal, NewHeap,
							   swap_toast_by_content ? OldHeap : NULL,
							   ident_key, ident_key_nentries, iistate);

	/*
	 * Acquire AccessExclusiveLock on the table, its TOAST relation (if there
	 * is one), all its indexes, so that we can swap the files.
	 *
	 * Before that, unlock the index temporarily to avoid deadlock in case
	 * another transaction is trying to lock it while holding the lock on the
	 * table.
	 */
	if (cl_index)
	{
		index_close(cl_index, ShareUpdateExclusiveLock);
		cl_index = NULL;
	}
	/* For the same reason, unlock TOAST relation. */
	if (OldHeap->rd_rel->reltoastrelid)
		LockRelationOid(OldHeap->rd_rel->reltoastrelid, AccessExclusiveLock);
	/* Finally lock the table */
	LockRelationOid(old_table_oid, AccessExclusiveLock);

	/*
	 * Lock all indexes now, not only the clustering one: all indexes need to
	 * have their files swapped. While doing that, store their relation
	 * references in an array, to handle predicate locks below.
	 */
	ind_refs_p = ind_refs = palloc_array(Relation, list_length(ind_oids_old));
	nind = 0;
	foreach(lc, ind_oids_old)
	{
		Oid			ind_oid;
		Relation	index;

		ind_oid = lfirst_oid(lc);
		index = index_open(ind_oid, AccessExclusiveLock);

		/*
		 * TODO 1) Do we need to check if ALTER INDEX was executed since the
		 * new index was created in build_new_indexes()? 2) Specifically for
		 * the clustering index, should check_index_is_clusterable() be called
		 * here? (Not sure about the latter: ShareUpdateExclusiveLock on the
		 * table probably blocks all commands that affect the result of
		 * check_index_is_clusterable().)
		 */
		*ind_refs_p = index;
		ind_refs_p++;
		nind++;
	}

	/*
	 * In addition, lock the OldHeap's TOAST relation exclusively - again, the
	 * lock is needed to swap the files.
	 */
	if (OidIsValid(OldHeap->rd_rel->reltoastrelid))
		LockRelationOid(OldHeap->rd_rel->reltoastrelid, AccessExclusiveLock);

	/*
	 * Tuples and pages of the old heap will be gone, but the heap will stay.
	 */
	TransferPredicateLocksToHeapRelation(OldHeap);
	/* The same for indexes. */
	for (int i = 0; i < nind; i++)
	{
		Relation	index = ind_refs[i];

		TransferPredicateLocksToHeapRelation(index);

		/*
		 * References to indexes on the old relation are not needed anymore,
		 * however locks stay till the end of the transaction.
		 */
		index_close(index, NoLock);
	}
	pfree(ind_refs);

	/*
	 * Flush anything we see in WAL, to make sure that all changes committed
	 * while we were waiting for the exclusive lock are available for
	 * decoding. This should not be necessary if all backends had
	 * synchronous_commit set, but we can't rely on this setting.
	 *
	 * Unfortunately, GetInsertRecPtr() may lag behind the actual insert
	 * position, and GetLastImportantRecPtr() points at the start of the last
	 * record rather than at the end. Thus the simplest way to determine the
	 * insert position is to insert a dummy record and use its LSN.
	 *
	 * XXX Consider using GetLastImportantRecPtr() and adding the size of the
	 * last record (plus the total size of all the page headers the record
	 * spans)?
	 */
	XLogBeginInsert();
	XLogRegisterData(&dummy_rec_data, 1);
	wal_insert_ptr = XLogInsert(RM_XLOG_ID, XLOG_NOOP);
	XLogFlush(wal_insert_ptr);
	end_of_wal = GetFlushRecPtr(NULL);

	/* Apply the concurrent changes again. */
	process_concurrent_changes(ctx, end_of_wal, NewHeap,
							   swap_toast_by_content ? OldHeap : NULL,
							   ident_key, ident_key_nentries, iistate);

	/* Remember info about rel before closing OldHeap */
	relpersistence = OldHeap->rd_rel->relpersistence;
	is_system_catalog = IsSystemRelation(OldHeap);

	pgstat_progress_update_param(PROGRESS_REPACK_PHASE,
								 PROGRESS_REPACK_PHASE_SWAP_REL_FILES);

	/*
	 * Even ShareUpdateExclusiveLock should have prevented others from
	 * creating / dropping indexes (even using the CONCURRENTLY option), so we
	 * do not need to check whether the lists match.
	 */
	forboth(lc, ind_oids_old, lc2, ind_oids_new)
	{
		Oid			ind_old = lfirst_oid(lc);
		Oid			ind_new = lfirst_oid(lc2);
		Oid			mapped_tables[4];

		/* Zero out possible results from swapped_relation_files */
		memset(mapped_tables, 0, sizeof(mapped_tables));

		swap_relation_files(ind_old, ind_new,
							(old_table_oid == RelationRelationId),
							swap_toast_by_content,
							true,
							InvalidTransactionId,
							InvalidMultiXactId,
							mapped_tables);

#ifdef USE_ASSERT_CHECKING

		/*
		 * Concurrent processing is not supported for system relations, so
		 * there should be no mapped tables.
		 */
		for (int i = 0; i < 4; i++)
			Assert(mapped_tables[i] == 0);
#endif
	}

	/* The new indexes must be visible for deletion. */
	CommandCounterIncrement();

	/* Close the old heap but keep lock until transaction commit. */
	table_close(OldHeap, NoLock);
	/* Close the new heap. (We didn't have to open its indexes). */
	table_close(NewHeap, NoLock);

	/* Cleanup what we don't need anymore. (And close the identity index.) */
	pfree(ident_key);
	free_index_insert_state(iistate);

	/*
	 * Swap the relations and their TOAST relations and TOAST indexes. This
	 * also drops the new relation and its indexes.
	 *
	 * (System catalogs are currently not supported.)
	 */
	Assert(!is_system_catalog);
	finish_heap_swap(old_table_oid, new_table_oid,
					 is_system_catalog,
					 swap_toast_by_content,
					 false, true, false,
					 frozenXid, cutoffMulti,
					 relpersistence);
}

/*
 * Build indexes on NewHeap according to those on OldHeap.
 *
 * OldIndexes is the list of index OIDs on OldHeap.
 *
 * A list of OIDs of the corresponding indexes created on NewHeap is
 * returned. The order of items does match, so we can use these arrays to swap
 * index storage.
 */
static List *
build_new_indexes(Relation NewHeap, Relation OldHeap, List *OldIndexes)
{
	StringInfo	ind_name;
	ListCell   *lc;
	List	   *result = NIL;

	pgstat_progress_update_param(PROGRESS_REPACK_PHASE,
								 PROGRESS_REPACK_PHASE_REBUILD_INDEX);

	ind_name = makeStringInfo();

	foreach(lc, OldIndexes)
	{
		Oid			ind_oid,
					ind_oid_new,
					tbsp_oid;
		Relation	ind;
		IndexInfo  *ind_info;
		int			i,
					heap_col_id;
		List	   *colnames;
		int16		indnatts;
		Oid		   *collations,
				   *opclasses;
		HeapTuple	tup;
		bool		isnull;
		Datum		d;
		oidvector  *oidvec;
		int2vector *int2vec;
		size_t		oid_arr_size;
		size_t		int2_arr_size;
		int16	   *indoptions;
		text	   *reloptions = NULL;
		bits16		flags;
		Datum	   *opclassOptions;
		NullableDatum *stattargets;

		ind_oid = lfirst_oid(lc);
		ind = index_open(ind_oid, AccessShareLock);
		ind_info = BuildIndexInfo(ind);

		tbsp_oid = ind->rd_rel->reltablespace;

		/*
		 * Index name really doesn't matter, we'll eventually use only their
		 * storage. Just make them unique within the table.
		 */
		resetStringInfo(ind_name);
		appendStringInfo(ind_name, "ind_%d",
						 list_cell_number(OldIndexes, lc));

		flags = 0;
		if (ind->rd_index->indisprimary)
			flags |= INDEX_CREATE_IS_PRIMARY;

		colnames = NIL;
		indnatts = ind->rd_index->indnatts;
		oid_arr_size = sizeof(Oid) * indnatts;
		int2_arr_size = sizeof(int16) * indnatts;

		collations = (Oid *) palloc(oid_arr_size);
		for (i = 0; i < indnatts; i++)
		{
			char	   *colname;

			heap_col_id = ind->rd_index->indkey.values[i];
			if (heap_col_id > 0)
			{
				Form_pg_attribute att;

				/* Normal attribute. */
				att = TupleDescAttr(OldHeap->rd_att, heap_col_id - 1);
				colname = pstrdup(NameStr(att->attname));
				collations[i] = att->attcollation;
			}
			else if (heap_col_id == 0)
			{
				HeapTuple	tuple;
				Form_pg_attribute att;

				/*
				 * Expression column is not present in relcache. What we need
				 * here is an attribute of the *index* relation.
				 */
				tuple = SearchSysCache2(ATTNUM,
										ObjectIdGetDatum(ind_oid),
										Int16GetDatum(i + 1));
				if (!HeapTupleIsValid(tuple))
					elog(ERROR,
						 "cache lookup failed for attribute %d of relation %u",
						 i + 1, ind_oid);
				att = (Form_pg_attribute) GETSTRUCT(tuple);
				colname = pstrdup(NameStr(att->attname));
				collations[i] = att->attcollation;
				ReleaseSysCache(tuple);
			}
			else
				elog(ERROR, "Unexpected column number: %d",
					 heap_col_id);

			colnames = lappend(colnames, colname);
		}

		/*
		 * Special effort needed for variable length attributes of
		 * Form_pg_index.
		 */
		tup = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(ind_oid));
		if (!HeapTupleIsValid(tup))
			elog(ERROR, "cache lookup failed for index %u", ind_oid);
		d = SysCacheGetAttr(INDEXRELID, tup, Anum_pg_index_indclass, &isnull);
		Assert(!isnull);
		oidvec = (oidvector *) DatumGetPointer(d);
		opclasses = (Oid *) palloc(oid_arr_size);
		memcpy(opclasses, oidvec->values, oid_arr_size);

		d = SysCacheGetAttr(INDEXRELID, tup, Anum_pg_index_indoption,
							&isnull);
		Assert(!isnull);
		int2vec = (int2vector *) DatumGetPointer(d);
		indoptions = (int16 *) palloc(int2_arr_size);
		memcpy(indoptions, int2vec->values, int2_arr_size);
		ReleaseSysCache(tup);

		tup = SearchSysCache1(RELOID, ObjectIdGetDatum(ind_oid));
		if (!HeapTupleIsValid(tup))
			elog(ERROR, "cache lookup failed for index relation %u", ind_oid);
		d = SysCacheGetAttr(RELOID, tup, Anum_pg_class_reloptions, &isnull);
		reloptions = !isnull ? DatumGetTextPCopy(d) : NULL;
		ReleaseSysCache(tup);

		opclassOptions = palloc0(sizeof(Datum) * ind_info->ii_NumIndexAttrs);
		for (i = 0; i < ind_info->ii_NumIndexAttrs; i++)
			opclassOptions[i] = get_attoptions(ind_oid, i + 1);

		stattargets = get_index_stattargets(ind_oid, ind_info);

		/*
		 * Neither parentIndexRelid nor parentConstraintId needs to be passed
		 * since the new catalog entries (pg_constraint, pg_inherits) would
		 * eventually be dropped. Therefore there's no need to record valid
		 * dependency on parents.
		 */
		ind_oid_new = index_create(NewHeap,
								   ind_name->data,
								   InvalidOid,
								   InvalidOid,	/* parentIndexRelid */
								   InvalidOid,	/* parentConstraintId */
								   InvalidOid,
								   ind_info,
								   colnames,
								   ind->rd_rel->relam,
								   tbsp_oid,
								   collations,
								   opclasses,
								   opclassOptions,
								   indoptions,
								   stattargets,
								   PointerGetDatum(reloptions),
								   flags,	/* flags */
								   0,	/* constr_flags */
								   false,	/* allow_system_table_mods */
								   false,	/* is_internal */
								   NULL /* constraintId */
			);
		result = lappend_oid(result, ind_oid_new);

		index_close(ind, AccessShareLock);
		list_free_deep(colnames);
		pfree(collations);
		pfree(opclasses);
		pfree(indoptions);
		if (reloptions)
			pfree(reloptions);
	}

	return result;
}
