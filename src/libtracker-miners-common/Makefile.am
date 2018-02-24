AM_CPPFLAGS = \
	$(BUILD_CFLAGS) \
	-I$(top_srcdir)/src \
	-DSHAREDIR=\""$(datadir)"\" \
	$(LIBTRACKER_COMMON_CFLAGS)

libtracker_miners_commondir = $(libdir)/tracker-$(TRACKER_API_VERSION)
noinst_LTLIBRARIES = libtracker-miners-common.la

libtracker_miners_common_la_SOURCES = \
	$(BUILT_SOURCES) \
	tracker-date-time.c \
	tracker-dbus.c \
	tracker-domain-ontology.c \
	tracker-file-utils.c \
	tracker-ioprio.c \
	tracker-log.c \
	tracker-sched.c \
	tracker-seccomp.c \
	tracker-type-utils.c \
	tracker-utils.c \
	tracker-locale.c

noinst_HEADERS = \
	tracker-dbus.h \
	tracker-enums.h \
	tracker-ioprio.h \
	tracker-log.h \
	tracker-common.h \
	tracker-date-time.h \
	tracker-domain-ontology.h \
	tracker-file-utils.h \
	tracker-sched.h \
	tracker-seccomp.h \
	tracker-type-utils.h \
	tracker-utils.h \
	tracker-locale.h

libtracker_miners_common_la_LIBADD = \
	$(BUILD_LIBS) \
	$(LIBTRACKER_COMMON_LIBS) \
	-lm

tracker-miners-enum-types.h: tracker-miners-enum-types.h.template tracker-enums.h
	$(AM_V_GEN) (cd $(srcdir) && $(GLIB_MKENUMS) --template tracker-miners-enum-types.h.template $(noinst_HEADERS)) > $@

tracker-miners-enum-types.c: tracker-miners-enum-types.c.template tracker-enums.h
	$(AM_V_GEN) (cd $(srcdir) && $(GLIB_MKENUMS) --template tracker-miners-enum-types.c.template $(noinst_HEADERS)) > $@


BUILT_SOURCES =	                                       \
	tracker-miners-enum-types.c                           \
	tracker-miners-enum-types.h

CLEANFILES =                                           \
	tracker-miners-enum-types.c                           \
	tracker-miners-enum-types.h

EXTRA_DIST =                                           \
	libtracker-miners-common.vapi                  \
	tracker-miners-enum-types.c.template                  \
	tracker-miners-enum-types.h.template                  \
	meson.build
