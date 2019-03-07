/*
 * Copyright (C) 2006, Jamie McCracken <jamiemcc@gnome.org>
 * Copyright (C) 2008, Nokia <ivan.frade@nokia.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 */

#include "config.h"

#define _XOPEN_SOURCE
#include <time.h>
#include <stdlib.h>
#include <locale.h>
#include <errno.h>
#include <sys/types.h>
#include <unistd.h>

#include <glib-object.h>
#include <glib-unix.h>
#include <glib/gi18n.h>
#include <glib/gprintf.h>
#include <gio/gio.h>

#ifndef G_OS_WIN32
#include <sys/resource.h>
#endif

#include <libtracker-miners-common/tracker-common.h>

#include "tracker-config.h"
#include "tracker-main.h"
#include "tracker-extract.h"
#include "tracker-extract-controller.h"
#include "tracker-extract-decorator.h"

#ifdef THREAD_ENABLE_TRACE
#warning Main thread traces enabled
#endif /* THREAD_ENABLE_TRACE */

#define ABOUT	  \
	"Tracker " PACKAGE_VERSION "\n"

#define LICENSE	  \
	"This program is free software and comes without any warranty.\n" \
	"It is licensed under version 2 or later of the General Public " \
	"License which can be viewed at:\n" \
	"\n" \
	"  http://www.gnu.org/licenses/gpl.txt\n"

#define DBUS_NAME_SUFFIX "Tracker1.Miner.Extract"
#define DBUS_PATH "/org/freedesktop/Tracker1/Miner/Extract"

static GMainLoop *main_loop;

static gint verbosity = -1;
static gchar *filename;
static gchar *mime_type;
static gchar *force_module;
static gchar *output_format_name;
static gboolean version;
static gchar *domain_ontology_name = NULL;
static guint shutdown_timeout_id = 0;

static TrackerConfig *config;

static GOptionEntry entries[] = {
	{ "verbosity", 'v', 0,
	  G_OPTION_ARG_INT, &verbosity,
	  N_("Logging, 0 = errors only, "
	     "1 = minimal, 2 = detailed and 3 = debug (default = 0)"),
	  NULL },
	{ "file", 'f', 0,
	  G_OPTION_ARG_FILENAME, &filename,
	  N_("File to extract metadata for"),
	  N_("FILE") },
	{ "mime", 't', 0,
	  G_OPTION_ARG_STRING, &mime_type,
	  N_("MIME type for file (if not provided, this will be guessed)"),
	  N_("MIME") },
	{ "force-module", 'm', 0,
	  G_OPTION_ARG_STRING, &force_module,
	  N_("Force a module to be used for extraction (e.g. “foo” for “foo.so”)"),
	  N_("MODULE") },
	{ "output-format", 'o', 0, G_OPTION_ARG_STRING, &output_format_name,
	  N_("Output results format: “sparql”, “turtle” or “json-ld”"),
	  N_("FORMAT") },
	{ "domain-ontology", 'd', 0,
	  G_OPTION_ARG_STRING, &domain_ontology_name,
	  N_("Runs for a specific domain ontology"),
	  NULL },
	{ "version", 'V', 0,
	  G_OPTION_ARG_NONE, &version,
	  N_("Displays version information"),
	  NULL },
	{ NULL }
};

static void
initialize_priority_and_scheduling (TrackerSchedIdle sched_idle,
                                    gboolean         first_time_index)
{
	/* Set CPU priority */
	if (sched_idle == TRACKER_SCHED_IDLE_ALWAYS ||
	    (sched_idle == TRACKER_SCHED_IDLE_FIRST_INDEX && first_time_index)) {
		tracker_sched_idle ();
	}

	/* Set disk IO priority and scheduling */
	tracker_ioprio_init ();

	/* Set process priority:
	 * The nice() function uses attribute "warn_unused_result" and
	 * so complains if we do not check its returned value. But it
	 * seems that since glibc 2.2.4, nice() can return -1 on a
	 * successful call so we have to check value of errno too.
	 * Stupid...
	 */
	g_message ("Setting priority nice level to 19");

	if (nice (19) == -1) {
		const gchar *str = g_strerror (errno);

		g_message ("Couldn't set nice value to 19, %s",
		           str ? str : "no error given");
	}
}

static void
initialize_directories (void)
{
	gchar *user_data_dir;

	/* NOTE: We don't create the database directories here, the
	 * tracker-db-manager does that for us.
	 */

	user_data_dir = g_build_filename (g_get_user_data_dir (),
	                                  "tracker",
	                                  NULL);

	/* g_message ("Checking directory exists:'%s'", user_data_dir); */
	g_mkdir_with_parents (user_data_dir, 00755);

	g_free (user_data_dir);
}

static gboolean
signal_handler (gpointer user_data)
{
	int signo = GPOINTER_TO_INT (user_data);

	static gboolean in_loop = FALSE;

	/* Die if we get re-entrant signals handler calls */
	if (in_loop) {
		_exit (EXIT_FAILURE);
	}

	switch (signo) {
	case SIGTERM:
	case SIGINT:
		in_loop = TRUE;
		g_main_loop_quit (main_loop);

		/* Fall through */
	default:
		if (g_strsignal (signo)) {
			g_print ("\n");
			g_print ("Received signal:%d->'%s'\n",
			         signo,
			         g_strsignal (signo));
		}
		break;
	}

	return G_SOURCE_CONTINUE;
}

static void
initialize_signal_handler (void)
{
#ifndef G_OS_WIN32
	g_unix_signal_add (SIGTERM, signal_handler, GINT_TO_POINTER (SIGTERM));
	g_unix_signal_add (SIGINT, signal_handler, GINT_TO_POINTER (SIGINT));
#endif /* G_OS_WIN32 */
}

static void
log_handler (const gchar    *domain,
             GLogLevelFlags  log_level,
             const gchar    *message,
             gpointer        user_data)
{
	switch (log_level) {
	case G_LOG_LEVEL_WARNING:
	case G_LOG_LEVEL_CRITICAL:
	case G_LOG_LEVEL_ERROR:
	case G_LOG_FLAG_RECURSION:
	case G_LOG_FLAG_FATAL:
		g_fprintf (stderr, "%s\n", message);
		fflush (stderr);
		break;
	case G_LOG_LEVEL_MESSAGE:
	case G_LOG_LEVEL_INFO:
	case G_LOG_LEVEL_DEBUG:
	case G_LOG_LEVEL_MASK:
	default:
		g_fprintf (stdout, "%s\n", message);
		fflush (stdout);
		break;
	}
}

static void
sanity_check_option_values (TrackerConfig *config)
{
	g_message ("General options:");
	g_message ("  Verbosity  ............................  %d",
	           tracker_config_get_verbosity (config));
	g_message ("  Sched Idle  ...........................  %d",
	           tracker_config_get_sched_idle (config));
	g_message ("  Max bytes (per file)  .................  %d",
	           tracker_config_get_max_bytes (config));
}

TrackerConfig *
tracker_main_get_config (void)
{
	return config;
}

static int
run_standalone (TrackerConfig *config)
{
	TrackerExtract *object;
	GFile *file;
	gchar *uri;
	GEnumClass *enum_class;
	GEnumValue *enum_value;
	TrackerSerializationFormat output_format;

	/* Set log handler for library messages */
	g_log_set_default_handler (log_handler, NULL);

	/* Set the default verbosity if unset */
	if (verbosity == -1) {
		verbosity = 3;
	}

	if (!output_format_name) {
		output_format_name = "turtle";
	}

	/* Look up the output format by name */
	enum_class = g_type_class_ref (TRACKER_TYPE_SERIALIZATION_FORMAT);
	enum_value = g_enum_get_value_by_nick (enum_class, output_format_name);
	g_type_class_unref (enum_class);
	if (!enum_value) {
		g_printerr (N_("Unsupported serialization format “%s”\n"), output_format_name);
		return EXIT_FAILURE;
	}
	output_format = enum_value->value;

	tracker_locale_sanity_check ();

	/* This makes sure we don't steal all the system's resources */
	initialize_priority_and_scheduling (tracker_config_get_sched_idle (config), TRUE);

	file = g_file_new_for_commandline_arg (filename);
	uri = g_file_get_uri (file);

	object = tracker_extract_new (TRUE, force_module);

	if (!object) {
		g_object_unref (file);
		g_free (uri);
		return EXIT_FAILURE;
	}

	tracker_extract_get_metadata_by_cmdline (object, uri, mime_type, output_format);

	g_object_unref (object);
	g_object_unref (file);
	g_free (uri);

	return EXIT_SUCCESS;
}

static void
on_domain_vanished (GDBusConnection *connection,
                    const gchar     *name,
                    gpointer         user_data)
{
	GMainLoop *loop = user_data;
	g_main_loop_quit (loop);
}

static void
on_decorator_items_available (TrackerDecorator *decorator)
{
	if (shutdown_timeout_id) {
		g_source_remove (shutdown_timeout_id);
		shutdown_timeout_id = 0;
	}
}

static gboolean
shutdown_timeout_cb (gpointer user_data)
{
	GMainLoop *loop = user_data;

	g_debug ("Shutting down after 10 seconds inactivity");
	g_main_loop_quit (loop);
	shutdown_timeout_id = 0;
	return G_SOURCE_REMOVE;
}

static void
on_decorator_finished (TrackerDecorator *decorator,
                       GMainLoop        *loop)
{
	if (shutdown_timeout_id != 0)
		return;
	shutdown_timeout_id = g_timeout_add_seconds (10, shutdown_timeout_cb,
	                                             main_loop);
}

int
main (int argc, char *argv[])
{
	GOptionContext *context;
	GError *error = NULL;
	TrackerExtract *extract;
	TrackerDecorator *decorator;
	TrackerExtractController *controller;
	gchar *log_filename = NULL;
	GMainLoop *my_main_loop;
	GDBusConnection *connection;
	TrackerMinerProxy *proxy;
	TrackerDomainOntology *domain_ontology;
	gchar *domain_name, *dbus_name;

	bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
	bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
	textdomain (GETTEXT_PACKAGE);

	/* Translators: this message will appear immediately after the  */
	/* usage string - Usage: COMMAND [OPTION]... <THIS_MESSAGE>     */
	context = g_option_context_new (_("— Extract file meta data"));

	g_option_context_add_main_entries (context, entries, NULL);
	g_option_context_parse (context, &argc, &argv, &error);

	if (error) {
		g_printerr ("%s\n", error->message);
		g_error_free (error);
		return EXIT_FAILURE;
	}

	if (!filename && mime_type) {
		gchar *help;

		g_printerr ("%s\n\n",
		            _("Filename and mime type must be provided together"));

		help = g_option_context_get_help (context, TRUE, NULL);
		g_option_context_free (context);
		g_printerr ("%s", help);
		g_free (help);

		return EXIT_FAILURE;
	}

	g_option_context_free (context);

	if (version) {
		g_print ("\n" ABOUT "\n" LICENSE "\n");
		return EXIT_SUCCESS;
	}

	g_set_application_name ("tracker-extract");

	setlocale (LC_ALL, "");

	tracker_sparql_connection_set_domain (domain_ontology_name);

	domain_ontology = tracker_domain_ontology_new (domain_ontology_name, NULL, &error);
	if (error) {
		g_critical ("Could not load domain ontology '%s': %s",
		            domain_ontology_name, error->message);
		g_error_free (error);
		return EXIT_FAILURE;
	}

	connection = g_bus_get_sync (TRACKER_IPC_BUS, NULL, &error);
	if (error) {
		g_critical ("Could not create DBus connection: %s\n",
		            error->message);
		g_error_free (error);
		return EXIT_FAILURE;
	}

	config = tracker_config_new ();

	/* Extractor command line arguments */
	if (verbosity > -1) {
		tracker_config_set_verbosity (config, verbosity);
	}

	tracker_log_init (tracker_config_get_verbosity (config), &log_filename);
	if (log_filename != NULL) {
		g_message ("Using log file:'%s'", log_filename);
		g_free (log_filename);
	}

	sanity_check_option_values (config);

	/* Set conditions when we use stand alone settings */
	if (filename) {
		return run_standalone (config);
	}

	/* Initialize subsystems */
	initialize_directories ();

	/* This makes sure we don't steal all the system's resources */
	initialize_priority_and_scheduling (tracker_config_get_sched_idle (config), TRUE);

	extract = tracker_extract_new (TRUE, force_module);

	if (!extract) {
		g_object_unref (config);
		tracker_log_shutdown ();
		return EXIT_FAILURE;
	}

	tracker_module_manager_load_modules ();

	decorator = tracker_extract_decorator_new (extract, NULL, &error);

	if (error) {
		g_critical ("Could not start decorator: %s\n", error->message);
		g_object_unref (config);
		tracker_log_shutdown ();
		return EXIT_FAILURE;
	}

	proxy = tracker_miner_proxy_new (TRACKER_MINER (decorator), connection, DBUS_PATH, NULL, &error);
	if (error) {
		g_critical ("Could not create miner DBus proxy: %s\n", error->message);
		g_error_free (error);
		g_object_unref (decorator);
		g_object_unref (config);
		tracker_log_shutdown ();
		return EXIT_FAILURE;
	}

#ifdef THREAD_ENABLE_TRACE
	g_debug ("Thread:%p (Main) --- Waiting for extract requests...",
	         g_thread_self ());
#endif /* THREAD_ENABLE_TRACE */

	tracker_locale_sanity_check ();

	controller = tracker_extract_controller_new (decorator, connection);
	tracker_miner_start (TRACKER_MINER (decorator));

	/* Request DBus name */
	dbus_name = tracker_domain_ontology_get_domain (domain_ontology, DBUS_NAME_SUFFIX);

	if (!tracker_dbus_request_name (connection, dbus_name, &error)) {
		g_critical ("Could not request DBus name '%s': %s",
		            dbus_name, error->message);
		g_error_free (error);
		g_free (dbus_name);
		return EXIT_FAILURE;
	}

	g_free (dbus_name);

	/* Main loop */
	main_loop = g_main_loop_new (NULL, FALSE);

	if (domain_ontology && domain_ontology_name) {
		/* If we are running for a specific domain, we tie the lifetime of this
		 * process to the domain. For example, if the domain name is
		 * org.example.MyApp then this tracker-extract process will exit as
		 * soon as org.example.MyApp exits.
		 */
		domain_name = tracker_domain_ontology_get_domain (domain_ontology, NULL);
		g_bus_watch_name_on_connection (connection, domain_name,
		                                G_BUS_NAME_WATCHER_FLAGS_NONE,
		                                NULL, on_domain_vanished,
		                                main_loop, NULL);
		g_free (domain_name);
	}

	g_signal_connect (decorator, "finished",
	                  G_CALLBACK (on_decorator_finished),
	                  main_loop);
	g_signal_connect (decorator, "items-available",
	                  G_CALLBACK (on_decorator_items_available),
	                  main_loop);

	initialize_signal_handler ();

	g_main_loop_run (main_loop);

	my_main_loop = main_loop;
	main_loop = NULL;
	g_main_loop_unref (my_main_loop);

	tracker_miner_stop (TRACKER_MINER (decorator));

	/* Shutdown subsystems */
	g_object_unref (extract);
	g_object_unref (decorator);
	g_object_unref (controller);
	g_object_unref (proxy);
	g_object_unref (connection);
	g_object_unref (domain_ontology);

	tracker_log_shutdown ();

	g_object_unref (config);

	return EXIT_SUCCESS;
}
