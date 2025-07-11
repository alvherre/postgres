# Copyright (c) 2022-2025, PostgreSQL Global Development Group

backend_sources += files(
  'acl.c',
  'amutils.c',
  'array_expanded.c',
  'array_selfuncs.c',
  'array_typanalyze.c',
  'array_userfuncs.c',
  'arrayfuncs.c',
  'arraysubs.c',
  'arrayutils.c',
  'ascii.c',
  'bool.c',
  'bytea.c',
  'cash.c',
  'char.c',
  'cryptohashfuncs.c',
  'date.c',
  'datetime.c',
  'datum.c',
  'dbsize.c',
  'domains.c',
  'encode.c',
  'enum.c',
  'expandeddatum.c',
  'expandedrecord.c',
  'float.c',
  'format_type.c',
  'formatting.c',
  'genfile.c',
  'geo_ops.c',
  'geo_selfuncs.c',
  'geo_spgist.c',
  'hbafuncs.c',
  'inet_cidr_ntop.c',
  'inet_net_pton.c',
  'int.c',
  'int8.c',
  'json.c',
  'jsonb.c',
  'jsonb_gin.c',
  'jsonb_op.c',
  'jsonb_util.c',
  'jsonbsubs.c',
  'jsonfuncs.c',
  'jsonpath.c',
  'jsonpath_exec.c',
  'like.c',
  'like_support.c',
  'lockfuncs.c',
  'mac.c',
  'mac8.c',
  'mcxtfuncs.c',
  'misc.c',
  'multirangetypes.c',
  'multirangetypes_selfuncs.c',
  'name.c',
  'network.c',
  'network_gist.c',
  'network_selfuncs.c',
  'network_spgist.c',
  'numeric.c',
  'numutils.c',
  'oid.c',
  'oracle_compat.c',
  'orderedsetaggs.c',
  'partitionfuncs.c',
  'pg_locale.c',
  'pg_locale_builtin.c',
  'pg_locale_icu.c',
  'pg_locale_libc.c',
  'pg_lsn.c',
  'pg_upgrade_support.c',
  'pgstatfuncs.c',
  'pseudorandomfuncs.c',
  'pseudotypes.c',
  'quote.c',
  'rangetypes.c',
  'rangetypes_gist.c',
  'rangetypes_selfuncs.c',
  'rangetypes_spgist.c',
  'rangetypes_typanalyze.c',
  'regexp.c',
  'regproc.c',
  'ri_triggers.c',
  'rowtypes.c',
  'ruleutils.c',
  'selfuncs.c',
  'skipsupport.c',
  'tid.c',
  'timestamp.c',
  'trigfuncs.c',
  'tsginidx.c',
  'tsgistidx.c',
  'tsquery.c',
  'tsquery_cleanup.c',
  'tsquery_gist.c',
  'tsquery_op.c',
  'tsquery_rewrite.c',
  'tsquery_util.c',
  'tsrank.c',
  'tsvector.c',
  'tsvector_op.c',
  'tsvector_parser.c',
  'uuid.c',
  'varbit.c',
  'varchar.c',
  'varlena.c',
  'version.c',
  'waitfuncs.c',
  'windowfuncs.c',
  'xid.c',
  'xid8funcs.c',
  'xml.c',
)


jsonpath_scan = custom_target('jsonpath_scan',
  input: 'jsonpath_scan.l',
  output: 'jsonpath_scan.c',
  command: [flex_cmd, '--no-backup', '--', '-CF', '-p', '-p'],
)
generated_sources += jsonpath_scan

jsonpath_gram = custom_target('jsonpath_parse',
  input: 'jsonpath_gram.y',
  kwargs: bison_kw,
)
generated_sources += jsonpath_gram.to_list()

# so we don't need to add . as an include dir for the whole backend
backend_link_with += static_library('jsonpath',
  jsonpath_scan, jsonpath_gram,
  dependencies: [backend_code],
  include_directories: include_directories('.'),
  kwargs: internal_lib_args,
)

#generated_backend_sources += jsonpath_gram.to_list()
