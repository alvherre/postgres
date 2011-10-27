/*
 * multixact.h
 *
 * PostgreSQL multi-transaction-log manager
 *
 * Portions Copyright (c) 1996-2011, PostgreSQL Global Development Group
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

#define MultiXactIdIsValid(multi) ((multi) != InvalidMultiXactId)

/* Number of SLRU buffers to use for multixact */
#define NUM_MXACTOFFSET_BUFFERS		8
#define NUM_MXACTMEMBER_BUFFERS		16

/*
 * Possible multixact lock modes ("status").  The first three modes are for
 * tuple locks (FOR KEY SHARE, FOR SHARE and FOR UPDATE, respectively); the
 * fourth is used for an update that doesn't modify key columns.  The fifth one
 * is used for other updates and deletes.  Note that we only use two bits to
 * represent them on disk, which means we don't have space to represent the
 * last one.  This is okay, because a multixact can never contain such an
 * operation; this mode is only used to wait for other modes.
 */
typedef enum
{
	MultiXactStatusForKeyShare = 0x00,
	MultiXactStatusForShare = 0x01,
	MultiXactStatusForUpdate = 0x02,
	MultiXactStatusUpdate = 0x03,
	MultiXactStatusKeyUpdate = 0x04,
} MultiXactStatus;

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
extern bool MultiXactIdIsRunning(MultiXactId multi);
extern bool MultiXactIdIsCurrent(MultiXactId multi);
extern void MultiXactIdWait(MultiXactId multi, MultiXactStatus status);
extern bool ConditionalMultiXactIdWait(MultiXactId multi,
						   MultiXactStatus status);
extern void MultiXactIdSetOldestMember(void);
extern int	GetMultiXactIdMembers(MultiXactId multi, MultiXactMember **xids);

extern void AtEOXact_MultiXact(void);
extern void AtPrepare_MultiXact(void);
extern void PostPrepare_MultiXact(TransactionId xid);

extern Size MultiXactShmemSize(void);
extern void MultiXactShmemInit(void);
extern void BootStrapMultiXact(void);
extern void StartupMultiXact(void);
extern void ShutdownMultiXact(void);
extern void MultiXactGetCheckptMulti(bool is_shutdown,
						 MultiXactId *nextMulti,
						 MultiXactOffset *nextMultiOffset);
extern void CheckPointMultiXact(void);
extern void MultiXactSetNextMXact(MultiXactId nextMulti,
					  MultiXactOffset nextMultiOffset);
extern void MultiXactAdvanceNextMXact(MultiXactId minMulti,
						  MultiXactOffset minMultiOffset);

extern void multixact_twophase_recover(TransactionId xid, uint16 info,
						   void *recdata, uint32 len);
extern void multixact_twophase_postcommit(TransactionId xid, uint16 info,
							  void *recdata, uint32 len);
extern void multixact_twophase_postabort(TransactionId xid, uint16 info,
							 void *recdata, uint32 len);

extern void multixact_redo(XLogRecPtr lsn, XLogRecord *record);
extern void multixact_desc(StringInfo buf, uint8 xl_info, char *rec);

#endif   /* MULTIXACT_H */
