#!/usr/bin/env python3

# Copyright (C) 2010, Nokia (ivan.frade@nokia.com)
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

#
# TODO:
#     These tests are for files... we need to write them for folders!
#
"""
Monitor a test directory and copy/move/remove/update files and folders there.
Check the basic data of the files is updated accordingly in tracker.
"""
import os
import shutil
import time

import unittest as ut
from common.utils.helpers import log
from common.utils.minertest import CommonTrackerMinerTest

NFO_DOCUMENT = 'http://www.semanticdesktop.org/ontologies/2007/03/22/nfo#Document'


class MinerCrawlTest (CommonTrackerMinerTest):
    """
    Test cases to check if miner is able to monitor files that are created, deleted or moved
    """

    def __get_text_documents(self):
        return self.tracker.query("""
          SELECT ?url WHERE {
              ?u a nfo:TextDocument ;
                 nie:url ?url.
          }
          """)

    def __get_parent_urn(self, filepath):
        result = self.tracker.query("""
          SELECT nfo:belongsToContainer(?u) WHERE {
              ?u a nfo:FileDataObject ;
                 nie:url \"%s\" .
          }
          """ % (self.uri(filepath)))
        self.assertEqual(len(result), 1)
        return result[0][0]

    def __get_file_urn(self, filepath):
        result = self.tracker.query("""
          SELECT ?u WHERE {
              ?u a nfo:FileDataObject ;
                 nie:url \"%s\" .
          }
          """ % (self.uri(filepath)))
        self.assertEqual(len(result), 1)
        return result[0][0]

    """
    Boot the miner with the correct configuration and check everything is fine
    """

    def test_01_initial_crawling(self):
        """
        The precreated files and folders should be there
        """
        result = self.__get_text_documents()
        self.assertEqual(len(result), 3)
        unpacked_result = [r[0] for r in result]
        self.assertIn(self.uri("test-monitored/file1.txt"), unpacked_result)
        self.assertIn(self.uri("test-monitored/dir1/file2.txt"), unpacked_result)
        self.assertIn(self.uri("test-monitored/dir1/dir2/file3.txt"), unpacked_result)

        # We don't check (yet) folders, because Applications module is injecting results


# class copy(TestUpdate):
# FIXME all tests in one class because the miner-fs restarting takes some time (~5 sec)
# Maybe we can move the miner-fs initialization to setUpModule and then move these
# tests to different classes


    def test_02_copy_from_unmonitored_to_monitored(self):
        """
        Copy an file from unmonitored directory to monitored directory
        and verify if data base is updated accordingly
        """
        source = os.path.join(self.workdir, "test-no-monitored", "file0.txt")
        dest = os.path.join(self.workdir, "test-monitored", "file0.txt")
        shutil.copyfile(source, dest)

        dest_id, dest_urn = self.system.store.await_resource_inserted(NFO_DOCUMENT, self.uri(dest))

        # verify if miner indexed this file.
        result = self.__get_text_documents()
        self.assertEqual(len(result), 4)
        unpacked_result = [r[0] for r in result]
        self.assertIn(self.uri("test-monitored/file1.txt"), unpacked_result)
        self.assertIn(self.uri("test-monitored/dir1/file2.txt"), unpacked_result)
        self.assertIn(self.uri("test-monitored/dir1/dir2/file3.txt"), unpacked_result)
        self.assertIn(self.uri("test-monitored/file0.txt"), unpacked_result)

        # Clean the new file so the test directory is as before
        log("Remove and wait")
        os.remove(dest)
        self.system.store.await_resource_deleted(NFO_DOCUMENT, dest_id)

    def test_03_copy_from_monitored_to_unmonitored(self):
        """
        Copy an file from a monitored location to an unmonitored location
        Nothing should change
        """

        # Copy from monitored to unmonitored
        source = os.path.join(self.workdir, "test-monitored", "file1.txt")
        dest = os.path.join(self.workdir, "test-no-monitored", "file1.txt")
        shutil.copyfile(source, dest)

        time.sleep(1)
        # Nothing changed
        result = self.__get_text_documents()
        self.assertEqual(len(result), 3, "Results:" + str(result))
        unpacked_result = [r[0] for r in result]
        self.assertIn(self.uri("test-monitored/file1.txt"), unpacked_result)
        self.assertIn(self.uri("test-monitored/dir1/file2.txt"), unpacked_result)
        self.assertIn(self.uri("test-monitored/dir1/dir2/file3.txt"), unpacked_result)

        # Clean the file
        os.remove(dest)

    def test_04_copy_from_monitored_to_monitored(self):
        """
        Copy a file between monitored directories
        """
        source = os.path.join(self.workdir, "test-monitored", "file1.txt")
        dest = os.path.join(self.workdir, "test-monitored", "dir1", "dir2", "file-test04.txt")
        shutil.copyfile(source, dest)

        dest_id, dest_urn = self.system.store.await_resource_inserted(NFO_DOCUMENT, self.uri(dest))

        result = self.__get_text_documents()
        self.assertEqual(len(result), 4)
        unpacked_result = [r[0] for r in result]
        self.assertIn(self.uri("test-monitored/file1.txt"), unpacked_result)
        self.assertIn(self.uri("test-monitored/dir1/file2.txt"), unpacked_result)
        self.assertIn(self.uri("test-monitored/dir1/dir2/file3.txt"), unpacked_result)
        self.assertIn(self.uri("test-monitored/dir1/dir2/file-test04.txt"), unpacked_result)

        # Clean the file
        os.remove(dest)
        self.system.store.await_resource_deleted(NFO_DOCUMENT, dest_id)
        self.assertEqual(3, self.tracker.count_instances("nfo:TextDocument"))

    @ut.skip("https://gitlab.gnome.org/GNOME/tracker-miners/issues/56")
    def test_05_move_from_unmonitored_to_monitored(self):
        """
        Move a file from unmonitored to monitored directory
        """
        source = os.path.join(self.workdir, "test-no-monitored", "file0.txt")
        dest = os.path.join(self.workdir, "test-monitored", "dir1", "file-test05.txt")
        shutil.move(source, dest)
        dest_id, dest_urn = self.system.store.await_resource_inserted(NFO_DOCUMENT, self.uri(dest))

        result = self.__get_text_documents()
        self.assertEqual(len(result), 4)
        unpacked_result = [r[0] for r in result]
        self.assertIn(self.uri("test-monitored/file1.txt"), unpacked_result)
        self.assertIn(self.uri("test-monitored/dir1/file2.txt"), unpacked_result)
        self.assertIn(self.uri("test-monitored/dir1/dir2/file3.txt"), unpacked_result)
        self.assertIn(self.uri("test-monitored/dir1/file-test05.txt"), unpacked_result)

        # Clean the file
        os.remove(dest)
        self.system.store.await_resource_deleted(NFO_DOCUMENT, dest_id)
        self.assertEqual(3, self.tracker.count_instances("nfo:TextDocument"))

## """ move operation and tracker-miner response test cases """
# class move(TestUpdate):

    def test_06_move_from_monitored_to_unmonitored(self):
        """
        Move a file from monitored to unmonitored directory
        """
        source = self.path("test-monitored/dir1/file2.txt")
        dest = self.path("test-no-monitored/file2.txt")
        source_id = self.system.store.get_resource_id(self.uri(source))
        shutil.move(source, dest)
        self.system.store.await_resource_deleted(NFO_DOCUMENT, source_id)

        result = self.__get_text_documents()
        self.assertEqual(len(result), 2)
        unpacked_result = [r[0] for r in result]
        self.assertIn(self.uri("test-monitored/file1.txt"), unpacked_result)
        self.assertIn(self.uri("test-monitored/dir1/dir2/file3.txt"), unpacked_result)

        # Restore the file
        shutil.move(dest, source)
        self.system.store.await_resource_inserted(NFO_DOCUMENT, self.uri(source))
        self.assertEqual(3, self.tracker.count_instances("nfo:TextDocument"))

    def test_07_move_from_monitored_to_monitored(self):
        """
        Move a file between monitored directories
        """

        source = self.path("test-monitored/dir1/file2.txt")
        dest = self.path("test-monitored/file2.txt")

        resource_id = self.tracker.get_resource_id(url=self.uri(source))

        source_dir_urn = self.__get_file_urn(os.path.dirname(source))
        parent_before = self.__get_parent_urn(source)
        self.assertEqual(source_dir_urn, parent_before)

        shutil.move(source, dest)
        self.tracker.await_property_changed(NFO_DOCUMENT, resource_id, 'nie:url')

        # Checking fix for NB#214413: After a move operation, nfo:belongsToContainer
        # should be changed to the new one
        dest_dir_urn = self.__get_file_urn(os.path.dirname(dest))
        parent_after = self.__get_parent_urn(dest)
        self.assertNotEqual(parent_before, parent_after)
        self.assertEqual(dest_dir_urn, parent_after)

        result = self.__get_text_documents()
        self.assertEqual(len(result), 3)
        unpacked_result = [r[0] for r in result]
        self.assertIn(self.uri("test-monitored/file1.txt"), unpacked_result)
        self.assertIn(self.uri("test-monitored/file2.txt"), unpacked_result)
        self.assertIn(self.uri("test-monitored/dir1/dir2/file3.txt"), unpacked_result)

        # Restore the file
        shutil.move(dest, source)
        self.tracker.await_property_changed(NFO_DOCUMENT, resource_id, 'nie:url')

        result = self.__get_text_documents()
        self.assertEqual(len(result), 3)
        unpacked_result = [r[0] for r in result]
        self.assertIn(self.uri("test-monitored/dir1/file2.txt"), unpacked_result)

    def test_08_deletion_single_file(self):
        """
        Delete one of the files
        """
        victim = self.path("test-monitored/dir1/file2.txt")
        victim_id = self.system.store.get_resource_id(self.uri(victim))
        os.remove(victim)
        self.system.store.await_resource_deleted(NFO_DOCUMENT, victim_id)

        result = self.__get_text_documents()
        self.assertEqual(len(result), 2)
        unpacked_result = [r[0] for r in result]
        self.assertIn(self.uri("test-monitored/file1.txt"), unpacked_result)
        self.assertIn(self.uri("test-monitored/dir1/dir2/file3.txt"), unpacked_result)

        # Restore the file
        f = open(victim, "w")
        f.write("Don't panic, everything is fine")
        f.close()
        self.system.store.await_resource_inserted(NFO_DOCUMENT, self.uri(victim))

    def test_09_deletion_directory(self):
        """
        Delete a directory
        """
        victim = self.path("test-monitored/dir1")
        victim_id = self.system.store.get_resource_id(self.uri(victim))
        shutil.rmtree(victim)

        file_inside_victim_url = self.uri(os.path.join(victim, "file2.txt"))
        file_inside_victim_id = self.system.store.get_resource_id(file_inside_victim_url)
        self.system.store.await_resource_deleted(NFO_DOCUMENT, file_inside_victim_id)

        result = self.__get_text_documents()
        self.assertEqual(len(result), 1)
        unpacked_result = [r[0] for r in result]
        self.assertIn(self.uri("test-monitored/file1.txt"), unpacked_result)

        # Restore the dirs
        os.makedirs(self.path("test-monitored/dir1"))
        os.makedirs(self.path("test-monitored/dir1/dir2"))
        for f in ["test-monitored/dir1/file2.txt",
                  "test-monitored/dir1/dir2/file3.txt"]:
            filename = self.path(f)
            writer = open(filename, "w")
            writer.write("Don't panic, everything is fine")
            writer.close()
            self.system.store.await_resource_inserted(NFO_DOCUMENT, self.uri(f))

        # Check everything is fine
        result = self.__get_text_documents()
        self.assertEqual(len(result), 3)


if __name__ == "__main__":
    ut.main(failfast=True)
