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
step "s1s"      { SELECT * FROM A WHERE AID = 1 FOR UPDATE; }
step "s1c"      { COMMIT; }

session "s2"
setup           { BEGIN; }
step "s2u"      { UPDATE A SET AID = 2 WHERE AID = 1; }
step "s2c"      { COMMIT; }
