include $(top_srcdir)/Makefile.decl

SUBDIRS =                                              \
	common                                         \
	libtracker-miners-common

if HAVE_TRACKER_EXTRACT
SUBDIRS += libtracker-extract
endif

if HAVE_TRACKER_WRITEBACK
SUBDIRS += tracker-writeback
endif

if DIST_FUNCTIONAL_TESTS
SUBDIRS += functional-tests
endif

EXTRA_DIST += meson.build
