/*
 * contrib/pgrowlocks/pgrowlocks.c
 *
 * Copyright (c) 2005-2006	Tatsuo Ishii
 *
 * Permission to use, copy, modify, and distribute this software and
 * its documentation for any purpose, without fee, and without a
 * written agreement is hereby granted, provided that the above
 * copyright notice and this paragraph and the following two
 * paragraphs appear in all copies.
 *
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE TO ANY PARTY FOR DIRECT,
 * INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES, INCLUDING
 * LOST PROFITS, ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS
 * DOCUMENTATION, EVEN IF THE UNIVERSITY OF CALIFORNIA HAS BEEN ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * THE AUTHOR SPECIFICALLY DISCLAIMS ANY WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS ON AN "AS
 * IS" BASIS, AND THE AUTHOR HAS NO OBLIGATIONS TO PROVIDE MAINTENANCE,
 * SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS.
 */

#include "postgres.h"

#include "access/multixact.h"
#include "access/relscan.h"
#include "access/xact.h"
#include "catalog/namespace.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/procarray.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/tqual.h"


PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(pgrowlocks);

extern Datum pgrowlocks(PG_FUNCTION_ARGS);

/* ----------
 * pgrowlocks:
 * returns tids of rows being locked
 * ----------
 */

#define NCHARS 32

typedef struct
{
	Relation	rel;
	HeapScanDesc scan;
	int			ncolumns;
	MemoryContext memcxt;
} MyData;

#define		Atnum_tid		0
#define		Atnum_type		1
#define		Atnum_xmax		2
#define		Atnum_ismulti	3
#define		Atnum_xids		4
#define		Atnum_modes		5
#define		Atnum_pids		6

Datum
pgrowlocks(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	HeapScanDesc scan;
	HeapTuple	tuple;
	TupleDesc	tupdesc;
	AttInMetadata *attinmeta;
	Datum		result;
	MyData	   *mydata;
	Relation	rel;

	if (SRF_IS_FIRSTCALL())
	{
		text	   *relname;
		RangeVar   *relrv;
		MemoryContext oldcontext;
		AclResult	aclresult;

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/* Build a tuple descriptor for our result type */
		if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
			elog(ERROR, "return type must be a row type");

		attinmeta = TupleDescGetAttInMetadata(tupdesc);
		funcctx->attinmeta = attinmeta;

		relname = PG_GETARG_TEXT_P(0);
		relrv = makeRangeVarFromNameList(textToQualifiedNameList(relname));
		rel = heap_openrv(relrv, AccessShareLock);

		/* check permissions: must have SELECT on table */
		aclresult = pg_class_aclcheck(RelationGetRelid(rel), GetUserId(),
									  ACL_SELECT);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, ACL_KIND_CLASS,
						   RelationGetRelationName(rel));


		scan = heap_beginscan(rel, SnapshotNow, 0, NULL);
		mydata = palloc(sizeof(*mydata));
		mydata->rel = rel;
		mydata->scan = scan;
		mydata->ncolumns = tupdesc->natts;
		mydata->memcxt = AllocSetContextCreate(CurrentMemoryContext,
											   "pgrowlocks cxt",
											   ALLOCSET_DEFAULT_MINSIZE,
											   ALLOCSET_DEFAULT_INITSIZE,
											   ALLOCSET_DEFAULT_MAXSIZE);
		funcctx->user_fctx = mydata;

		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	attinmeta = funcctx->attinmeta;
	mydata = (MyData *) funcctx->user_fctx;
	scan = mydata->scan;

	/* scan the relation */
	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		/* must hold a buffer lock to call HeapTupleSatisfiesUpdate */
		LockBuffer(scan->rs_cbuf, BUFFER_LOCK_SHARE);

		if (HeapTupleSatisfiesUpdate(tuple->t_data,
									 GetCurrentCommandId(false),
									 scan->rs_cbuf) == HeapTupleBeingUpdated)
		{
			char	  **values;
			MemoryContext	oldcxt;

			oldcxt = MemoryContextSwitchTo(mydata->memcxt);
			values = (char **) palloc(mydata->ncolumns * sizeof(char *));

			values[Atnum_tid] = (char *) DirectFunctionCall1(tidout, PointerGetDatum(&tuple->t_self));

			values[Atnum_type] = palloc(36);
			values[Atnum_type][0] = '\0';
			if (tuple->t_data->t_infomask & HEAP_XMAX_KEYSHR_LOCK)
				strcat(values[Atnum_type], "KeyShare ");
			if (tuple->t_data->t_infomask & HEAP_XMAX_EXCL_LOCK)
				strcat(values[Atnum_type], "Exclusive ");
			if (tuple->t_data->t_infomask & HEAP_XMAX_IS_NOT_UPDATE)
				strcat(values[Atnum_type], "IsNotUpdate ");

			values[Atnum_xmax] = palloc(NCHARS * sizeof(char));
			snprintf(values[Atnum_xmax], NCHARS, "%d", HeapTupleHeaderGetXmax(tuple->t_data));
			if (tuple->t_data->t_infomask & HEAP_XMAX_IS_MULTI)
			{
				MultiXactMember *members;
				int			nmembers;
				int			j;
				bool		isValidXid = false;		/* any valid xid ever exists? */

				values[Atnum_ismulti] = pstrdup("true");
				nmembers = GetMultiXactIdMembers(HeapTupleHeaderGetXmax(tuple->t_data), &members);
				if (nmembers == -1)
				{
					elog(ERROR, "GetMultiXactIdMembers returns error");
				}

				values[Atnum_xids] = palloc(NCHARS * nmembers);
				values[Atnum_modes] = palloc(NCHARS * nmembers);
				values[Atnum_pids] = palloc(NCHARS * nmembers);
				strcpy(values[Atnum_xids], "{");
				strcpy(values[Atnum_modes], "{");
				strcpy(values[Atnum_pids], "{");

				for (j = 0; j < nmembers; j++)
				{
					char		buf[NCHARS];

					if (isValidXid)
					{
						strcat(values[Atnum_xids], ",");
						strcat(values[Atnum_modes], ",");
						strcat(values[Atnum_pids], ",");
					}
					snprintf(buf, NCHARS, "%d", members[j].xid);
					strcat(values[Atnum_xids], buf);
					switch (members[j].status)
					{
						case MultiXactStatusKeyUpdate:
							snprintf(buf, NCHARS, "keyupd");
							break;
						case MultiXactStatusUpdate:
							snprintf(buf, NCHARS, "upd");
							break;
						case MultiXactStatusShare:
							snprintf(buf, NCHARS, "shr");
							break;
						case MultiXactStatusKeyShare:
							snprintf(buf, NCHARS, "keyshr");
							break;
					}
					strcat(values[Atnum_modes], buf);
					snprintf(buf, NCHARS, "%d", BackendXidGetPid(members[j].xid));
					strcat(values[Atnum_pids], buf);

					isValidXid = true;
				}

				strcat(values[Atnum_xids], "}");
				strcat(values[Atnum_modes], "}");
				strcat(values[Atnum_pids], "}");
			}
			else
			{
				values[Atnum_ismulti] = pstrdup("false");

				values[Atnum_xids] = palloc(NCHARS * sizeof(char));
				snprintf(values[Atnum_xids], NCHARS, "{%d}", HeapTupleHeaderGetXmax(tuple->t_data));

				values[Atnum_pids] = palloc(NCHARS * sizeof(char));
				snprintf(values[Atnum_pids], NCHARS, "{%d}", BackendXidGetPid(HeapTupleHeaderGetXmax(tuple->t_data)));
			}

			LockBuffer(scan->rs_cbuf, BUFFER_LOCK_UNLOCK);

			/* build a tuple */
			MemoryContextSwitchTo(oldcxt);
			tuple = BuildTupleFromCStrings(attinmeta, values);

			/* make the tuple into a datum */
			result = HeapTupleGetDatum(tuple);

			/* Clean up */
			MemoryContextReset(mydata->memcxt);

			SRF_RETURN_NEXT(funcctx, result);
		}
		else
		{
			LockBuffer(scan->rs_cbuf, BUFFER_LOCK_UNLOCK);
		}
	}

	heap_endscan(scan);
	heap_close(mydata->rel, AccessShareLock);
	/* no need to delete the memory context */

	SRF_RETURN_DONE(funcctx);
}
