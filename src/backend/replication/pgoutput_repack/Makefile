#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for src/backend/replication/pgoutput_repack
#
# IDENTIFICATION
#    src/backend/replication/pgoutput_repack
#
#-------------------------------------------------------------------------

subdir = src/backend/replication/pgoutput_repack
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = \
	$(WIN32RES) \
	pgoutput_repack.o
PGFILEDESC = "pgoutput_repack - logical replication output plugin for REPACK command"
NAME = pgoutput_repack

all: all-shared-lib

include $(top_srcdir)/src/Makefile.shlib

install: all installdirs install-lib

installdirs: installdirs-lib

uninstall: uninstall-lib

clean distclean: clean-lib
	rm -f $(OBJS)
