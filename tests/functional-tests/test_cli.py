# Copyright (C) 2020, Sam Thursfield <sam@afuera.me.uk>
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

"""
Test `tracker` commandline tool
"""

from typing import *
import dataclasses
import pathlib
import os
import re

import configuration
import fixtures
import shutil


class TestCli(fixtures.TrackerCommandLineTestCase):
    def test_search(self):
        datadir = pathlib.Path(__file__).parent.joinpath("data/content")

        # FIXME: synchronous `tracker index` isn't ready yet;
        # see https://gitlab.gnome.org/GNOME/tracker/-/issues/188
        # in the meantime we manually wait for it to finish.

        file1 = datadir.joinpath("text/Document 1.txt")
        target1 = pathlib.Path(os.path.join(self.indexed_dir, os.path.basename(file1)))
        with self.await_document_inserted(target1):
            shutil.copy(file1, self.indexed_dir)

        file2 = datadir.joinpath("text/Document 2.txt")
        target2 = pathlib.Path(os.path.join(self.indexed_dir, os.path.basename(file2)))
        with self.await_document_inserted(target2):
            shutil.copy(file2, self.indexed_dir)

        folder_name = "test-folder"
        folder_path = pathlib.Path(os.path.join(self.indexed_dir, folder_name))
        with self.await_insert_dir(folder_path):
            try:
                os.mkdir(os.path.join(self.indexed_dir, folder_name))
            except OSError as error:
                print(error)

        # FIXME: the --all should NOT be needed.
        # See: https://gitlab.gnome.org/GNOME/tracker-miners/-/issues/116
        output = self.run_cli(["tracker3", "search", "--all", "banana"])
        self.assertIn(target1.as_uri(), output)
        self.assertNotIn(target2.as_uri(), output)

        folder_output = self.run_cli(
            ["tracker3", "search", "--folders", "test-monitored"]
        )
        self.assertIn(self.indexed_dir, folder_output)
        self.assertNotIn(folder_path.as_uri(), folder_output)

    def test_search_filename(self):
        datadir = pathlib.Path(__file__).parent.joinpath("data/content")

        file1 = datadir.joinpath("text/mango.txt")
        target1 = pathlib.Path(os.path.join(self.indexed_dir, os.path.basename(file1)))
        with self.await_document_inserted(target1):
            shutil.copy(file1, self.indexed_dir)

        target2 = pathlib.Path(os.path.join(self.indexed_dir, "Document 2.txt"))

        search_output = self.run_cli(["tracker3", "search", "mango"])
        self.assertIn(target1.as_uri(), search_output)
        self.assertNotIn(target2.as_uri(), search_output)


@dataclasses.dataclass
class TagInfo:
    name: str
    description: str
    file_count: int
    files: List[str]


class TrackerTagOutputParser:
    """Manual parsing of the human-readable `tracker3 tag --list` output."""

    # Adding a `--output-format=json` option to `tracker3 tag` would allow
    # us to simplify this.

    def _validate_header(self, header_line):
        assert header_line.startswith("Tags")

    def _parse_tag_line(self, tag_line) -> Tuple[str, str]:
        name, description = re.match(r"  (\w+) \(([^)]+)\)", tag_line).groups()
        return (name, description)

    def parse_tag_list(self, output) -> List[TagInfo]:
        lines = output.strip().splitlines()
        self._validate_header(lines.pop(0))

        result = []
        while len(lines) > 0:
            name, description = self._parse_tag_line(lines.pop(0))
            _resource_id = lines.pop(0).strip()
            file_count = int(lines.pop(0).strip().split()[0])
            result.append(TagInfo(name, description, file_count, []))

        return result

    def parse_tag_list_with_files(self, output) -> List[TagInfo]:
        lines = output.strip().splitlines()
        self._validate_header(lines.pop(0))

        result = []
        while len(lines) > 0:
            name, description = self._parse_tag_line(lines.pop(0))
            files = []
            while len(lines) > 0 and lines[0].startswith("    "):
                files.append(lines.pop(0).strip())
            result.append(TagInfo(name, description, len(files), files))

        return result

    def assert_tag_list_empty(self, output):
        lines = output.strip().splitlines()
        assert lines[0].startswith("Tags")
        assert lines[1].find("None") >= 0
        assert len(lines) == 2


class TestCliSearch(fixtures.TrackerCommandLineTestCase):
    def test_cli_tags(self):
        """Basic "smoke test" that we can create and delete tags."""
        datadir = pathlib.Path(__file__).parent.joinpath("data/content")
        file1 = datadir.joinpath("text/Document 1.txt")
        target1 = pathlib.Path(os.path.join(self.indexed_dir, os.path.basename(file1)))
        with self.await_document_inserted(target1):
            shutil.copy(file1, self.indexed_dir)

        parser = TrackerTagOutputParser()

        # We should have no tags yet.
        output = self.run_cli(["tracker3", "tag", "--list"])
        parser.assert_tag_list_empty(output)

        # Create a tag
        output = self.run_cli(
            [
                "tracker3",
                "tag",
                target1,
                "--add=test_tag_1",
                "--description=This is my new favourite tag.",
            ]
        )

        # Assert tag is in the list.
        output = self.run_cli(["tracker3", "tag", "--list"])
        tag_infos = parser.parse_tag_list(output)
        assert len(tag_infos) == 1
        assert tag_infos[0].name == "test_tag_1"
        assert tag_infos[0].description == "This is my new favourite tag."
        assert tag_infos[0].file_count == 1

        output = self.run_cli(["tracker3", "tag", "--list", "--show-files"])
        tag_infos = parser.parse_tag_list_with_files(output)
        assert len(tag_infos) == 1
        assert tag_infos[0].name == "test_tag_1"
        assert tag_infos[0].file_count == 1
        assert tag_infos[0].files[0] == target1.as_uri()

        # Delete the tag from the file
        output = self.run_cli(
            [
                "tracker3",
                "tag",
                target1,
                "--delete=test_tag_1",
            ]
        )

	# The tag should still exist, but be assigned to no files
        output = self.run_cli(["tracker3", "tag", "--list"])
        tag_infos = parser.parse_tag_list(output)
        assert len(tag_infos) == 1
        assert tag_infos[0].name == "test_tag_1"
        assert tag_infos[0].description == "This is my new favourite tag."
        assert tag_infos[0].file_count == 0

        # Delete the tag entirely
        output = self.run_cli(
            [
                "tracker3",
                "tag",
                "--delete=test_tag_1",
            ]
        )

        # We should have no tags again.
        output = self.run_cli(["tracker3", "tag", "--list"])
        parser.assert_tag_list_empty(output)


if __name__ == "__main__":
    fixtures.tracker_test_main()
