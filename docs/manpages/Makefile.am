tmrss = tracker-miner-rss.1

common = \
	tracker-extract.1 \
	tracker-miner-fs.1 \
	tracker-writeback.1

# Require only common and enabled manpages
man_MANS = $(common)

if HAVE_TRACKER_MINER_RSS
man_MANS += $(tmrss)
endif

# Make sure even disabled manpages get distributed
EXTRA_DIST = \
	$(common) \
	$(tmrss) \
	meson.build
