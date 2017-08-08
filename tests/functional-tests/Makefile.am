SUBDIRS = \
	common \
	test-extraction-data \
	test-writeback-data \
	test-apps-data \
	unittest2 \
	test-ontologies

configdir = $(datadir)/tracker-tests

config_DATA = \
	trackertest

config_SCRIPTS = \
	__init__.py \
	$(slow_tests) \
	$(standard_tests)

standard_tests = \
	01-insertion.py \
	02-sparql-bugs.py

standard_tests += \
	04-group-concat.py \
	05-coalesce.py \
	06-distance.py \
	07-graph.py \
	08-unique-insertions.py \
	09-concurrent-query.py \
	14-signals.py \
	15-statistics.py \
	16-collation.py \
	17-ontology-changes.py  \
	200-backup-restore.py \
	300-miner-basic-ops.py \
	301-miner-resource-removal.py

standard_tests += \
	400-extractor-metadata.py \
	410-extractor-decorator.py \
	500-writeback.py \
	501-writeback-details.py \
	600-applications-camera.py \
	601-applications-sync.py

slow_tests = \
	12-transactions.py \
	13-threaded-store.py

TEST_RUNNER = $(top_srcdir)/tests/functional-tests/test-runner.sh

functional-test: ${standard_tests}
	for test in ${standard_tests} ; do \
		$(TEST_RUNNER) python $(top_srcdir)/tests/functional-tests/$$test; \
	done

functional-test-slow: ${slow_tests}
	@for test in ${slow_tests} ; do \
		$(TEST_RUNNER) python $(top_srcdir)/tests/functional-tests/$$test; \
	done

EXTRA_DIST = \
	$(config_SCRIPTS) \
	$(config_DATA) \
	test-runner.sh \
	meson.build
