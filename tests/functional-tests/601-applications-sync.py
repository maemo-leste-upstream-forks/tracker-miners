#!/usr/bin/env python3
#
# Copyright (C) 2011, Nokia Corporation <ivan.frade@nokia.com>
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
# 02110-1301, USA.
#

"""
Tests trying to simulate the behaviour of applications working with tracker
"""

import sys
import os
import unittest
import time
import random
import string
import datetime
import shutil
import fcntl

from common.utils import configuration as cfg
import unittest as ut
from common.utils.applicationstest import CommonTrackerApplicationTest as CommonTrackerApplicationTest
from common.utils.helpers import log


NMM_MUSICPIECE = 'http://www.tracker-project.org/temp/nmm#MusicPiece'


class TrackerSyncApplicationTests (CommonTrackerApplicationTest):

    def test_01_sync_audio_nb219946(self):
        """
        Sync simulation (after fix for NB#219946):

        1. Create resource in the store for the new file, using blank nodes
        2. Write the file
        3. Wait for miner-fs to index it
        4. Ensure no duplicates are found

        During stage 3 you should see the following error from the FS miner, if
        viewing its logs:

            (tracker-miner-fs:16008): Tracker-CRITICAL **:   (Sparql buffer)
                Error in task 0 of the array-update: UNIQUE constraint failed:
                nie:DataObject.nie:url (strerror of errno ...)

            (tracker-miner-fs:16008): Tracker-CRITICAL **: Could not execute
                sparql: UNIQUE constraint failed: nie:DataObject.nie:url
                (strerror of errno ...)

        This is because the test already inserted the resource in the store.
        """

        self.system.miner_fs.await_wakeup_count(1)

        origin_filepath = os.path.join(self.get_data_dir(), self.get_test_music())
        dest_filepath = os.path.join(self.get_dest_dir(), self.get_test_music())
        dest_fileuri = "file://" + dest_filepath

        log("Synchronizing audio file in '%s'..." % (dest_filepath))

        # Insert new resource in the store
        insert = """
        DELETE { ?file a rdfs:Resource }
        WHERE  { ?file nie:url '%s'}

        INSERT { _:x a                       nie:DataObject,
                                             nmm:MusicPiece,
                                             nfo:Media,
                                             nfo:Audio,
                                             nie:InformationElement ;
                     nie:url                 '%s' ;
                     nmm:musicAlbum          <urn:album:SinCos> ;
                     nfo:duration            '15' ;
                     nmm:performer           <urn:artist:AbBaby> ;
                     nmm:trackNumber         '13' ;
                     nfo:averageAudioBitrate '32000' ;
                     nfo:genre               'Pop' ;
                     nfo:isContentEncrypted  'false' ;
                     nie:title               'Simply Juvenile' ;
                     nie:isStoredAs          _:x
        }

        INSERT { <urn:album:SinCos> a         nmm:MusicAlbum;
                                    nie:title 'SinCos'
        }

        INSERT { <urn:artist:AbBaby> a              nmm:Artist;
                                     nmm:artistName 'AbBaby'
        }
        """ % (dest_fileuri, dest_fileuri)
        self.tracker.update(insert)
        self.assertEqual(self.get_urn_count_by_url(dest_fileuri), 1)

        resource_id = self.tracker.get_resource_id(dest_fileuri)

        miner_wakeup_count = self.system.miner_fs.wakeup_count()

        # Copy the image to the dest path
        self.slowcopy_file(origin_filepath, dest_filepath)
        assert os.path.exists(dest_filepath)

        self.system.miner_fs.await_wakeup_count(miner_wakeup_count + 1)

        self.assertEqual(self.get_urn_count_by_url(dest_fileuri), 1)

        # Clean the new file so the test directory is as before
        log("Remove and wait")
        os.remove(dest_filepath)
        self.tracker.await_resource_deleted(NMM_MUSICPIECE, resource_id)
        self.assertEqual(self.get_urn_count_by_url(dest_fileuri), 0)


if __name__ == "__main__":
    ut.main()
