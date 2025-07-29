/*-------------------------------------------------------------------------
 *
 * cluster.h
 *	  header file for postgres cluster command stuff
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994-5, Regents of the University of California
 *
 * src/include/commands/cluster.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef CLUSTER_H
#define CLUSTER_H

#include "nodes/execnodes.h"
#include "nodes/parsenodes.h"
#include "parser/parse_node.h"
#include "replication/logical.h"
#include "storage/lock.h"
#include "storage/relfilelocator.h"
#include "utils/relcache.h"
#include "utils/resowner.h"
#include "utils/tuplestore.h"


/* flag bits for ClusterParams->options */
#define CLUOPT_VERBOSE 0x01		/* print progress info */
#define CLUOPT_RECHECK 0x02		/* recheck relation state */
#define CLUOPT_RECHECK_ISCLUSTERED 0x04 /* recheck relation state for
										 * indisclustered */
#define CLUOPT_CONCURRENT 0x08	/* allow concurrent data changes */

/* options for CLUSTER */
typedef struct ClusterParams
{
	bits32		options;		/* bitmask of CLUOPT_* */
} ClusterParams;


/*
 * The following definitions are used by REPACK CONCURRENTLY.
 */

extern RelFileLocator repacked_rel_locator;
extern RelFileLocator repacked_rel_toast_locator;

typedef enum
{
	CHANGE_INSERT,
	CHANGE_UPDATE_OLD,
	CHANGE_UPDATE_NEW,
	CHANGE_DELETE,
	CHANGE_TRUNCATE
} ConcurrentChangeKind;

typedef struct ConcurrentChange
{
	/* See the enum above. */
	ConcurrentChangeKind kind;

	/*
	 * The actual tuple.
	 *
	 * The tuple data follows the ConcurrentChange structure. Before use make
	 * sure the tuple is correctly aligned (ConcurrentChange can be stored as
	 * bytea) and that tuple->t_data is fixed.
	 */
	HeapTupleData tup_data;
} ConcurrentChange;

#define SizeOfConcurrentChange (offsetof(ConcurrentChange, tup_data) + \
								sizeof(HeapTupleData))

/*
 * Logical decoding state.
 *
 * Here we store the data changes that we decode from WAL while the table
 * contents is being copied to a new storage. Also the necessary metadata
 * needed to apply these changes to the table is stored here.
 */
typedef struct RepackDecodingState
{
	/* The relation whose changes we're decoding. */
	Oid			relid;

	/*
	 * Decoded changes are stored here. Although we try to avoid excessive
	 * batches, it can happen that the changes need to be stored to disk. The
	 * tuplestore does this transparently.
	 */
	Tuplestorestate *tstore;

	/* The current number of changes in tstore. */
	double		nchanges;

	/*
	 * Descriptor to store the ConcurrentChange structure serialized (bytea).
	 * We can't store the tuple directly because tuplestore only supports
	 * minimum tuple and we may need to transfer OID system column from the
	 * output plugin. Also we need to transfer the change kind, so it's better
	 * to put everything in the structure than to use 2 tuplestores "in
	 * parallel".
	 */
	TupleDesc	tupdesc_change;

	/* Tuple descriptor needed to update indexes. */
	TupleDesc	tupdesc;

	/* Slot to retrieve data from tstore. */
	TupleTableSlot *tsslot;

	ResourceOwner resowner;
} RepackDecodingState;



extern void ExecRepack(ParseState *pstate, RepackStmt *stmt, bool isTopLevel);

extern void cluster_rel(RepackCommand command, bool usingindex,
						Relation OldHeap, Oid indexOid, ClusterParams *params,
						bool isTopLevel);
extern void check_index_is_clusterable(Relation OldHeap, Oid indexOid,
									   LOCKMODE lockmode);
extern void mark_index_clustered(Relation rel, Oid indexOid, bool is_internal);

extern void repack_decode_concurrent_changes(LogicalDecodingContext *ctx,
											 XLogRecPtr end_of_wal);

extern Oid	make_new_heap(Oid OIDOldHeap, Oid NewTableSpace, Oid NewAccessMethod,
						  char relpersistence, LOCKMODE lockmode);
extern void finish_heap_swap(Oid OIDOldHeap, Oid OIDNewHeap,
							 bool is_system_catalog,
							 bool swap_toast_by_content,
							 bool check_constraints,
							 bool is_internal,
							 bool reindex,
							 TransactionId frozenXid,
							 MultiXactId cutoffMulti,
							 char newrelpersistence);

#endif							/* CLUSTER_H */
