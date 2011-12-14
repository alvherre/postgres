setup
{
    CREATE TABLE A (
        AID integer not null,
        Col1 integer,
        PRIMARY KEY (AID)
    );

    CREATE TABLE B (
	BID integer not null,
	AID integer not null,
	Col2 integer,
	PRIMARY KEY (BID),
	FOREIGN KEY (AID) REFERENCES A(AID)
    );

    INSERT INTO A (AID,Col1) VALUES (1,0);
}

teardown
{
    DROP TABLE B;
    DROP TABLE A;
}

#
# This tests the "FOR KEY SHARE vs. FOR KEY UPDATE" conflict.
#
# To grab FOR KEY UPDATE we must either DELETE the tuple or UPDATE the
# referenced FK, but either of these would fail due to FK constraint,
# since there's still a tuple in B which references tuple in A being
# DELETE-d or UPDATE-d.
#
# So we start with no referencing tuples in B and try to DELETE A's
# tuple, while INSERT-ing into B in the other transaction.
#
session "s1"
setup           { BEGIN; }
step "s1d"      { DELETE FROM A WHERE AID = 1; }
step "s1c"      { COMMIT; }

session "s2"
setup           { BEGIN; }
step "s2i"      { INSERT INTO B (BID,AID,Col2) VALUES (2,1,0); }
step "s2c"      { COMMIT; }
