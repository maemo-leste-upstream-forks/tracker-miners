/*
 * Copyright (C) 2008, Nokia <ivan.frade@nokia.com>
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

#include <glib/gstdio.h>

#include <gio/gio.h>

#include <libtracker-miners-common/tracker-common.h>

#include "tracker-miner-files.h"
#include "tracker-miner-files-methods.h"
#include "tracker-config.h"
#include "tracker-storage.h"
#include "tracker-extract-watchdog.h"
#include "tracker-utils.h"

#define DISK_SPACE_CHECK_FREQUENCY 10
#define SECONDS_PER_DAY 86400

/* Stamp files to know crawling/indexing state */
#define FIRST_INDEX_FILENAME          "first-index.txt"
#define LAST_CRAWL_FILENAME           "last-crawl.txt"
#define NEED_MTIME_CHECK_FILENAME     "no-need-mtime-check.txt"

#define DEFAULT_GRAPH "tracker:FileSystem"

#define FILE_ATTRIBUTES	  \
	G_FILE_ATTRIBUTE_UNIX_IS_MOUNTPOINT "," \
	G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN "," \
	G_FILE_ATTRIBUTE_STANDARD_NAME "," \
	G_FILE_ATTRIBUTE_STANDARD_TYPE "," \
	G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME "," \
	G_FILE_ATTRIBUTE_STANDARD_SIZE "," \
	G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN "," \
	G_FILE_ATTRIBUTE_TIME_MODIFIED "," \
	G_FILE_ATTRIBUTE_TIME_CREATED "," \
	G_FILE_ATTRIBUTE_TIME_ACCESS

#define TRACKER_MINER_FILES_GET_PRIVATE(o) (tracker_miner_files_get_instance_private (TRACKER_MINER_FILES (o)))

static GQuark miner_files_error_quark = 0;

struct TrackerMinerFilesPrivate {
	TrackerConfig *config;
	TrackerStorage *storage;

	TrackerExtractWatchdog *extract_watchdog;
	guint grace_period_timeout_id;

	gchar *domain;
	TrackerDomainOntology *domain_ontology;

	guint disk_space_check_id;
	gboolean disk_space_pause;

	gboolean low_battery_pause;

	gboolean start_extractor;

#ifdef HAVE_POWER
	TrackerPower *power;
#endif /* HAVE_POWER) */
	gulong finished_handler;

	gboolean mtime_check;

	guint stale_volumes_check_id;
};

enum {
	PROP_0,
	PROP_CONFIG,
	PROP_DOMAIN,
};

static void        miner_files_set_property             (GObject              *object,
                                                         guint                 param_id,
                                                         const GValue         *value,
                                                         GParamSpec           *pspec);
static void        miner_files_get_property             (GObject              *object,
                                                         guint                 param_id,
                                                         GValue               *value,
                                                         GParamSpec           *pspec);
static void        miner_files_constructed              (GObject              *object);
static void        miner_files_finalize                 (GObject              *object);
static void        miner_files_initable_iface_init      (GInitableIface       *iface);
static gboolean    miner_files_initable_init            (GInitable            *initable,
                                                         GCancellable         *cancellable,
                                                         GError              **error);
#ifdef HAVE_POWER
static void        check_battery_status                 (TrackerMinerFiles    *fs);
static void        battery_status_cb                    (GObject              *object,
                                                         GParamSpec           *pspec,
                                                         gpointer              user_data);
static void        index_on_battery_cb                  (GObject    *object,
                                                         GParamSpec *pspec,
                                                         gpointer    user_data);
#endif /* HAVE_POWER */
static void        init_index_roots                     (TrackerMinerFiles    *miner);
static void        init_stale_volume_removal            (TrackerMinerFiles    *miner);
static void        disk_space_check_start               (TrackerMinerFiles    *mf);
static void        disk_space_check_stop                (TrackerMinerFiles    *mf);
static void        low_disk_space_limit_cb              (GObject              *gobject,
                                                         GParamSpec           *arg1,
                                                         gpointer              user_data);

static void        miner_files_process_file             (TrackerMinerFS       *fs,
                                                         GFile                *file,
                                                         GFileInfo            *info,
                                                         TrackerSparqlBuffer  *buffer,
                                                         gboolean              create);
static void        miner_files_process_file_attributes  (TrackerMinerFS       *fs,
                                                         GFile                *file,
                                                         GFileInfo            *info,
                                                         TrackerSparqlBuffer  *buffer);
static void        miner_files_remove_children          (TrackerMinerFS       *fs,
                                                         GFile                *file,
                                                         TrackerSparqlBuffer  *buffer);
static void        miner_files_remove_file              (TrackerMinerFS       *fs,
                                                         GFile                *file,
                                                         TrackerSparqlBuffer  *buffer,
                                                         gboolean              is_dir);
static void        miner_files_move_file                (TrackerMinerFS       *fs,
                                                         GFile                *file,
                                                         GFile                *source_file,
                                                         TrackerSparqlBuffer  *buffer,
                                                         gboolean              recursive);
static void        miner_files_finished                 (TrackerMinerFS       *fs,
                                                         gdouble               elapsed,
                                                         gint                  directories_found,
                                                         gint                  directories_ignored,
                                                         gint                  files_found,
                                                         gint                  files_ignored);
static void        miner_finished_cb                    (TrackerMinerFS *fs,
                                                         gdouble         seconds_elapsed,
                                                         guint           total_directories_found,
                                                         guint           total_directories_ignored,
                                                         guint           total_files_found,
                                                         guint           total_files_ignored,
                                                         gpointer        user_data);

static void        miner_files_in_removable_media_remove_by_date  (TrackerMinerFiles  *miner,
                                                                   const gchar        *date);

static GInitableIface* miner_files_initable_parent_iface;

G_DEFINE_TYPE_WITH_CODE (TrackerMinerFiles, tracker_miner_files, TRACKER_TYPE_MINER_FS,
                         G_ADD_PRIVATE (TrackerMinerFiles)
                         G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE,
                                                miner_files_initable_iface_init))

static void
miner_files_started (TrackerMiner *miner)
{
	TRACKER_MINER_CLASS (tracker_miner_files_parent_class)->started (miner);
	init_index_roots (TRACKER_MINER_FILES (miner));
}

static void
tracker_miner_files_class_init (TrackerMinerFilesClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	TrackerMinerClass *miner_class = TRACKER_MINER_CLASS (klass);
	TrackerMinerFSClass *miner_fs_class = TRACKER_MINER_FS_CLASS (klass);

	object_class->constructed = miner_files_constructed;
	object_class->finalize = miner_files_finalize;
	object_class->get_property = miner_files_get_property;
	object_class->set_property = miner_files_set_property;

	miner_class->started = miner_files_started;

	miner_fs_class->process_file = miner_files_process_file;
	miner_fs_class->process_file_attributes = miner_files_process_file_attributes;
	miner_fs_class->finished = miner_files_finished;
	miner_fs_class->remove_file = miner_files_remove_file;
	miner_fs_class->remove_children = miner_files_remove_children;
	miner_fs_class->move_file = miner_files_move_file;

	g_object_class_install_property (object_class,
	                                 PROP_CONFIG,
	                                 g_param_spec_object ("config",
	                                                      "Config",
	                                                      "Config",
	                                                      TRACKER_TYPE_CONFIG,
	                                                      G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
	g_object_class_install_property (object_class,
	                                 PROP_DOMAIN,
	                                 g_param_spec_string ("domain",
	                                                      "Domain",
	                                                      "Domain",
	                                                      NULL,
	                                                      G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));

	miner_files_error_quark = g_quark_from_static_string ("TrackerMinerFiles");
}

static void
tracker_miner_files_check_unextracted (TrackerMinerFiles *mf)
{
	if (!mf->private->start_extractor)
		return;

	mf->private->start_extractor = FALSE;
	g_debug ("Starting extractor");
	tracker_extract_watchdog_ensure_started (mf->private->extract_watchdog);
}

static gboolean
extractor_lost_timeout_cb (gpointer user_data)
{
	TrackerMinerFiles *mf = user_data;

	tracker_miner_files_check_unextracted (mf);
	mf->private->grace_period_timeout_id = 0;
	return G_SOURCE_REMOVE;
}


static void
on_extractor_lost (TrackerExtractWatchdog *watchdog,
                   TrackerMinerFiles      *mf)
{
	g_debug ("tracker-extract vanished, maybe restarting.");

	/* Give a period of grace before restarting, so we allow replacing
	 * from eg. a terminal.
	 */
	mf->private->grace_period_timeout_id =
		g_timeout_add_seconds (1, extractor_lost_timeout_cb, mf);
}

static void
on_extractor_status (TrackerExtractWatchdog *watchdog,
                     const gchar            *status,
                     gdouble                 progress,
                     gint                    remaining,
                     TrackerMinerFiles      *mf)
{
	if (!tracker_miner_is_paused (TRACKER_MINER (mf))) {
		g_object_set (mf,
		              "status", status,
		              "progress", progress,
		              "remaining-time", remaining,
		              NULL);
	}
}

static void
tracker_miner_files_init (TrackerMinerFiles *mf)
{
	TrackerMinerFilesPrivate *priv;

	priv = mf->private = TRACKER_MINER_FILES_GET_PRIVATE (mf);

	priv->storage = tracker_storage_new ();

#ifdef HAVE_POWER
	priv->power = tracker_power_new ();

	if (priv->power) {
		g_signal_connect (priv->power, "notify::on-low-battery",
		                  G_CALLBACK (battery_status_cb),
		                  mf);
		g_signal_connect (priv->power, "notify::on-battery",
		                  G_CALLBACK (battery_status_cb),
		                  mf);
	}
#endif /* HAVE_POWER */

	priv->finished_handler = g_signal_connect_after (mf, "finished",
	                                                 G_CALLBACK (miner_finished_cb),
	                                                 NULL);

	priv->mtime_check = TRUE;
}

static void
miner_files_initable_iface_init (GInitableIface *iface)
{
	miner_files_initable_parent_iface = g_type_interface_peek_parent (iface);
	iface->init = miner_files_initable_init;
}

static void
removable_days_threshold_changed (TrackerMinerFiles *mf)
{
	/* Check if the stale volume removal configuration changed from enabled to disabled
	 * or from disabled to enabled */
	if (tracker_config_get_removable_days_threshold (mf->private->config) == 0 &&
	    mf->private->stale_volumes_check_id != 0) {
		/* From having the check enabled to having it disabled, remove the timeout */
		TRACKER_NOTE (CONFIG, g_message ("Stale volume removal now disabled, removing timeout"));
		g_source_remove (mf->private->stale_volumes_check_id);
		mf->private->stale_volumes_check_id = 0;
	} else if (tracker_config_get_removable_days_threshold (mf->private->config) > 0 &&
	           mf->private->stale_volumes_check_id == 0) {
		TRACKER_NOTE (CONFIG, g_message ("Stale volume removal now enabled, initializing timeout"));
		/* From having the check disabled to having it enabled, so fire up the
		 * timeout. */
		init_stale_volume_removal (TRACKER_MINER_FILES (mf));
	}
}

static gboolean
miner_files_initable_init (GInitable     *initable,
                           GCancellable  *cancellable,
                           GError       **error)
{
	TrackerMinerFiles *mf;
	GError *inner_error = NULL;
	gchar *domain_name;

	/* Chain up parent's initable callback before calling child's one */
	if (!miner_files_initable_parent_iface->init (initable, cancellable, &inner_error)) {
		g_propagate_error (error, inner_error);
		return FALSE;
	}

	mf = TRACKER_MINER_FILES (initable);

	mf->private->domain_ontology = tracker_domain_ontology_new (mf->private->domain, NULL, &inner_error);
	if (!mf->private->domain_ontology) {
		g_propagate_error (error, inner_error);
		return FALSE;
	}

	/* We must have a configuration setup here */
	if (G_UNLIKELY (!mf->private->config)) {
		g_set_error (error,
		             TRACKER_MINER_ERROR,
		             0,
		             "No config set for miner %s",
		             G_OBJECT_TYPE_NAME (mf));
		return FALSE;
	}

#ifdef HAVE_POWER
	check_battery_status (mf);
#endif /* HAVE_POWER */

	/* We want to get notified when config changes */
	g_signal_connect (mf->private->config, "notify::low-disk-space-limit",
	                  G_CALLBACK (low_disk_space_limit_cb),
	                  mf);
	g_signal_connect_swapped (mf->private->config,
	                          "notify::removable-days-threshold",
	                          G_CALLBACK (removable_days_threshold_changed),
	                          mf);

#ifdef HAVE_POWER
	g_signal_connect (mf->private->config, "notify::index-on-battery",
	                  G_CALLBACK (index_on_battery_cb),
	                  mf);
	g_signal_connect (mf->private->config, "notify::index-on-battery-first-time",
	                  G_CALLBACK (index_on_battery_cb),
	                  mf);
#endif /* HAVE_POWER */

	disk_space_check_start (mf);

	domain_name = tracker_domain_ontology_get_domain (mf->private->domain_ontology, NULL);
	mf->private->extract_watchdog = tracker_extract_watchdog_new (domain_name);
	g_signal_connect (mf->private->extract_watchdog, "lost",
	                  G_CALLBACK (on_extractor_lost), mf);
	g_signal_connect (mf->private->extract_watchdog, "status",
	                  G_CALLBACK (on_extractor_status), mf);
	g_free (domain_name);

	return TRUE;
}

static void
miner_files_set_property (GObject      *object,
                          guint         prop_id,
                          const GValue *value,
                          GParamSpec   *pspec)
{
	TrackerMinerFilesPrivate *priv;

	priv = TRACKER_MINER_FILES_GET_PRIVATE (object);

	switch (prop_id) {
	case PROP_CONFIG:
		priv->config = g_value_dup_object (value);
		break;
	case PROP_DOMAIN:
		priv->domain = g_value_dup_string (value);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
miner_files_get_property (GObject    *object,
                          guint       prop_id,
                          GValue     *value,
                          GParamSpec *pspec)
{
	TrackerMinerFilesPrivate *priv;

	priv = TRACKER_MINER_FILES_GET_PRIVATE (object);

	switch (prop_id) {
	case PROP_CONFIG:
		g_value_set_object (value, priv->config);
		break;
	case PROP_DOMAIN:
		g_value_set_string (value, priv->domain);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
miner_files_finalize (GObject *object)
{
	TrackerMinerFiles *mf;
	TrackerMinerFilesPrivate *priv;
	TrackerIndexingTree *indexing_tree;

	mf = TRACKER_MINER_FILES (object);
	priv = mf->private;

	g_free (priv->domain);

	indexing_tree = tracker_miner_fs_get_indexing_tree (TRACKER_MINER_FS (mf));
	g_signal_handlers_disconnect_by_data (indexing_tree, mf);

	if (priv->grace_period_timeout_id != 0) {
		g_source_remove (priv->grace_period_timeout_id);
		priv->grace_period_timeout_id = 0;
	}

	g_signal_handlers_disconnect_by_func (priv->extract_watchdog,
	                                      on_extractor_lost,
	                                      NULL);
	g_clear_object (&priv->extract_watchdog);

	if (priv->config) {
		g_signal_handlers_disconnect_by_func (priv->config,
		                                      low_disk_space_limit_cb,
		                                      NULL);
		g_object_unref (priv->config);
	}

	disk_space_check_stop (TRACKER_MINER_FILES (object));

#ifdef HAVE_POWER
	if (priv->power) {
		g_object_unref (priv->power);
	}
#endif /* HAVE_POWER */

	tracker_domain_ontology_unref (priv->domain_ontology);

	if (priv->storage) {
		g_object_unref (priv->storage);
	}

	if (priv->stale_volumes_check_id) {
		g_source_remove (priv->stale_volumes_check_id);
		priv->stale_volumes_check_id = 0;
	}

	G_OBJECT_CLASS (tracker_miner_files_parent_class)->finalize (object);
}

static void
set_up_mount_point_cb (GObject      *source,
                       GAsyncResult *result,
                       gpointer      user_data)
{
	g_autofree GError *error = NULL;

	tracker_sparql_statement_update_finish (TRACKER_SPARQL_STATEMENT (source),
	                                        result, &error);

	if (error) {
		g_critical ("Could not set mount point in database, %s",
		            error->message);
	}
}

static void
set_up_mount_point (TrackerMinerFiles *miner,
                    GFile             *mount_point,
                    gboolean           mounted,
                    TrackerBatch      *batch)
{
	TrackerSparqlConnection *conn;
	g_autoptr (TrackerSparqlStatement) stmt = NULL;
	g_autofree gchar *now = NULL, *uri = NULL;

	uri = g_file_get_uri (mount_point);
	now = tracker_date_to_string (time (NULL));

	g_debug ("Mount point state (%s) being set in DB for mount_point '%s'",
	         mounted ? "MOUNTED" : "UNMOUNTED",
	         uri);

	conn = tracker_miner_get_connection (TRACKER_MINER (miner));
	stmt = tracker_load_statement (conn, "update-mountpoint.rq", NULL);

	if (batch) {
		tracker_batch_add_statement (batch, stmt,
		                             "mountPoint", G_TYPE_STRING, uri,
		                             "mounted", G_TYPE_BOOLEAN, mounted,
		                             "currentDate", G_TYPE_STRING, now,
		                             NULL);
	} else {
		tracker_sparql_statement_bind_string (stmt, "mountPoint", uri);
		tracker_sparql_statement_bind_boolean (stmt, "mounted", mounted);
		tracker_sparql_statement_bind_string (stmt, "currentDate", now);
		tracker_sparql_statement_update_async (stmt,
		                                       NULL,
		                                       set_up_mount_point_cb,
		                                       NULL);
	}
}

static void
delete_index_root (TrackerMinerFiles *miner,
                   GFile             *mount_point,
                   TrackerBatch      *batch)
{
	g_autofree gchar *uri = NULL;
	TrackerSparqlConnection *conn = tracker_batch_get_connection (batch);
	g_autoptr (TrackerSparqlStatement) stmt = NULL;

	uri = g_file_get_uri (mount_point);
	stmt = tracker_load_statement (conn, "delete-index-root.rq", NULL);
	tracker_batch_add_statement (batch, stmt,
	                             "rootFolder", G_TYPE_STRING, uri,
	                             NULL);
}

static void
init_index_roots_cb (GObject      *source,
                     GAsyncResult *result,
                     gpointer      user_data)
{
	g_autofree GError *error = NULL;

	tracker_batch_execute_finish (TRACKER_BATCH (source), result, &error);

	if (error) {
		g_critical ("Could not initialize currently active mount points: %s",
		            error->message);
	} else {
		init_stale_volume_removal (user_data);
	}
}

static void
init_index_roots (TrackerMinerFiles *miner_files)
{
	TrackerMiner *miner = TRACKER_MINER (miner_files);
	TrackerSparqlConnection *conn;
	g_autoptr (TrackerSparqlStatement) stmt = NULL;
	TrackerIndexingTree *indexing_tree;
	g_autoptr (GList) roots = NULL;
	g_autoptr (GHashTable) handled = NULL;
	g_autoptr (TrackerBatch) batch = NULL;
	g_autoptr (GError) error = NULL;
	g_autoptr (TrackerSparqlCursor) cursor = NULL;
	GList *l;

	g_debug ("Initializing mount points...");

	conn = tracker_miner_get_connection (miner);
	stmt = tracker_load_statement (conn, "get-index-roots.rq", &error);

	if (stmt) {
		/* First, get all mounted volumes, according to tracker-store (SYNC!) */
		cursor = tracker_sparql_statement_execute (stmt, NULL, &error);
	}

	if (error) {
		g_critical ("Could not obtain the mounted volumes: %s", error->message);
		return;
	}

	batch = tracker_sparql_connection_create_batch (conn);
	indexing_tree = tracker_miner_fs_get_indexing_tree (TRACKER_MINER_FS (miner));
	handled = g_hash_table_new_full (g_file_hash, (GEqualFunc) g_file_equal,
	                                 g_object_unref, NULL);

	while (tracker_sparql_cursor_next (cursor, NULL, NULL)) {
		const gchar *uri;
		gboolean is_removable, is_optical;
		GFile *file;

		uri = tracker_sparql_cursor_get_string (cursor, 0, NULL);
		is_removable = tracker_sparql_cursor_get_boolean (cursor, 1);
		is_optical = tracker_sparql_cursor_get_boolean (cursor, 2);

		file = g_file_new_for_uri (uri);
		g_hash_table_add (handled, file);

		if (tracker_indexing_tree_file_is_root (indexing_tree, file)) {
			/* Directory is indexed and configured */
			if (is_removable || is_optical) {
				set_up_mount_point (TRACKER_MINER_FILES (miner),
				                    file,
				                    TRUE,
				                    batch);
			}
		} else {
			/* Directory is indexed, but no longer configured */
			if (tracker_config_get_removable_days_threshold (miner_files->private->config) > 0 &&
			    ((is_optical &&
			      tracker_config_get_index_optical_discs (miner_files->private->config)) ||
			     (!is_optical && is_removable &&
			      tracker_config_get_index_removable_devices (miner_files->private->config)))) {
				/* Preserve */
				set_up_mount_point (TRACKER_MINER_FILES (miner),
				                    file, FALSE, batch);
			} else {
				/* Not a removable device to preserve, or a no
				 * longer configured folder.
				 */
				delete_index_root (TRACKER_MINER_FILES (miner),
				                   file, batch);
			}
		}
	}

	roots = tracker_indexing_tree_list_roots (indexing_tree);

	for (l = roots; l; l = l->next) {
		TrackerStorage *storage = miner_files->private->storage;
		TrackerStorageType type = 0;
		const gchar *uuid;
		GFile *file = l->data;

		if (g_hash_table_contains (handled, file))
			continue;

		uuid = tracker_storage_get_uuid_for_file (storage, file);

		if (uuid)
			type = tracker_storage_get_type_for_uuid (storage, uuid);

		if ((type & TRACKER_STORAGE_REMOVABLE) != 0)
			set_up_mount_point (miner_files, file, TRUE, NULL);
	}

	tracker_batch_execute_async (batch,
	                             NULL,
	                             init_index_roots_cb,
	                             miner);
}

static gboolean
cleanup_stale_removable_volumes_cb (gpointer user_data)
{
	TrackerMinerFiles *miner = TRACKER_MINER_FILES (user_data);
	gint n_days_threshold;
	time_t n_days_ago;
	gchar *n_days_ago_as_string;

	n_days_threshold = tracker_config_get_removable_days_threshold (miner->private->config);

	if (n_days_threshold == 0)
		return TRUE;

	n_days_ago = (time (NULL) - (SECONDS_PER_DAY * n_days_threshold));
	n_days_ago_as_string = tracker_date_to_string (n_days_ago);

	g_debug ("Running stale volumes check...");

	miner_files_in_removable_media_remove_by_date (miner, n_days_ago_as_string);

	g_free (n_days_ago_as_string);

	return TRUE;
}

static void
init_stale_volume_removal (TrackerMinerFiles *miner)
{
	/* If disabled, make sure we don't do anything */
	if (tracker_config_get_removable_days_threshold (miner->private->config) == 0) {
		g_debug ("Stale volume check is disabled");
		return;
	}

	/* Run right away the first check */
	cleanup_stale_removable_volumes_cb (miner);

	g_debug ("Initializing stale volume check timeout...");

	/* Then, setup new timeout event every day */
	miner->private->stale_volumes_check_id =
		g_timeout_add_seconds (SECONDS_PER_DAY + 1,
		                       cleanup_stale_removable_volumes_cb,
		                       miner);
}

#ifdef HAVE_POWER

static void
set_up_throttle (TrackerMinerFiles *mf,
                 gboolean           enable)
{
	gdouble throttle;
	gint config_throttle;

	config_throttle = tracker_config_get_throttle (mf->private->config);
	throttle = (1.0 / 20) * config_throttle;

	if (enable) {
		throttle += 0.25;
	}

	throttle = CLAMP (throttle, 0, 1);

	g_debug ("Setting new throttle to %0.3f", throttle);
	tracker_miner_fs_set_throttle (TRACKER_MINER_FS (mf), throttle);
}

static void
check_battery_status (TrackerMinerFiles *mf)
{
	gboolean on_battery, on_low_battery;
	gboolean should_pause = FALSE;
	gboolean should_throttle = FALSE;

	if (mf->private->power == NULL) {
		return;
	}

	on_low_battery = tracker_power_get_on_low_battery (mf->private->power);
	on_battery = tracker_power_get_on_battery (mf->private->power);

	if (!on_battery) {
		g_debug ("Running on AC power");
		should_pause = FALSE;
		should_throttle = FALSE;
	} else if (on_low_battery) {
		g_message ("Running on LOW Battery, pausing");
		should_pause = TRUE;
		should_throttle = TRUE;
	} else {
		should_throttle = TRUE;

		/* Check if miner should be paused based on configuration */
		if (!tracker_config_get_index_on_battery (mf->private->config)) {
			if (!tracker_config_get_index_on_battery_first_time (mf->private->config)) {
				g_message ("Running on battery, but not enabled, pausing");
				should_pause = TRUE;
			} else if (tracker_miner_files_get_first_index_done (mf)) {
				g_debug ("Running on battery and first-time index "
				         "already done, pausing");
				should_pause = TRUE;
			} else {
				g_debug ("Running on battery, but first-time index not "
				         "already finished, keeping on");
			}
		} else {
			g_debug ("Running on battery");
		}
	}

	if (should_pause) {
		/* Don't try to pause again */
		if (!mf->private->low_battery_pause) {
			mf->private->low_battery_pause = TRUE;
			tracker_miner_pause (TRACKER_MINER (mf));
		}
	} else {
		/* Don't try to resume again */
		if (mf->private->low_battery_pause) {
			tracker_miner_resume (TRACKER_MINER (mf));
			mf->private->low_battery_pause = FALSE;
		}
	}

	set_up_throttle (mf, should_throttle);
}

/* Called when battery status change is detected */
static void
battery_status_cb (GObject    *object,
                   GParamSpec *pspec,
                   gpointer    user_data)
{
	TrackerMinerFiles *mf = user_data;

	check_battery_status (mf);
}

/* Called when battery-related configuration change is detected */
static void
index_on_battery_cb (GObject    *object,
                     GParamSpec *pspec,
                     gpointer    user_data)
{
	TrackerMinerFiles *mf = user_data;

	check_battery_status (mf);
}

#endif /* HAVE_POWER */

/* Called when mining has finished the first time */
static void
miner_finished_cb (TrackerMinerFS *fs,
                   gdouble         seconds_elapsed,
                   guint           total_directories_found,
                   guint           total_directories_ignored,
                   guint           total_files_found,
                   guint           total_files_ignored,
                   gpointer        user_data)
{
	TrackerMinerFiles *mf = TRACKER_MINER_FILES (fs);

	/* Create stamp file if not already there */
	if (!tracker_miner_files_get_first_index_done (mf)) {
		tracker_miner_files_set_first_index_done (mf, TRUE);
	}

	/* And remove the signal handler so that it's not
	 *  called again */
	if (mf->private->finished_handler) {
		g_signal_handler_disconnect (fs, mf->private->finished_handler);
		mf->private->finished_handler = 0;
	}

#ifdef HAVE_POWER
	check_battery_status (mf);
#endif /* HAVE_POWER */
}

static GFile *
get_cache_dir (TrackerMinerFiles *mf)
{
	GFile *cache;

	cache = tracker_domain_ontology_get_cache (mf->private->domain_ontology);
	return g_file_get_child (cache, "files");
}


static gboolean
disk_space_check (TrackerMinerFiles *mf)
{
	GFile *file;
	gint limit;
	gchar *data_dir;
	gdouble remaining;

	limit = tracker_config_get_low_disk_space_limit (mf->private->config);

	if (limit < 1) {
		return FALSE;
	}

	/* Get % of remaining space in the partition where the cache is */
	file = get_cache_dir (mf);
	data_dir = g_file_get_path (file);
	remaining = tracker_file_system_get_remaining_space_percentage (data_dir);
	g_free (data_dir);
	g_object_unref (file);

	if (remaining <= limit) {
		g_message ("WARNING: Available disk space (%lf%%) is below "
		           "configured threshold for acceptable working (%d%%)",
		           remaining, limit);
		return TRUE;
	}

	return FALSE;
}

static gboolean
disk_space_check_cb (gpointer user_data)
{
	TrackerMinerFiles *mf = user_data;

	if (disk_space_check (mf)) {
		/* Don't try to pause again */
		if (!mf->private->disk_space_pause) {
			mf->private->disk_space_pause = TRUE;
			tracker_miner_pause (TRACKER_MINER (mf));
		}
	} else {
		/* Don't try to resume again */
		if (mf->private->disk_space_pause) {
			tracker_miner_resume (TRACKER_MINER (mf));
			mf->private->disk_space_pause = FALSE;
		}
	}

	return TRUE;
}

static void
disk_space_check_start (TrackerMinerFiles *mf)
{
	gint limit;

	if (mf->private->disk_space_check_id != 0) {
		return;
	}

	limit = tracker_config_get_low_disk_space_limit (mf->private->config);

	if (limit != -1) {
		TRACKER_NOTE (CONFIG, g_message ("Starting disk space check for every %d seconds",
		                      DISK_SPACE_CHECK_FREQUENCY));
		mf->private->disk_space_check_id =
			g_timeout_add_seconds (DISK_SPACE_CHECK_FREQUENCY,
			                       disk_space_check_cb,
			                       mf);

		/* Call the function now too to make sure we have an
		 * initial value too!
		 */
		disk_space_check_cb (mf);
	} else {
		TRACKER_NOTE (CONFIG, g_message ("Not setting disk space, configuration is set to -1 (disabled)"));
	}
}

static void
disk_space_check_stop (TrackerMinerFiles *mf)
{
	if (mf->private->disk_space_check_id) {
		TRACKER_NOTE (CONFIG, g_message ("Stopping disk space check"));
		g_source_remove (mf->private->disk_space_check_id);
		mf->private->disk_space_check_id = 0;
	}
}

static void
low_disk_space_limit_cb (GObject    *gobject,
                         GParamSpec *arg1,
                         gpointer    user_data)
{
	TrackerMinerFiles *mf = user_data;

	disk_space_check_cb (mf);
}

static void
indexing_tree_directory_added_cb (TrackerIndexingTree *indexing_tree,
                                  GFile               *directory,
                                  gpointer             user_data)
{
	TrackerMinerFiles *miner_files = user_data;
	TrackerStorage *storage = miner_files->private->storage;
	TrackerStorageType type = 0;
	const gchar *uuid;

	uuid = tracker_storage_get_uuid_for_file (storage, directory);

	if (uuid)
		type = tracker_storage_get_type_for_uuid (storage, uuid);

	if ((type & TRACKER_STORAGE_REMOVABLE) != 0)
		set_up_mount_point (miner_files, directory, TRUE, NULL);
}

static void
indexing_tree_directory_removed_cb (TrackerIndexingTree *indexing_tree,
                                    GFile               *directory,
                                    gpointer             user_data)
{
	TrackerMinerFiles *miner_files = user_data;
	TrackerStorage *storage = miner_files->private->storage;
	TrackerStorageType type = 0;
	TrackerSparqlConnection *conn;
	g_autoptr (TrackerBatch) batch = NULL;
	g_autoptr (GError) error = NULL;
	const gchar *uuid;
	gboolean delete = FALSE, update_mount = FALSE;

	uuid = tracker_storage_get_uuid_for_file (storage, directory);

	if (uuid)
		type = tracker_storage_get_type_for_uuid (storage, uuid);

	if ((type & TRACKER_STORAGE_REMOVABLE) != 0) {
		if (!tracker_config_get_index_removable_devices (miner_files->private->config))
			delete = TRUE;
		else if ((type & TRACKER_STORAGE_OPTICAL) != 0 &&
		         !tracker_config_get_index_optical_discs (miner_files->private->config))
			delete = TRUE;
		else if (tracker_config_get_removable_days_threshold (miner_files->private->config) == 0)
			delete = TRUE;
		else
			update_mount = TRUE;
	} else {
		delete = TRUE;
	}

	conn = tracker_miner_get_connection (TRACKER_MINER (miner_files));
	batch = tracker_sparql_connection_create_batch (conn);

	if (delete)
		delete_index_root (miner_files, directory, batch);
	else if (update_mount)
		set_up_mount_point (miner_files, directory, FALSE, batch);

	if (!tracker_batch_execute (batch, NULL, &error))
		g_warning ("Error updating indexed folder: %s", error->message);
}

static void
miner_files_process_file (TrackerMinerFS       *fs,
                          GFile                *file,
                          GFileInfo            *info,
                          TrackerSparqlBuffer  *buffer,
                          gboolean              create)
{
	TrackerMinerFilesPrivate *priv = TRACKER_MINER_FILES (fs)->private;

	priv->start_extractor = TRUE;
	tracker_miner_files_process_file (fs, file, info, buffer, create);
}

static void
miner_files_process_file_attributes (TrackerMinerFS       *fs,
                                     GFile                *file,
                                     GFileInfo            *info,
                                     TrackerSparqlBuffer  *buffer)
{
	tracker_miner_files_process_file_attributes (fs, file, info, buffer);
}

static void
miner_files_finished (TrackerMinerFS *fs,
                      gdouble         elapsed,
                      gint            directories_found,
                      gint            directories_ignored,
                      gint            files_found,
                      gint            files_ignored)
{
	tracker_miner_files_set_last_crawl_done (TRACKER_MINER_FILES (fs), TRUE);

	tracker_miner_files_check_unextracted (TRACKER_MINER_FILES (fs));
}

static void
miner_files_remove_children (TrackerMinerFS      *fs,
                             GFile               *file,
                             TrackerSparqlBuffer *buffer)
{
	tracker_sparql_buffer_log_delete_content (buffer, file);
}

static void
miner_files_remove_file (TrackerMinerFS      *fs,
                         GFile               *file,
                         TrackerSparqlBuffer *buffer,
                         gboolean             is_dir)
{
	if (is_dir)
		tracker_sparql_buffer_log_delete_content (buffer, file);

	tracker_sparql_buffer_log_delete (buffer, file);
}

static void
miner_files_move_file (TrackerMinerFS      *fs,
                       GFile               *file,
                       GFile               *source_file,
                       TrackerSparqlBuffer *buffer,
                       gboolean             recursive)
{
	const gchar *data_source;

	data_source = tracker_miner_fs_get_identifier (fs, file);
	tracker_sparql_buffer_log_move (buffer, source_file, file,
	                                data_source);

	if (recursive)
		tracker_sparql_buffer_log_move_content (buffer, source_file, file);
}

static void
miner_files_constructed (GObject *object)
{
	TrackerIndexingTree *indexing_tree;

	G_OBJECT_CLASS (tracker_miner_files_parent_class)->constructed (object);

	indexing_tree = tracker_miner_fs_get_indexing_tree (TRACKER_MINER_FS (object));
	g_signal_connect (indexing_tree, "directory-added",
	                  G_CALLBACK (indexing_tree_directory_added_cb), object);
	g_signal_connect (indexing_tree, "directory-removed",
	                  G_CALLBACK (indexing_tree_directory_removed_cb), object);
}

TrackerMiner *
tracker_miner_files_new (TrackerSparqlConnection  *connection,
                         TrackerConfig            *config,
                         const gchar              *domain,
                         GError                  **error)
{
	return g_initable_new (TRACKER_TYPE_MINER_FILES,
	                       NULL,
	                       error,
	                       "connection", connection,
	                       "root", NULL,
	                       "config", config,
	                       "domain", domain,
	                       "processing-pool-wait-limit", 1,
	                       "processing-pool-ready-limit", 800,
	                       "file-attributes", FILE_ATTRIBUTES,
	                       NULL);
}

static void
remove_files_in_removable_media_cb (GObject      *object,
                                    GAsyncResult *result,
                                    gpointer      user_data)
{
	g_autofree GError *error = NULL;

	tracker_sparql_statement_update_finish (TRACKER_SPARQL_STATEMENT (object),
	                                        result, &error);

	if (error)
		g_critical ("Could not remove files in volumes: %s", error->message);
}

static void
miner_files_in_removable_media_remove_by_date (TrackerMinerFiles  *miner,
                                               const gchar        *date)
{
	TrackerSparqlConnection *conn;
	g_autoptr (TrackerSparqlStatement) stmt = NULL;

	g_debug ("  Removing all resources in store from removable or "
	         "optical devices not mounted after '%s'",
	         date);

	conn = tracker_miner_get_connection (TRACKER_MINER (miner));
	stmt = tracker_load_statement (conn, "delete-mountpoints-by-date.rq", NULL);

	tracker_sparql_statement_bind_string (stmt, "unmountDate", date);
	tracker_sparql_statement_update_async (stmt, NULL,
	                                       remove_files_in_removable_media_cb,
	                                       NULL);
}

inline static gchar *
get_first_index_filename (TrackerMinerFiles *mf)
{
	GFile *file;
	gchar *prefix, *path;

	file = get_cache_dir (mf);
	prefix = g_file_get_path (file);

	path = g_build_filename (prefix,
	                         FIRST_INDEX_FILENAME,
	                         NULL);
	g_free (prefix);
	g_object_unref (file);

	return path;
}

/**
 * tracker_miner_files_get_first_index_done:
 *
 * Check if first full index of files was already done.
 *
 * Returns: %TRUE if a first full index have been done, %FALSE otherwise.
 **/
gboolean
tracker_miner_files_get_first_index_done (TrackerMinerFiles *mf)
{
	gboolean exists;
	gchar *filename;

	filename = get_first_index_filename (mf);
	exists = g_file_test (filename, G_FILE_TEST_EXISTS);
	g_free (filename);

	return exists;
}

/**
 * tracker_miner_files_set_first_index_done:
 *
 * Set the status of the first full index of files. Should be set to
 *  %FALSE if the index was never done or if a reindex is needed. When
 *  the index is completed, should be set to %TRUE.
 **/
void
tracker_miner_files_set_first_index_done (TrackerMinerFiles *mf,
					  gboolean           done)
{
	gboolean already_exists;
	gchar *filename;

	filename = get_first_index_filename (mf);
	already_exists = g_file_test (filename, G_FILE_TEST_EXISTS);

	if (done && !already_exists) {
		GError *error = NULL;

		/* If done, create stamp file if not already there */
		if (!g_file_set_contents (filename, PACKAGE_VERSION, -1, &error)) {
			g_warning ("  Could not create file:'%s' failed, %s",
			           filename,
			           error->message);
			g_error_free (error);
		} else {
			g_info ("  First index file:'%s' created", filename);
		}
	} else if (!done && already_exists) {
		/* If NOT done, remove stamp file */
		g_info ("  Removing first index file:'%s'", filename);

		if (g_remove (filename)) {
			g_warning ("    Could not remove file:'%s': %m",
			           filename);
		}
	}

	g_free (filename);
}

static inline gchar *
get_last_crawl_filename (TrackerMinerFiles *mf)
{
	GFile *file;
	gchar *prefix, *path;

	file = get_cache_dir (mf);
	prefix = g_file_get_path (file);

	path = g_build_filename (prefix,
	                         LAST_CRAWL_FILENAME,
	                         NULL);
	g_free (prefix);
	g_object_unref (file);

	return path;
}

/**
 * tracker_miner_files_get_last_crawl_done:
 *
 * Check when last crawl was performed.
 *
 * Returns: time_t() value when last crawl occurred, otherwise 0.
 **/
guint64
tracker_miner_files_get_last_crawl_done (TrackerMinerFiles *mf)
{
	gchar *filename;
	gchar *content;
	guint64 then;

	filename = get_last_crawl_filename (mf);

	if (!g_file_get_contents (filename, &content, NULL, NULL)) {
		g_info ("  No previous timestamp, crawling forced");
		return 0;
	}

	then = g_ascii_strtoull (content, NULL, 10);
	g_free (content);

	return then;
}

/**
 * tracker_miner_files_set_last_crawl_done:
 *
 * Set the time stamp of the last full index of files.
 **/
void
tracker_miner_files_set_last_crawl_done (TrackerMinerFiles *mf,
					 gboolean           done)
{
	gboolean already_exists;
	gchar *filename;

	filename = get_last_crawl_filename (mf);
	already_exists = g_file_test (filename, G_FILE_TEST_EXISTS);

	if (done) {
		GError *error = NULL;
		gchar *content;
		content = g_strdup_printf ("%" G_GUINT64_FORMAT, (guint64) time (NULL));
		if (already_exists) {
			g_info ("  Overwriting last crawl file:'%s'", filename);
		} else {
			g_info ("  Creating last crawl file:'%s'", filename);
		}
		/* Create/update time stamp file */
		if (!g_file_set_contents (filename, content, -1, &error)) {
			g_warning ("  Could not create/overwrite file:'%s' failed, %s",
			           filename,
			           error->message);
			g_error_free (error);
		} else {
			g_info ("  Last crawl file:'%s' updated", filename);
		}

		g_free (content);
	} else {
		g_info ("  Crawl not done yet, doesn't update last crawl file.");
	}
	g_free (filename);
}

inline static gchar *
get_need_mtime_check_filename (TrackerMinerFiles *mf)
{
	GFile *file;
	gchar *prefix, *path;

	file = get_cache_dir (mf);
	prefix = g_file_get_path (file);

	path = g_build_filename (prefix,
	                         NEED_MTIME_CHECK_FILENAME,
	                         NULL);
	g_free (prefix);
	g_object_unref (file);

	return path;
}

/**
 * tracker_miner_files_get_need_mtime_check:
 *
 * Check if the miner-fs was cleanly shutdown or not.
 *
 * Returns: %TRUE if we need to check mtimes for directories against
 * the database on the next start for the miner-fs, %FALSE otherwise.
 **/
gboolean
tracker_miner_files_get_need_mtime_check (TrackerMinerFiles *mf)
{
	gboolean exists;
	gchar *filename;

	filename = get_need_mtime_check_filename (mf);
	exists = g_file_test (filename, G_FILE_TEST_EXISTS);
	g_free (filename);

	/* Existence of the file means we cleanly shutdown before and
	 * don't need to do the mtime check again on this start.
	 */
	return !exists;
}

/**
 * tracker_miner_files_set_need_mtime_check:
 * @needed: a #gboolean
 *
 * If the next start of miner-fs should perform a full mtime check
 * against each directory found and those in the database (for
 * complete synchronisation), then @needed should be #TRUE, otherwise
 * #FALSE.
 *
 * Creates a file in $HOME/.cache/tracker/ if an mtime check is not
 * needed. The idea behind this is that a check is forced if the file
 * is not cleaned up properly on shutdown (i.e. due to a crash or any
 * other uncontrolled shutdown reason).
 **/
void
tracker_miner_files_set_need_mtime_check (TrackerMinerFiles *mf,
					  gboolean           needed)
{
	gboolean already_exists;
	gchar *filename;

	filename = get_need_mtime_check_filename (mf);
	already_exists = g_file_test (filename, G_FILE_TEST_EXISTS);

	/* !needed = add file
	 *  needed = remove file
	 */
	if (!needed && !already_exists) {
		GError *error = NULL;

		/* Create stamp file if not already there */
		if (!g_file_set_contents (filename, PACKAGE_VERSION, -1, &error)) {
			g_warning ("  Could not create file:'%s' failed, %s",
			           filename,
			           error->message);
			g_error_free (error);
		} else {
			g_info ("  Need mtime check file:'%s' created", filename);
		}
	} else if (needed && already_exists) {
		/* Remove stamp file */
		g_info ("  Removing need mtime check file:'%s'", filename);

		if (g_remove (filename)) {
			g_warning ("    Could not remove file:'%s': %m",
			           filename);
		}
	}

	g_free (filename);
}

void
tracker_miner_files_set_mtime_checking (TrackerMinerFiles *mf,
                                        gboolean           mtime_check)
{
	mf->private->mtime_check = mtime_check;
}

TrackerStorage *
tracker_miner_files_get_storage (TrackerMinerFiles *mf)
{
	return mf->private->storage;
}
