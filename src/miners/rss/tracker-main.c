/*
 * Copyright (C) 2009/2010, Roberto Guido <madbob@users.barberaware.org>
 *                          Michele Tameni <michele@amdplanet.it>
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

#include "config-miners.h"

#include <stdlib.h>

#include <locale.h>
#include <glib/gi18n.h>

#include <libtracker-miners-common/tracker-common.h>

#include "tracker-miner-rss.h"

#define DBUS_NAME_SUFFIX "Tracker3.Miner.RSS"
#define DBUS_PATH "/org/freedesktop/Tracker3/Miner/RSS"

static gchar *add_feed;
static gchar *list_feeds;
static gchar *delete_feed;
static gchar *title;
static gchar *domain_ontology_name = NULL;

static GOptionEntry entries[] = {
	{ "add-feed", 'a', 0,
	  G_OPTION_ARG_STRING, &add_feed,
	  /* Translators: this is a "feed" as in RSS */
	  N_("Add feed"),
	  N_("URL") },
	{ "title", 't', 0,
	  G_OPTION_ARG_STRING, &title,
	  N_("Title to use (must be used with --add-feed)"),
	  NULL },
	{ "list-feeds", 'l', 0,
	  G_OPTION_ARG_NONE, &list_feeds,
	  /* Translators: this is a "feed" as in RSS */
	  N_("List feeds"),
	  NULL },
	{ "delete-feed", 'x', 0,
	  G_OPTION_ARG_STRING, &delete_feed,
	  /* Translators: this is a "feed" as in RSS */
	  N_("Delete feed"),
	  N_("URL") },
	{ "domain-ontology", 'd', 0,
	  G_OPTION_ARG_STRING, &domain_ontology_name,
	  N_("Runs for a specific domain ontology"),
	  NULL },
	{ NULL }
};

static void
on_domain_vanished (GDBusConnection *connection,
                    const gchar     *name,
                    gpointer         user_data)
{
	GMainLoop *loop = user_data;
	g_main_loop_quit (loop);
}

TrackerSparqlConnectionFlags
get_fts_connection_flags (void)
{
	TrackerSparqlConnectionFlags flags = 0;
	TrackerFTSConfig *fts_config;

	fts_config = tracker_fts_config_new ();

	if (tracker_fts_config_get_enable_stemmer (fts_config))
		flags |= TRACKER_SPARQL_CONNECTION_FLAGS_FTS_ENABLE_STEMMER;
	if (tracker_fts_config_get_enable_unaccent (fts_config))
		flags |= TRACKER_SPARQL_CONNECTION_FLAGS_FTS_ENABLE_UNACCENT;
	if (tracker_fts_config_get_ignore_numbers (fts_config))
		flags |= TRACKER_SPARQL_CONNECTION_FLAGS_FTS_IGNORE_NUMBERS;

	g_object_unref (fts_config);

	return flags;
}

static gboolean
setup_connection_and_endpoint (TrackerDomainOntology    *domain,
                               GDBusConnection          *connection,
                               TrackerSparqlConnection **sparql_conn,
                               TrackerEndpointDBus     **endpoint,
                               GError                  **error)
{
	GFile *cache, *store, *ontology;

	cache = tracker_domain_ontology_get_cache (domain);
	store = g_file_get_child (cache, "rss");
	ontology = tracker_sparql_get_ontology_nepomuk ();
	*sparql_conn = tracker_sparql_connection_new (get_fts_connection_flags (),
	                                              store,
	                                              ontology,
	                                              NULL,
	                                              error);
	g_object_unref (store);
	g_object_unref (ontology);

	if (!*sparql_conn)
		return FALSE;

	*endpoint = tracker_endpoint_dbus_new (*sparql_conn,
	                                       connection,
	                                       NULL,
	                                       NULL,
	                                       error);
	if (!*endpoint)
		return FALSE;

	return TRUE;
}

int
handle_add_feed_option(GOptionContext *context)
{
	GError *error = NULL;

	if (!add_feed) {
		gchar *help;

		help = g_option_context_get_help (context, TRUE, NULL);
		g_option_context_free (context);
		g_printerr ("%s", help);
		g_free (help);

		return EXIT_FAILURE;
	}

	/* Command line stuff doesn't use logging, so we're using g_print*() */
	TrackerSparqlConnection *connection;
	GString *query;

	g_print ("Adding feed:\n"
	         "  title:'%s'\n"
	         "  url:'%s'\n",
	         title,
	         add_feed);

	connection = tracker_sparql_connection_bus_new ("org.freedesktop.Tracker3.Miner.RSS",
	                                                NULL, NULL, &error);

	if (!connection) {
		g_printerr ("%s: %s\n",
		            _("Could not establish a connection to Tracker"),
		            error ? error->message : _("No error given"));
		g_clear_error (&error);
		return EXIT_FAILURE;
	}

	/* FIXME: Make interval configurable */
	query = g_string_new ("INSERT {"
	                      "  _:FeedSettings a mfo:FeedSettings ;"
	                      "                   mfo:updateInterval 20 ."
	                      "  _:Feed a nie:DataObject, mfo:FeedChannel ;"
	                      "           mfo:feedSettings _:FeedSettings ;");

	if (title)
		g_string_append_printf (query, "nie:title \"%s\";", title);

	g_string_append_printf (query, " nie:url \"%s\" }", add_feed);

	tracker_sparql_connection_update (connection,
	                                  query->str,
	                                  NULL,
	                                  &error);
	g_string_free (query, TRUE);

	if (error) {
		g_printerr ("%s, %s\n",
		            _("Could not add feed"),
		            error->message);
		g_error_free (error);
		g_object_unref (connection);

		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}

int
handle_remove_feed_option(GOptionContext *context)
{
	TrackerSparqlConnection *connection;
	GError *error = NULL;
	gchar* query = g_strdup_printf("DELETE WHERE { "
	                               "    ?feed a mfo:FeedChannel ."
	                               "    ?feed nie:url \"%s\""
	                               "}",
	                               delete_feed);

	connection = tracker_sparql_connection_bus_new ("org.freedesktop.Tracker3.Miner.RSS",
	                                                NULL, NULL, &error);
	if (!connection) {
		g_printerr ("%s: %s\n",
		            _("Could not establish a connection to Tracker"),
		            error ? error->message : _("No error given"));
		g_clear_error (&error);
		return EXIT_FAILURE;
	}

	tracker_sparql_connection_update (connection,
	                                  query,
	                                  NULL,
	                                  &error);
	if (error) {
		g_printerr ("%s, %s\n",
		            _("Could not remove feed"),
		            error->message);
		g_error_free (error);
		g_object_unref (connection);

		return EXIT_FAILURE;
	}

	g_print ("Done\n");
	return EXIT_SUCCESS;
}

int
handle_list_feeds_option(GOptionContext *context)
{
	GError *error = NULL;
	TrackerSparqlConnection *connection;
	const char *query;
	g_autoptr (TrackerSparqlCursor) cursor = NULL;
	g_autoptr (TrackerSparqlStatement) stmt = NULL;

	connection = tracker_sparql_connection_bus_new ("org.freedesktop.Tracker3.Miner.RSS",
	                                                NULL, NULL, &error);
	if (!connection) {
		g_printerr ("%s: %s\n",
		            _("Could not establish a connection to Tracker"),
		            error ? error->message : _("No error given"));
		g_clear_error (&error);
		return EXIT_FAILURE;
	}

	query = "SELECT ?url ?title {"
	        "    ?feed a mfo:FeedChannel . "
	        "    ?feed nie:url ?url "
	        "    OPTIONAL { ?feed nie:title ?title }"
	        "}";

	stmt = tracker_sparql_connection_query_statement (connection,
	                                                  query,
	                                                  NULL,
	                                                  &error);
	if (!stmt) {
		g_printerr ("Couldn't create a prepared statement: '%s'",
		            error->message);
		return EXIT_FAILURE;
	}

	cursor = tracker_sparql_statement_execute (stmt, NULL, &error);
	if (!cursor) {
		g_printerr ("Couldn't execute query: '%s'",
		            error->message);
		return EXIT_FAILURE;
	}

	while (tracker_sparql_cursor_next (cursor, NULL, &error)) {
		if (tracker_sparql_cursor_get_string (cursor, 1, NULL)) {
			g_print ("%s - %s\n",
			         tracker_sparql_cursor_get_string (cursor, 0, NULL),
			         tracker_sparql_cursor_get_string (cursor, 1, NULL));
		}
		else {
			g_print ("%s\n",
			        tracker_sparql_cursor_get_string (cursor, 0, NULL));
		}
	}

	if (error) {
		g_printerr ("%s, %s\n",
		            _("Could not list feeds"),
		            error->message);
		g_error_free (error);
		g_object_unref (connection);
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}

int
handle_default()
{
	GMainLoop *loop;
	TrackerMinerRSS *miner;
	GError *error = NULL;
	GDBusConnection *connection;
	TrackerSparqlConnection *sparql_conn;
	TrackerEndpointDBus *endpoint;
	TrackerMinerProxy *proxy;
	TrackerDomainOntology *domain_ontology;
	gchar *domain_name, *dbus_name;

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

	if (!setup_connection_and_endpoint (domain_ontology,
	                                    connection,
	                                    &sparql_conn,
	                                    &endpoint,
	                                    &error)) {

		g_critical ("Could not create store/endpoint: %s",
		            error->message);
		g_error_free (error);

		return EXIT_FAILURE;
	}

	miner = tracker_miner_rss_new (sparql_conn, &error);
	if (!miner) {
		g_critical ("Could not create new RSS miner: '%s', exiting...\n",
		            error ? error->message : "unknown error");
		return EXIT_FAILURE;
	}

	tracker_miner_start (TRACKER_MINER (miner));
	proxy = tracker_miner_proxy_new (TRACKER_MINER (miner), connection, DBUS_PATH, NULL, &error);
	if (error) {
		g_critical ("Could not create miner DBus proxy: %s\n", error->message);
		g_error_free (error);
		return EXIT_FAILURE;
	}

	dbus_name = tracker_domain_ontology_get_domain (domain_ontology, DBUS_NAME_SUFFIX);

	if (!tracker_dbus_request_name (connection, dbus_name, &error)) {
		g_critical ("Could not request DBus name '%s': %s",
		            dbus_name, error->message);
		g_error_free (error);
		g_free (dbus_name);
		return EXIT_FAILURE;
	}

	g_free (dbus_name);

	loop = g_main_loop_new (NULL, FALSE);

	if (domain_ontology_name) {
		/* If we are running for a specific domain, we tie the lifetime of this
		 * process to the domain. For example, if the domain name is
		 * org.example.MyApp then this tracker-miner-rss process will exit as
		 * soon as org.example.MyApp exits.
		 */
		domain_name = tracker_domain_ontology_get_domain (domain_ontology, NULL);
		g_bus_watch_name_on_connection (connection, domain_name,
		                                G_BUS_NAME_WATCHER_FLAGS_NONE,
		                                NULL, on_domain_vanished,
		                                loop, NULL);
		g_free (domain_name);
	}

	g_main_loop_run (loop);

	g_main_loop_unref (loop);
	tracker_sparql_connection_close (sparql_conn);
	g_object_unref (sparql_conn);
	g_object_unref (endpoint);
	g_object_unref (miner);
	g_object_unref (connection);
	g_object_unref (proxy);
	g_clear_pointer (&domain_ontology, tracker_domain_ontology_unref);

	return EXIT_SUCCESS;
}

void
setup_locale() {
	setlocale (LC_ALL, "");
	bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
	bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
	textdomain (GETTEXT_PACKAGE);
	tzset ();
}

int
main (int argc, char **argv)
{
	GOptionContext *context;

	setup_locale();

	/* Translators: this messagge will apper immediately after the
	 * usage string - Usage: COMMAND <THIS_MESSAGE>
	 */
	context = g_option_context_new (_("— start the feeds indexer"));
	g_option_context_add_main_entries (context, entries, NULL);
	g_option_context_parse (context, &argc, &argv, NULL);

	g_option_context_free (context);

	if (add_feed)
		return handle_add_feed_option(context);

	if (list_feeds)
		return handle_list_feeds_option(context);

	if (delete_feed)
		return handle_remove_feed_option(context);

	return handle_default();
}
