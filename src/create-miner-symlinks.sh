#!/bin/sh
# Post-install script for install stuff that Meson doesn't support directly.
#
# We can't pass the necessary variables directly to the script, so we
# substitute them using configure_file(). It's a bit of a Heath Robinson hack.

set -e

dbus_services_dir="$1"
tracker_miner_services_dir="$2"

mkdir -p ${DESTDIR}/${tracker_miner_services_dir}
ln -sf "${dbus_services_dir}/org.freedesktop.Tracker1.Miner.Applications.service" "${DESTDIR}/${tracker_miner_services_dir}/"
ln -sf "${dbus_services_dir}/org.freedesktop.Tracker1.Miner.Extract.service" "${DESTDIR}/${tracker_miner_services_dir}/"
ln -sf "${dbus_services_dir}/org.freedesktop.Tracker1.Miner.Files.service" "${DESTDIR}/${tracker_miner_services_dir}/"
ln -sf "${dbus_services_dir}/org.freedesktop.Tracker1.Miner.RSS.service" "${DESTDIR}/${tracker_miner_services_dir}/"
