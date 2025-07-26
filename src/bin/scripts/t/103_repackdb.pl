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
	[ 'pg_repackdb', 'postgres' ],
	qr/statement: REPACK.*;/,
	'SQL REPACK run');


done_testing();
