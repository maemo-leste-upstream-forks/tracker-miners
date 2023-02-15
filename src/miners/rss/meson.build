miner_rss_enums = gnome.mkenums_simple('tracker-enum-types',
    sources: 'tracker-enums.h',
)

sources = [
    'tracker-main.c',
    'tracker-miner-online.c',
    'tracker-miner-rss.c',
]

executable('tracker-miner-rss-@0@'.format(tracker_api_major),
    sources,
    miner_rss_enums[0], miner_rss_enums[1],
    dependencies: [libgrss, network_manager, tracker_miners_common_dep, tracker_extract_dep],
    c_args: tracker_c_args,
    install: true,
    install_dir: get_option('libexecdir'),
    install_rpath: tracker_internal_libs_dir,
)

# This file allows starting the service with D-Bus autoactivation.
# It's also used by tracker-control to identify the miner.
dbus_service_file = configure_file(
    input: 'org.freedesktop.Tracker3.Miner.RSS.service.in',
    output: 'org.freedesktop.Tracker3.Miner.RSS.service',
    configuration: conf,
    install: true,
    install_dir: dbus_services_dir,
)

# This file allows starting the service automatically using XDG autostart.
# systemd user service activation is preferred if available.
desktop_file = configure_file(
    input: 'tracker-miner-rss.desktop.in',
    output: 'tracker-miner-rss-@0@.desktop'.format(tracker_api_major),
    configuration: conf,
    install: true,
    install_dir: xdg_autostart_dir)

if get_option('systemd_user_services')
  # This file allows starting the service as a systemd user service.
  configure_file(
      input: 'tracker-miner-rss.service.in',
      output: 'tracker-miner-rss-@0@.service'.format(tracker_api_major),
      configuration: conf,
      install: true,
      install_dir: systemd_user_services_dir)
endif
