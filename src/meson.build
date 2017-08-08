# Shared common code
subdir('libtracker-common')

# Internal data extraction helpers
subdir('libtracker-extract')

# Public data extract tool & modules
subdir('tracker-extract')

# Public miners
subdir('miners')

# Public metadata writeback tool & modiles
subdir('tracker-writeback')

meson.add_install_script('create-miner-symlinks.sh', dbus_services_dir, tracker_miner_services_dir)
