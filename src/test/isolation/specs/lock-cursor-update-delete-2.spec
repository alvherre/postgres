setup
{
    CREATE TABLE A (
        AID integer not null,
        Col1 integer,
        PRIMARY KEY (AID)
    );

    INSERT INTO A (AID,Col1) VALUES (1,0);
}

teardown
{
    DROP TABLE A;
}

session "s1"
setup           { BEGIN; }
step "s1e"      { DECLARE foo CURSOR FOR SELECT * FROM A FOR KEY SHARE; }
step "s1f"      { FETCH NEXT FROM foo; }
step "s1c"      { COMMIT; }

session "s2"
setup           { BEGIN; }
step "s2s"      { SELECT * FROM A WHERE AID = 1 FOR UPDATE; }
step "s2u"      { UPDATE A SET Col1 = -1 WHERE AID = 1; }
step "s2c1"     { COMMIT; BEGIN; }
step "s2d"      { DELETE FROM A WHERE AID = 1; }
step "s2c2"     { COMMIT; }

permutation "s1e" "s1f" "s2u" "s2c1" "s2d" "s1c" "s2c2"

#
# Blocks on DELETE in subsequent transaction of session 2.  Expected,
# but shouldn't happen with the current code.
#
# Notice the SELECT FOR UPDATE step is omitted.
#

# starting permutation: s1e s1f s2u s2c1 s2d s1c s2c2
# step s1e: DECLARE foo CURSOR FOR SELECT * FROM A FOR KEY SHARE;
# step s1f: FETCH NEXT FROM foo;
# aid            col1           

# 1              0              
# step s2u: UPDATE A SET Col1 = -1 WHERE AID = 1;
# step s2c1: COMMIT; BEGIN;
# step s2d: DELETE FROM A WHERE AID = 1; <waiting ...>
# step s1c: COMMIT;
# step s2d: <... completed>
# step s2c2: COMMIT;

permutation "s1e" "s2s" "s1f" "s2u" "s2c1" "s2d" "s1c" "s2c2"

#
# Doesn't block on DELETE.  Wonder why is this any different from when
# SELECT FOR UPDATE is omitted.
#

# starting permutation: s1e s2s s1f s2u s2c1 s2d s1c s2c2
# step s1e: DECLARE foo CURSOR FOR SELECT * FROM A FOR KEY SHARE;
# step s2s: SELECT * FROM A WHERE AID = 1 FOR UPDATE;
# aid            col1           

# 1              0              
# step s1f: FETCH NEXT FROM foo;
# aid            col1           

# 1              0              
# step s2u: UPDATE A SET Col1 = -1 WHERE AID = 1;
# step s2c1: COMMIT; BEGIN;
# step s2d: DELETE FROM A WHERE AID = 1;
# step s1c: COMMIT;
# step s2c2: COMMIT;
