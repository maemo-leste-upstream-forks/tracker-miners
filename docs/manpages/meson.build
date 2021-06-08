daemon_manpages = [
    ['miner-fs', 'tracker-miner-fs-3', 1],
    ['writeback', 'tracker-writeback-3', 1],
]

if have_tracker_miner_rss
  daemon_manpages += [['miner-rss', 'tracker-miner-rss-3', 1]]
endif

cli_manpages = [
  ['daemon', 'tracker3-daemon', 1],
  ['extract', 'tracker3-extract', 1],
  ['index', 'tracker3-index', 1],
  ['info', 'tracker3-info', 1],
  ['reset', 'tracker3-reset', 1],
  ['search', 'tracker3-search', 1],
  ['status', 'tracker3-status', 1],
  ['tag', 'tracker3-tag', 1],
]

foreach m : daemon_manpages + cli_manpages
  src = m[0]
  dst = m[1]
  section = m[2]

  manpage_src = 'tracker-@0@.@1@.txt'.format(src, section)
  manpage_xml = '@0@.@1@.xml'.format(dst, section)
  manpage = '@0@.@1@'.format(dst, section)

  xml = custom_target(manpage_xml,
    command: [asciidoc,
              '--attribute=author=The Tracker developers',
              '--attribute=manversion=@0@'.format(meson.project_version()),
              '--attribute=manmanual=Tracker manual',
              '--backend', 'docbook',
              '--doctype', 'manpage',
              '--out-file', '@OUTPUT@', '@INPUT@'],
    input: manpage_src,
    output: manpage_xml
  )

  custom_target(manpage,
    command: [xsltproc,
              '--output', '@OUTPUT@',
              '--stringparam', 'man.authors.section.enabled', '0',
              '/etc/asciidoc/docbook-xsl/manpage.xsl', '@INPUT@'],
    input: xml,
    output: manpage,
    install: true,
    install_dir: get_option('mandir') / 'man@0@'.format(section),
  )
endforeach
