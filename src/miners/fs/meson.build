files_extract = files('tracker-miner-files-methods.c')

sources = [
    'tracker-config.c',
    'tracker-extract-watchdog.c',
    'tracker-files-interface.c',
    'tracker-main.c',
    'tracker-miner-files.c',
    'tracker-storage.c',
    files_extract,
]

tracker_miner_fs_deps = [
     tracker_miner,
     tracker_miners_common_dep,
     tracker_extract_dep
]

if battery_detection_library_name == 'upower'
  sources += 'tracker-power-upower.c'
  tracker_miner_fs_deps += upower
elif battery_detection_library_name == 'hal'
  sources += 'tracker-power-hal.c'
  tracker_miner_fs_deps += hal
endif

executable('tracker-miner-fs-@0@'.format(tracker_api_major),
    sources,
    dependencies: tracker_miner_fs_deps,
    c_args: tracker_c_args,
    install: true,
    install_dir: get_option('libexecdir'),
    install_rpath: tracker_internal_libs_dir,
)

# This file allows starting the service with D-Bus autoactivation.
# It's also used by tracker-control to identify the miner.
tmp_conf = configuration_data()
tmp_conf.merge_from(conf)
if get_option('domain_prefix') == 'org.freedesktop'
  tmp_conf.set ('SYSTEMD_SERVICE', 'SystemdService=tracker-miner-fs-3.service')
else
  tmp_conf.set ('SYSTEMD_SERVICE', '')
endif
dbus_service_file = configure_file(
    input: 'org.freedesktop.Tracker3.Miner.Files.service.in',
    output: get_option('domain_prefix') + '.Tracker3.Miner.Files.service',
    configuration: tmp_conf,
    install: true,
    install_dir: dbus_services_dir,
)

# This file allows starting the service automatically using XDG autostart.
# systemd user service activation is preferred if available.
desktop_file = configure_file(
    input: 'tracker-miner-fs.desktop.in',
    output: 'tracker-miner-fs-@0@.desktop'.format(tracker_api_major),
    configuration: conf,
    install: true,
    install_dir: xdg_autostart_dir)

if get_option('systemd_user_services')
  # This file allows starting the service as a systemd user service.
  configure_file(
      input: 'tracker-miner-fs.service.in',
      output: 'tracker-miner-fs-@0@.service'.format(tracker_api_major),
      configuration: conf,
      install: true,
      install_dir: systemd_user_services_dir)
endif
