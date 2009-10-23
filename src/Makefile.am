DB2_SOURCES = db2.cc

lib_LTLIBRARIES = db2.la
db2_la_SOURCES = $(DB2_SOURCES)
db2_la_LDFLAGS = -module -avoid-version ${DB2_LIBS}

AM_CPPFLAGS = ${QORE_CPPFLAGS} ${DB2_CPPFLAGS}

if COND_PROFILE
AM_CXXFLAGS=-g -pg
else
if COND_DEBUG
AM_CXXFLAGS=-g
endif
endif

install-exec-hook:
	mv $(DESTDIR)$(libdir)/db2.$(MODULE_SUFFIX) $(DESTDIR)$(libdir)/db2-api-$(MODULE_API).qmod
	rm -f $(DESTDIR)$(libdir)/db2.la $(libdir)/db2.a
