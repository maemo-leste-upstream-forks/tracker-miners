# Extract modules are tracked here as arrays with the following elements:
#
#   [ name, source-files, rule-files, dependencies ]
#
# This might be nicer if Meson supported dictionary types.

modules = [
  ['extract-bmp', 'tracker-extract-bmp.c', ['10-bmp.rule'], [tracker_miners_common_dep]],
  ['extract-desktop', 'tracker-extract-desktop.c', ['10-desktop.rule'], [tracker_miners_common_dep]],
  ['extract-dummy', 'tracker-extract-dummy.c', ['10-comics.rule', '10-ebooks.rule'], [tracker_miners_common_dep]]
]

if get_option('abiword')
  modules += [['extract-abw', 'tracker-extract-abw.c', ['10-abw.rule'], [tracker_miners_common_dep]]]
endif

if get_option('dvi')
  modules += [['extract-dvi', 'tracker-extract-dvi.c', ['10-dvi.rule'], [tracker_miners_common_dep]]]
endif

if libgsf.found()
  modules += [['extract-epub', 'tracker-extract-epub.c', ['10-epub.rule'], [libgsf, tracker_miners_common_dep]]]
  modules += [['extract-msoffice', 'tracker-extract-msoffice.c', ['10-msoffice.rule'], [libgsf, tracker_miners_common_dep]]]
  modules += [['extract-msoffice-xml', 'tracker-extract-msoffice-xml.c', ['11-msoffice-xml.rule'], [libgsf, tracker_miners_common_dep]]]
  modules += [['extract-oasis', 'tracker-extract-oasis.c', ['10-oasis.rule'], [libgsf, tracker_miners_common_dep]]]
endif

if flac.found()
  modules += [['extract-flac', 'tracker-extract-flac.c', ['10-flac.rule'], [flac, tracker_miners_common_dep]]]
endif

if gexiv2.found()
  modules += [['extract-raw', 'tracker-extract-raw.c', ['10-raw.rule'], [gexiv2, tracker_miners_common_dep]]]
endif

if libgif.found()
  modules += [['extract-gif', 'tracker-extract-gif.c', ['10-gif.rule'], [libgif, tracker_miners_common_dep]]]
endif

if generic_media_handler_name == 'gstreamer'
  sources = ['tracker-extract-gstreamer.c', 'tracker-cue-sheet.c']
  rules = ['10-svg.rule', '15-gstreamer-guess.rule', '90-gstreamer-audio-generic.rule', '90-gstreamer-video-generic.rule']
  dependencies = [gstreamer, gstreamer_pbutils, libcue, tracker_miners_common_dep]

  if gstreamer_backend_name == 'GUPnP-DLNA'
    rules += '91-gstreamer-generic-dlna.rule'
    dependencies += gupnp_dlna
  elif gstreamer_backend_name == 'discoverer'
    dependencies += gstreamer_pbutils
  endif

  modules += [['extract-gstreamer', sources, rules, dependencies]]
endif

if libxml2.found()
  modules += [['extract-html', 'tracker-extract-html.c', ['10-html.rule'], [libxml2]]]
endif

if libjpeg.found()
  modules += [['extract-jpeg', 'tracker-extract-jpeg.c', ['10-jpeg.rule'], [libjpeg, tracker_miners_common_dep]]]
endif

if generic_media_handler_name == 'libav'
  rules = ['90-libav-audio-generic.rule', '90-libav-video-generic.rule']
  dependencies = [avcodec, avformat, avutil, tracker_miners_common_dep]
  modules += [['extract-libav', 'tracker-extract-libav.c', rules, dependencies]]
endif

if get_option('icon')
  modules += [['extract-icon', 'tracker-extract-icon.c', ['10-ico.rule'], []]]
endif

if libosinfo.found()
  modules += [['extract-iso', 'tracker-extract-iso.c', ['11-iso.rule'], [libosinfo]]]
endif

if get_option('mp3')
  modules += [['extract-mp3', 'tracker-extract-mp3.c', ['10-mp3.rule'], [tracker_miners_common_dep]]]
endif

if poppler.found()
  modules += [['extract-pdf', 'tracker-extract-pdf.c', ['10-pdf.rule'], [poppler, tracker_miners_common_dep]]]
endif

if totem_plparser.found()
  modules += [['extract-playlist', 'tracker-extract-playlist.c', ['15-playlist.rule'], [totem_plparser]]]
endif

if libpng.found()
  modules += [['extract-png', 'tracker-extract-png.c', ['10-png.rule'], [libpng, tracker_miners_common_dep]]]
endif

if get_option('ps')
  modules += [['extract-ps', 'tracker-extract-ps.c', ['10-ps.rule'], [tracker_miners_common_dep]]]
endif

if get_option('text')
  modules += [['extract-text', 'tracker-extract-text.c', ['15-source-code.rule', '90-text-generic.rule'], []]]
endif

if libtiff.found()
  modules += [['extract-tiff', 'tracker-extract-tiff.c', ['10-tiff.rule'], [libtiff, tracker_miners_common_dep]]]
endif

if libvorbis.found()
  modules += [['extract-vorbis', 'tracker-extract-vorbis.c', ['10-vorbis.rule'], [libvorbis, tracker_miners_common_dep]]]
endif

if exempi.found()
  modules += [['extract-xmp', 'tracker-extract-xmp.c', ['10-xmp.rule'], []]]
endif

if libgxps.found()
  modules += [['extract-xps', 'tracker-extract-xps.c', ['10-xps.rule'], [libgxps, tracker_miners_common_dep]]]
endif

enabled_rule_files = []

foreach module : modules
  name = module[0]
  sources = module[1]
  rules = module[2]
  dependencies = module[3]

  shared_module(name, sources,
    c_args: tracker_c_args,
    dependencies: [tracker_extract_dep] + dependencies,
    include_directories: configinc,
    install: true,
    install_dir: tracker_extract_modules_dir)

  install_data(rules, install_dir: tracker_extract_rules_dir)

  enabled_rule_files += rules
endforeach

tracker_extract_priority_dbus = gnome.gdbus_codegen(
  'tracker-extract-priority-dbus',
  'tracker-extract-priority.xml',
  interface_prefix: 'org.freedesktop.Tracker1.Extract.',
  namespace: 'TrackerExtractDBus')

tracker_extract_sources = [
  'tracker-config.c',
  'tracker-extract.c',
  'tracker-extract-controller.c',
  'tracker-extract-decorator.c',
  'tracker-extract-persistence.c',
  'tracker-read.c',
  'tracker-main.c',
  tracker_extract_priority_dbus
]

tracker_extract_dependencies = [
  tracker_miner, tracker_miners_common_dep, tracker_extract_dep
]

if libgsf.found()
  tracker_extract_sources += 'tracker-gsf.c'
  tracker_extract_dependencies += libgsf
endif

executable('tracker-extract',
  tracker_extract_sources,
  # Manually add the root dir to work around https://github.com/mesonbuild/meson/issues/1387
  c_args: tracker_c_args + ['-I' + meson.build_root()],
  dependencies: tracker_extract_dependencies,
  install: true,
  install_dir: join_paths(get_option('prefix'), get_option('libexecdir')),
  install_rpath: tracker_install_rpath)

dbus_service_file_untranslated = configure_file(
  input: 'org.freedesktop.Tracker1.Miner.Extract.service.in.in',
  output: 'org.freedesktop.Tracker1.Miner.Extract.service.in',
  configuration: conf)

dbus_service_file = custom_target('tracker-extract-dbus-service-file',
  input: dbus_service_file_untranslated,
  output: 'org.freedesktop.Tracker1.Miner.Extract.service',
  command: ['intltool-merge', '--quiet', '--desktop-style', '--utf8', join_paths(meson.source_root(), 'po'), '@INPUT@', '@OUTPUT@'],
  install: true,
  install_dir: dbus_services_dir)

desktop_file_untranslated = configure_file(
  input: 'tracker-extract.desktop.in.in',
  output: 'tracker-extract.desktop.in',
  configuration: conf)

desktop_file = custom_target('tracker-extract-desktop-file',
  input: desktop_file_untranslated,
  output: 'tracker-extract.desktop',
  command: ['intltool-merge', '--quiet', '--desktop-style', '--utf8', join_paths(meson.source_root(), 'po'), '@INPUT@', '@OUTPUT@'])

if get_option('autostart')
  install_data(desktop_file,
      install: true,
      install_dir: join_paths(get_option('sysconfdir'), 'xdg/autostart'))
endif

if install_systemd_user_services
  configure_file(
    input: 'tracker-extract.service.in',
    output: 'tracker-extract.service',
    configuration: conf,
    install: true,
    install_dir: systemd_user_services_dir)
endif

# Populate a directory inside the build tree with the extract rules that are
# enabled in this build configuration.
setup_extract_rules = join_paths(meson.current_source_dir(), 'setup-extract-rules.sh')
result = run_command(setup_extract_rules, meson.current_source_dir(), tracker_uninstalled_extract_rules_dir, enabled_rule_files)
if result.returncode() != 0
    output = result.stdout().strip()
    errortxt = result.stderr().strip()
    error('Failed to run @0@.\n@1@\n@2@'.format(setup_extract_rules, errortxt, output))
endif
