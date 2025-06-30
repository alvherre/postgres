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

#include "nodes/parsenodes.h"
#include "parser/parse_node.h"
#include "storage/lock.h"
#include "utils/relcache.h"


/* flag bits for ClusterParams->options */
#define CLUOPT_VERBOSE 0x01		/* print progress info */
#define CLUOPT_RECHECK 0x02		/* recheck relation state */
#define CLUOPT_RECHECK_ISCLUSTERED 0x04 /* recheck relation state for
										 * indisclustered */

/* options for CLUSTER */
typedef struct ClusterParams
{
	bits32		options;		/* bitmask of CLUOPT_* */
} ClusterParams;

/*
 * cluster.c currently implements three nearly identical commands: CLUSTER,
 * VACUUM FULL and REPACK. Where needed, use this enumeration to distinguish
 * which of these commands is being executed.
 *
 * Remove this stuff when removing the (now deprecated) CLUSTER and VACUUM
 * FULL commands.
 */
typedef enum ClusterCommand
{
	CLUSTER_COMMAND_CLUSTER,
	CLUSTER_COMMAND_REPACK,
	CLUSTER_COMMAND_VACUUM
} ClusterCommand;

extern void cluster(ParseState *pstate, ClusterStmt *stmt, bool isTopLevel);
extern void cluster_rel(Relation OldHeap, Oid indexOid, ClusterParams *params,
						ClusterCommand cmd);
extern void check_index_is_clusterable(Relation OldHeap, Oid indexOid,
									   LOCKMODE lockmode);
extern void mark_index_clustered(Relation rel, Oid indexOid, bool is_internal);

extern Oid	make_new_heap(Oid OIDOldHeap, Oid NewTableSpace, Oid NewAccessMethod,
						  char relpersistence, LOCKMODE lockmode);
extern void finish_heap_swap(Oid OIDOldHeap, Oid OIDNewHeap,
							 bool is_system_catalog,
							 bool swap_toast_by_content,
							 bool check_constraints,
							 bool is_internal,
							 TransactionId frozenXid,
							 MultiXactId cutoffMulti,
							 char newrelpersistence);

extern void repack(ParseState *pstate, RepackStmt *stmt, bool isTopLevel);
#endif							/* CLUSTER_H */
