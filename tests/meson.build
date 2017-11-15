subdir('common')

subdir('libtracker-common')

if have_tracker_extract
  subdir('libtracker-extract')
endif

# The test case for writeback doesn't seem to work.
#if enable_writeback
#  subdir('tracker-writeback')
#endif

if get_option('functional_tests')
  subdir('functional-tests')
endif
