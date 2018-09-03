#!/usr/bin/python

# Copyright (C) 2016, Sam Thursfield (sam@afuera.me.uk)
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2.1 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, write to the
# Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
# Boston, MA  02110-1301, USA.

"""
Tests failure cases of tracker-extract.
"""

import unittest2 as ut

from gi.repository import GLib

import os
import shutil
import tempfile
import time

import common.utils.configuration as cfg
from common.utils.helpers import log
from common.utils.system import TrackerSystemAbstraction


CORRUPT_FILE = os.path.join(
    os.path.dirname(__file__), 'test-extraction-data', 'audio',
    'audio-corrupt.mp3')

VALID_FILE = os.path.join(
    os.path.dirname(__file__), 'test-extraction-data', 'audio',
    'audio-test-1.mp3')
VALID_FILE_CLASS = 'http://www.tracker-project.org/temp/nmm#MusicPiece'
VALID_FILE_TITLE = 'Simply Juvenile'

TRACKER_EXTRACT_FAILURE_DATA_SOURCE = 'tracker:extractor-failure-data-source'

def ensure_dir_exists(dirname):
    if not os.path.exists(dirname):
        os.makedirs(dirname)


class ExtractorDecoratorTest(ut.TestCase):
    def setUp(self):
        ensure_dir_exists(cfg.TEST_MONITORED_TMP_DIR)

        # It's important that this directory is NOT inside /tmp, because
        # monitoring files in /tmp usually doesn't work.
        self.datadir = tempfile.mkdtemp(dir=cfg.TEST_MONITORED_TMP_DIR)

        config = {
            cfg.DCONF_MINER_SCHEMA: {
                'index-recursive-directories': GLib.Variant.new_strv([]),
                'index-single-directories': GLib.Variant.new_strv([self.datadir]),
                'index-optical-discs': GLib.Variant.new_boolean(False),
                'index-removable-devices': GLib.Variant.new_boolean(False),
            },
            'org.freedesktop.Tracker.Store': {
                'graphupdated-delay': GLib.Variant('i', 100)
            }
        }

        self.system = TrackerSystemAbstraction(config)
        self.system.tracker_miner_fs_testing_start()

    def tearDown(self):
        self.system.tracker_miner_fs_testing_stop()

        shutil.rmtree(self.datadir)

    def test_reextraction(self):
        """Tests whether known files are still re-extracted on user request."""
        miner_fs = self.system.miner_fs
        store = self.system.store

        # Insert a valid file and wait extraction of its metadata.
        file_path = os.path.join(self.datadir, os.path.basename(VALID_FILE))
        shutil.copy(VALID_FILE, file_path)
        file_id, file_urn = store.await_resource_inserted(
            VALID_FILE_CLASS, title=VALID_FILE_TITLE)

        # Remove a key piece of metadata.
        store.update(
            'DELETE { <%s> nie:title ?title }'
            ' WHERE { <%s> nie:title ?title }' % (file_urn, file_urn))
        store.await_property_changed(VALID_FILE_CLASS, file_id, 'nie:title')
        assert not store.ask('ASK { <%s> nie:title ?title }' % file_urn)

        log("Sending re-index request")
        # Request re-indexing (same as `tracker index --file ...`)
        miner_fs.index_file('file://' + os.path.join (self.datadir, file_path))

        # The extractor should reindex the file and re-add the metadata that we
        # deleted, so we should see the nie:title property change.
        store.await_property_changed(VALID_FILE_CLASS, file_id, 'nie:title')

        title_result = store.query('SELECT ?title { <%s> nie:title ?title }' % file_urn)
        assert len(title_result) == 1
        self.assertEqual(title_result[0][0], VALID_FILE_TITLE)


if __name__ == '__main__':
    ut.main()
