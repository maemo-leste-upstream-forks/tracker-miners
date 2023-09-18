/*
 * Copyright (C) 2009, Nokia <ivan.frade@nokia.com>
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

#include "config-miners.h"

#include <libtracker-miners-common/tracker-common.h>

#include "tracker-miner-fs.h"
#include "tracker-monitor.h"
#include "tracker-utils.h"
#include "tracker-priority-queue.h"
#include "tracker-task-pool.h"
#include "tracker-sparql-buffer.h"
#include "tracker-file-notifier.h"
#include "tracker-lru.h"

#define BUFFER_POOL_LIMIT 800
#define DEFAULT_URN_LRU_SIZE 100

#define BIG_QUEUE_THRESHOLD 1000

/* Put tasks processing at a lower priority so other events
 * (timeouts, monitor events, etc...) are guaranteed to be
 * dispatched promptly.
 */
#define TRACKER_TASK_PRIORITY G_PRIORITY_DEFAULT_IDLE + 10

#define MAX_SIMULTANEOUS_ITEMS 64

#define TRACKER_CRAWLER_MAX_TIMEOUT_INTERVAL 1000

/**
 * SECTION:tracker-miner-fs
 * @short_description: Abstract base class for filesystem miners
 * @include: libtracker-miner/tracker-miner.h
 *
 * #TrackerMinerFS is an abstract base class for miners that collect data
 * from a filesystem where parent/child relationships need to be
 * inserted into the database correctly with queue management.
 *
 * All the filesystem crawling and monitoring is abstracted away,
 * leaving to implementations the decisions of what directories/files
 * should it process, and the actual data extraction.
 **/

typedef struct {
	guint16 type;
	guint attributes_update : 1;
	guint is_dir : 1;
	GFile *file;
	GFile *dest_file;
	GFileInfo *info;
	GList *root_node;
	GList *queue_node;
} QueueEvent;

typedef struct {
	GFile *file;
	gchar *urn;
	gint priority;
	GCancellable *cancellable;
	TrackerMiner *miner;
	TrackerTask *task;
} UpdateProcessingTaskContext;

struct _TrackerMinerFSPrivate {
	TrackerPriorityQueue *items;
	GHashTable *items_by_file;

	guint item_queues_handler_id;

	TrackerIndexingTree *indexing_tree;
	TrackerFileNotifier *file_notifier;

	/* Sparql insertion tasks */
	TrackerTaskPool *task_pool;
	TrackerSparqlBuffer *sparql_buffer;

	/* Folder URN cache */
	TrackerLRU *urn_lru;

	/* Properties */
	gdouble throttle;
	gchar *file_attributes;

	/* Status */
	GTimer *timer;
	GTimer *extraction_timer;

	guint shown_totals : 1;     /* TRUE if totals have been shown */
	guint is_paused : 1;        /* TRUE if miner is paused */
	guint flushing : 1;         /* TRUE if flushing SPARQL */

	guint timer_stopped : 1;    /* TRUE if main timer is stopped */
	guint extraction_timer_stopped : 1; /* TRUE if the extraction
	                                     * timer is stopped */

	GHashTable *roots_to_notify;        /* Used to signal indexing
	                                     * trees finished */

	/*
	 * Statistics
	 */

	/* How many we found during crawling and how many were black
	 * listed (ignored). Reset to 0 when processing stops. */
	guint total_directories_found;
	guint total_directories_ignored;
	guint total_files_found;
	guint total_files_ignored;

	/* How many we indexed and how many had errors indexing. */
	guint changes_processed;
	guint total_files_notified_error;
};

typedef enum {
	QUEUE_ACTION_NONE           = 0,
	QUEUE_ACTION_DELETE_FIRST   = 1 << 0,
	QUEUE_ACTION_DELETE_SECOND  = 1 << 1,
} QueueCoalesceAction;

typedef enum {
	TRACKER_MINER_FS_EVENT_CREATED,
	TRACKER_MINER_FS_EVENT_UPDATED,
	TRACKER_MINER_FS_EVENT_DELETED,
	TRACKER_MINER_FS_EVENT_MOVED,
} TrackerMinerFSEventType;

enum {
	FINISHED,
	FINISHED_ROOT,
	LAST_SIGNAL
};

enum {
	PROP_0,
	PROP_THROTTLE,
	PROP_FILE_ATTRIBUTES,
	PROP_INDEXING_TREE,
};

static void           fs_finalize                         (GObject              *object);
static void           fs_constructed                      (GObject              *object);
static void           fs_set_property                     (GObject              *object,
                                                           guint                 prop_id,
                                                           const GValue         *value,
                                                           GParamSpec           *pspec);
static void           fs_get_property                     (GObject              *object,
                                                           guint                 prop_id,
                                                           GValue               *value,
                                                           GParamSpec           *pspec);

static void           miner_started                       (TrackerMiner         *miner);
static void           miner_stopped                       (TrackerMiner         *miner);
static void           miner_paused                        (TrackerMiner         *miner);
static void           miner_resumed                       (TrackerMiner         *miner);

static void           indexing_tree_directory_removed     (TrackerIndexingTree  *indexing_tree,
                                                           GFile                *directory,
                                                           gpointer              user_data);
static void           file_notifier_file_created          (TrackerFileNotifier  *notifier,
                                                           GFile                *file,
                                                           GFileInfo            *info,
                                                           gpointer              user_data);
static void           file_notifier_file_deleted          (TrackerFileNotifier  *notifier,
                                                           GFile                *file,
                                                           gboolean              is_dir,
                                                           gpointer              user_data);
static void           file_notifier_file_updated          (TrackerFileNotifier  *notifier,
                                                           GFile                *file,
                                                           GFileInfo            *info,
                                                           gboolean              attributes_only,
                                                           gpointer              user_data);
static void           file_notifier_file_moved            (TrackerFileNotifier  *notifier,
                                                           GFile                *source,
                                                           GFile                *dest,
                                                           gboolean              is_dir,
                                                           gpointer              user_data);
static void           file_notifier_directory_started     (TrackerFileNotifier *notifier,
                                                           GFile               *directory,
                                                           gpointer             user_data);
static void           file_notifier_directory_finished    (TrackerFileNotifier *notifier,
                                                           GFile               *directory,
                                                           guint                directories_found,
                                                           guint                directories_ignored,
                                                           guint                files_found,
                                                           guint                files_ignored,
                                                           gpointer             user_data);
static void           file_notifier_finished              (TrackerFileNotifier *notifier,
                                                           gpointer             user_data);

static void           item_queue_handlers_set_up          (TrackerMinerFS       *fs);

static void           task_pool_limit_reached_notify_cb       (GObject        *object,
                                                               GParamSpec     *pspec,
                                                               gpointer        user_data);

static GQuark quark_last_queue_event = 0;
static guint signals[LAST_SIGNAL] = { 0, };

G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE (TrackerMinerFS, tracker_miner_fs, TRACKER_TYPE_MINER)

/* For TRACKER_DEBUG=miner-fs-events */
#ifdef G_ENABLE_DEBUG
#define EVENT_QUEUE_LOG_PREFIX "[Event Queues] "
#define EVENT_QUEUE_STATUS_TIMEOUT_SECS 30

static void
debug_print_event (QueueEvent *event)
{
	const gchar *event_type_name[] = { "CREATED", "UPDATED", "DELETED", "MOVED" };
	gchar *uri1 = g_file_get_uri (event->file);
	gchar *uri2 = event->dest_file ? g_file_get_uri (event->dest_file) : NULL;
	g_message ("%s New %s event: %s%s%s%s",
	            EVENT_QUEUE_LOG_PREFIX,
	            event_type_name[event->type],
	            event->attributes_update ? "(attributes only) " : "",
	            uri1,
	            uri2 ? "->" : "",
	            uri2 ? uri2 : "");
	g_free (uri1);
	g_free (uri2);
}

#define trace_eq(message, ...) TRACKER_NOTE (MINER_FS_EVENTS, g_message (EVENT_QUEUE_LOG_PREFIX message, ##__VA_ARGS__))
#define trace_eq_event(event) TRACKER_NOTE (MINER_FS_EVENTS, debug_print_event (event));

#else
#define trace_eq(...)
#define trace_eq_event(...)
#endif /* G_ENABLE_DEBUG */

static void
tracker_miner_fs_class_init (TrackerMinerFSClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	TrackerMinerClass *miner_class = TRACKER_MINER_CLASS (klass);

	object_class->finalize = fs_finalize;
	object_class->constructed = fs_constructed;
	object_class->set_property = fs_set_property;
	object_class->get_property = fs_get_property;

	miner_class->started = miner_started;
	miner_class->stopped = miner_stopped;
	miner_class->paused  = miner_paused;
	miner_class->resumed = miner_resumed;

	g_object_class_install_property (object_class,
	                                 PROP_THROTTLE,
	                                 g_param_spec_double ("throttle",
	                                                      "Throttle",
	                                                      "Modifier for the indexing speed, 0 is max speed",
	                                                      0, 1, 0,
	                                                      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
	g_object_class_install_property (object_class,
	                                 PROP_FILE_ATTRIBUTES,
	                                 g_param_spec_string ("file-attributes",
	                                                      "File attributes",
	                                                      "File attributes",
	                                                      NULL,
	                                                      G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
	g_object_class_install_property (object_class,
	                                 PROP_INDEXING_TREE,
	                                 g_param_spec_object ("indexing-tree",
	                                                      NULL, NULL,
	                                                      TRACKER_TYPE_INDEXING_TREE,
	                                                      G_PARAM_READWRITE |
	                                                      G_PARAM_CONSTRUCT_ONLY |
	                                                      G_PARAM_STATIC_STRINGS));

	/**
	 * TrackerMinerFS::finished:
	 * @miner_fs: the #TrackerMinerFS
	 * @elapsed: elapsed time since mining was started
	 * @directories_found: number of directories found
	 * @directories_ignored: number of ignored directories
	 * @files_found: number of files found
	 * @files_ignored: number of ignored files
	 * @changes: number of changes processed
	 *
	 * The ::finished signal is emitted when @miner_fs has finished
	 * all pending processing.
	 *
	 * Since: 0.8
	 **/
	signals[FINISHED] =
		g_signal_new ("finished",
		              G_TYPE_FROM_CLASS (object_class),
		              G_SIGNAL_RUN_LAST,
		              G_STRUCT_OFFSET (TrackerMinerFSClass, finished),
		              NULL, NULL,
		              NULL,
		              G_TYPE_NONE,
		              6,
		              G_TYPE_DOUBLE,
		              G_TYPE_UINT,
		              G_TYPE_UINT,
		              G_TYPE_UINT,
		              G_TYPE_UINT,
		              G_TYPE_UINT);

	/**
	 * TrackerMinerFS::finished-root:
	 * @miner_fs: the #TrackerMinerFS
	 * @file: a #GFile
	 *
	 * The ::finished-crawl signal is emitted when @miner_fs has
	 * finished finding all resources that need to be indexed
	 * with the root location of @file. At this point, it's likely
	 * many are still in the queue to be added to the database,
	 * but this gives some indication that a location is
	 * processed.
	 *
	 * Since: 1.2
	 **/
	signals[FINISHED_ROOT] =
		g_signal_new ("finished-root",
		              G_TYPE_FROM_CLASS (object_class),
		              G_SIGNAL_RUN_LAST,
		              G_STRUCT_OFFSET (TrackerMinerFSClass, finished_root),
		              NULL, NULL,
		              NULL,
		              G_TYPE_NONE,
		              1,
		              G_TYPE_FILE);

	quark_last_queue_event = g_quark_from_static_string ("tracker-last-queue-event");
}

static void
tracker_miner_fs_init (TrackerMinerFS *object)
{
	TrackerMinerFSPrivate *priv;

	object->priv = tracker_miner_fs_get_instance_private (object);

	priv = object->priv;

	priv->timer = g_timer_new ();
	priv->extraction_timer = g_timer_new ();

	g_timer_stop (priv->timer);
	g_timer_stop (priv->extraction_timer);

	priv->timer_stopped = TRUE;
	priv->extraction_timer_stopped = TRUE;

	priv->items = tracker_priority_queue_new ();
	priv->items_by_file = g_hash_table_new_full (g_file_hash,
	                                             (GEqualFunc) g_file_equal,
	                                             g_object_unref, NULL);

	priv->roots_to_notify = g_hash_table_new_full (g_file_hash,
	                                               (GEqualFunc) g_file_equal,
	                                               g_object_unref,
	                                               (GDestroyNotify) g_queue_free);
	priv->urn_lru = tracker_lru_new (DEFAULT_URN_LRU_SIZE,
	                                 g_file_hash,
	                                 (GEqualFunc) g_file_equal,
	                                 g_object_unref,
	                                 g_free);
}

static QueueEvent *
queue_event_new (TrackerMinerFSEventType  type,
                 GFile                   *file,
                 GFileInfo               *info)
{
	QueueEvent *event;

	g_assert (type != TRACKER_MINER_FS_EVENT_MOVED);

	event = g_new0 (QueueEvent, 1);
	event->type = type;
	g_set_object (&event->file, file);
	g_set_object (&event->info, info);

	return event;
}

static QueueEvent *
queue_event_moved_new (GFile    *source,
                       GFile    *dest,
                       gboolean  is_dir)
{
	QueueEvent *event;

	event = g_new0 (QueueEvent, 1);
	event->type = TRACKER_MINER_FS_EVENT_MOVED;
	event->is_dir = !!is_dir;
	g_set_object (&event->dest_file, dest);
	g_set_object (&event->file, source);

	return event;
}

static void
queue_event_free (QueueEvent *event)
{
	if (event->root_node) {
		GQueue *root_queue;

		root_queue = event->root_node->data;
		g_queue_delete_link (root_queue, event->root_node);
	}

	g_clear_object (&event->dest_file);
	g_clear_object (&event->file);
	g_clear_object (&event->info);
	g_free (event);
}

static QueueCoalesceAction
queue_event_coalesce (const QueueEvent  *first,
		      const QueueEvent  *second,
		      QueueEvent       **replacement)
{
	if (!g_file_equal (first->file, second->file))
		return QUEUE_ACTION_NONE;

	*replacement = NULL;

	if (first->type == TRACKER_MINER_FS_EVENT_CREATED) {
		if (second->type == TRACKER_MINER_FS_EVENT_CREATED ||
		    (second->type == TRACKER_MINER_FS_EVENT_UPDATED &&
		     !second->attributes_update)) {
			return QUEUE_ACTION_DELETE_FIRST;
		} else if (second->type == TRACKER_MINER_FS_EVENT_MOVED) {
			*replacement = queue_event_new (TRACKER_MINER_FS_EVENT_CREATED,
			                                second->dest_file,
			                                NULL);
			return (QUEUE_ACTION_DELETE_FIRST |
				QUEUE_ACTION_DELETE_SECOND);
		} else if (second->type == TRACKER_MINER_FS_EVENT_DELETED) {
			/* We can't be sure that "create" is replacing a file
			 * here. Preserve the second event just in case.
			 */
			return QUEUE_ACTION_DELETE_FIRST;
		}
	} else if (first->type == TRACKER_MINER_FS_EVENT_UPDATED) {
		if (second->type == TRACKER_MINER_FS_EVENT_UPDATED) {
			if (first->attributes_update && !second->attributes_update)
				return QUEUE_ACTION_DELETE_FIRST;
			else
				return QUEUE_ACTION_DELETE_SECOND;
		} else if (second->type == TRACKER_MINER_FS_EVENT_DELETED) {
			return QUEUE_ACTION_DELETE_FIRST;
		}
	} else if (first->type == TRACKER_MINER_FS_EVENT_MOVED) {
		if (second->type == TRACKER_MINER_FS_EVENT_MOVED) {
			if (first->file != second->dest_file) {
				*replacement = queue_event_moved_new (first->file,
				                                      second->dest_file,
				                                      first->is_dir);
			}

			return (QUEUE_ACTION_DELETE_FIRST |
				QUEUE_ACTION_DELETE_SECOND);
		} else if (second->type == TRACKER_MINER_FS_EVENT_DELETED) {
			*replacement = queue_event_new (TRACKER_MINER_FS_EVENT_DELETED,
			                                first->file,
			                                NULL);
			return (QUEUE_ACTION_DELETE_FIRST |
				QUEUE_ACTION_DELETE_SECOND);
		}
	} else if (first->type == TRACKER_MINER_FS_EVENT_DELETED &&
		   second->type == TRACKER_MINER_FS_EVENT_DELETED) {
		return QUEUE_ACTION_DELETE_SECOND;
	}

	return QUEUE_ACTION_NONE;
}

static gboolean
queue_event_is_equal_or_descendant (QueueEvent *event,
				    GFile      *prefix)
{
	return (g_file_equal (event->file, prefix) ||
		g_file_has_prefix (event->file, prefix));
}

static void
fs_finalize (GObject *object)
{
	TrackerMinerFSPrivate *priv;

	priv = TRACKER_MINER_FS (object)->priv;

	g_timer_destroy (priv->timer);
	g_timer_destroy (priv->extraction_timer);

	g_clear_pointer (&priv->urn_lru, tracker_lru_unref);

	if (priv->item_queues_handler_id) {
		g_source_remove (priv->item_queues_handler_id);
		priv->item_queues_handler_id = 0;
	}

	if (priv->file_notifier) {
		tracker_file_notifier_stop (priv->file_notifier);
	}

	if (priv->sparql_buffer) {
		g_object_unref (priv->sparql_buffer);
	}

	g_hash_table_unref (priv->items_by_file);
	tracker_priority_queue_foreach (priv->items,
					(GFunc) queue_event_free,
					NULL);
	tracker_priority_queue_unref (priv->items);

	if (priv->indexing_tree) {
		g_object_unref (priv->indexing_tree);
	}

	if (priv->file_notifier) {
		g_object_unref (priv->file_notifier);
	}

	g_hash_table_unref (priv->roots_to_notify);
	g_free (priv->file_attributes);

	G_OBJECT_CLASS (tracker_miner_fs_parent_class)->finalize (object);
}

static void
fs_constructed (GObject *object)
{
	TrackerMinerFSPrivate *priv;

	/* NOTE: We have to do this in this order because initables
	 * are called _AFTER_ constructed and for subclasses that are
	 * not initables we don't have any other way than to chain
	 * constructed and root/indexing tree must exist at that
	 * point.
	 *
	 * If priv->indexing_tree is NULL after this function, the
	 * initiable functions will fail and this class will not be
	 * created anyway.
	 */
	G_OBJECT_CLASS (tracker_miner_fs_parent_class)->constructed (object);

	priv = TRACKER_MINER_FS (object)->priv;

	g_signal_connect (priv->indexing_tree, "directory-removed",
	                  G_CALLBACK (indexing_tree_directory_removed),
	                  object);

	priv->sparql_buffer = tracker_sparql_buffer_new (tracker_miner_get_connection (TRACKER_MINER (object)),
	                                                 BUFFER_POOL_LIMIT);
	g_signal_connect (priv->sparql_buffer, "notify::limit-reached",
	                  G_CALLBACK (task_pool_limit_reached_notify_cb),
	                  object);

	/* Create the file notifier */
	priv->file_notifier = tracker_file_notifier_new (priv->indexing_tree,
	                                                 tracker_miner_get_connection (TRACKER_MINER (object)),
	                                                 priv->file_attributes);

	g_signal_connect (priv->file_notifier, "file-created",
	                  G_CALLBACK (file_notifier_file_created),
	                  object);
	g_signal_connect (priv->file_notifier, "file-updated",
	                  G_CALLBACK (file_notifier_file_updated),
	                  object);
	g_signal_connect (priv->file_notifier, "file-deleted",
	                  G_CALLBACK (file_notifier_file_deleted),
	                  object);
	g_signal_connect (priv->file_notifier, "file-moved",
	                  G_CALLBACK (file_notifier_file_moved),
	                  object);
	g_signal_connect (priv->file_notifier, "directory-started",
	                  G_CALLBACK (file_notifier_directory_started),
	                  object);
	g_signal_connect (priv->file_notifier, "directory-finished",
	                  G_CALLBACK (file_notifier_directory_finished),
	                  object);
	g_signal_connect (priv->file_notifier, "finished",
	                  G_CALLBACK (file_notifier_finished),
	                  object);
}

static void
fs_set_property (GObject      *object,
                 guint         prop_id,
                 const GValue *value,
                 GParamSpec   *pspec)
{
	TrackerMinerFS *fs = TRACKER_MINER_FS (object);

	switch (prop_id) {
	case PROP_THROTTLE:
		tracker_miner_fs_set_throttle (TRACKER_MINER_FS (object),
		                               g_value_get_double (value));
		break;
	case PROP_FILE_ATTRIBUTES:
		fs->priv->file_attributes = g_value_dup_string (value);
		break;
	case PROP_INDEXING_TREE:
		fs->priv->indexing_tree = g_value_dup_object (value);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
fs_get_property (GObject    *object,
                 guint       prop_id,
                 GValue     *value,
                 GParamSpec *pspec)
{
	TrackerMinerFS *fs;

	fs = TRACKER_MINER_FS (object);

	switch (prop_id) {
	case PROP_THROTTLE:
		g_value_set_double (value, fs->priv->throttle);
		break;
	case PROP_FILE_ATTRIBUTES:
		g_value_set_string (value, fs->priv->file_attributes);
		break;
	case PROP_INDEXING_TREE:
		g_value_set_object (value, fs->priv->indexing_tree);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
task_pool_limit_reached_notify_cb (GObject    *object,
				   GParamSpec *pspec,
				   gpointer    user_data)
{
	if (!tracker_task_pool_limit_reached (TRACKER_TASK_POOL (object))) {
		item_queue_handlers_set_up (TRACKER_MINER_FS (user_data));
	}
}

static void
miner_started (TrackerMiner *miner)
{
	TrackerMinerFS *fs;

	fs = TRACKER_MINER_FS (miner);

	if (fs->priv->timer_stopped) {
		g_timer_start (fs->priv->timer);
		fs->priv->timer_stopped = FALSE;
	}

	g_object_set (miner,
	              "progress", 0.0,
	              "status", "Initializing",
	              "remaining-time", 0,
	              NULL);

	tracker_file_notifier_start (fs->priv->file_notifier);
}

static void
miner_stopped (TrackerMiner *miner)
{
	g_object_set (miner,
	              "progress", 1.0,
	              "status", "Idle",
	              "remaining-time", -1,
	              NULL);
}

static void
miner_paused (TrackerMiner *miner)
{
	TrackerMinerFS *fs;

	fs = TRACKER_MINER_FS (miner);

	fs->priv->is_paused = TRUE;

	tracker_file_notifier_stop (fs->priv->file_notifier);

	if (fs->priv->item_queues_handler_id) {
		g_source_remove (fs->priv->item_queues_handler_id);
		fs->priv->item_queues_handler_id = 0;
	}
}

static void
miner_resumed (TrackerMiner *miner)
{
	TrackerMinerFS *fs;

	fs = TRACKER_MINER_FS (miner);

	fs->priv->is_paused = FALSE;

	tracker_file_notifier_start (fs->priv->file_notifier);

	/* Only set up queue handler if we have items waiting to be
	 * processed.
	 */
	if (tracker_miner_fs_has_items_to_process (fs)) {
		item_queue_handlers_set_up (fs);
	}
}

static void
notify_roots_finished (TrackerMinerFS *fs)
{
	GHashTableIter iter;
	gpointer key, value;

	g_hash_table_iter_init (&iter, fs->priv->roots_to_notify);

	while (g_hash_table_iter_next (&iter, &key, &value)) {
		GFile *root = key;
		GQueue *queue = value;

		if (!g_queue_is_empty (queue))
			continue;

		/* Signal root is finished */
		g_signal_emit (fs, signals[FINISHED_ROOT], 0, root);

		/* Remove from hash table */
		g_hash_table_iter_remove (&iter);
	}
}

static void
log_stats (TrackerMinerFS *fs)
{
#ifdef G_ENABLE_DEBUG
	if (TRACKER_DEBUG_CHECK (STATISTICS)) {
		/* Only do this the first time, otherwise the results are
		 * likely to be inaccurate. Devices can be added or removed so
		 * we can't assume stats are correct.
		 */
		if (!fs->priv->shown_totals) {
			fs->priv->shown_totals = TRUE;

			g_info ("--------------------------------------------------");
			g_info ("Total directories : %d (%d ignored)",
			        fs->priv->total_directories_found,
			        fs->priv->total_directories_ignored);
			g_info ("Total files       : %d (%d ignored)",
			        fs->priv->total_files_found,
			        fs->priv->total_files_ignored);
			g_info ("Changes processed : %d (%d errors)",
			        fs->priv->changes_processed,
			        fs->priv->total_files_notified_error);
			g_info ("--------------------------------------------------\n");
		}
	}
#endif
}

static void
process_stop (TrackerMinerFS *fs)
{
	/* Now we have finished crawling, we enable monitor events */

	log_stats (fs);

	g_timer_stop (fs->priv->timer);
	g_timer_stop (fs->priv->extraction_timer);

	fs->priv->timer_stopped = TRUE;
	fs->priv->extraction_timer_stopped = TRUE;

	g_object_set (fs,
	              "progress", 1.0,
	              "status", "Idle",
	              "remaining-time", 0,
	              NULL);

	/* Make sure we signal _ALL_ roots as finished before the
	 * main FINISHED signal
	 */
	notify_roots_finished (fs);

	g_signal_emit (fs, signals[FINISHED], 0,
	               g_timer_elapsed (fs->priv->timer, NULL),
	               fs->priv->total_directories_found,
	               fs->priv->total_directories_ignored,
	               fs->priv->total_files_found,
	               fs->priv->total_files_ignored,
	               fs->priv->changes_processed);

	fs->priv->total_directories_found = 0;
	fs->priv->total_directories_ignored = 0;
	fs->priv->total_files_found = 0;
	fs->priv->total_files_ignored = 0;
	fs->priv->changes_processed = 0;
	fs->priv->total_files_notified_error = 0;
}

static void
check_notifier_high_water (TrackerMinerFS *fs)
{
	gboolean high_water;

	/* If there is more than worth 2 batches left processing, we can tell
	 * the notifier to stop a bit.
	 */
	high_water = (tracker_priority_queue_get_length (fs->priv->items) >
	              2 * BUFFER_POOL_LIMIT);
	tracker_file_notifier_set_high_water (fs->priv->file_notifier, high_water);
}

static void
sparql_buffer_flush_cb (GObject      *object,
                        GAsyncResult *result,
                        gpointer      user_data)
{
	TrackerMinerFS *fs = user_data;
	GPtrArray *tasks;
	GError *error = NULL;
	TrackerTask *task;
	GFile *task_file;
	guint i;

	tasks = tracker_sparql_buffer_flush_finish (TRACKER_SPARQL_BUFFER (object),
	                                            result, &error);

	if (error) {
		g_warning ("Could not execute sparql: %s", error->message);
	}

	for (i = 0; i < tasks->len; i++) {
		task = g_ptr_array_index (tasks, i);
		task_file = tracker_task_get_file (task);

		if (error) {
			gchar *sparql;

			sparql = tracker_sparql_task_get_sparql (task);
			tracker_error_report (task_file, error->message, sparql);
			fs->priv->total_files_notified_error++;
			g_free (sparql);
		} else {
			tracker_error_report_delete (task_file);
		}
	}

	fs->priv->flushing = FALSE;

	if (tracker_task_pool_limit_reached (TRACKER_TASK_POOL (object))) {
		if (tracker_sparql_buffer_flush (TRACKER_SPARQL_BUFFER (object),
						 "SPARQL buffer again full after flush",
						 sparql_buffer_flush_cb,
						 fs))
			fs->priv->flushing = TRUE;

		/* Check if we've finished inserting for given prefixes ... */
		notify_roots_finished (fs);
	}

	check_notifier_high_water (fs);
	item_queue_handlers_set_up (fs);

	g_ptr_array_unref (tasks);
	g_clear_error (&error);
}

static gboolean
item_add_or_update (TrackerMinerFS *fs,
                    GFile          *file,
                    GFileInfo      *info,
                    gboolean        attributes_update,
                    gboolean        create)
{
	gchar *uri;

	if (info) {
		g_object_ref (info);
	} else {
		info = g_file_query_info (file,
		                          fs->priv->file_attributes,
		                          G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
		                          NULL, NULL);
		if (!info)
			return TRUE;
	}

	uri = g_file_get_uri (file);

	if (!attributes_update) {
		TRACKER_NOTE (MINER_FS_EVENTS, g_message ("Processing file '%s'...", uri));
		TRACKER_MINER_FS_GET_CLASS (fs)->process_file (fs, file, info,
		                                               fs->priv->sparql_buffer,
		                                               create);
	} else {
		TRACKER_NOTE (MINER_FS_EVENTS, g_message ("Processing attributes in file '%s'...", uri));
		TRACKER_MINER_FS_GET_CLASS (fs)->process_file_attributes (fs, file, info,
		                                                          fs->priv->sparql_buffer);
	}

	g_free (uri);
	g_object_unref (info);

	return TRUE;
}

static gboolean
item_remove (TrackerMinerFS *fs,
             GFile          *file,
             gboolean        is_dir,
             gboolean        only_children)
{
	gchar *uri;

	uri = g_file_get_uri (file);

	TRACKER_NOTE (MINER_FS_EVENTS,
	              g_message ("Removing item: '%s' (Deleted from filesystem or no longer monitored)", uri));

	tracker_lru_remove_foreach (fs->priv->urn_lru,
	                            (GEqualFunc) g_file_has_parent,
	                            file);
	tracker_lru_remove (fs->priv->urn_lru, file);

	/* Call the implementation to generate a SPARQL update for the removal. */
	if (only_children) {
		TRACKER_MINER_FS_GET_CLASS (fs)->remove_children (fs, file,
		                                                  fs->priv->sparql_buffer);
	} else {
		TRACKER_MINER_FS_GET_CLASS (fs)->remove_file (fs, file,
		                                              fs->priv->sparql_buffer,
		                                              is_dir);
	}

	g_free (uri);

	return TRUE;
}

static gboolean
item_move (TrackerMinerFS *fs,
           GFile          *dest_file,
           GFile          *source_file,
           gboolean        is_dir)
{
	gchar *uri, *source_uri;
	TrackerDirectoryFlags source_flags, flags;
	gboolean recursive;

	uri = g_file_get_uri (dest_file);
	source_uri = g_file_get_uri (source_file);

	TRACKER_NOTE (MINER_FS_EVENTS,
	              g_message ("Moving item from '%s' to '%s'",
	                         source_uri, uri));

	tracker_indexing_tree_get_root (fs->priv->indexing_tree, source_file, &source_flags);
	tracker_indexing_tree_get_root (fs->priv->indexing_tree, dest_file, &flags);
	recursive = ((source_flags & TRACKER_DIRECTORY_FLAG_RECURSE) != 0 &&
	             (flags & TRACKER_DIRECTORY_FLAG_RECURSE) != 0 &&
	             is_dir);

	if (!is_dir) {
		/* Delete destination item from store if any */
		item_remove (fs, dest_file, is_dir, FALSE);
	}

	/* If the original location is recursive, but the destination location
	 * is not, remove all children.
	 */
	if (!recursive &&
	    (source_flags & TRACKER_DIRECTORY_FLAG_RECURSE) != 0)
		item_remove (fs, source_file, is_dir, TRUE);

	TRACKER_MINER_FS_GET_CLASS (fs)->move_file (fs, dest_file, source_file,
	                                            fs->priv->sparql_buffer,
	                                            recursive);
	g_free (uri);
	g_free (source_uri);

	return TRUE;
}

static gboolean
maybe_remove_file_event_node (TrackerMinerFS *fs,
                              QueueEvent     *event)
{
	QueueEvent *item_event;

	item_event = g_hash_table_lookup (fs->priv->items_by_file, event->file);

	if (item_event == event) {
		g_hash_table_remove (fs->priv->items_by_file, event->file);
		return TRUE;
	}

	return FALSE;
}

static gboolean
remove_items_by_file_foreach (gpointer key,
                              gpointer value,
                              gpointer user_data)
{
	GFile *file = key;
	GFile *prefix = user_data;

	return (g_file_equal (file, prefix) ||
	        g_file_has_prefix (file, prefix));
}

static void
item_queue_get_next_file (TrackerMinerFS           *fs,
                          GFile                   **file,
                          GFile                   **source_file,
                          GFileInfo               **info,
                          TrackerMinerFSEventType  *type,
                          gboolean                 *attributes_update,
                          gboolean                 *is_dir)
{
	QueueEvent *event;

	*file = NULL;
	*source_file = NULL;

	event = tracker_priority_queue_pop (fs->priv->items, NULL);

	if (event) {
		if (event->type == TRACKER_MINER_FS_EVENT_MOVED) {
			g_set_object (file, event->dest_file);
			g_set_object (source_file, event->file);
		} else {
			g_set_object (file, event->file);
		}

		*type = event->type;
		*attributes_update = event->attributes_update;
		*is_dir = event->is_dir;
		g_set_object (info, event->info);

		maybe_remove_file_event_node (fs, event);
		queue_event_free (event);
	}
}

static gdouble
item_queue_get_progress (TrackerMinerFS *fs,
                         guint          *n_items_processed,
                         guint          *n_items_remaining)
{
	guint items_to_process = 0;
	guint items_total = 0;

	items_to_process += tracker_priority_queue_get_length (fs->priv->items);

	items_total += fs->priv->total_directories_found;
	items_total += fs->priv->total_files_found;

	if (n_items_processed) {
		*n_items_processed = ((items_total >= items_to_process) ?
		                      (items_total - items_to_process) : 0);
	}

	if (n_items_remaining) {
		*n_items_remaining = items_to_process;
	}

	if (items_total == 0 ||
	    items_to_process == 0 ||
	    items_to_process > items_total) {
		return 1.0;
	}

	return (gdouble) (items_total - items_to_process) / items_total;
}

static gboolean
miner_handle_next_item (TrackerMinerFS *fs)
{
	GFile *file = NULL;
	GFile *source_file = NULL;
	gint64 time_now;
	static gint64 time_last = 0;
	gboolean keep_processing = TRUE;
	gboolean attributes_update = FALSE;
	gboolean is_dir = FALSE;
	TrackerMinerFSEventType type;
	GFileInfo *info = NULL;

	item_queue_get_next_file (fs, &file, &source_file, &info, &type,
	                          &attributes_update, &is_dir);

	if (fs->priv->timer_stopped) {
		g_timer_start (fs->priv->timer);
		fs->priv->timer_stopped = FALSE;
	}

	if (file == NULL && !fs->priv->extraction_timer_stopped) {
		g_timer_stop (fs->priv->extraction_timer);
		fs->priv->extraction_timer_stopped = TRUE;
	} else if (file != NULL && fs->priv->extraction_timer_stopped) {
		g_timer_continue (fs->priv->extraction_timer);
		fs->priv->extraction_timer_stopped = FALSE;
	}

	/* Update progress, but don't spam it. */
	time_now = g_get_monotonic_time ();

	if ((time_now - time_last) >= 1000000) {
		guint items_processed, items_remaining;
		gdouble progress_now;
		static gdouble progress_last = 0.0;
		static gint info_last = 0;
		gdouble seconds_elapsed, extraction_elapsed;

		time_last = time_now;

		/* Update progress? */
		progress_now = item_queue_get_progress (fs,
		                                        &items_processed,
		                                        &items_remaining);
		seconds_elapsed = g_timer_elapsed (fs->priv->timer, NULL);
		extraction_elapsed = g_timer_elapsed (fs->priv->extraction_timer, NULL);

		if (!tracker_file_notifier_is_active (fs->priv->file_notifier)) {
			gchar *status;
			gint remaining_time;

			g_object_get (fs, "status", &status, NULL);

			/* Compute remaining time */
			remaining_time = (gint)tracker_seconds_estimate (extraction_elapsed,
			                                                 items_processed,
			                                                 items_remaining);

			/* CLAMP progress so it doesn't go back below
			 * 2% (which we use for crawling)
			 */
			if (g_strcmp0 (status, "Processing…") != 0) {
				/* Don't spam this */
				g_object_set (fs,
				              "status", "Processing…",
				              "progress", CLAMP (progress_now, 0.02, 1.00),
				              "remaining-time", remaining_time,
				              NULL);
			} else {
				g_object_set (fs,
				              "progress", CLAMP (progress_now, 0.02, 1.00),
				              "remaining-time", remaining_time,
				              NULL);
			}

			g_free (status);
		}

		if (++info_last >= 5 &&
		    (gint) (progress_last * 100) != (gint) (progress_now * 100)) {
			gchar *str1, *str2;

			info_last = 0;
			progress_last = progress_now;

			/* Log estimated remaining time */
			str1 = tracker_seconds_estimate_to_string (extraction_elapsed,
			                                           TRUE,
			                                           items_processed,
			                                           items_remaining);
			str2 = tracker_seconds_to_string (seconds_elapsed, TRUE);

			g_info ("Processed %u/%u, estimated %s left, %s elapsed",
			        items_processed,
			        items_processed + items_remaining,
			        str1,
			        str2);

			g_free (str2);
			g_free (str1);
		}
	}

	if (file == NULL) {
		if (!tracker_file_notifier_is_active (fs->priv->file_notifier)) {
			if (!fs->priv->flushing &&
			    tracker_task_pool_get_size (TRACKER_TASK_POOL (fs->priv->sparql_buffer)) == 0) {
				/* Print stats and signal finished */
				process_stop (fs);
			} else {
				/* Flush any possible pending update here */
				if (tracker_sparql_buffer_flush (fs->priv->sparql_buffer,
								 "Queue handlers NONE",
								 sparql_buffer_flush_cb,
								 fs))
					fs->priv->flushing = TRUE;

				/* Check if we've finished inserting for given prefixes ... */
				notify_roots_finished (fs);
			}
		}

		/* No more files left to process */
		return FALSE;
	}

	fs->priv->changes_processed++;

	/* Handle queues */
	switch (type) {
	case TRACKER_MINER_FS_EVENT_MOVED:
		keep_processing = item_move (fs, file, source_file, is_dir);
		break;
	case TRACKER_MINER_FS_EVENT_DELETED:
		keep_processing = item_remove (fs, file, is_dir, FALSE);
		break;
	case TRACKER_MINER_FS_EVENT_CREATED:
		keep_processing = item_add_or_update (fs, file, info, FALSE, TRUE);
		break;
	case TRACKER_MINER_FS_EVENT_UPDATED:
		keep_processing = item_add_or_update (fs, file, info, attributes_update, FALSE);
		break;
	default:
		g_assert_not_reached ();
	}

	if (tracker_task_pool_limit_reached (TRACKER_TASK_POOL (fs->priv->sparql_buffer))) {
		if (tracker_sparql_buffer_flush (fs->priv->sparql_buffer,
						 "SPARQL buffer limit reached",
						 sparql_buffer_flush_cb,
						 fs)) {
			fs->priv->flushing = TRUE;
		} else {
			/* If we cannot flush, wait for the pending operations
			 * to finish.
			 */
			keep_processing = FALSE;
		}

		/* Check if we've finished inserting for given prefixes ... */
		notify_roots_finished (fs);
	}

	item_queue_handlers_set_up (fs);

	g_clear_object (&file);
	g_clear_object (&source_file);
	g_clear_object (&info);

	return keep_processing;
}

static gboolean
item_queue_handlers_cb (gpointer user_data)
{
	TrackerMinerFS *fs = user_data;
	gboolean retval = FALSE;
	gint i;

	for (i = 0; i < MAX_SIMULTANEOUS_ITEMS; i++) {
		retval = miner_handle_next_item (fs);
		if (retval == FALSE)
			break;
	}

	if (retval == FALSE) {
		fs->priv->item_queues_handler_id = 0;
	}

	return retval;
}

static guint
_tracker_idle_add (TrackerMinerFS *fs,
                   GSourceFunc     func,
                   gpointer        user_data)
{
	guint interval;

	interval = TRACKER_CRAWLER_MAX_TIMEOUT_INTERVAL * fs->priv->throttle;

	if (interval == 0) {
		return g_idle_add_full (TRACKER_TASK_PRIORITY, func, user_data, NULL);
	} else {
		return g_timeout_add_full (TRACKER_TASK_PRIORITY, interval, func, user_data, NULL);
	}
}

static void
item_queue_handlers_set_up (TrackerMinerFS *fs)
{
	trace_eq ("Setting up queue handlers...");
	if (fs->priv->item_queues_handler_id != 0) {
		trace_eq ("   cancelled: already one active");
		return;
	}

	if (fs->priv->is_paused) {
		trace_eq ("   cancelled: paused");
		return;
	}

	/* Already processing max number of sparql updates */
	if (tracker_task_pool_limit_reached (TRACKER_TASK_POOL (fs->priv->sparql_buffer))) {
		trace_eq ("   cancelled: pool limit reached (sparql buffer: %u)",
		          tracker_task_pool_get_limit (TRACKER_TASK_POOL (fs->priv->sparql_buffer)));
		return;
	}

	if (!tracker_file_notifier_is_active (fs->priv->file_notifier)) {
		gchar *status;
		gdouble progress;

		g_object_get (fs,
		              "progress", &progress,
		              "status", &status,
		              NULL);

		/* Don't spam this */
		if (progress > 0.01 && g_strcmp0 (status, "Processing…") != 0) {
			g_object_set (fs, "status", "Processing…", NULL);
		}

		g_free (status);
	}

	trace_eq ("   scheduled in idle");
	fs->priv->item_queues_handler_id =
		_tracker_idle_add (fs,
		                   item_queue_handlers_cb,
		                   fs);
}

static gint
miner_fs_get_queue_priority (TrackerMinerFS *fs,
                             GFile          *file)
{
	TrackerDirectoryFlags flags;

	tracker_indexing_tree_get_root (fs->priv->indexing_tree,
	                                file, &flags);

	return (flags & TRACKER_DIRECTORY_FLAG_PRIORITY) ?
	        G_PRIORITY_HIGH : G_PRIORITY_DEFAULT;
}

static void
assign_root_node (TrackerMinerFS *fs,
                  QueueEvent     *event)
{
	GFile *root, *file;
	GQueue *queue;

	file = event->dest_file ? event->dest_file : event->file;
	root = tracker_indexing_tree_get_root (fs->priv->indexing_tree,
	                                       file, NULL);
	if (!root)
		return;

	queue = g_hash_table_lookup (fs->priv->roots_to_notify,
	                             root);
	if (!queue) {
		queue = g_queue_new ();
		g_hash_table_insert (fs->priv->roots_to_notify,
		                     g_object_ref (root), queue);
	}

	event->root_node = g_list_alloc ();
	event->root_node->data = queue;
	g_queue_push_head_link (queue, event->root_node);
}

static void
miner_fs_queue_event (TrackerMinerFS *fs,
		      QueueEvent     *event,
		      guint           priority)
{
	QueueEvent *old = NULL;

	if (event->type == TRACKER_MINER_FS_EVENT_MOVED) {
		/* Remove all children of the dest location from being processed. */
		g_hash_table_foreach_remove (fs->priv->items_by_file,
		                             remove_items_by_file_foreach,
		                             event->dest_file);
		tracker_priority_queue_foreach_remove (fs->priv->items,
						       (GEqualFunc) queue_event_is_equal_or_descendant,
						       event->dest_file,
						       (GDestroyNotify) queue_event_free);
	}

	old = g_hash_table_lookup (fs->priv->items_by_file, event->file);

	if (old) {
		QueueCoalesceAction action;
		QueueEvent *replacement = NULL;

		action = queue_event_coalesce (old, event, &replacement);

		if (action & QUEUE_ACTION_DELETE_FIRST) {
			g_hash_table_remove (fs->priv->items_by_file, old->file);
			tracker_priority_queue_remove_node (fs->priv->items,
							    old->queue_node);
			queue_event_free (old);
		}

		if (action & QUEUE_ACTION_DELETE_SECOND) {
			queue_event_free (event);
			event = NULL;
		}

		if (replacement)
			event = replacement;
	}

	if (event) {
		if (event->is_dir &&
		    event->type == TRACKER_MINER_FS_EVENT_DELETED &&
		    g_hash_table_size (fs->priv->items_by_file) < BIG_QUEUE_THRESHOLD) {
			/* Attempt to optimize by removing any children
			 * of this directory from being processed.
			 */
			g_hash_table_foreach_remove (fs->priv->items_by_file,
			                             remove_items_by_file_foreach,
			                             event->file);
			tracker_priority_queue_foreach_remove (fs->priv->items,
							       (GEqualFunc) queue_event_is_equal_or_descendant,
							       event->file,
							       (GDestroyNotify) queue_event_free);
		}

		trace_eq_event (event);

		assign_root_node (fs, event);
		event->queue_node =
			tracker_priority_queue_add (fs->priv->items, event, priority);
		g_hash_table_replace (fs->priv->items_by_file,
		                      g_object_ref (event->file), event);
		item_queue_handlers_set_up (fs);
		check_notifier_high_water (fs);
	}
}

static void
file_notifier_file_created (TrackerFileNotifier  *notifier,
                            GFile                *file,
                            GFileInfo            *info,
                            gpointer              user_data)
{
	TrackerMinerFS *fs = user_data;
	QueueEvent *event;

	event = queue_event_new (TRACKER_MINER_FS_EVENT_CREATED, file, info);
	miner_fs_queue_event (fs, event, miner_fs_get_queue_priority (fs, file));
}

static void
file_notifier_file_deleted (TrackerFileNotifier  *notifier,
                            GFile                *file,
                            gboolean              is_dir,
                            gpointer              user_data)
{
	TrackerMinerFS *fs = user_data;
	QueueEvent *event;

	event = queue_event_new (TRACKER_MINER_FS_EVENT_DELETED, file, NULL);
	event->is_dir = !!is_dir;
	miner_fs_queue_event (fs, event, miner_fs_get_queue_priority (fs, file));
}

static void
file_notifier_file_updated (TrackerFileNotifier  *notifier,
                            GFile                *file,
                            GFileInfo            *info,
                            gboolean              attributes_only,
                            gpointer              user_data)
{
	TrackerMinerFS *fs = user_data;
	QueueEvent *event;

	event = queue_event_new (TRACKER_MINER_FS_EVENT_UPDATED, file, info);
	event->attributes_update = attributes_only;
	miner_fs_queue_event (fs, event, miner_fs_get_queue_priority (fs, file));
}

static void
file_notifier_file_moved (TrackerFileNotifier *notifier,
                          GFile               *source,
                          GFile               *dest,
                          gboolean             is_dir,
                          gpointer             user_data)
{
	TrackerMinerFS *fs = user_data;
	QueueEvent *event;

	event = queue_event_moved_new (source, dest, is_dir);
	miner_fs_queue_event (fs, event, miner_fs_get_queue_priority (fs, source));
}

static void
file_notifier_directory_started (TrackerFileNotifier *notifier,
                                 GFile               *directory,
                                 gpointer             user_data)
{
	TrackerMinerFS *fs = user_data;
	TrackerDirectoryFlags flags;
	gchar *str, *uri;

	uri = g_file_get_uri (directory);
	tracker_indexing_tree_get_root (fs->priv->indexing_tree,
					directory, &flags);

	if ((flags & TRACKER_DIRECTORY_FLAG_RECURSE) != 0) {
                str = g_strdup_printf ("Crawling recursively directory '%s'", uri);
        } else {
                str = g_strdup_printf ("Crawling single directory '%s'", uri);
        }

	/* Always set the progress here to at least 1%, and the remaining time
         * to -1 as we cannot guess during crawling (we don't know how many directories
         * we will find) */
        g_object_set (fs,
                      "progress", 0.01,
                      "status", str,
                      "remaining-time", -1,
                      NULL);
	g_free (str);
	g_free (uri);
}

static void
file_notifier_directory_finished (TrackerFileNotifier *notifier,
                                  GFile               *directory,
                                  guint                directories_found,
                                  guint                directories_ignored,
                                  guint                files_found,
                                  guint                files_ignored,
                                  gpointer             user_data)
{
	TrackerMinerFS *fs = user_data;
	gchar *str, *uri;

	/* Update stats */
	fs->priv->total_directories_found += directories_found;
	fs->priv->total_directories_ignored += directories_ignored;
	fs->priv->total_files_found += files_found;
	fs->priv->total_files_ignored += files_ignored;

	uri = g_file_get_uri (directory);
	str = g_strdup_printf ("Crawl finished for directory '%s'", uri);

        g_object_set (fs,
                      "progress", 0.01,
                      "status", str,
                      "remaining-time", -1,
                      NULL);

	g_free (str);
	g_free (uri);

	if (directories_found == 0 &&
	    files_found == 0) {
		/* Signal now because we have nothing to index */
		g_signal_emit (fs, signals[FINISHED_ROOT], 0, directory);
	}
}

static void
file_notifier_finished (TrackerFileNotifier *notifier,
                        gpointer             user_data)
{
	TrackerMinerFS *fs = user_data;

	item_queue_handlers_set_up (fs);
}

static void
indexing_tree_directory_removed (TrackerIndexingTree *indexing_tree,
                                 GFile               *directory,
                                 gpointer             user_data)
{
	TrackerMinerFS *fs = user_data;
	TrackerMinerFSPrivate *priv = fs->priv;
	GTimer *timer = g_timer_new ();

	TRACKER_NOTE (MINER_FS_EVENTS, g_message ("  Cancelled processing pool tasks at %f\n", g_timer_elapsed (timer, NULL)));

	/* Remove anything contained in the removed directory
	 * from all relevant processing queues.
	 */
	g_hash_table_foreach_remove (fs->priv->items_by_file,
	                             remove_items_by_file_foreach,
	                             directory);
	tracker_priority_queue_foreach_remove (priv->items,
					       (GEqualFunc) queue_event_is_equal_or_descendant,
					       directory,
					       (GDestroyNotify) queue_event_free);

	TRACKER_NOTE (MINER_FS_EVENTS, g_message ("  Removed files at %f\n", g_timer_elapsed (timer, NULL)));
	g_timer_destroy (timer);
}

/**
 * tracker_miner_fs_set_throttle:
 * @fs: a #TrackerMinerFS
 * @throttle: a double between 0.0 and 1.0
 *
 * Tells the filesystem miner to throttle its operations. A value of
 * 0.0 means no throttling at all, so the miner will perform
 * operations at full speed, 1.0 is the slowest value. With a value of
 * 1.0, the @fs is typically waiting one full second before handling
 * the next batch of queued items to be processed.
 *
 * Since: 0.8
 **/
void
tracker_miner_fs_set_throttle (TrackerMinerFS *fs,
                               gdouble         throttle)
{
	g_return_if_fail (TRACKER_IS_MINER_FS (fs));

	throttle = CLAMP (throttle, 0, 1);

	if (fs->priv->throttle == throttle) {
		return;
	}

	fs->priv->throttle = throttle;

	/* Update timeouts */
	if (fs->priv->item_queues_handler_id != 0) {
		g_source_remove (fs->priv->item_queues_handler_id);

		fs->priv->item_queues_handler_id =
			_tracker_idle_add (fs,
			                   item_queue_handlers_cb,
			                   fs);
	}
}

/**
 * tracker_miner_fs_get_throttle:
 * @fs: a #TrackerMinerFS
 *
 * Gets the current throttle value, see
 * tracker_miner_fs_set_throttle() for more details.
 *
 * Returns: a double representing a value between 0.0 and 1.0.
 *
 * Since: 0.8
 **/
gdouble
tracker_miner_fs_get_throttle (TrackerMinerFS *fs)
{
	g_return_val_if_fail (TRACKER_IS_MINER_FS (fs), 0);

	return fs->priv->throttle;
}

const gchar *
tracker_miner_fs_get_identifier (TrackerMinerFS *fs,
				 GFile          *file)
{
	GFileInfo *info;
	gchar *str;

	g_return_val_if_fail (TRACKER_IS_MINER_FS (fs), NULL);
	g_return_val_if_fail (G_IS_FILE (file), NULL);

	if (tracker_lru_find (fs->priv->urn_lru, file, (gpointer*) &str))
		return str;

	info = g_file_query_info (file,
	                          G_FILE_ATTRIBUTE_STANDARD_TYPE ","
	                          G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN ","
	                          G_FILE_ATTRIBUTE_ID_FILESYSTEM ","
	                          G_FILE_ATTRIBUTE_UNIX_INODE,
	                          G_FILE_QUERY_INFO_NONE,
	                          NULL,
	                          NULL);
	if (!info)
		return NULL;

	if (!tracker_indexing_tree_file_is_indexable (fs->priv->indexing_tree, file, info)) {
		g_object_unref (info);
		return NULL;
	}

	str = tracker_file_get_content_identifier (file, info, NULL);
	tracker_lru_add (fs->priv->urn_lru, g_object_ref (file), str);
	g_object_unref (info);

	return str;
}

/**
 * tracker_miner_fs_has_items_to_process:
 * @fs: a #TrackerMinerFS
 *
 * The @fs keeps many priority queus for content it is processing.
 * This function returns %TRUE if the sum of all (or any) priority
 * queues is more than 0. This includes items deleted, created,
 * updated, moved or being written back.
 *
 * Returns: %TRUE if there are items to process in the internal
 * queues, otherwise %FALSE.
 *
 * Since: 0.10
 **/
gboolean
tracker_miner_fs_has_items_to_process (TrackerMinerFS *fs)
{
	g_return_val_if_fail (TRACKER_IS_MINER_FS (fs), FALSE);

	if (tracker_file_notifier_is_active (fs->priv->file_notifier) ||
	    !tracker_priority_queue_is_empty (fs->priv->items)) {
		return TRUE;
	}

	return FALSE;
}

/**
 * tracker_miner_fs_get_indexing_tree:
 * @fs: a #TrackerMinerFS
 *
 * Returns the #TrackerIndexingTree which determines
 * what files/directories are indexed by @fs
 *
 * Returns: (transfer none): The #TrackerIndexingTree
 *          holding the indexing configuration
 **/
TrackerIndexingTree *
tracker_miner_fs_get_indexing_tree (TrackerMinerFS *fs)
{
	g_return_val_if_fail (TRACKER_IS_MINER_FS (fs), NULL);

	return fs->priv->indexing_tree;
}
