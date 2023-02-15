/*
 * Copyright (C) 2011, Nokia <ivan.frade@nokia.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 */

#include <time.h>
#include <string.h>

#include <glib.h>
#include <glib-object.h>

#include <libtracker-miners-common/tracker-date-time.h>

static void
test_string_to_date_failures_subprocess ()
{
	GError *error = NULL;

	tracker_string_to_date (NULL, NULL, &error);

	if (error) {
		g_warning ("%s", error->message);
		g_error_free (error);
	}
}

static void
test_string_to_date_failures ()
{
	g_test_trap_subprocess ("/libtracker-common/date-time/string_to_date_failures/subprocess", 0, 0);
	g_test_trap_assert_failed ();
	g_test_trap_assert_stderr ("*Empty date string*");
}

static void
test_string_to_date (void)
{
	GDate     *expected;
	GDate     *result;
	time_t     result_time_t;
	const gchar  *input = "2008-06-16T11:10:10+0600";
	gchar  *timezone = g_strdup (g_getenv ("TZ"));
	GError *error = NULL;

	if (! g_setenv ("TZ", "UTC", TRUE)) {
		g_test_message ("unable to set timezone, test results are invalid, skipping\n");
		if (timezone) {
			g_free (timezone);
		}
		return;
	}

	expected = g_date_new_dmy (16, G_DATE_JUNE, 2008);

	result_time_t = tracker_string_to_date (input, NULL, &error);
	g_assert_no_error (error);

	result = g_date_new ();
	g_date_set_time_t (result, result_time_t);

	if (timezone) {
		g_setenv ("TZ", timezone, TRUE);
		g_free (timezone);
	} else {
		g_unsetenv ("TZ");
	}

	g_assert_cmpint (g_date_get_year (expected), ==, g_date_get_year (result));
	g_assert_cmpint (g_date_get_day (expected), ==, g_date_get_day (result));
	g_assert_cmpint (g_date_get_month (expected), ==, g_date_get_month (result));

	result_time_t = tracker_string_to_date ("", NULL, &error);
	g_assert_cmpint (result_time_t, ==, -1);
	g_assert_error (error, TRACKER_DATE_ERROR, TRACKER_DATE_ERROR_INVALID_ISO8601);
	g_error_free (error);
	error = NULL;

	result_time_t = tracker_string_to_date ("i am not a date", NULL, &error);
	g_assert_cmpint (result_time_t, ==, -1);
	g_assert_error (error, TRACKER_DATE_ERROR, TRACKER_DATE_ERROR_INVALID_ISO8601);
	g_error_free (error);
	error = NULL;

	g_date_free (result);
	g_date_free (expected);
}

static void
test_date_to_string (void)
{
	struct tm *original;
	time_t     input;
	gchar     *result;

	original = g_new0 (struct tm, 1);
	original->tm_sec = 10;
	original->tm_min = 53;
	original->tm_hour = 23;
	original->tm_mday = 16;
	original->tm_mon = 5;
	original->tm_year = 108;
	original->tm_isdst = 0;

#if !(defined(__FreeBSD__) || defined(__OpenBSD__))
	input = mktime (original) - timezone;
#else
	input = timegm (original);
#endif

	result = tracker_date_to_string (input);

	g_assert_true (result != NULL && strncmp (result, "2008-06-16T23:53:10Z", 19) == 0);

	g_free (result);
	g_free (original);
}

gint
main (gint    argc,
      gchar **argv)
{
	g_test_init (&argc, &argv, NULL);

	g_test_add_func ("/libtracker-common/date-time/date_to_string",
			 test_date_to_string);
	g_test_add_func ("/libtracker-common/date-time/string_to_date",
			 test_string_to_date);
	g_test_add_func ("/libtracker-common/date-time/string_to_date_failures",
			 test_string_to_date_failures);
	g_test_add_func ("/libtracker-common/date-time/string_to_date_failures/subprocess",
			 test_string_to_date_failures_subprocess);

	return g_test_run ();
}
