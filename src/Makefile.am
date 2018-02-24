SUBDIRS =                                              \
	libtracker-miners-common                       \
	libtracker-extract                             \
	miners

if HAVE_TRACKER_EXTRACT
SUBDIRS += tracker-extract
endif

if HAVE_TRACKER_WRITEBACK
SUBDIRS += tracker-writeback
endif

EXTRA_DIST = \
	meson.build \
	create-miner-symlinks.sh \
	install-generated-header.sh
