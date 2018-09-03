include $(top_srcdir)/Makefile.decl

noinst_PROGRAMS += $(test_programs)

test_programs = \
	tracker-utils-test                             \
	tracker-xmp-test			       \
	tracker-extract-info-test		       \
	tracker-guarantee-test

if HAVE_EXIF
test_programs += tracker-exif-test
endif

if HAVE_IPTC
if HAVE_LIBJPEG
test_programs += tracker-iptc-test
endif
endif

if HAVE_ENCA
test_programs += tracker-encoding
endif

AM_CPPFLAGS =                                          \
	-DTOP_SRCDIR=\"$(abs_top_srcdir)\"             \
	-DTOP_BUILDDIR=\"$(abs_top_builddir)\"         \
	$(BUILD_CFLAGS)                                \
	-I$(top_srcdir)/src                            \
	-I$(top_builddir)/src                          \
	-I$(top_srcdir)/tests/common                   \
	$(LIBTRACKER_EXTRACT_CFLAGS)

LDADD =                                                \
	$(top_builddir)/tests/common/libtracker-testcommon.la \
	$(top_builddir)/src/libtracker-miners-common/libtracker-miners-common.la \
	$(top_builddir)/src/libtracker-extract/libtracker-extract.la \
	$(BUILD_LIBS)                                  \
	$(LIBTRACKER_EXTRACT_LIBS)

tracker_encoding_SOURCES = tracker-encoding-test.c

tracker_utils_test_SOURCES = tracker-utils-test.c

tracker_xmp_test_SOURCES = tracker-xmp-test.c

tracker_extract_info_test_SOURCES = tracker-extract-info-test.c

tracker_exif_test_SOURCES = tracker-exif-test.c

tracker_guarantee_test_SOURCES = tracker-guarantee-test.c

tracker_iptc_test_SOURCES = tracker-iptc-test.c
tracker_iptc_test_LDADD = $(LDADD) $(LIBJPEG_LIBS)
tracker_iptc_test_CFLAGS = $(LIBJPEG_CFLAGS)

EXTRA_DIST += \
	encoding-detect.bin             \
	areas.xmp 			\
	areas-with-contacts.xmp 	\
	areas-ns.xmp 			\
	nb282393.xmp 			\
	nb282393_simple.xmp		\
	exif-img.jpg			\
	exif-free-img.jpg		\
	guarantee-mtime-test.txt	\
	getline-test.txt		\
	iptc-img.jpg			\
	meson.build
