# Copyright (c) 2021-2025, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

program_help_ok('pg_repackdb');
program_version_ok('pg_repackdb');
program_options_handling_ok('pg_repackdb');

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->start;

$node->issues_sql_like(
	[ 'pg_repackdb', 'postgres', '-t', 'pg_class'],
	qr/statement: REPACK.*pg_class;/,
	'pg_repackdb processes a single table');

$node->safe_psql('postgres', 'CREATE USER testusr;
	GRANT CREATE ON SCHEMA public TO testusr');
$node->safe_psql('postgres',
	'CREATE TABLE cluster_1 (a int primary key);
	ALTER TABLE cluster_1 CLUSTER ON cluster_1_pkey;
	CREATE TABLE cluster_2 (a int unique);
	ALTER TABLE cluster_2 CLUSTER ON cluster_2_a_key;',
	extra_params => ['-U' => 'testusr']);

$node->issues_sql_like(
	[ 'pg_repackdb', 'postgres', '-U', 'testusr' ],
	qr/statement: REPACK.*;/,
	'SQL REPACK run');

$node->issues_sql_like(
	[ 'pg_repackdb', 'postgres', '--index'],
	qr/statement: REPACK.*cluster_1 USING INDEX.*statement: REPACK.*cluster_2 USING INDEX/ms,
	'pg_repackdb --index chooses multiple tables');

$node->issues_sql_like(
	[ 'pg_repackdb', 'postgres', '--analyze', '-t', 'cluster_1'],
	qr/statement: REPACK \(ANALYZE\) public.cluster_1/,
	'pg_repackdb --analyze works');

done_testing();
