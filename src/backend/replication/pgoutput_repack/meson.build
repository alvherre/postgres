# Copyright (c) 2022-2024, PostgreSQL Global Development Group

pgoutput_repack_sources = files(
  'pgoutput_repack.c',
)

if host_system == 'windows'
  pgoutput_repack_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'pgoutput_repack',
    '--FILEDESC', 'pgoutput_repack - logical replication output plugin for REPACK command',])
endif

pgoutput_repack = shared_module('pgoutput_repack',
  pgoutput_repack_sources,
  kwargs: pg_mod_args,
)

backend_targets += pgoutput_repack
