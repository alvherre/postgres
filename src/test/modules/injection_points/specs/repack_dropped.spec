setup {
	CREATE EXTENSION injection_points;

	CREATE TABLE repack_dropped (id int PRIMARY KEY, a text, b text);
	ALTER TABLE repack_dropped ALTER COLUMN b SET STORAGE EXTERNAL;
	INSERT INTO repack_dropped (id, a, b) VALUES (1, 'one',
		repeat(encode(sha256('1'), 'hex'), current_setting('block_size')::int / 32));
	CREATE FUNCTION repack_dropped_f() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN return OLD; END $$;
	CREATE TRIGGER repack_dropped_t BEFORE UPDATE ON repack_dropped FOR EACH ROW EXECUTE FUNCTION repack_dropped_f();
	ALTER TABLE repack_dropped DROP COLUMN b;
}

teardown {
	DROP TABLE repack_dropped;
	DROP FUNCTION repack_dropped_f;
}

session s1

step s1_size
{
	SELECT pg_column_size(repack_dropped) < current_setting('block_size')::int
		FROM repack_dropped;
}

step s1_unlock
{
	SELECT injection_points_wakeup('repack-concurrently-before-lock');
}

session s2
setup
{
	SELECT injection_points_set_local();
	SELECT injection_points_attach('repack-concurrently-before-lock', 'wait');
}

step s2_repack
{
	REPACK (CONCURRENTLY) repack_dropped;
}

session s3
step s3_updates
{
	UPDATE repack_dropped SET a = a || a;
}

permutation
	s1_size
	s2_repack
	s3_updates
	s1_unlock
	s1_size
