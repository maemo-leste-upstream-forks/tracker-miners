/*
 * Copyright (C) 2010, Nokia <ivan.frade@nokia.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

#include "config-miners.h"

#include <locale.h>

#include <libtracker-miner/tracker-crawler.h>

typedef struct CrawlerTest CrawlerTest;

struct CrawlerTest {
	GMainLoop *main_loop;
	guint directories_found;
	guint directories_ignored;
	guint files_found;
	guint files_ignored;
	gboolean interrupted;
	gboolean stopped;

	/* signals statistics */
	guint n_check_directory;
	guint n_check_directory_contents;
	guint n_check_file;
};

static void
crawler_get_cb (GObject      *source,
                GAsyncResult *result,
                gpointer      user_data)
{
	CrawlerTest *test = user_data;
	GError *error = NULL;
	guint directories_found, directories_ignored;
	guint files_found, files_ignored;
	GNode *tree;

	if (!tracker_crawler_get_finish (TRACKER_CRAWLER (source),
	                                 result,
	                                 NULL, &tree,
	                                 &directories_found, &directories_ignored,
	                                 &files_found, &files_ignored,
	                                 &error)) {
		if (error && g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
			test->interrupted = TRUE;

		test->stopped = TRUE;
	} else {
		test->directories_found = directories_found;
		test->directories_ignored = directories_ignored;
		test->files_found = files_found;
		test->files_ignored = files_ignored;

		g_assert_cmpint (g_node_n_nodes (tree, G_TRAVERSE_ALL), ==, directories_found + files_found);
	}

	if (test->main_loop)
		g_main_loop_quit (test->main_loop);
}

static gboolean
check_func (TrackerCrawler           *crawler,
            TrackerCrawlerCheckFlags  flags,
            GFile                    *file,
            GFileInfo                *file_info,
            const GList              *children,
            gpointer                  user_data)
{
	CrawlerTest *test = user_data;

	if (flags & TRACKER_CRAWLER_CHECK_FILE)
		test->n_check_file++;
	if (flags & TRACKER_CRAWLER_CHECK_DIRECTORY)
		test->n_check_directory++;
	if (flags & TRACKER_CRAWLER_CHECK_CONTENT)
		test->n_check_directory_contents++;

	return TRUE;
}

static void
test_crawler_crawl (void)
{
	TrackerCrawler *crawler;
	CrawlerTest test = { 0 };
	GFile *file;

	test.main_loop = g_main_loop_new (NULL, FALSE);

	crawler = tracker_crawler_new (NULL);

	file = g_file_new_for_path (TEST_DATA_DIR);

	tracker_crawler_get (crawler, file, TRACKER_DIRECTORY_FLAG_NONE,
	                     NULL, crawler_get_cb, &test);

	g_main_loop_run (test.main_loop);

	g_assert_cmpint (test.interrupted, ==, 0);

	g_main_loop_unref (test.main_loop);
	g_object_unref (crawler);
	g_object_unref (file);
}

static void
test_crawler_crawl_interrupted (void)
{
	TrackerCrawler *crawler;
	CrawlerTest test = { 0 };
	GCancellable *cancellable;
	GFile *file;

	test.main_loop = g_main_loop_new (NULL, FALSE);

	crawler = tracker_crawler_new (NULL);

	file = g_file_new_for_path (TEST_DATA_DIR);
	cancellable = g_cancellable_new ();

	tracker_crawler_get (crawler, file, TRACKER_DIRECTORY_FLAG_NONE,
	                     cancellable, crawler_get_cb, &test);

	g_cancellable_cancel (cancellable);

	g_main_loop_run (test.main_loop);

	g_assert_cmpint (test.interrupted, ==, 1);

	g_object_unref (cancellable);
	g_object_unref (crawler);
	g_object_unref (file);
}

static void
test_crawler_crawl_nonexisting (void)
{
	TrackerCrawler *crawler;
	CrawlerTest test = { 0 };
	GFile *file;

	test.main_loop = g_main_loop_new (NULL, FALSE);

	crawler = tracker_crawler_new (NULL);
	file = g_file_new_for_path (TEST_DATA_DIR "-idontexist");

	tracker_crawler_get (crawler, file, TRACKER_DIRECTORY_FLAG_NONE,
	                     NULL, crawler_get_cb, &test);

	g_main_loop_run (test.main_loop);

	g_assert_cmpint (test.stopped, ==, 1);

	g_object_unref (crawler);
	g_object_unref (file);
}

static void
test_crawler_crawl_non_recursive (void)
{
	TrackerCrawler *crawler;
	CrawlerTest test = { 0 };
	GFile *file;

	test.main_loop = g_main_loop_new (NULL, FALSE);

	crawler = tracker_crawler_new (NULL);

	file = g_file_new_for_path (TEST_DATA_DIR);

	tracker_crawler_get (crawler, file, TRACKER_DIRECTORY_FLAG_NONE,
	                     NULL, crawler_get_cb, &test);

	g_main_loop_run (test.main_loop);

	/* There are 3 directories (including parent) and 1 file in toplevel dir */
	g_assert_cmpint (test.directories_found, ==, 3);
	g_assert_cmpint (test.directories_ignored, ==, 0);
	g_assert_cmpint (test.files_found, ==, 1);
	g_assert_cmpint (test.files_ignored, ==, 0);

	g_main_loop_unref (test.main_loop);
	g_object_unref (crawler);
	g_object_unref (file);
}

static void
test_crawler_crawl_n_signals_non_recursive (void)
{
	TrackerCrawler *crawler;
	CrawlerTest test = { 0 };
	GFile *file;

	setlocale (LC_ALL, "");

	test.main_loop = g_main_loop_new (NULL, FALSE);

	crawler = tracker_crawler_new (NULL);
	tracker_crawler_set_check_func (crawler, check_func, &test, NULL);

	file = g_file_new_for_path (TEST_DATA_DIR);

	tracker_crawler_get (crawler, file, TRACKER_DIRECTORY_FLAG_NONE,
	                     NULL, crawler_get_cb, &test);

	g_main_loop_run (test.main_loop);

	g_assert_cmpint (test.directories_found, ==, test.n_check_directory);
	g_assert_cmpint (1, ==, test.n_check_directory_contents);
	g_assert_cmpint (test.files_found, ==, test.n_check_file);

	g_main_loop_unref (test.main_loop);
	g_object_unref (crawler);
	g_object_unref (file);
}

int
main (int    argc,
      char **argv)
{
	g_test_init (&argc, &argv, NULL);

	g_test_message ("Testing filesystem crawler");

	g_test_add_func ("/libtracker-miner/tracker-crawler/crawl",
	                 test_crawler_crawl);
	g_test_add_func ("/libtracker-miner/tracker-crawler/crawl-interrupted",
	                 test_crawler_crawl_interrupted);
	g_test_add_func ("/libtracker-miner/tracker-crawler/crawl-nonexisting",
	                 test_crawler_crawl_nonexisting);

	g_test_add_func ("/libtracker-miner/tracker-crawler/crawl-non-recursive",
	                 test_crawler_crawl_non_recursive);

	g_test_add_func ("/libtracker-miner/tracker-crawler/crawl-n-signals-non-recursive",
	                 test_crawler_crawl_n_signals_non_recursive);

	return g_test_run ();
}
