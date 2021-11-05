/*-------------------------------------------------------------------------
 *
 * parse_merge.c
 *	  handle merge-statement in parser
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/parser/parse_merge.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "miscadmin.h"

#include "access/sysattr.h"
#include "nodes/makefuncs.h"
#include "parser/analyze.h"
#include "parser/parse_collate.h"
#include "parser/parsetree.h"
#include "parser/parser.h"
#include "parser/parse_clause.h"
#include "parser/parse_cte.h"
#include "parser/parse_merge.h"
#include "parser/parse_relation.h"
#include "parser/parse_target.h"
#include "utils/rel.h"
#include "utils/relcache.h"

static void	transformMergeJoinClause(ParseState *pstate, Node *merge,
									 List **mergeSourceTargetList);
static void setNamespaceForMergeWhen(ParseState *pstate,
									 MergeWhenClause *mergeWhenClause);
static void setNamespaceVisibilityForRTE(List *namespace, RangeTblEntry *rte,
										 bool rel_visible,
										 bool cols_visible);

/*
 *	Special handling for MERGE statement is required because we assemble
 *	the query manually. This is similar to setTargetTable() followed
 *	by transformFromClause() but with a few less steps.
 *
 *	Process the FROM clause and add items to the query's range table,
 *	joinlist, and namespace.
 *
 *	A special targetlist comprising of the columns from the right-subtree of
 *	the join is populated and returned. Note that when the JoinExpr is
 *	setup by transformMergeStmt, the left subtree has the target result
 *	relation and the right subtree has the source relation.
 */
static void
transformMergeJoinClause(ParseState *pstate, Node *merge,
						 List **mergeSourceTargetList)
{
	ParseNamespaceItem *top_nsitem;
	ParseNamespaceItem *right_nsitem;
	List	   *namespace;
	Node	   *n;
	int			mergeSourceRTE;

	/*
	 * Transform our ficticious join of the target and the source tables, and
	 * add it to the rtable.
	 */
	n = transformFromClauseItem(pstate, merge,
								&top_nsitem,
								&right_nsitem,
								&namespace);
	/*
	 * the Join RTE that was added is at the end of the rtable; that's the
	 * merge source relation.
	 */
	mergeSourceRTE = list_length(pstate->p_rtable);

	/* That's also MERGE's tuple source */
	pstate->p_joinlist = list_make1(n);

	/*
	 * We created an internal join between the target and the source relation
	 * to carry out the MERGE actions. Normally such an unaliased join hides
	 * the joining relations, unless the column references are qualified.
	 * Also, any unqualified column references are resolved to the Join RTE,
	 * if there is a matching entry in the targetlist. But the way MERGE
	 * execution is later setup, we expect all column references to resolve to
	 * either the source or the target relation. Hence we must not add the
	 * Join RTE to the namespace.
	 *
	 * The last entry must be for the top-level Join RTE. We don't want to
	 * resolve any references to the Join RTE. So discard that.
	 *
	 * We also do not want to resolve any references from the leftside of the
	 * Join since that corresponds to the target relation. References to the
	 * columns of the target relation must be resolved from the result
	 * relation and not the one that is used in the join.
	 *
	 * XXX this would be less hackish if we told transformFromClauseItem not
	 * to add the new RTE element to the namespace
	 */
	Assert(list_length(namespace) > 1);
	namespace = list_truncate(namespace, list_length(namespace) - 1);
	pstate->p_namespace = list_concat(pstate->p_namespace, namespace);

	setNamespaceVisibilityForRTE(pstate->p_namespace,
								 rt_fetch(mergeSourceRTE, pstate->p_rtable),
								 false, false);

	/*
	 * Expand the right relation and add its columns to the
	 * mergeSourceTargetList. Note that the right relation can either be a
	 * plain relation or a subquery or anything that can have a
	 * RangeTableEntry.
	 *
	 * XXX originally this only expanded right_nsitem, not top_nsitem; but
	 * that meant that some targetlist entries would be missing that are
	 * needed at setrefs.c time.  I'm not sure that this change is correct,
	 * but at least it makes the tests pass.
	 *
	 * FIXME With this change, we don't need the &right_nsitem kludge that
	 * is added to transformFromClauseItem, so consider removing it.
	 */
	*mergeSourceTargetList = expandNSItemAttrs(pstate, top_nsitem, 0, false, -1);
}

/*
 * Make appropriate changes to the namespace visibility while transforming
 * individual action's quals and targetlist expressions. In particular, for
 * INSERT actions we must only see the source relation (since INSERT action is
 * invoked for NOT MATCHED tuples and hence there is no target tuple to deal
 * with). On the other hand, UPDATE and DELETE actions can see both source and
 * target relations.
 *
 * Also, since the internal Join node can hide the source and target
 * relations, we must explicitly make the respective relation as visible so
 * that columns can be referenced unqualified from these relations.
 */
static void
setNamespaceForMergeWhen(ParseState *pstate, MergeWhenClause *mergeWhenClause)
{
	RangeTblEntry *targetRelRTE,
			   *sourceRelRTE;

	/* Assume target relation is at index 1 */
	targetRelRTE = rt_fetch(1, pstate->p_rtable);

	/*
	 * Assume that the top-level join RTE is at the end. The source relation
	 * is just before that.
	 */
	sourceRelRTE = rt_fetch(list_length(pstate->p_rtable) - 1, pstate->p_rtable);

	switch (mergeWhenClause->commandType)
	{
		case CMD_INSERT:

			/*
			 * Inserts can't see target relation, but they can see source
			 * relation.
			 */
			setNamespaceVisibilityForRTE(pstate->p_namespace,
										 targetRelRTE, false, false);
			setNamespaceVisibilityForRTE(pstate->p_namespace,
										 sourceRelRTE, true, true);
			break;

		case CMD_UPDATE:
		case CMD_DELETE:

			/*
			 * Updates and deletes can see both target and source relations.
			 */
			setNamespaceVisibilityForRTE(pstate->p_namespace,
										 targetRelRTE, true, true);
			setNamespaceVisibilityForRTE(pstate->p_namespace,
										 sourceRelRTE, true, true);
			break;

		case CMD_NOTHING:
			break;
		default:
			elog(ERROR, "unknown action in MERGE WHEN clause");
	}
}

/*
 * transformMergeStmt -
 *	  transforms a MERGE statement
 */
Query *
transformMergeStmt(ParseState *pstate, MergeStmt *stmt)
{
	Query	   *qry = makeNode(Query);
	ListCell   *l;
	AclMode		targetPerms = ACL_NO_RIGHTS;
	bool		is_terminal[2];
	JoinExpr   *joinexpr;
	List	   *mergeSourceTargetList;
	List	   *mergeActionList;

	/* There can't be any outer WITH to worry about */
	Assert(pstate->p_ctenamespace == NIL);

	qry->commandType = CMD_MERGE;
	qry->hasRecursive = false;

	/* process the WITH clause independently of all else */
	if (stmt->withClause)
	{
		if (stmt->withClause->recursive)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("WITH RECURSIVE is not supported for MERGE statement")));

		qry->cteList = transformWithClause(pstate, stmt->withClause);
		qry->hasModifyingCTE = pstate->p_hasModifyingCTE;
	}

	/*
	 * Check WHEN clauses for permissions and sanity
	 */
	is_terminal[0] = false;
	is_terminal[1] = false;
	foreach(l, stmt->mergeWhenClauses)
	{
		MergeWhenClause *mergeWhenClause = (MergeWhenClause *) lfirst(l);
		int			when_type = (mergeWhenClause->matched ? 0 : 1);

		/*
		 * Collect action types so we can check Target permissions
		 */
		switch (mergeWhenClause->commandType)
		{
			case CMD_INSERT:
				targetPerms |= ACL_INSERT;
				break;
			case CMD_UPDATE:
				targetPerms |= ACL_UPDATE;
				break;
			case CMD_DELETE:
				targetPerms |= ACL_DELETE;
				break;
			case CMD_NOTHING:
				break;
			default:
				elog(ERROR, "unknown action in MERGE WHEN clause");
		}

		/*
		 * Check for unreachable WHEN clauses
		 */
		if (mergeWhenClause->condition == NULL)
			is_terminal[when_type] = true;
		else if (is_terminal[when_type])
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("unreachable WHEN clause specified after unconditional WHEN clause")));
	}

	/*
	 * Construct a query of the form SELECT relation.ctid	--junk attribute
	 * ,relation.tableoid	--junk attribute ,source_relation.<somecols>
	 * ,relation.<somecols> FROM relation RIGHT JOIN source_relation ON
	 * join_condition; -- no WHERE clause - all conditions are applied in
	 * executor
	 *
	 * stmt->relation is the target relation, given as a RangeVar
	 * stmt->source_relation is a RangeVar or subquery
	 *
	 * We specify the join as a RIGHT JOIN as a simple way of forcing the
	 * first (larg) RTE to refer to the target table.
	 *
	 * The MERGE query's join can be tuned in some cases, see below for these
	 * special case tweaks.
	 *
	 * We set QSRC_PARSER to show query constructed in parse analysis
	 *
	 * Note that we have only one Query for a MERGE statement and the planner
	 * is called only once. That query is executed once to produce our stream
	 * of candidate change rows, so the query must contain all of the columns
	 * required by each of the targetlist or conditions for each action.
	 *
	 * As top-level statements INSERT, UPDATE and DELETE have a Query, whereas
	 * with MERGE the individual actions do not require separate planning,
	 * only different handling in the executor. See nodeModifyTable handling
	 * of commandType CMD_MERGE.
	 *
	 * A sub-query can include the Target, but otherwise the sub-query cannot
	 * reference the outermost Target table at all.
	 */

	/*
	 * Setup the MERGE target table.
	 */
	qry->resultRelation = setTargetTable(pstate, stmt->relation,
										 stmt->relation->inh,
										 false, targetPerms);

	/*
	 * Create a JOIN between the target and the source relation.
	 */
	joinexpr = makeNode(JoinExpr);
	joinexpr->isNatural = false;
	joinexpr->alias = NULL;
	joinexpr->usingClause = NIL;
	joinexpr->quals = stmt->join_condition;
	joinexpr->larg = (Node *) makeNode(RangeTblRef);
	((RangeTblRef *) joinexpr->larg)->rtindex = qry->resultRelation;
	joinexpr->rarg = (Node *) stmt->source_relation;

	/*
	 * Simplify the MERGE query as much as possible
	 *
	 * These seem like things that could go into Optimizer, but they are
	 * semantic simplifications rather than optimizations, per se.
	 *
	 * If there are no INSERT actions we won't be using the non-matching
	 * candidate rows for anything, so no need for an outer join. We do still
	 * need an inner join for UPDATE and DELETE actions.
	 */
	if (targetPerms & ACL_INSERT)
		joinexpr->jointype = JOIN_RIGHT;
	else
		joinexpr->jointype = JOIN_INNER;

	/*
	 * We use a special purpose transformation here because the normal
	 * routines don't quite work right for the MERGE case.
	 *
	 * A special mergeSourceTargetList is setup by transformMergeJoinClause().
	 * It refers to all the attributes output by the join.
	 */
	transformMergeJoinClause(pstate, (Node *) joinexpr,
							 &mergeSourceTargetList);
	qry->targetList = mergeSourceTargetList;


	/* qry has no WHERE clause so absent quals are shown as NULL */
	/* FIXME -- we need to make the jointree be the merge target table only;
	 * the fake jointree needs to be in a separate RTE
	 */

	qry->jointree = makeFromExpr(pstate->p_joinlist, NULL);
	qry->rtable = pstate->p_rtable;

	/*
	 * XXX MERGE is unsupported in various cases
	 */
	if (!(pstate->p_target_relation->rd_rel->relkind == RELKIND_RELATION ||
		  pstate->p_target_relation->rd_rel->relkind == RELKIND_PARTITIONED_TABLE))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("MERGE is not supported for this relation type")));

	if (pstate->p_target_relation->rd_rel->relkind != RELKIND_PARTITIONED_TABLE &&
		pstate->p_target_relation->rd_rel->relhassubclass)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("MERGE is not supported for relations with inheritance")));

	if (pstate->p_target_relation->rd_rel->relhasrules)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("MERGE is not supported for relations with rules")));

	/*
	 * We now have a good query shape, so now look at the when conditions and
	 * action targetlists.
	 *
	 * Overall, the MERGE Query's targetlist is NIL.
	 *
	 * Each individual action has its own targetlist that needs separate
	 * transformation. These transforms don't do anything to the overall
	 * targetlist, since that is only used for resjunk columns.
	 *
	 * We can reference any column in Target or Source, which is OK because
	 * both of those already have RTEs. There is nothing like the EXCLUDED
	 * pseudo-relation for INSERT ON CONFLICT.
	 */
	mergeActionList = NIL;
	foreach(l, stmt->mergeWhenClauses)
	{
		MergeWhenClause *mergeWhenClause = lfirst_node(MergeWhenClause, l);
		MergeAction *action;

		action = makeNode(MergeAction);
		action->commandType = mergeWhenClause->commandType;
		action->matched = mergeWhenClause->matched;

		/*
		 * Set namespace for the specific action. This must be done before
		 * analyzing the WHEN quals and the action targetlist.
		 */
		setNamespaceForMergeWhen(pstate, mergeWhenClause);

		/*
		 * Transform the when condition.
		 *
		 * Note that these quals are NOT added to the join quals; instead they
		 * are evaluated separately during execution to decide which of the
		 * WHEN MATCHED or WHEN NOT MATCHED actions to execute.
		 */
		action->qual = transformWhereClause(pstate, mergeWhenClause->condition,
											EXPR_KIND_MERGE_WHEN_AND, "WHEN");

		/*
		 * Transform target lists for each INSERT and UPDATE action stmt
		 */
		switch (action->commandType)
		{
			case CMD_INSERT:
				{
					List	   *exprList = NIL;
					ListCell   *lc;
					RangeTblEntry *rte;
					ListCell   *icols;
					ListCell   *attnos;
					List	   *icolumns;
					List	   *attrnos;

					pstate->p_is_insert = true;

					icolumns = checkInsertTargets(pstate,
												  mergeWhenClause->cols,
												  &attrnos);
					Assert(list_length(icolumns) == list_length(attrnos));

					action->override = mergeWhenClause->override;

					/*
					 * Handle INSERT much like in transformInsertStmt
					 */
					if (mergeWhenClause->values == NIL)
					{
						/*
						 * We have INSERT ... DEFAULT VALUES.  We can handle
						 * this case by emitting an empty targetlist --- all
						 * columns will be defaulted when the planner expands
						 * the targetlist.
						 */
						exprList = NIL;
					}
					else
					{
						/*
						 * Process INSERT ... VALUES with a single VALUES
						 * sublist.  We treat this case separately for
						 * efficiency.  The sublist is just computed directly
						 * as the Query's targetlist, with no VALUES RTE.  So
						 * it works just like a SELECT without any FROM.
						 */

						/*
						 * Do basic expression transformation (same as a ROW()
						 * expr, but allow SetToDefault at top level)
						 */
						exprList = transformExpressionList(pstate,
														   mergeWhenClause->values,
														   EXPR_KIND_VALUES_SINGLE,
														   true);

						/* Prepare row for assignment to target table */
						exprList = transformInsertRow(pstate, exprList,
													  mergeWhenClause->cols,
													  icolumns, attrnos,
													  false);
					}

					/*
					 * Generate action's target list using the computed list
					 * of expressions. Also, mark all the target columns as
					 * needing insert permissions.
					 */
					rte = pstate->p_target_nsitem->p_rte;
					icols = list_head(icolumns);
					attnos = list_head(attrnos);
					foreach(lc, exprList)
					{
						Expr	   *expr = (Expr *) lfirst(lc);
						ResTarget  *col;
						AttrNumber	attr_num;
						TargetEntry *tle;

						col = lfirst_node(ResTarget, icols);
						attr_num = (AttrNumber) lfirst_int(attnos);

						tle = makeTargetEntry(expr,
											  attr_num,
											  col->name,
											  false);
						action->targetList = lappend(action->targetList, tle);

						rte->insertedCols =
							bms_add_member(rte->insertedCols,
										   attr_num - FirstLowInvalidHeapAttributeNumber);

						icols = lnext(icolumns, icols);
						attnos = lnext(attrnos, attnos);
					}
				}
				break;
			case CMD_UPDATE:
				{
					pstate->p_is_insert = false;
					action->targetList =
						transformUpdateTargetList(pstate,
												  mergeWhenClause->targetList);
				}
				break;
			case CMD_DELETE:
				break;

			case CMD_NOTHING:
				action->targetList = NIL;
				break;
			default:
				elog(ERROR, "unknown action in MERGE WHEN clause");
		}

		mergeActionList = lappend(mergeActionList, action);
	}

	qry->mergeActionList = mergeActionList;

	/* XXX maybe later */
	qry->returningList = NULL;

	qry->hasTargetSRFs = false;
	qry->hasSubLinks = pstate->p_hasSubLinks;

	assign_query_collations(pstate, qry);

	return qry;
}

static void
setNamespaceVisibilityForRTE(List *namespace, RangeTblEntry *rte,
							 bool rel_visible,
							 bool cols_visible)
{
	ListCell   *lc;

	foreach(lc, namespace)
	{
		ParseNamespaceItem *nsitem = (ParseNamespaceItem *) lfirst(lc);

		if (nsitem->p_rte == rte)
		{
			nsitem->p_rel_visible = rel_visible;
			nsitem->p_cols_visible = cols_visible;
			break;
		}
	}
}
