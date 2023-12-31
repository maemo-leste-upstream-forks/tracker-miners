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

"""
Test that resource removal does not leave debris or clobber too much,
especially in the case where nie:InformationElement != nie:DataObject
"""

from common.utils import configuration as cfg
from common.utils.minertest import CommonTrackerMinerTest

from gi.repository import GLib

import os
import unittest as ut


NFO_DOCUMENT = 'http://www.semanticdesktop.org/ontologies/2007/03/22/nfo#Document'
NMM_MUSIC_PIECE = 'http://www.tracker-project.org/temp/nmm#MusicPiece'


class MinerResourceRemovalTest (CommonTrackerMinerTest):

    def prepare_directories(self):
        # Override content from the base class
        pass

    def create_test_content(self, file_urn, title):
        sparql = "INSERT { \
                    _:ie a nmm:MusicPiece ; \
                         nie:title \"%s\" ; \
                         nie:isStoredAs <%s> \
                  } " % (title, file_urn)

        self.tracker.update(sparql)

        return self.tracker.await_resource_inserted(rdf_class=NMM_MUSIC_PIECE,
                                                    title=title)

    def create_test_file(self, file_name):
        file_path = self.path(file_name)

        file = open(file_path, 'w')
        file.write("Test")
        file.close()

        return self.tracker.await_resource_inserted(rdf_class=NFO_DOCUMENT,
                                                    url=self.uri(file_name))

    @ut.skip("https://gitlab.gnome.org/GNOME/tracker-miners/issues/57")
    def test_01_file_deletion(self):
        """
        Ensure every logical resource (nie:InformationElement) contained with
        in a file is deleted when the file is deleted.
        """

        (file_1_id, file_1_urn) = self.create_test_file("test-monitored/test_1.txt")
        (file_2_id, file_2_urn) = self.create_test_file("test-monitored/test_2.txt")
        (ie_1_id, ie_1_urn) = self.create_test_content(file_1_urn, "Test resource 1")
        (ie_2_id, ie_2_urn) = self.create_test_content(file_2_urn, "Test resource 2")

        os.unlink(self.path("test-monitored/test_1.txt"))

        self.tracker.await_resource_deleted(NFO_DOCUMENT, file_1_id)

        self.assertResourceMissing(file_1_urn)
        # Ensure the logical resource is deleted when the relevant file is
        # removed.
        self.assertResourceMissing(ie_1_urn)

        self.assertResourceExists(file_2_urn)
        self.assertResourceExists(ie_2_urn)

    # def test_02_removable_device_data (self):
    #    """
    #    Tracker does periodic cleanups of data on removable volumes that haven't
    #    been seen since 'removable-days-threshold', and will also remove all data
    #    from removable volumes if 'index-removable-devices' is disabled.
    #
    #    FIXME: not yet possible to test this - we need some way of mounting
    #    a fake removable volume: https://bugzilla.gnome.org/show_bug.cgi?id=659739
    #    """

        #dconf = DConfClient ()
        #dconf.write (cfg.DCONF_MINER_SCHEMA, 'index-removable-devices', 'true')

        #self.mount_test_removable_volume ()

        #self.add_test_resource ("urn:test:1", test_volume_urn)
        #self.add_test_resource ("urn:test:2", None)

        # Trigger removal of all resources from removable devices
        #dconf.write (cfg.DCONF_MINER_SCHEMA, 'index-removable-devices', 'false')

        # Check that only the data on the removable volume was deleted
        #self.await_updates (2)


if __name__ == "__main__":
    ut.main(failfast=True)
