/*-------------------------------------------------------------------------
 *
 * multixact.c
 *		PostgreSQL multi-transaction-log manager
 *
 * The pg_multixact manager is a pg_clog-like manager that stores an array
 * of MultiXactMember for each MultiXactId.	It is a fundamental part of the
 * shared-row-lock implementation.	A share-locked tuple stores a
 * MultiXactId in its Xmax, and a transaction that needs to wait for the
 * tuple to be unlocked can sleep on the potentially-several TransactionIds
 * that compose the MultiXactId.
 *
 * We use two SLRU areas, one for storing the offsets at which the data
 * starts for each MultiXactId in the other one.  This trick allows us to
 * store variable length arrays of TransactionIds.	(We could alternatively
 * use one area containing counts and TransactionIds, with valid MultiXactId
 * values pointing at slots containing counts; but that way seems less robust
 * since it would get completely confused if someone inquired about a bogus
 * MultiXactId that pointed to an intermediate slot containing an XID.)
 *
 * XLOG interactions: this module generates an XLOG record whenever a new
 * OFFSETs or MEMBERs page is initialized to zeroes, as well as an XLOG record
 * whenever a new MultiXactId is defined.  This allows us to completely
 * rebuild the data entered since the last checkpoint during XLOG replay.
 * Because this is possible, we need not follow the normal rule of
 * "write WAL before data"; the only correctness guarantee needed is that
 * we flush and sync all dirty OFFSETs and MEMBERs pages to disk before a
 * checkpoint is considered complete.  If a page does make it to disk ahead
 * of corresponding WAL records, it will be forcibly zeroed before use anyway.
 * Therefore, we don't need to mark our pages with LSN information; we have
 * enough synchronization already.
 *
 * Like clog.c, and unlike subtrans.c, we have to preserve state across
 * crashes and ensure that MXID and offset numbering increases monotonically
 * across a crash.	We do this in the same way as it's done for transaction
 * IDs: the WAL record is guaranteed to contain evidence of every MXID we
 * could need to worry about, and we just make sure that at the end of
 * replay, the next-MXID and next-offset counters are at least as large as
 * anything we saw during replay.
 *
 *
 * Portions Copyright (c) 1996-2011, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/access/transam/multixact.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>

#include "access/multixact.h"
#include "access/slru.h"
#include "access/transam.h"
#include "access/twophase.h"
#include "access/twophase_rmgr.h"
#include "access/xact.h"
#include "miscadmin.h"
#include "pg_trace.h"
#include "storage/lmgr.h"
#include "storage/procarray.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"


/*
 * Defines for MultiXactOffset page sizes.	A page is the same BLCKSZ as is
 * used everywhere else in Postgres.
 *
 * Note: because both MultiXactOffsets and TransactionIds are 32 bits and
 * wrap around at 0xFFFFFFFF, MultiXact page numbering also wraps around at
 * 0xFFFFFFFF/MULTIXACT_*_PER_PAGE, and segment numbering at
 * 0xFFFFFFFF/MULTIXACT_*_PER_PAGE/SLRU_SEGMENTS_PER_PAGE.	We need take no
 * explicit notice of that fact in this module, except when comparing segment
 * and page numbers in TruncateMultiXact
 * (see MultiXact{Offset,Member}PagePrecedes).
 */

/* We need four bytes per offset */
#define MULTIXACT_OFFSETS_PER_PAGE (BLCKSZ / sizeof(MultiXactOffset))

#define MultiXactIdToOffsetPage(xid) \
	((xid) / (MultiXactOffset) MULTIXACT_OFFSETS_PER_PAGE)
#define MultiXactIdToOffsetEntry(xid) \
	((xid) % (MultiXactOffset) MULTIXACT_OFFSETS_PER_PAGE)

/*
 * The situation for members is a bit more complex: we need to store two
 * additional flag bits for each TransactionId.  To do this without getting
 * into alignment issues, we store four bytes of flags (so 16 bit pairs), and
 * then the corresponding 16 Xids.  Each such 17-word (68-byte) set we call a
 * "group", and are stored as a whole in pages.  Thus, with 8kB BLCKSZ, we keep
 * 120 groups per page.  This wastes 32 bytes per page, but that's OK --
 * simplicity (and performance) trumps space efficiency here.
 *
 * Note that the "offset" macros work with byte offset, not array indexes, so
 * arithmetic must be done using "char *" pointers.
 */
/* We need two bits per xact, so four xacts fit in a byte */
#define MXACT_MEMBER_BITS_PER_XACT			2
#define MXACT_MEMBER_FLAGS_PER_BYTE			4
#define MXACT_MEMBER_XACT_BITMASK	((1 << MXACT_MEMBER_BITS_PER_XACT) - 1)

/* how many full bytes of flags are there in a group? */
#define MULTIXACT_FLAGBYTES_PER_GROUP		4
#define MULTIXACT_MEMBERS_PER_MEMBERGROUP	\
	(MULTIXACT_FLAGBYTES_PER_GROUP * MXACT_MEMBER_FLAGS_PER_BYTE)
/* size in bytes of a complete group */
#define MULTIXACT_MEMBERGROUP_SIZE \
	(sizeof(TransactionId) * MULTIXACT_MEMBERS_PER_MEMBERGROUP + MULTIXACT_FLAGBYTES_PER_GROUP)
#define MULTIXACT_MEMBERGROUPS_PER_PAGE (BLCKSZ / MULTIXACT_MEMBERGROUP_SIZE)
#define MULTIXACT_MEMBERS_PER_PAGE	\
	(MULTIXACT_MEMBERGROUPS_PER_PAGE * MULTIXACT_MEMBERS_PER_MEMBERGROUP)

/* page in which a member is to be found */
#define MXOffsetToMemberPage(xid) ((xid) / (TransactionId) MULTIXACT_MEMBERS_PER_PAGE)

/* Location (byte offset within page) of flag word for a given member */
#define MXOffsetToFlagsOffset(xid) \
	((((xid) / (TransactionId) MULTIXACT_MEMBERS_PER_MEMBERGROUP) % \
	  (TransactionId) MULTIXACT_MEMBERGROUPS_PER_PAGE) * \
	 (TransactionId) MULTIXACT_MEMBERGROUP_SIZE)
#define MXOffsetToFlagsBitShift(xid) \
	(((xid) % (TransactionId) MULTIXACT_MEMBERS_PER_MEMBERGROUP) * \
	 MXACT_MEMBER_BITS_PER_XACT)

/* Location (byte offset within page) of TransactionId of given member */
#define MXOffsetToMemberOffset(xid) \
	(MXOffsetToFlagsOffset(xid) + MULTIXACT_FLAGBYTES_PER_GROUP + \
	 ((xid) % MULTIXACT_MEMBERS_PER_MEMBERGROUP) * sizeof(TransactionId))


/*
 * Links to shared-memory data structures for MultiXact control
 */
static SlruCtlData MultiXactOffsetCtlData;
static SlruCtlData MultiXactMemberCtlData;

#define MultiXactOffsetCtl	(&MultiXactOffsetCtlData)
#define MultiXactMemberCtl	(&MultiXactMemberCtlData)

/*
 * MultiXact state shared across all backends.	All this state is protected
 * by MultiXactGenLock.  (We also use MultiXactOffsetControlLock and
 * MultiXactMemberControlLock to guard accesses to the two sets of SLRU
 * buffers.  For concurrency's sake, we avoid holding more than one of these
 * locks at a time.)
 */
typedef struct MultiXactStateData
{
	/* next-to-be-assigned MultiXactId */
	MultiXactId nextMXact;

	/* next-to-be-assigned offset */
	MultiXactOffset nextOffset;

	/* truncation info for the oldest segment in the offset SLRU area */
	TransactionId	truncateXid;
	uint32			truncateXidEpoch;

	/*
	 * oldest multixact that is still on disk.  Anything older than this should
	 * not be consulted.
	 */
	MultiXactId		oldestMultiXactId;
} MultiXactStateData;

/* Pointer to the state data in shared memory */
static MultiXactStateData *MultiXactState;

#define firstPageOf(segment) ((segment) * SLRU_PAGES_PER_SEGMENT)

/*
 * structs to pass data around in our private SlruScanDirectory callback for
 * the offset truncation support code.
 */
typedef struct SegmentInfo
{
	int				segno;			/* segment number */
	TransactionId	truncateXid;	/* after this Xid is frozen, the previous
									 * segment can be removed */
	uint32			truncateXidEpoch;	/* epoch of above Xid */
	MultiXactOffset	firstOffset;	/* first valid offset in segment */
} SegmentInfo;

typedef struct TruncateCbData
{
	int				remaining_alloc;
	int				remaining_used;
	SegmentInfo	   *remaining;
} TruncateCbData;

/*
 * MultiXactZeroOffsetPage xlog record
 */
typedef struct MxactZeroOffPg
{
	int				pageno;
	TransactionId	truncateXid;
	TransactionId	truncateXidEpoch;
} MxactZeroOffPg;

/*
 * Definitions for the backend-local MultiXactId cache.
 *
 * We use this cache to store known MultiXacts, so we don't need to go to
 * SLRU areas everytime.
 *
 * The cache lasts for the duration of a single transaction, the rationale
 * for this being that most entries will contain our own TransactionId and
 * so they will be uninteresting by the time our next transaction starts.
 * (XXX not clear that this is correct --- other members of the MultiXact
 * could hang around longer than we did.  However, it's not clear what a
 * better policy for flushing old cache entries would be.)  FIXME actually
 * this is plain wrong now that multixact's may contain update Xids.
 *
 * We allocate the cache entries in a memory context that is deleted at
 * transaction end, so we don't need to do retail freeing of entries.
 */
typedef struct mXactCacheEnt
{
	struct mXactCacheEnt *next;
	MultiXactId multi;
	int			nmembers;
	MultiXactMember members[FLEXIBLE_ARRAY_MEMBER];
} mXactCacheEnt;

static mXactCacheEnt *MXactCache = NULL;
static MemoryContext MXactContext = NULL;

/* status conflict table */
static const bool MultiXactConflicts[5][5] =
{
	{	/* ForKeyShare */
		false, false, false, false, true
	},
	{	/* ForShare */
		false, false, true, true, true
	},
	{	/* ForUpdate */
		false, true, true, true, true
	},
	{	/* Update */
		false, true, true, true, true
	},
	{	/* KeyUpdate */
		true, true, true, true, true
	}
};

#define MultiXactStatusConflict(status1, status2) \
	MultiXactConflicts[status1][status2]


#define MULTIXACT_DEBUG
#ifdef MULTIXACT_DEBUG
#define debug_elog2(a,b) elog(a,b)
#define debug_elog3(a,b,c) elog(a,b,c)
#define debug_elog4(a,b,c,d) elog(a,b,c,d)
#define debug_elog5(a,b,c,d,e) elog(a,b,c,d,e)
#define debug_elog7(a,b,c,d,e,f,g) elog(a,b,c,d,e,f,g)
#else
#define debug_elog2(a,b)
#define debug_elog3(a,b,c)
#define debug_elog4(a,b,c,d)
#define debug_elog5(a,b,c,d,e)
#define debug_elog7(a,b,c,d,e,f,g)
#endif

/* internal MultiXactId management */
static MultiXactId CreateMultiXactId(int nmembers, MultiXactMember *members);
static void RecordNewMultiXact(MultiXactId multi, MultiXactOffset offset,
				   int nmembers, MultiXactMember *members);
static MultiXactId GetNewMultiXactId(int nmembers, MultiXactOffset *offset);
static MultiXactId HandleMxactOffsetCornerCases(MultiXactId multi);

/* MultiXact cache management */
static int mxactMemberComparator(const void *arg1, const void *arg2);
static MultiXactId mXactCacheGetBySet(int nmembers, MultiXactMember *members);
static int	mXactCacheGetById(MultiXactId multi, MultiXactMember **members);
static void mXactCachePut(MultiXactId multi, int nmembers, MultiXactMember *members);

#ifdef MULTIXACT_DEBUG
static char *mxid_to_string(MultiXactId multi, int nmembers, MultiXactMember *members);
#endif

/* management of SLRU infrastructure */
static int	ZeroMultiXactOffsetPage(int pageno, bool writeXlog,
						TransactionId truncateXid, uint32 truncateXidEpoch);
static int	ZeroMultiXactMemberPage(int pageno, bool writeXlog);
static bool MultiXactOffsetPagePrecedes(int page1, int page2);
static bool MultiXactMemberPagePrecedes(int page1, int page2);
static bool MultiXactIdPrecedes(MultiXactId multi1, MultiXactId multi2);
static bool MultiXactOffsetPrecedes(MultiXactOffset offset1,
						MultiXactOffset offset2);
static void ExtendMultiXactOffset(MultiXactId multi);
static void ExtendMultiXactMember(MultiXactOffset offset, int nmembers);
static void fillSegmentInfoData(SlruCtl ctl, SegmentInfo *segment);
static int	compareTruncateXidEpoch(const void *a, const void *b);
static void WriteMZeroOffsetPageXlogRec(int pageno, TransactionId truncateXid,
							uint32 truncateXidEpoch);
static void WriteMZeroMemberPageXlogRec(int pageno);


/*
 * MultiXactIdCreateSingleton
 * 		Construct a MultiXactId representing a single transaction.
 *
 * NB - we don't worry about our local MultiXactId cache here, because that
 * is handled by the lower-level routines.
 */
MultiXactId
MultiXactIdCreateSingleton(TransactionId xid, MultiXactStatus status)
{
	MultiXactId	newMulti;
	MultiXactMember	member[1];

	AssertArg(TransactionIdIsValid(xid));

	member[0].xid = xid;
	member[0].status = status;

	newMulti = CreateMultiXactId(1, member);

	debug_elog4(DEBUG2, "Create: returning %u for %u",
			   newMulti, xid);

	return newMulti;
}

/*
 * MultiXactIdCreate
 *		Construct a MultiXactId representing two TransactionIds.
 *
 * The two XIDs must be different, or be requesting different lock modes.
 *
 * NB - we don't worry about our local MultiXactId cache here, because that
 * is handled by the lower-level routines.
 */
MultiXactId
MultiXactIdCreate(TransactionId xid1, MultiXactStatus status1,
				  TransactionId xid2, MultiXactStatus status2)
{
	MultiXactId newMulti;
	MultiXactMember members[2];

	AssertArg(TransactionIdIsValid(xid1));
	AssertArg(TransactionIdIsValid(xid2));

	Assert(!TransactionIdEquals(xid1, xid2) || (status1 != status2));

	/*
	 * Note: unlike MultiXactIdExpand, we don't bother to check that both XIDs
	 * are still running.  In typical usage, xid2 will be our own XID and the
	 * caller just did a check on xid1, so it'd be wasted effort.
	 */

	members[0].xid = xid1;
	members[0].status = status1;
	members[1].xid = xid2;
	members[1].status = status2;

	newMulti = CreateMultiXactId(2, members);

	/* XXX -- need better debug? */
	debug_elog5(DEBUG2, "Create: returning %u for %u, %u",
				newMulti, xid1, xid2);

	return newMulti;
}

/*
 * MultiXactIdExpand
 *		Add a TransactionId to a pre-existing MultiXactId.
 *
 * If the TransactionId is already a member of the passed MultiXactId with the
 * same status, just return it as-is.
 *
 * Note that we do NOT actually modify the membership of a pre-existing
 * MultiXactId; instead we create a new one.  This is necessary to avoid
 * a race condition against MultiXactIdWait (see notes there).
 *
 * NB - we don't worry about our local MultiXactId cache here, because that
 * is handled by the lower-level routines.
 */
MultiXactId
MultiXactIdExpand(MultiXactId multi, TransactionId xid, MultiXactStatus status)
{
	MultiXactId newMulti;
	MultiXactMember *members;
	MultiXactMember *newMembers;
	int			nmembers;
	int			i;
	int			j;

	AssertArg(MultiXactIdIsValid(multi));
	AssertArg(TransactionIdIsValid(xid));

	debug_elog4(DEBUG2, "Expand: received multi %u, xid %u",
				multi, xid);

	nmembers = GetMultiXactIdMembers(multi, &members);

	if (nmembers < 0)
	{
		MultiXactMember		member;

		/*
		 * The MultiXactId is obsolete.  This can only happen if all the
		 * MultiXactId members stop running between the caller checking and
		 * passing it to us.  It would be better to return that fact to the
		 * caller, but it would complicate the API and it's unlikely to happen
		 * too often, so just deal with it by creating a singleton MultiXact.
		 */
		member.xid = xid;
		member.status = status;
		newMulti = CreateMultiXactId(1, &member);

		debug_elog4(DEBUG2, "Expand: %u has no members, create singleton %u",
					multi, newMulti);
		return newMulti;
	}

	/*
	 * If the TransactionId is already a member of the MultiXactId with the
	 * same status, just return the existing MultiXactId.
	 */
	for (i = 0; i < nmembers; i++)
	{
		if (TransactionIdEquals(members[i].xid, xid) &&
			(members[i].status == status))
		{
			debug_elog4(DEBUG2, "Expand: %u is already a member of %u",
						xid, multi);
			pfree(members);
			return multi;
		}
	}

	/*
	 * Determine which of the members of the MultiXactId are still running,
	 * and use them to create a new one.  (Removing dead members is just an
	 * optimization, but a useful one.	Note we have the same race condition
	 * here as above: j could be 0 at the end of the loop.)
	 */
	newMembers = (MultiXactMember *)
		palloc(sizeof(MultiXactMember) * (nmembers + 1));

	for (i = 0, j = 0; i < nmembers; i++)
	{
		if (TransactionIdIsInProgress(members[i].xid))
		{
			newMembers[j].xid = members[i].xid;
			newMembers[j++].status = members[i].status;
		}
	}

	newMembers[j].xid = xid;
	newMembers[j++].status = status;
	newMulti = CreateMultiXactId(j, newMembers);

	pfree(members);
	pfree(newMembers);

	debug_elog3(DEBUG2, "Expand: returning new multi %u", newMulti);

	return newMulti;
}

/*
 * MultiXactIdIsRunning
 *		Returns whether a MultiXactId is "running".
 *
 * We return true if at least one member of the given MultiXactId is still
 * running.  Note that a "false" result is certain not to change,
 * because it is not legal to add members to an existing MultiXactId.
 */
bool
MultiXactIdIsRunning(MultiXactId multi)
{
	MultiXactMember *members;
	int			nmembers;
	int			i;

	debug_elog3(DEBUG2, "IsRunning %u?", multi);

	nmembers = GetMultiXactIdMembers(multi, &members);

	if (nmembers < 0)
	{
		debug_elog2(DEBUG2, "IsRunning: no members");
		return false;
	}

	/*
	 * Checking for myself is cheap compared to looking in shared memory;
	 * return true if any live subtransaction of the current top-level
	 * transaction is a member.
	 *
	 * This is not needed for correctness, it's just a fast path.
	 */
	for (i = 0; i < nmembers; i++)
	{
		if (TransactionIdIsCurrentTransactionId(members[i].xid))
		{
			debug_elog3(DEBUG2, "IsRunning: I (%d) am running!", i);
			pfree(members);
			return true;
		}
	}

	/*
	 * This could be made faster by having another entry point in procarray.c,
	 * walking the PGPROC array only once for all the members.	But in most
	 * cases nmembers should be small enough that it doesn't much matter.
	 */
	for (i = 0; i < nmembers; i++)
	{
		if (TransactionIdIsInProgress(members[i].xid))
		{
			debug_elog4(DEBUG2, "IsRunning: member %d (%u) is running",
						i, members[i].xid);
			pfree(members);
			return true;
		}
	}

	pfree(members);

	debug_elog3(DEBUG2, "IsRunning: %u is not running", multi);

	return false;
}

/*
 * MultiXactIdWait
 *		Sleep on a MultiXactId.
 *
 * We do this by sleeping on each member using XactLockTableWait.  Any
 * members that belong to the current backend are *not* waited for, however;
 * this would not merely be useless but would lead to Assert failure inside
 * XactLockTableWait.  By the time this returns, it is certain that all
 * transactions *of other backends* that were members of the MultiXactId
 * that conflict with the requested status are dead (and no new ones can have
 * been added, since it is not legal to add members to an existing
 * MultiXactId).
 *
 * We return the number of members that we did not test for.  This is dubbed
 * "remaining" as in "the number of members that remaing running", but this is
 * slightly incorrect, because lockers whose status did not conflict with ours
 * are not even considered and so might have gone away anyway.
 *
 * But by the time we finish sleeping, someone else may have changed the Xmax
 * of the containing tuple, so the caller needs to iterate on us somehow.
 */
void
MultiXactIdWait(MultiXactId multi, MultiXactStatus status, int *remaining)
{
	MultiXactMember *members;
	int			nmembers;
	int			remain = 0;

	nmembers = GetMultiXactIdMembers(multi, &members);

	if (nmembers >= 0)
	{
		int			i;

		for (i = 0; i < nmembers; i++)
		{
			debug_elog4(DEBUG2, "MultiXactIdWait: waiting for %d (%u)",
						i, members[i].xid);
			if (TransactionIdIsCurrentTransactionId(members[i].xid) ||
				!MultiXactStatusConflict(members[i].status, status))
			{
				remain++;
				continue;
			}

			XactLockTableWait(members[i].xid);
		}
	}

	*remaining = remain;
}

/*
 * ConditionalMultiXactIdWait
 *		As above, but only lock if we can get the lock without blocking.
 *
 * Note that in case we return false, the number of remaining members is
 * not to be trusted.
 */
bool
ConditionalMultiXactIdWait(MultiXactId multi, MultiXactStatus status,
						   int *remaining)
{
	bool		result = true;
	MultiXactMember *members;
	int			nmembers;
	int			remain = 0;

	nmembers = GetMultiXactIdMembers(multi, &members);

	if (nmembers >= 0)
	{
		int			i;

		for (i = 0; i < nmembers; i++)
		{
			TransactionId member = members[i].xid;

			debug_elog4(DEBUG2, "ConditionalMultiXactIdWait: trying %d (%u)",
						i, member);
			if (TransactionIdIsCurrentTransactionId(member) ||
				!MultiXactStatusConflict(members[i].status, status))
			{
				remain++;
				continue;
			}
			result = ConditionalXactLockTableWait(member);
			if (!result)
				break;
		}

		pfree(members);
	}

	*remaining = remain;

	return result;
}

/*
 * CreateMultiXactId
 *		Make a new MultiXactId
 *
 * Make XLOG, SLRU and cache entries for a new MultiXactId, recording the
 * given TransactionIds as members.  Returns the newly created MultiXactId.
 *
 * NB: the passed members[] array will be sorted in-place.
 */
static MultiXactId
CreateMultiXactId(int nmembers, MultiXactMember *members)
{
	MultiXactId multi;
	MultiXactOffset offset;
	XLogRecData rdata[2];
	xl_multixact_create xlrec;

	debug_elog3(DEBUG2, "Create: %s",
				mxid_to_string(InvalidMultiXactId, nmembers, members));

	/*
	 * See if the same set of members already exists in our cache; if so, just
	 * re-use that MultiXactId.  (Note: it might seem that looking in our
	 * cache is insufficient, and we ought to search disk to see if a
	 * duplicate definition already exists.  But since we only ever create
	 * MultiXacts containing our own XID, in most cases any such MultiXacts
	 * were in fact created by us, and so will be in our cache.  There are
	 * corner cases where someone else added us to a MultiXact without our
	 * knowledge, but it's not worth checking for.)
	 */
	multi = mXactCacheGetBySet(nmembers, members);
	if (MultiXactIdIsValid(multi))
	{
		debug_elog2(DEBUG2, "Create: in cache!");
		return multi;
	}

	/*
	 * Assign the MXID and offsets range to use, and make sure there is space
	 * in the OFFSETs and MEMBERs files.  NB: this routine does
	 * START_CRIT_SECTION().
	 */
	multi = GetNewMultiXactId(nmembers, &offset);

	/*
	 * Make an XLOG entry describing the new MXID.
	 *
	 * Note: we need not flush this XLOG entry to disk before proceeding. The
	 * only way for the MXID to be referenced from any data page is for
	 * heap_lock_tuple() to have put it there, and heap_lock_tuple() generates
	 * an XLOG record that must follow ours.  The normal LSN interlock between
	 * the data page and that XLOG record will ensure that our XLOG record
	 * reaches disk first.	If the SLRU members/offsets data reaches disk
	 * sooner than the XLOG record, we do not care because we'll overwrite it
	 * with zeroes unless the XLOG record is there too; see notes at top of
	 * this file.
	 */
	xlrec.mid = multi;
	xlrec.moff = offset;
	xlrec.nmembers = nmembers;

	/*
	 * XXX Note: there's a lot of padding space in MultiXactMember.  We could
	 * find a more compact representation of this Xlog record -- perhaps all the
	 * status flags in one XLogRecData, then all the xids in another one?
	 */
	rdata[0].data = (char *) (&xlrec);
	rdata[0].len = MinSizeOfMultiXactCreate;
	rdata[0].buffer = InvalidBuffer;
	rdata[0].next = &(rdata[1]);
	rdata[1].data = (char *) members;
	rdata[1].len = nmembers * sizeof(MultiXactMember);
	rdata[1].buffer = InvalidBuffer;
	rdata[1].next = NULL;

	(void) XLogInsert(RM_MULTIXACT_ID, XLOG_MULTIXACT_CREATE_ID, rdata);

	/* Now enter the information into the OFFSETs and MEMBERs logs */
	RecordNewMultiXact(multi, offset, nmembers, members);

	/* Done with critical section */
	END_CRIT_SECTION();

	/* Store the new MultiXactId in the local cache, too */
	mXactCachePut(multi, nmembers, members);

	debug_elog2(DEBUG2, "Create: all done");

	return multi;
}

/*
 * RecordNewMultiXact
 *		Write info about a new multixact into the offsets and members files
 *
 * This is broken out of CreateMultiXactId so that xlog replay can use it.
 */
static void
RecordNewMultiXact(MultiXactId multi, MultiXactOffset offset,
				   int nmembers, MultiXactMember *members)
{
	int			pageno;
	int			prev_pageno;
	int			entryno;
	int			slotno;
	MultiXactOffset *offptr;
	int			i;

	LWLockAcquire(MultiXactOffsetControlLock, LW_EXCLUSIVE);

	pageno = MultiXactIdToOffsetPage(multi);
	entryno = MultiXactIdToOffsetEntry(multi);

	/*
	 * Note: we pass the MultiXactId to SimpleLruReadPage as the "transaction"
	 * to complain about if there's any I/O error.  This is kinda bogus, but
	 * since the errors will always give the full pathname, it should be clear
	 * enough that a MultiXactId is really involved.  Perhaps someday we'll
	 * take the trouble to generalize the slru.c error reporting code.
	 */
	slotno = SimpleLruReadPage(MultiXactOffsetCtl, pageno, true, multi);
	offptr = (MultiXactOffset *) MultiXactOffsetCtl->shared->page_buffer[slotno];
	offptr += entryno;

	*offptr = offset;

	MultiXactOffsetCtl->shared->page_dirty[slotno] = true;

	/* Exchange our lock */
	LWLockRelease(MultiXactOffsetControlLock);

	LWLockAcquire(MultiXactMemberControlLock, LW_EXCLUSIVE);

	prev_pageno = -1;

	for (i = 0; i < nmembers; i++, offset++)
	{
		TransactionId *memberptr;
		uint32	   *flagsptr;
		uint32		flagsval;
		int			bshift;
		int			flagsoff;
		int			memberoff;

		/* this status value is not representable on disk */
		Assert(members[i].status < MultiXactStatusKeyUpdate);

		pageno = MXOffsetToMemberPage(offset);
		memberoff = MXOffsetToMemberOffset(offset);
		flagsoff = MXOffsetToFlagsOffset(offset);
		bshift = MXOffsetToFlagsBitShift(offset);

		if (pageno != prev_pageno)
		{
			slotno = SimpleLruReadPage(MultiXactMemberCtl, pageno, true, multi);
			prev_pageno = pageno;
		}

		memberptr = (TransactionId *)
			(MultiXactMemberCtl->shared->page_buffer[slotno] + memberoff);

		*memberptr = members[i].xid;

		flagsptr = (uint32 *)
			(MultiXactMemberCtl->shared->page_buffer[slotno] + flagsoff);

		flagsval = *flagsptr;
		flagsval &= ~(((1 << MXACT_MEMBER_BITS_PER_XACT) - 1) << bshift);
		flagsval |= (members[i].status << bshift);
		*flagsptr = flagsval;

		MultiXactMemberCtl->shared->page_dirty[slotno] = true;
	}

	LWLockRelease(MultiXactMemberControlLock);
}

/*
 * GetNewMultiXactId
 *		Get the next MultiXactId.
 *
 * Also, reserve the needed amount of space in the "members" area.	The
 * starting offset of the reserved space is returned in *offset.
 *
 * This may generate XLOG records for expansion of the offsets and/or members
 * files.  Unfortunately, we have to do that while holding MultiXactGenLock
 * to avoid race conditions --- the XLOG record for zeroing a page must appear
 * before any backend can possibly try to store data in that page!
 *
 * We start a critical section before advancing the shared counters.  The
 * caller must end the critical section after writing SLRU data.
 */
static MultiXactId
GetNewMultiXactId(int nmembers, MultiXactOffset *offset)
{
	MultiXactId result;
	MultiXactOffset nextOffset;

	debug_elog3(DEBUG2, "GetNew: for %d xids", nmembers);

	LWLockAcquire(MultiXactGenLock, LW_EXCLUSIVE);

	/* Handle corner cases of the nextMXact counter */
	MultiXactState->nextMXact =
		HandleMxactOffsetCornerCases(MultiXactState->nextMXact);

	/*
	 * Assign the MXID, and make sure there is room for it in the file.
	 */
	result = MultiXactState->nextMXact;

	ExtendMultiXactOffset(result);

	/*
	 * Reserve the members space, similarly to above.  Also, be careful not to
	 * return zero as the starting offset for any multixact. See
	 * GetMultiXactIdMembers() for motivation.
	 */
	nextOffset = MultiXactState->nextOffset;
	if (nextOffset == 0)
	{
		*offset = 1;
		nmembers++;				/* allocate member slot 0 too */
	}
	else
		*offset = nextOffset;

	ExtendMultiXactMember(nextOffset, nmembers);

	/*
	 * Critical section from here until caller has written the data into the
	 * just-reserved SLRU space; we don't want to error out with a partly
	 * written MultiXact structure.  (In particular, failing to write our
	 * start offset after advancing nextMXact would effectively corrupt the
	 * previous MultiXact.)
	 */
	START_CRIT_SECTION();

	/*
	 * Advance counters.  As in GetNewTransactionId(), this must not happen
	 * until after file extension has succeeded!
	 *
	 * We don't care about MultiXactId wraparound here; it will be handled by
	 * the next iteration.	But note that nextMXact may be InvalidMultiXactId
	 * or the first value on a segment-beginning page after this routine exits,
	 * so anyone else looking at the variable must be prepared to deal with
	 * either case.  Similarly, nextOffset may be zero, but we won't use that
	 * as the actual start offset of the next multixact.
	 */
	(MultiXactState->nextMXact)++;

	MultiXactState->nextOffset += nmembers;

	LWLockRelease(MultiXactGenLock);

	debug_elog4(DEBUG2, "GetNew: returning %u offset %u", result, *offset);
	return result;
}

/*
 * HandleMxactOffsetCornerCases
 * 		Properly handle corner cases of MultiXactId enumeration
 *
 * This function takes a MultiXactId and returns a value that's actually a
 * valid multi, that is, it skips the first two values of any segment-
 * beginning page, which are used to store the truncateXid and
 * truncateXidEpoch.
 */
static MultiXactId
HandleMxactOffsetCornerCases(MultiXactId multi)
{
	if (multi < FirstMultiXactId)
		return FirstMultiXactId;

	if (MultiXactIdToOffsetEntry(multi) == 0 &&
		multi % SLRU_PAGES_PER_SEGMENT == 0)
		return multi + 2;

	return multi;
}

/*
 * GetMultiXactIdMembers
 *		Returns the set of MultiXactMembers that make up a MultiXactId
 *
 * We return -1 if the MultiXactId is too old to possibly have any members
 * still running; in that case we have not actually looked them up, and
 * *members is not set.
 */
int
GetMultiXactIdMembers(MultiXactId multi, MultiXactMember **members)
{
	int			pageno;
	int			prev_pageno;
	int			entryno;
	int			slotno;
	MultiXactOffset *offptr;
	MultiXactOffset offset;
	int			length;
	int			truelength;
	int			i;
	MultiXactId oldestMXact;
	MultiXactId nextMXact;
	MultiXactId tmpMXact;
	MultiXactOffset nextOffset;
	MultiXactMember *ptr;

	debug_elog3(DEBUG2, "GetMembers: asked for %u", multi);

	Assert(MultiXactIdIsValid(multi));

	/* See if the MultiXactId is in the local cache */
	length = mXactCacheGetById(multi, members);
	if (length >= 0)
	{
		debug_elog3(DEBUG2, "GetMembers: found %s in the cache",
					mxid_to_string(multi, length, *members));
		return length;
	}

	/*
	 * We check known limits on MultiXact before resorting to the SLRU area.
	 *
	 * An ID older than MultiXactState->oldestMultiXactId cannot possibly be
	 * useful; it should have already been frozen by vacuum.  We've truncated
	 * the on-disk structures anyway.  Since returning the wrong state could
	 * lead to an incorrect visibility result, this now raises an error.
	 * Versions prior to 9.2 silently returned an empty array, but this is no
	 * longer safe.
	 *
	 * Conversely, an ID >= nextMXact shouldn't ever be seen here; if it is
	 * seen, it implies undetected ID wraparound has occurred.	This raises
	 * an error, as in the case above.
	 *
	 * Shared lock is enough here since we aren't modifying any global state.
	 *
	 * Acquire the shared lock just long enough to grab the current counter
	 * values.	We may need both nextMXact and nextOffset; see below.
	 */
	LWLockAcquire(MultiXactGenLock, LW_SHARED);

	oldestMXact = MultiXactState->oldestMultiXactId;
	nextMXact = MultiXactState->nextMXact;
	nextOffset = MultiXactState->nextOffset;

	LWLockRelease(MultiXactGenLock);

	if (MultiXactIdPrecedes(multi, oldestMXact))
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("MultiXactId %u does no longer exist -- apparent wraparound",
						multi)));

	if (!MultiXactIdPrecedes(multi, nextMXact))
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("MultiXactId %u has not been created yet -- apparent wraparound",
						multi)));

	/*
	 * Find out the offset at which we need to start reading MultiXactMembers
	 * and the number of members in the multixact.	We determine the latter as
	 * the difference between this multixact's starting offset and the next
	 * one's.  However, there are some corner cases to worry about:
	 *
	 * 1. This multixact may be the latest one created, in which case there is
	 * no next one to look at.	In this case the nextOffset value we just
	 * saved is the correct endpoint.
	 *
	 * 2. The next multixact may still be in process of being filled in: that
	 * is, another process may have done GetNewMultiXactId but not yet written
	 * the offset entry for that ID.  In that scenario, it is guaranteed that
	 * the offset entry for that multixact exists (because GetNewMultiXactId
	 * won't release MultiXactGenLock until it does) but contains zero
	 * (because we are careful to pre-zero offset pages). Because
	 * GetNewMultiXactId will never return zero as the starting offset for a
	 * multixact, when we read zero as the next multixact's offset, we know we
	 * have this case.	We sleep for a bit and try again.
	 *
	 * 3. Because GetNewMultiXactId increments offset zero to offset one to
	 * handle case #2, there is an ambiguity near the point of offset
	 * wraparound.	If we see next multixact's offset is one, is that our
	 * multixact's actual endpoint, or did it end at zero with a subsequent
	 * increment?  We handle this using the knowledge that if the zero'th
	 * member slot wasn't filled, it'll contain zero, and zero isn't a valid
	 * transaction ID so it can't be a multixact member.  Therefore, if we
	 * read a zero from the members array, just ignore it.
	 *
	 * This is all pretty messy, but the mess occurs only in infrequent corner
	 * cases, so it seems better than holding the MultiXactGenLock for a long
	 * time on every multixact creation.
	 */
retry:
	LWLockAcquire(MultiXactOffsetControlLock, LW_EXCLUSIVE);

	pageno = MultiXactIdToOffsetPage(multi);
	entryno = MultiXactIdToOffsetEntry(multi);

	slotno = SimpleLruReadPage(MultiXactOffsetCtl, pageno, true, multi);
	offptr = (MultiXactOffset *) MultiXactOffsetCtl->shared->page_buffer[slotno];
	offptr += entryno;
	offset = *offptr;

	Assert(offset != 0);

	/*
	 * Use the same increment rule as GetNewMultiXactId(), that is, don't
	 * handle wraparound explicitly until needed.
	 */
	tmpMXact = multi + 1;

	if (nextMXact == tmpMXact)
	{
		/* Corner case 1: there is no next multixact */
		length = nextOffset - offset;
	}
	else
	{
		MultiXactOffset nextMXOffset;

		/* Handle corner cases if needed */
		tmpMXact = HandleMxactOffsetCornerCases(tmpMXact);

		prev_pageno = pageno;

		pageno = MultiXactIdToOffsetPage(tmpMXact);
		entryno = MultiXactIdToOffsetEntry(tmpMXact);

		if (pageno != prev_pageno)
			slotno = SimpleLruReadPage(MultiXactOffsetCtl, pageno, true, tmpMXact);

		offptr = (MultiXactOffset *) MultiXactOffsetCtl->shared->page_buffer[slotno];
		offptr += entryno;
		nextMXOffset = *offptr;

		if (nextMXOffset == 0)
		{
			/* Corner case 2: next multixact is still being filled in */
			LWLockRelease(MultiXactOffsetControlLock);
			pg_usleep(1000L);
			goto retry;
		}

		length = nextMXOffset - offset;
	}

	LWLockRelease(MultiXactOffsetControlLock);

	ptr = (MultiXactMember *) palloc(length * sizeof(MultiXactMember));
	*members = ptr;

	/* Now get the members themselves. */
	LWLockAcquire(MultiXactMemberControlLock, LW_EXCLUSIVE);

	truelength = 0;
	prev_pageno = -1;
	for (i = 0; i < length; i++, offset++)
	{
		TransactionId *xactptr;
		uint32	   *flagsptr;
		int			flagsoff;
		int			bshift;
		int			memberoff;

		pageno = MXOffsetToMemberPage(offset);
		memberoff = MXOffsetToMemberOffset(offset);

		if (pageno != prev_pageno)
		{
			slotno = SimpleLruReadPage(MultiXactMemberCtl, pageno, true, multi);
			prev_pageno = pageno;
		}

		xactptr = (TransactionId *)
			(MultiXactMemberCtl->shared->page_buffer[slotno] + memberoff);

		if (!TransactionIdIsValid(*xactptr))
		{
			/* Corner case 3: we must be looking at unused slot zero */
			Assert(offset == 0);
			continue;
		}

		flagsoff = MXOffsetToFlagsOffset(offset);
		bshift = MXOffsetToFlagsBitShift(offset);
		flagsptr = (uint32 *) (MultiXactMemberCtl->shared->page_buffer[slotno] + flagsoff);

		ptr[truelength].xid = *xactptr;
		ptr[truelength].status = (*flagsptr >> bshift) & MXACT_MEMBER_XACT_BITMASK;
		truelength++;
	}

	LWLockRelease(MultiXactMemberControlLock);

	/*
	 * Copy the result into the local cache.
	 */
	mXactCachePut(multi, truelength, ptr);

	debug_elog3(DEBUG2, "GetMembers: no cache for %s",
				mxid_to_string(multi, truelength, ptr));
	return truelength;
}

/*
 * mxactMemberComparator
 *		qsort comparison function for MultiXactMember
 *
 * We can't use wraparound comparison for XIDs because that does not respect
 * the triangle inequality!  Any old sort order will do.
 */
static int
mxactMemberComparator(const void *arg1, const void *arg2)
{
	MultiXactMember member1 = *(const MultiXactMember *) arg1;
	MultiXactMember member2 = *(const MultiXactMember *) arg2;

	if (member1.xid > member2.xid)
		return 1;
	if (member1.xid < member2.xid)
		return -1;
	if (member1.status > member2.status)
		return 1;
	if (member1.status < member2.status)
		return -1;
	return 0;
}

/*
 * mXactCacheGetBySet
 *		returns a MultiXactId from the cache based on the set of
 *		TransactionIds that compose it, or InvalidMultiXactId if
 *		none matches.
 *
 * This is helpful, for example, if two transactions want to lock a huge
 * table.  By using the cache, the second will use the same MultiXactId
 * for the majority of tuples, thus keeping MultiXactId usage low (saving
 * both I/O and wraparound issues).
 *
 * NB: the passed members array will be sorted in-place.
 */
static MultiXactId
mXactCacheGetBySet(int nmembers, MultiXactMember *members)
{
	mXactCacheEnt *entry;

	debug_elog3(DEBUG2, "CacheGet: looking for %s",
				mxid_to_string(InvalidMultiXactId, nmembers, members));

	/* sort the array so comparison is easy */
	qsort(members, nmembers, sizeof(MultiXactMember), mxactMemberComparator);

	for (entry = MXactCache; entry != NULL; entry = entry->next)
	{
		if (entry->nmembers != nmembers)
			continue;

		/* We assume the cache entries are sorted */
		/* XXX we assume the unused bits in "status" are zeroed */
		if (memcmp(members, entry->members, nmembers * sizeof(MultiXactMember)) == 0)
		{
			debug_elog3(DEBUG2, "CacheGet: found %u", entry->multi);
			return entry->multi;
		}
	}

	debug_elog2(DEBUG2, "CacheGet: not found :-(");
	return InvalidMultiXactId;
}

/*
 * mXactCacheGetById
 *		returns the composing MultiXactMember set from the cache for a
 *		given MultiXactId, if present.
 *
 * If successful, *xids is set to the address of a palloc'd copy of the
 * MultiXactMember set.  Return value is number of members, or -1 on failure.
 */
static int
mXactCacheGetById(MultiXactId multi, MultiXactMember **members)
{
	mXactCacheEnt *entry;

	debug_elog3(DEBUG2, "CacheGet: looking for %u", multi);

	for (entry = MXactCache; entry != NULL; entry = entry->next)
	{
		if (entry->multi == multi)
		{
			MultiXactMember *ptr;
			Size		size;

			size = sizeof(MultiXactMember) * entry->nmembers;
			ptr = (MultiXactMember *) palloc(size);
			*members = ptr;

			memcpy(ptr, entry->members, size);

			debug_elog3(DEBUG2, "CacheGet: found %s",
						mxid_to_string(multi, entry->nmembers, entry->members));
			return entry->nmembers;
		}
	}

	debug_elog2(DEBUG2, "CacheGet: not found");
	return -1;
}

/*
 * mXactCachePut
 *		Add a new MultiXactId and its composing set into the local cache.
 */
static void
mXactCachePut(MultiXactId multi, int nmembers, MultiXactMember *members)
{
	mXactCacheEnt *entry;

	debug_elog3(DEBUG2, "CachePut: storing %s",
				mxid_to_string(multi, nmembers, members));

	if (MXactContext == NULL)
	{
		/* The cache only lives as long as the current transaction */
		debug_elog2(DEBUG2, "CachePut: initializing memory context");
		MXactContext = AllocSetContextCreate(TopTransactionContext,
											 "MultiXact Cache Context",
											 ALLOCSET_SMALL_MINSIZE,
											 ALLOCSET_SMALL_INITSIZE,
											 ALLOCSET_SMALL_MAXSIZE);
	}

	entry = (mXactCacheEnt *)
		MemoryContextAlloc(MXactContext,
						   offsetof(mXactCacheEnt, members) +
						   nmembers * sizeof(MultiXactMember));

	entry->multi = multi;
	entry->nmembers = nmembers;
	memcpy(entry->members, members, nmembers * sizeof(MultiXactMember));

	/* mXactCacheGetBySet assumes the entries are sorted, so sort them */
	qsort(entry->members, nmembers, sizeof(MultiXactMember), mxactMemberComparator);

	entry->next = MXactCache;
	MXactCache = entry;
}

#ifdef MULTIXACT_DEBUG
static char *
mxstatus_to_string(MultiXactStatus status)
{
	switch (status)
	{
		case MultiXactStatusForKeyShare:
			return "keysh";
		case MultiXactStatusForShare:
			return "sh";
		case MultiXactStatusForUpdate:
			return "forupd";
		case MultiXactStatusUpdate:
			return "upd";
		case MultiXactStatusKeyUpdate:
			return "keyup";
		default:
			elog(ERROR, "unrecognized multixact status %d", status);
			return "";
	}
}

static char *
mxid_to_string(MultiXactId multi, int nmembers, MultiXactMember *members)
{
	char	   *str = palloc(15 * (nmembers + 1) + 4);
	int			i;

	snprintf(str, 47, "%u %d[%u (%s)", multi, nmembers, members[0].xid,
			 mxstatus_to_string(members[0].status));

	for (i = 1; i < nmembers; i++)
		snprintf(str + strlen(str), 17, ", %u (%s)", members[i].xid,
				 mxstatus_to_string(members[i].status));

	strcat(str, "]");
	return str;
}
#endif

/*
 * AtEOXact_MultiXact
 *		Handle transaction end for MultiXact
 *
 * This is called at top transaction commit or abort (we don't care which).
 */
void
AtEOXact_MultiXact(void)
{
	/*
	 * Discard the local MultiXactId cache.  Since MXactContext was created as
	 * a child of TopTransactionContext, we needn't delete it explicitly.
	 */
	MXactContext = NULL;
	MXactCache = NULL;
}

/*
 * AtPrepare_MultiXact
 *		Save multixact state at 2PC tranasction prepare
 */
void
AtPrepare_MultiXact(void)
{
	/* nothing to do */
}

/*
 * PostPrepare_MultiXact
 *		Clean up after successful PREPARE TRANSACTION
 */
void
PostPrepare_MultiXact(TransactionId xid)
{
	/*
	 * Discard the local MultiXactId cache like in AtEOX_MultiXact
	 */
	MXactContext = NULL;
	MXactCache = NULL;
}

/*
 * multixact_twophase_recover
 *		Recover the state of a prepared transaction at startup
 */
void
multixact_twophase_recover(TransactionId xid, uint16 info,
						   void *recdata, uint32 len)
{
	/* nothing to do */
}

/*
 * multixact_twophase_postcommit
 *		Similar to AtEOX_MultiXact but for COMMIT PREPARED
 */
void
multixact_twophase_postcommit(TransactionId xid, uint16 info,
							  void *recdata, uint32 len)
{
	/* nothing to do */
}

/*
 * multixact_twophase_postabort
 *		This is actually just the same as the COMMIT case.
 */
void
multixact_twophase_postabort(TransactionId xid, uint16 info,
							 void *recdata, uint32 len)
{
	/* nothing to do */
}

/*
 * Initialization of shared memory for MultiXact.  We use two SLRU areas,
 * thus double memory.	Also, reserve space for the shared MultiXactState
 * struct and the per-backend MultiXactId arrays (two of those, too).
 */
Size
MultiXactShmemSize(void)
{
	Size		size;

	size = sizeof(MultiXactStateData);
	size = add_size(size, SimpleLruShmemSize(NUM_MXACTOFFSET_BUFFERS, 0));
	size = add_size(size, SimpleLruShmemSize(NUM_MXACTMEMBER_BUFFERS, 0));

	return size;
}

void
MultiXactShmemInit(void)
{
	bool		found;

	debug_elog2(DEBUG2, "Shared Memory Init for MultiXact");

	MultiXactOffsetCtl->PagePrecedes = MultiXactOffsetPagePrecedes;
	MultiXactMemberCtl->PagePrecedes = MultiXactMemberPagePrecedes;

	SimpleLruInit(MultiXactOffsetCtl,
				  "MultiXactOffset Ctl", NUM_MXACTOFFSET_BUFFERS, 0,
				  MultiXactOffsetControlLock, "pg_multixact/offsets");
	SimpleLruInit(MultiXactMemberCtl,
				  "MultiXactMember Ctl", NUM_MXACTMEMBER_BUFFERS, 0,
				  MultiXactMemberControlLock, "pg_multixact/members");

	/* Initialize our shared state struct */
	MultiXactState = ShmemInitStruct("Shared MultiXact State",
									 sizeof(MultiXactStateData),
									 &found);
	if (!IsUnderPostmaster)
	{
		Assert(!found);

		/* Make sure we zero out the per-backend state */
		MemSet(MultiXactState, 0, sizeof(MultiXactStateData));
	}
	else
		Assert(found);
}

/*
 * This func must be called ONCE on system install.  It creates the initial
 * MultiXact segments.	(The MultiXacts directories are assumed to have been
 * created by initdb, and MultiXactShmemInit must have been called already.)
 */
void
BootStrapMultiXact(void)
{
	int			slotno;

	LWLockAcquire(MultiXactOffsetControlLock, LW_EXCLUSIVE);

	/* Create and zero the first page of the offsets log */
	slotno = ZeroMultiXactOffsetPage(0, false, InvalidTransactionId, 0);

	/* Make sure it's written out */
	SimpleLruWritePage(MultiXactOffsetCtl, slotno);
	Assert(!MultiXactOffsetCtl->shared->page_dirty[slotno]);

	LWLockRelease(MultiXactOffsetControlLock);

	LWLockAcquire(MultiXactMemberControlLock, LW_EXCLUSIVE);

	/* Create and zero the first page of the members log */
	slotno = ZeroMultiXactMemberPage(0, false);

	/* Make sure it's written out */
	SimpleLruWritePage(MultiXactMemberCtl, slotno);
	Assert(!MultiXactMemberCtl->shared->page_dirty[slotno]);

	LWLockRelease(MultiXactMemberControlLock);
}

/*
 * Initialize (or reinitialize) a page of MultiXactOffset to zeroes.
 * If writeXlog is TRUE, also emit an XLOG record saying we did this.
 *
 * If truncateXid is valid, store it in the first position of the page.
 *
 * The page is not actually written, just set up in shared memory.
 * The slot number of the new page is returned.
 *
 * Control lock must be held at entry, and will be held at exit.
 */
static int
ZeroMultiXactOffsetPage(int pageno, bool writeXlog, TransactionId truncateXid,
						uint32 truncateXidEpoch)
{
	int			slotno;

	slotno = SimpleLruZeroPage(MultiXactOffsetCtl, pageno);

	if (writeXlog)
		WriteMZeroOffsetPageXlogRec(pageno, truncateXid, truncateXidEpoch);

	if (TransactionIdIsValid(truncateXid))
	{
		MultiXactOffset *offptr;

		offptr = (MultiXactOffset *) MultiXactOffsetCtl->shared->page_buffer[slotno];
		*(offptr++) = truncateXid;
		*offptr = truncateXidEpoch;

		MultiXactOffsetCtl->shared->page_dirty[slotno] = true;
	}

	return slotno;
}

/*
 * Ditto for MultiXactMember, except these don't worry about truncation info.
 */
static int
ZeroMultiXactMemberPage(int pageno, bool writeXlog)
{
	int			slotno;

	slotno = SimpleLruZeroPage(MultiXactMemberCtl, pageno);

	if (writeXlog)
		WriteMZeroMemberPageXlogRec(pageno);

	return slotno;
}

/*
 * This must be called ONCE during postmaster or standalone-backend startup.
 *
 * StartupXLOG has already established nextMXact/nextOffset by calling
 * MultiXactSetNextMXact and/or MultiXactAdvanceNextMXact.	Note that we
 * may already have replayed WAL data into the SLRU files.
 *
 * We don't need any locks here, really; the SLRU locks are taken
 * only because slru.c expects to be called with locks held.
 */
void
StartupMultiXact(void)
{
	MultiXactId multi = MultiXactState->nextMXact;
	MultiXactOffset offset = MultiXactState->nextOffset;
	int			pageno;
	int			entryno;
	int			flagsoff;

	/* Clean up offsets state */
	LWLockAcquire(MultiXactOffsetControlLock, LW_EXCLUSIVE);

	/*
	 * Initialize our idea of the latest page number.
	 */
	pageno = MultiXactIdToOffsetPage(multi);
	MultiXactOffsetCtl->shared->latest_page_number = pageno;

	/*
	 * Zero out the remainder of the current offsets page.	See notes in
	 * StartupCLOG() for motivation.
	 */
	entryno = MultiXactIdToOffsetEntry(multi);
	if (entryno != 0)
	{
		int			slotno;
		MultiXactOffset *offptr;

		slotno = SimpleLruReadPage(MultiXactOffsetCtl, pageno, true, multi);
		offptr = (MultiXactOffset *) MultiXactOffsetCtl->shared->page_buffer[slotno];
		offptr += entryno;

		MemSet(offptr, 0, BLCKSZ - (entryno * sizeof(MultiXactOffset)));

		MultiXactOffsetCtl->shared->page_dirty[slotno] = true;
	}

	LWLockRelease(MultiXactOffsetControlLock);

	/* And the same for members */
	LWLockAcquire(MultiXactMemberControlLock, LW_EXCLUSIVE);

	/*
	 * Initialize our idea of the latest page number.
	 */
	pageno = MXOffsetToMemberPage(offset);
	MultiXactMemberCtl->shared->latest_page_number = pageno;

	/*
	 * Zero out the remainder of the current members page.	See notes in
	 * TrimCLOG() for motivation.
	 */
	flagsoff = MXOffsetToFlagsOffset(offset);
	if (flagsoff != 0)
	{
		int			slotno;
		TransactionId *xidptr;
		int			memberoff;

		memberoff = MXOffsetToMemberOffset(offset);
		slotno = SimpleLruReadPage(MultiXactMemberCtl, pageno, true, offset);
		xidptr = (TransactionId *)
			(MultiXactMemberCtl->shared->page_buffer[slotno] + memberoff);

		MemSet(xidptr, 0, BLCKSZ - memberoff);

		/*
		 * Note: we don't need to zero out the flag bits in the remaining
		 * members of the current group, because they are always reset before
		 * writing.
		 */

		MultiXactMemberCtl->shared->page_dirty[slotno] = true;
	}

	LWLockRelease(MultiXactMemberControlLock);
}

/*
 * This must be called ONCE during postmaster or standalone-backend shutdown
 */
void
ShutdownMultiXact(void)
{
	/* Flush dirty MultiXact pages to disk */
	TRACE_POSTGRESQL_MULTIXACT_CHECKPOINT_START(false);
	SimpleLruFlush(MultiXactOffsetCtl, false);
	SimpleLruFlush(MultiXactMemberCtl, false);
	TRACE_POSTGRESQL_MULTIXACT_CHECKPOINT_DONE(false);
}

/*
 * Get the next MultiXactId, offset and truncate info to save in a checkpoint
 * record
 */
void
MultiXactGetCheckptMulti(bool is_shutdown,
						 MultiXactId *nextMulti,
						 MultiXactOffset *nextMultiOffset,
						 TransactionId *oldestTruncateXid,
						 uint32 *oldestTruncateXidEpoch,
						 MultiXactId *oldestMulti)
{
	LWLockAcquire(MultiXactGenLock, LW_SHARED);

	*nextMulti = MultiXactState->nextMXact;
	*nextMultiOffset = MultiXactState->nextOffset;
	*oldestTruncateXid = MultiXactState->truncateXid;
	*oldestTruncateXidEpoch = MultiXactState->truncateXidEpoch;
	*oldestMulti = MultiXactState->oldestMultiXactId;

	LWLockRelease(MultiXactGenLock);

	debug_elog7(DEBUG2,
				"MultiXact: checkpoint is nextMulti %u, nextOffset %u; truncate xid %u, epoch %u; oldest multi %u",
				*nextMulti, *nextMultiOffset, *oldestTruncateXid,
				*oldestTruncateXidEpoch, *oldestMulti);
}

/*
 * Perform a checkpoint --- either during shutdown, or on-the-fly
 */
void
CheckPointMultiXact(void)
{
	TRACE_POSTGRESQL_MULTIXACT_CHECKPOINT_START(true);

	/* Flush dirty MultiXact pages to disk */
	SimpleLruFlush(MultiXactOffsetCtl, true);
	SimpleLruFlush(MultiXactMemberCtl, true);

	TRACE_POSTGRESQL_MULTIXACT_CHECKPOINT_DONE(true);
}

/*
 * Set the next-to-be-assigned MultiXactId and offset
 *
 * This is used when we can determine the correct next ID/offset exactly
 * from a checkpoint record.  We need no locking since it is only called
 * during bootstrap and XLog replay.
 */
void
MultiXactSetNextMXact(MultiXactId nextMulti,
					  MultiXactOffset nextMultiOffset)
{
	debug_elog4(DEBUG2, "MultiXact: setting next multi to %u offset %u",
				nextMulti, nextMultiOffset);
	MultiXactState->nextMXact = nextMulti;
	MultiXactState->nextOffset = nextMultiOffset;
}

/*
 * Ensure the next-to-be-assigned MultiXactId is at least minMulti,
 * and similarly nextOffset is at least minMultiOffset.
 *
 * This is used when we can determine minimum safe values from an XLog
 * record (either an on-line checkpoint or an mxact creation log entry).
 * We need no locking since it is only called during XLog replay.
 */
void
MultiXactAdvanceNextMXact(MultiXactId minMulti,
						  MultiXactOffset minMultiOffset)
{
	if (MultiXactIdPrecedes(MultiXactState->nextMXact, minMulti))
	{
		debug_elog3(DEBUG2, "MultiXact: setting next multi to %u", minMulti);
		MultiXactState->nextMXact = minMulti;
	}
	if (MultiXactOffsetPrecedes(MultiXactState->nextOffset, minMultiOffset))
	{
		debug_elog3(DEBUG2, "MultiXact: setting next offset to %u",
					minMultiOffset);
		MultiXactState->nextOffset = minMultiOffset;
	}
}

/*
 * Make sure that MultiXactOffset has room for a newly-allocated MultiXactId.
 *
 * If the newly allocated page is the first page on the segment, store an
 * appropriate truncate Xid value in the page first position.
 *
 * NB: this is called while holding MultiXactGenLock.  We want it to be very
 * fast most of the time; even when it's not so fast, no actual I/O need
 * happen unless we're forced to write out a dirty log or xlog page to make
 * room in shared memory.
 */
static void
ExtendMultiXactOffset(MultiXactId multi)
{
	int			pageno;
	TransactionId truncateXid;
	uint32		truncateXidEpoch;

	/*
	 * No work except at first MultiXactId of a page.  But beware: just after
	 * wraparound, the first MultiXactId of page zero is FirstMultiXactId.
	 */
	if (MultiXactIdToOffsetEntry(multi) != 0 &&
		multi != FirstMultiXactId)
		return;

	pageno = MultiXactIdToOffsetPage(multi);

	/*
	 * Determine the truncateXid and epoch that the new segment needs, if
	 * this is the first page of the segment.
	 */
	if (pageno % SLRU_PAGES_PER_SEGMENT == 0)
	{
		TransactionId	nextXid;

		Assert(TransactionIdIsValid(RecentGlobalXmin));
		truncateXid = RecentGlobalXmin;

		GetNextXidAndEpoch(&nextXid, &truncateXidEpoch);
		/*
		 * nextXid is certainly logically later than RecentGlobalXmin.  So if
		 * it's numerically less, it must have wrapped into the next epoch.
		 */
		if (nextXid < truncateXid)
			truncateXidEpoch--;
	}
	else
	{
		truncateXid = InvalidTransactionId;
		truncateXidEpoch = 0;
	}

	LWLockAcquire(MultiXactOffsetControlLock, LW_EXCLUSIVE);

	/*
	 * Zero the page, mark it with its truncate info, and make an XLOG entry
	 * about it.
	 */
	ZeroMultiXactOffsetPage(pageno, true, truncateXid, truncateXidEpoch);
	LWLockRelease(MultiXactOffsetControlLock);

	/*
	 * Finally, record the new truncation point in shared memory, if
	 * there isn't one already.
	 */
	if (!TransactionIdIsValid(MultiXactState->truncateXid))
	{
		MultiXactState->truncateXid = truncateXid;
		MultiXactState->truncateXidEpoch = truncateXidEpoch;
	}
}

/*
 * Make sure that MultiXactMember has room for the members of a newly-
 * allocated MultiXactId.
 *
 * Like the above routine, this is called while holding MultiXactGenLock;
 * same comments apply.
 */
static void
ExtendMultiXactMember(MultiXactOffset offset, int nmembers)
{
	/*
	 * It's possible that the members span more than one page of the members
	 * file, so we loop to ensure we consider each page.  The coding is not
	 * optimal if the members span several pages, but that seems unusual
	 * enough to not worry much about.
	 */
	while (nmembers > 0)
	{
		int			flagsoff;
		int			flagsbit;
		int			difference;

		/*
		 * Only zero when at first entry of a page.
		 */
		flagsoff = MXOffsetToFlagsOffset(offset);
		flagsbit = MXOffsetToFlagsBitShift(offset);
		if (flagsoff == 0 && flagsbit == 0)
		{
			int			pageno;

			pageno = MXOffsetToMemberPage(offset);

			LWLockAcquire(MultiXactMemberControlLock, LW_EXCLUSIVE);

			/* Zero the page and make an XLOG entry about it */
			ZeroMultiXactMemberPage(pageno, true);

			LWLockRelease(MultiXactMemberControlLock);
		}

		/* Advance to next page (OK if nmembers goes negative) */
		difference = MULTIXACT_MEMBERS_PER_PAGE - offset % MULTIXACT_MEMBERS_PER_PAGE;
		offset += difference;
		nmembers -= difference;
	}
}

/*
 * Complete a SegmentInfo with the truncate Xid and epoch, as read from its
 * first page.
 */
static void
fillSegmentInfoData(SlruCtl ctl, SegmentInfo *segment)
{
	int			slotno;
	MultiXactId *offptr;

	/* lock is acquired by SimpleLruReadPage_ReadOnly */
	/* FIXME it'd be nice not to trash the entire SLRU cache while at this */
	slotno = SimpleLruReadPage_ReadOnly(ctl, segment->segno, InvalidTransactionId);
	offptr = (MultiXactId *) MultiXactOffsetCtl->shared->page_buffer[slotno];
	segment->truncateXid = *offptr;
	offptr++;
	segment->truncateXidEpoch = *offptr;
	offptr++;
	segment->firstOffset = *offptr;
	LWLockRelease(ctl->shared->ControlLock);
}

/* SegmentInfo comparator, for qsort and bsearch */
static int
compareTruncateXidEpoch(const void *a, const void *b)
{
	const SegmentInfo *sega = (const SegmentInfo *) a;
	const SegmentInfo *segb = (const SegmentInfo *) b;
	uint32	epocha = sega->truncateXidEpoch;
	uint32	epochb = segb->truncateXidEpoch;
	TransactionId	xida = sega->truncateXid;
	TransactionId	xidb = segb->truncateXid;

	if (epocha < epochb)
		return -1;
	if (epocha > epochb)
		return 1;
	if (xida < xidb)
		return -1;
	if (xida > xidb)
		return 1;
	return 0;
}

/*
 * SlruScanDirectory callback
 * 		This callback is in charge of scanning all existing segments,
 * 		to determine their respective truncation points.
 *
 * This does not delete any segments.
 */
static bool
mxactSlruGathererCb(SlruCtl ctl, char *segname, int segpage,
					void *data)
{
	TruncateCbData *truncdata = (TruncateCbData *) data;
	SegmentInfo		seg;

	/*
	 * Keep track of the truncate Xid and other data for the caller to sort out
	 * the new truncation point.
	 */
	seg.segno = segpage % SLRU_PAGES_PER_SEGMENT;
	fillSegmentInfoData(ctl, &seg);

	if (truncdata->remaining == NULL)
	{
		truncdata->remaining_alloc = 8;
		truncdata->remaining_used = 0;
		truncdata->remaining = palloc(truncdata->remaining_alloc *
									  sizeof(SegmentInfo));
	}
	else if (truncdata->remaining_used == truncdata->remaining_alloc - 1)
	{
		truncdata->remaining_alloc *= 2;
		truncdata->remaining = repalloc(truncdata->remaining,
										truncdata->remaining_alloc);
	}
	truncdata->remaining[truncdata->remaining_used++] = seg;

	return false;	/* keep going */
}

/*
 * Remove all MultiXactOffset and MultiXactMember segments before the oldest
 * ones still of interest.
 *
 * The truncation rules for the Offset SLRU area are:
 *
 * 1. the current segment is never to be deleted.
 * 2. for all the remaining segments, keep track of their respective number
 *    and truncate Xid info.  The caller is to determine the new truncation
 *    point from this data.
 *
 * This is called only during checkpoints.	We assume no more than one
 * backend does this at a time.
 *
 * XXX do we have any issues with needing to checkpoint here?
 */
void
TruncateMultiXact(TransactionId frozenXid)
{
	TransactionId	currentXid;
	uint32		frozenXidEpoch;
	TruncateCbData	truncdata;
	SegmentInfo *truncateSegment;
	SegmentInfo	frozenPosition;
	int			cutoffPage;
	int			i;
	TransactionId	newTruncateXid;
	int		newTruncateXidEpoch;

	/*
	 * Quick exit #1: if the truncateXid is not valid, bail out.  We do this
	 * check without a lock so that it's fast in the common case when there's
	 * only one segment (which cannot be removed).  If a concurrent backend is
	 * creating a new segment, no problem: it just means we delay removing
	 * files until we're next called.  This assumes that storing an aligned
	 * 32-bit value is atomic.
	 */
	if (!TransactionIdIsValid(MultiXactState->truncateXid))
		return;

	/*
	 * Compute the epoch corresponding to the frozenXid value we were given.
	 *
	 * The current Xid value must be logically newer than frozenXid, so if it's
	 * numerically lower, it must belong to the next epoch.
	 */
	GetNextXidAndEpoch(&currentXid, &frozenXidEpoch);
	if (currentXid < frozenXid)
		frozenXidEpoch--;

	/*
	 * Quick exit #2: the oldest segment is not yet old enough to be removed.
	 * In that case we don't need to scan the whole directory.
	 */
	LWLockAcquire(MultiXactGenLock, LW_SHARED);
	Assert(frozenXidEpoch >= MultiXactState->truncateXidEpoch);
	if ((frozenXidEpoch == MultiXactState->truncateXidEpoch) &&
		(frozenXid < MultiXactState->truncateXid))
	{
		LWLockRelease(MultiXactGenLock);
		return;
	}
	LWLockRelease(MultiXactGenLock);

	/*
	 * Have our callback scan the SLRU directory to let us determine the
	 * truncation point.
	 */
	truncdata.remaining_used = 0;
	truncdata.remaining_alloc = 0;
	truncdata.remaining = NULL;
	SlruScanDirectory(MultiXactOffsetCtl, mxactSlruGathererCb, &truncdata);

	/*
	 * Determine the maximum segment whose truncateXid is less than the
	 * truncate point.
	 */
	frozenPosition.truncateXid = frozenXid;
	frozenPosition.truncateXidEpoch = frozenXidEpoch;
	truncateSegment = NULL;
	for (i = 0; i < truncdata.remaining_used; i++)
	{
		if ((compareTruncateXidEpoch(&frozenPosition,
									 &(truncdata.remaining[i])) > 0) &&
			(truncateSegment->segno < truncdata.remaining[i].segno))
		{
			truncateSegment = &(truncdata.remaining[i]);
		}
	}

	/*
	 * Nothing to delete? This shouldn't happen, due to quick exit #2 above,
	 * but we'd better cope.
	 */
	if (truncateSegment == NULL)
		return;

	/* truncate MultiXactOffset */
	SimpleLruTruncate(MultiXactOffsetCtl, firstPageOf(truncateSegment->segno));

	/*
	 * And truncate MultiXactMember at the first offset used by the oldest
	 * remaining segment.
	 */
	cutoffPage = MXOffsetToMemberPage(truncateSegment->firstOffset);

	SimpleLruTruncate(MultiXactMemberCtl, cutoffPage);

	/*
	 * Finally, update shared memory to keep track of the next usable
	 * truncation point, if any.  If the truncation point for offsets was the
	 * last remaining segment, then there's no next truncation point: it will
	 * be set when the next segment is created.  Otherwise, the second
	 * remaining segment determines the next truncation point.
	 */
	newTruncateXid = InvalidTransactionId;
	newTruncateXidEpoch = 0;
	for (i = 0; i < truncdata.remaining_used; i++)
	{
		if (truncdata.remaining[i].segno == truncateSegment->segno + 1)
		{
			newTruncateXid = truncdata.remaining[i].truncateXid;
			newTruncateXidEpoch = truncdata.remaining[i].truncateXidEpoch;
			break;
		}
	}

	LWLockAcquire(MultiXactGenLock, LW_EXCLUSIVE);

	/*
	 * FIXME there's a race condition here: somebody might have created a new
	 * segment after we finished scanning the dir.  That scenario would leave
	 * us with an invalid truncateXid in shared memory, which is not an easy
	 * situation to get out of.  Needs more thought.
	 */

	MultiXactState->truncateXid = newTruncateXid;
	MultiXactState->truncateXidEpoch = newTruncateXidEpoch;

	/*
	 * we also set the oldest visible MultiXactId to the frozenXid value we
	 * were given; although the segments we kept may have values earlier than
	 * that, they are not supposed to remain on disk anyway.
	 */
	MultiXactState->oldestMultiXactId = frozenXid;
	LWLockRelease(MultiXactGenLock);
}

/*
 * Decide which of two MultiXactOffset page numbers is "older" for truncation
 * purposes.
 *
 * We need to use comparison of MultiXactId here in order to do the right
 * thing with wraparound.  However, if we are asked about page number zero, we
 * don't want to hand InvalidMultiXactId to MultiXactIdPrecedes: it'll get
 * weird.  So, offset both multis by FirstMultiXactId to avoid that.
 * (Actually, the current implementation doesn't do anything weird with
 * InvalidMultiXactId, but there's no harm in leaving this code like this.)
 */
static bool
MultiXactOffsetPagePrecedes(int page1, int page2)
{
	MultiXactId multi1;
	MultiXactId multi2;

	multi1 = ((MultiXactId) page1) * MULTIXACT_OFFSETS_PER_PAGE;
	multi1 += FirstMultiXactId;
	multi2 = ((MultiXactId) page2) * MULTIXACT_OFFSETS_PER_PAGE;
	multi2 += FirstMultiXactId;

	return MultiXactIdPrecedes(multi1, multi2);
}

/*
 * Decide which of two MultiXactMember page numbers is "older" for truncation
 * purposes.  There is no "invalid offset number" so use the numbers verbatim.
 */
static bool
MultiXactMemberPagePrecedes(int page1, int page2)
{
	MultiXactOffset offset1;
	MultiXactOffset offset2;

	offset1 = ((MultiXactOffset) page1) * MULTIXACT_MEMBERS_PER_PAGE;
	offset2 = ((MultiXactOffset) page2) * MULTIXACT_MEMBERS_PER_PAGE;

	return MultiXactOffsetPrecedes(offset1, offset2);
}

/*
 * Decide which of two MultiXactIds is earlier.
 *
 * XXX do we need to do something special for InvalidMultiXactId?
 * (Doesn't look like it.)
 */
static bool
MultiXactIdPrecedes(MultiXactId multi1, MultiXactId multi2)
{
	int32		diff = (int32) (multi1 - multi2);

	return (diff < 0);
}

/*
 * Decide which of two offsets is earlier.
 */
static bool
MultiXactOffsetPrecedes(MultiXactOffset offset1, MultiXactOffset offset2)
{
	int32		diff = (int32) (offset1 - offset2);

	return (diff < 0);
}

static void
WriteMZeroOffsetPageXlogRec(int pageno, TransactionId truncateXid,
							uint32 truncateXidEpoch)
{
	XLogRecData	rdata;
	MxactZeroOffPg zerooff;

	zerooff.pageno = pageno;
	zerooff.truncateXid = truncateXid;
	zerooff.truncateXidEpoch = truncateXidEpoch;

	rdata.data = (char *) (&zerooff);
	rdata.len = sizeof(MxactZeroOffPg);
	rdata.buffer = InvalidBuffer;
	rdata.next = NULL;
	(void) XLogInsert(RM_MULTIXACT_ID, XLOG_MULTIXACT_ZERO_OFF_PAGE, &rdata);
}

/*
 * Write an xlog record reflecting the zeroing of either a MEMBERs page.
 */
static void
WriteMZeroMemberPageXlogRec(int pageno)
{
	XLogRecData rdata;

	rdata.data = (char *) (&pageno);
	rdata.len = sizeof(int);
	rdata.buffer = InvalidBuffer;
	rdata.next = NULL;
	(void) XLogInsert(RM_MULTIXACT_ID, XLOG_MULTIXACT_ZERO_MEM_PAGE, &rdata);
}

/*
 * MULTIXACT resource manager's routines
 */
void
multixact_redo(XLogRecPtr lsn, XLogRecord *record)
{
	uint8		info = record->xl_info & ~XLR_INFO_MASK;

	/* Backup blocks are not used in multixact records */
	Assert(!(record->xl_info & XLR_BKP_BLOCK_MASK));

	if (info == XLOG_MULTIXACT_ZERO_OFF_PAGE)
	{
		MxactZeroOffPg zerooff;
		int			slotno;

		memcpy(&zerooff, XLogRecGetData(record), sizeof(MxactZeroOffPg));

		LWLockAcquire(MultiXactOffsetControlLock, LW_EXCLUSIVE);

		slotno = ZeroMultiXactOffsetPage(zerooff.pageno, false,
										 zerooff.truncateXid,
										 zerooff.truncateXidEpoch);
		SimpleLruWritePage(MultiXactOffsetCtl, slotno);
		Assert(!MultiXactOffsetCtl->shared->page_dirty[slotno]);

		LWLockRelease(MultiXactOffsetControlLock);

		LWLockAcquire(MultiXactGenLock, LW_EXCLUSIVE);
		if (!TransactionIdIsValid(MultiXactState->truncateXid))
			MultiXactState->truncateXid = zerooff.truncateXid;
		LWLockRelease(MultiXactGenLock);
	}
	else if (info == XLOG_MULTIXACT_ZERO_MEM_PAGE)
	{
		int			pageno;
		int			slotno;

		memcpy(&pageno, XLogRecGetData(record), sizeof(int));

		LWLockAcquire(MultiXactMemberControlLock, LW_EXCLUSIVE);

		slotno = ZeroMultiXactMemberPage(pageno, false);
		SimpleLruWritePage(MultiXactMemberCtl, slotno);
		Assert(!MultiXactMemberCtl->shared->page_dirty[slotno]);

		LWLockRelease(MultiXactMemberControlLock);
	}
	else if (info == XLOG_MULTIXACT_CREATE_ID)
	{
		xl_multixact_create *xlrec = (xl_multixact_create *) XLogRecGetData(record);
		MultiXactMember *members = xlrec->members;
		TransactionId max_xid;
		int			i;

		/* Store the data back into the SLRU files */
		RecordNewMultiXact(xlrec->mid, xlrec->moff, xlrec->nmembers, members);

		/*
		 * Make sure nextMXact/nextOffset are beyond what this record has.
		 * We cannot compute a truncateXid from this.
		 */
		MultiXactAdvanceNextMXact(xlrec->mid + 1, xlrec->moff + xlrec->nmembers);

		/*
		 * Make sure nextXid is beyond any XID mentioned in the record. This
		 * should be unnecessary, since any XID found here ought to have other
		 * evidence in the XLOG, but let's be safe.
		 */
		max_xid = record->xl_xid;
		for (i = 0; i < xlrec->nmembers; i++)
		{
			if (TransactionIdPrecedes(max_xid, members[i].xid))
				max_xid = members[i].xid;
		}

		/*
		 * We don't expect anyone else to modify nextXid, hence startup
		 * process doesn't need to hold a lock while checking this. We still
		 * acquire the lock to modify it, though.
		 */
		if (TransactionIdFollowsOrEquals(max_xid,
										 ShmemVariableCache->nextXid))
		{
			LWLockAcquire(XidGenLock, LW_EXCLUSIVE);
			ShmemVariableCache->nextXid = max_xid;
			TransactionIdAdvance(ShmemVariableCache->nextXid);
			LWLockRelease(XidGenLock);
		}
	}
	else
		elog(PANIC, "multixact_redo: unknown op code %u", info);
}

void
multixact_desc(StringInfo buf, uint8 xl_info, char *rec)
{
	uint8		info = xl_info & ~XLR_INFO_MASK;

	if (info == XLOG_MULTIXACT_ZERO_OFF_PAGE)
	{
		MxactZeroOffPg zerooff;

		memcpy(&zerooff, XLogRecGetData(rec), sizeof(MxactZeroOffPg));
		appendStringInfo(buf, "zero offsets page: %d truncate: %u/%u",
						 zerooff.pageno,
						 zerooff.truncateXidEpoch,
						 zerooff.truncateXid);
	}
	else if (info == XLOG_MULTIXACT_ZERO_MEM_PAGE)
	{
		int			pageno;

		memcpy(&pageno, rec, sizeof(int));
		appendStringInfo(buf, "zero members page: %d", pageno);
	}
	else if (info == XLOG_MULTIXACT_CREATE_ID)
	{
		xl_multixact_create *xlrec = (xl_multixact_create *) rec;
		int			i;

		/* XXX describe status too? */
		appendStringInfo(buf, "create multixact %u offset %u:",
						 xlrec->mid, xlrec->moff);
		for (i = 0; i < xlrec->nmembers; i++)
			appendStringInfo(buf, " %u", xlrec->members[i].xid);
	}
	else
		appendStringInfo(buf, "UNKNOWN");
}
