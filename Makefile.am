AUTOMAKE_OPTIONS = foreign

SUBDIRS = src

#noinst_HEADERS = src/db2-config.h src/db2.h src/db2-module.h

EXTRA_DIST = COPYING ChangeLog AUTHORS README \
	RELEASE-NOTES \
	test/db-test.q \
	docs/db2-module-doc.html \
	qore-db2-module.spec

ACLOCAL_AMFLAGS=-I m4

libtool: $(LIBTOOL_DEPS)
	$(SHELL) ./config.status --recheck
