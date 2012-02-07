/*
 * multixact.h
 *
 * PostgreSQL multi-transaction-log manager
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/multixact.h
 */
#ifndef MULTIXACT_H
#define MULTIXACT_H

#include "access/xlog.h"


/*
 * The first two MultiXactId values are reserved to store the truncation Xid
 * and epoch of the first segment, so we start assigning multixact values from
 * 2.
 */
#define InvalidMultiXactId	((MultiXactId) 0)
#define FirstMultiXactId	((MultiXactId) 2)
#define MaxMultiXactId		((MultiXactId) 0xFFFFFFFF)

#define MultiXactIdIsValid(multi) ((multi) != InvalidMultiXactId)

/* Number of SLRU buffers to use for multixact */
#define NUM_MXACTOFFSET_BUFFERS		8
#define NUM_MXACTMEMBER_BUFFERS		16

/*
 * Possible multixact lock modes ("status").  The first four modes are for
 * tuple locks (FOR KEY SHARE, FOR SHARE, FOR UPDATE, FOR KEY UPDATE); the
 * next two are used for update and delete modes.
 */
typedef enum
{
	MultiXactStatusForKeyShare = 0x00,
	MultiXactStatusForShare = 0x01,
	MultiXactStatusForUpdate = 0x02,
	MultiXactStatusForKeyUpdate = 0x03,
	/* an update that doesn't touch "key" columns */
	MultiXactStatusUpdate = 0x04,
	/* other updates, and delete */
	MultiXactStatusKeyUpdate = 0x05
} MultiXactStatus;

#define MaxMultiXactStatus MultiXactStatusKeyUpdate


typedef struct MultiXactMember
{
	TransactionId	xid;
	MultiXactStatus	status;
} MultiXactMember;


/* ----------------
 *		multixact-related XLOG entries
 * ----------------
 */

#define XLOG_MULTIXACT_ZERO_OFF_PAGE	0x00
#define XLOG_MULTIXACT_ZERO_MEM_PAGE	0x10
#define XLOG_MULTIXACT_CREATE_ID		0x20

typedef struct xl_multixact_create
{
	MultiXactId mid;			/* new MultiXact's ID */
	MultiXactOffset moff;		/* its starting offset in members file */
	int32		nmembers;		/* number of member XIDs */
	MultiXactMember members[FLEXIBLE_ARRAY_MEMBER];
} xl_multixact_create;

#define MinSizeOfMultiXactCreate offsetof(xl_multixact_create, members)


extern MultiXactId MultiXactIdCreateSingleton(TransactionId xid,
						   MultiXactStatus status);
extern MultiXactId MultiXactIdCreate(TransactionId xid1,
				  MultiXactStatus status1, TransactionId xid2,
				  MultiXactStatus status2);
extern MultiXactId MultiXactIdExpand(MultiXactId multi, TransactionId xid,
				  MultiXactStatus status);
extern MultiXactId ReadNextMultiXactId(void);
extern bool MultiXactIdIsRunning(MultiXactId multi);
extern void MultiXactIdSetOldestMember(void);
extern int	GetMultiXactIdMembers(MultiXactId multi, MultiXactMember **xids);
extern bool MultiXactIdPrecedes(MultiXactId multi1, MultiXactId multi2);

extern void AtEOXact_MultiXact(void);

extern Size MultiXactShmemSize(void);
extern void MultiXactShmemInit(void);
extern void BootStrapMultiXact(void);
extern void StartupMultiXact(void);
extern void ShutdownMultiXact(void);
extern void SetMultiXactIdLimit(MultiXactid oldest_datminmxid,
					Oid oldest_datoid);
extern void MultiXactGetCheckptMulti(bool is_shutdown,
						 MultiXactId *nextMulti,
						 MultiXactOffset *nextMultiOffset,
						 TransactionId *oldestTruncateXid,
						 uint32 *oldestTruncateXidEpoch,
						 MultiXactId *oldestMulti);
extern void CheckPointMultiXact(void);
extern void TruncateMultiXact(TransactionId oldestXid);
extern void MultiXactSetNextMXact(MultiXactId nextMulti,
					  MultiXactOffset nextMultiOffset);
extern void MultiXactAdvanceNextMXact(MultiXactId minMulti,
						  MultiXactOffset minMultiOffset);

extern void multixact_redo(XLogRecPtr lsn, XLogRecord *record);
extern void multixact_desc(StringInfo buf, uint8 xl_info, char *rec);

#endif   /* MULTIXACT_H */
