/*
 * Copyright (C) 2014 Carlos Garnacho  <carlosg@gnome.org>
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

#include <string.h>

#include <libtracker-miners-common/tracker-common.h>

#include "tracker-decorator.h"

#define QUERY_BATCH_SIZE 200
#define DEFAULT_BATCH_SIZE 200

/**
 * SECTION:tracker-decorator
 * @short_description: A miner tasked with listening for DB resource changes and extracting metadata
 * @include: libtracker-miner/tracker-miner.h
 * @title: TrackerDecorator
 * @see_also: #TrackerDecoratorFS
 *
 * #TrackerDecorator watches for signal updates based on content changes
 * in the database. When new files are added initially, only simple
 * metadata exists, for example, name, size, mtime, etc. The
 * #TrackerDecorator queues files for extended metadata extraction
 * (i.e. for tracker-extract to fetch metadata specific to the file
 * type) for example 'nmm:whiteBalance' for a picture.
**/

typedef struct _TrackerDecoratorPrivate TrackerDecoratorPrivate;
typedef struct _ClassInfo ClassInfo;

struct _TrackerDecoratorInfo {
	GTask *task;
	gchar *url;
	gint id;
	gint ref_count;
};

struct _TrackerDecoratorPrivate {
	TrackerNotifier *notifier;

	gssize n_remaining_items;
	gssize n_processed_items;

	GQueue item_cache; /* Queue of TrackerDecoratorInfo */

	GStrv priority_graphs;

	GPtrArray *sparql_buffer; /* Array of TrackerExtractInfo */
	GPtrArray *commit_buffer; /* Array of TrackerExtractInfo */
	GTimer *timer;

	TrackerSparqlStatement *remaining_items_query;
	TrackerSparqlStatement *item_count_query;

	GCancellable *cancellable;
	GCancellable *task_cancellable;

	gint batch_size;

	guint updating   : 1;
	guint processing : 1;
	guint querying   : 1;
};

enum {
	PROP_COMMIT_BATCH_SIZE = 1,
};

enum {
	ITEMS_AVAILABLE,
	FINISHED,
	LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

static void decorator_task_done (GObject      *object,
                                 GAsyncResult *result,
                                 gpointer      user_data);
static void decorator_cache_next_items (TrackerDecorator *decorator);
static gboolean decorator_check_commit (TrackerDecorator *decorator);

/**
 * tracker_decorator_error_quark:
 *
 * Gives the caller the #GQuark used to identify #TrackerDecorator errors
 * in #GError structures. The #GQuark is used as the domain for the error.
 *
 * Returns: the #GQuark used for the domain of a #GError.
 *
 * Since: 0.18
 **/
G_DEFINE_QUARK (TrackerDecoratorError, tracker_decorator_error)

G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE (TrackerDecorator, tracker_decorator, TRACKER_TYPE_MINER)

static TrackerDecoratorInfo *
tracker_decorator_info_new (TrackerDecorator    *decorator,
                            TrackerSparqlCursor *cursor)
{
	TrackerDecoratorPrivate *priv;
	TrackerDecoratorInfo *info;

	priv = tracker_decorator_get_instance_private (decorator);

	info = g_slice_new0 (TrackerDecoratorInfo);
	info->url = g_strdup (tracker_sparql_cursor_get_string (cursor, 0, NULL));
	info->id = tracker_sparql_cursor_get_integer (cursor, 1);
	info->ref_count = 1;

	info->task = g_task_new (decorator,
	                         priv->task_cancellable,
	                         decorator_task_done,
	                         info);

	return info;
}

/**
 * tracker_decorator_info_ref:
 * @info: a #TrackerDecoratorInfo
 *
 * Increases the reference count of @info by 1.
 *
 * Returns: the same @info passed in, or %NULL on error.
 *
 * Since: 0.18
 **/
TrackerDecoratorInfo *
tracker_decorator_info_ref (TrackerDecoratorInfo *info)
{
	g_atomic_int_inc (&info->ref_count);
	return info;
}

/**
 * tracker_decorator_info_unref:
 * @info: a #TrackerDecoratorInfo
 *
 * Decreases the reference count of @info by 1 and frees it when the
 * reference count reaches 0.
 *
 * Since: 0.18
 **/
void
tracker_decorator_info_unref (TrackerDecoratorInfo *info)
{
	if (!g_atomic_int_dec_and_test (&info->ref_count))
		return;

	if (info->task)
		g_object_unref (info->task);
	g_free (info->url);
	g_slice_free (TrackerDecoratorInfo, info);
}

G_DEFINE_BOXED_TYPE (TrackerDecoratorInfo,
                     tracker_decorator_info,
                     tracker_decorator_info_ref,
                     tracker_decorator_info_unref)

static void
decorator_update_state (TrackerDecorator *decorator,
                        const gchar      *message,
                        gboolean          estimate_time)
{
	TrackerDecoratorPrivate *priv;
	gint remaining_time = -1;
	gdouble progress = 1;
	gsize total_items;

	priv = tracker_decorator_get_instance_private (decorator);
	remaining_time = 0;
	total_items = priv->n_remaining_items + priv->n_processed_items;

	if (priv->n_remaining_items > 0)
		progress = ((gdouble) priv->n_processed_items / total_items);

	if (priv->timer && estimate_time &&
	    !tracker_miner_is_paused (TRACKER_MINER (decorator))) {
		gdouble elapsed;

		/* FIXME: Quite naive calculation */
		elapsed = g_timer_elapsed (priv->timer, NULL);

		if (priv->n_processed_items > 0)
			remaining_time = (priv->n_remaining_items * elapsed) / priv->n_processed_items;
	}

	g_object_set (decorator,
	              "progress", progress,
	              "remaining-time", remaining_time,
	              NULL);

	if (message)
		g_object_set (decorator, "status", message, NULL);
}

static void
retry_synchronously (TrackerDecorator *decorator,
                     GPtrArray        *commit_buffer)
{
	TrackerSparqlConnection *sparql_conn;
	guint i;

	sparql_conn = tracker_miner_get_connection (TRACKER_MINER (decorator));

	for (i = 0; i < commit_buffer->len; i++) {
		g_autoptr (TrackerBatch) batch = NULL;
		TrackerExtractInfo *info;
		g_autoptr (GError) error = NULL;

		info = g_ptr_array_index (commit_buffer, i);
		batch = tracker_sparql_connection_create_batch (sparql_conn);

		TRACKER_DECORATOR_GET_CLASS (decorator)->update (decorator, info, batch);

		tracker_batch_execute (batch, NULL, &error);

		if (error) {
			TRACKER_DECORATOR_GET_CLASS (decorator)->error (decorator,
			                                                info,
			                                                error->message);
		}
	}
}

static void
tag_success (TrackerDecorator *decorator,
             GPtrArray        *commit_buffer)
{
	guint i;

	for (i = 0; i < commit_buffer->len; i++) {
		TrackerExtractInfo *info;

		info = g_ptr_array_index (commit_buffer, i);
		tracker_error_report_delete (tracker_extract_info_get_file (info));
	}
}

static void
decorator_commit_cb (GObject      *object,
                     GAsyncResult *result,
                     gpointer      user_data)
{
	TrackerDecoratorPrivate *priv;
	TrackerDecorator *decorator;
	TrackerBatch *batch;
	g_autoptr (GError) error = NULL;

	decorator = user_data;
	priv = tracker_decorator_get_instance_private (decorator);
	batch = TRACKER_BATCH (object);

	priv->updating = FALSE;

	if (!tracker_batch_execute_finish (batch, result, &error)) {
		if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
			return;

		g_debug ("SPARQL error detected in batch, retrying one by one");
		retry_synchronously (decorator, priv->commit_buffer);
	} else {
		tag_success (decorator, priv->commit_buffer);
	}

	g_clear_pointer (&priv->commit_buffer, g_ptr_array_unref);

	if (!decorator_check_commit (decorator))
		decorator_cache_next_items (decorator);
}

static gboolean
decorator_commit_info (TrackerDecorator *decorator)
{
	TrackerSparqlConnection *sparql_conn;
	TrackerDecoratorPrivate *priv;
	TrackerBatch *batch;
	gint i;

	priv = tracker_decorator_get_instance_private (decorator);

	if (!priv->sparql_buffer || priv->sparql_buffer->len == 0)
		return FALSE;

	if (priv->commit_buffer)
		return FALSE;

	/* Move sparql buffer to commit buffer */
	priv->commit_buffer = priv->sparql_buffer;
	priv->sparql_buffer = NULL;
	priv->updating = TRUE;

	sparql_conn = tracker_miner_get_connection (TRACKER_MINER (decorator));
	batch = tracker_sparql_connection_create_batch (sparql_conn);

	for (i = 0; i < priv->commit_buffer->len; i++) {
		TrackerExtractInfo *info;

		info = g_ptr_array_index (priv->commit_buffer, i);
		TRACKER_DECORATOR_GET_CLASS (decorator)->update (decorator, info, batch);
	}

	tracker_batch_execute_async (batch,
	                             priv->cancellable,
	                             decorator_commit_cb,
	                             decorator);
	decorator_update_state (decorator, NULL, TRUE);
	g_object_unref (batch);

	return TRUE;
}

static gboolean
decorator_check_commit (TrackerDecorator *decorator)
{
	TrackerDecoratorPrivate *priv;

	priv = tracker_decorator_get_instance_private (decorator);

	if (!priv->sparql_buffer ||
	    (priv->n_remaining_items > 0 &&
	     priv->sparql_buffer->len < (guint) priv->batch_size))
		return FALSE;

	return decorator_commit_info (decorator);
}

static void
decorator_start (TrackerDecorator *decorator)
{
	TrackerDecoratorPrivate *priv;

	priv = tracker_decorator_get_instance_private (decorator);

	if (priv->processing)
		return;

	priv->processing = TRUE;
	g_signal_emit (decorator, signals[ITEMS_AVAILABLE], 0);
	decorator_update_state (decorator, "Extracting metadata", TRUE);
}

static void
decorator_finish (TrackerDecorator *decorator)
{
	TrackerDecoratorPrivate *priv;

	priv = tracker_decorator_get_instance_private (decorator);

	priv->processing = FALSE;
	priv->n_remaining_items = priv->n_processed_items = 0;
	g_signal_emit (decorator, signals[FINISHED], 0);
	decorator_commit_info (decorator);
	decorator_update_state (decorator, "Idle", FALSE);
}

static void
decorator_rebuild_cache (TrackerDecorator *decorator)
{
	TrackerDecoratorPrivate *priv;

	priv = tracker_decorator_get_instance_private (decorator);

	priv->n_remaining_items = 0;
	g_queue_foreach (&priv->item_cache,
	                 (GFunc) tracker_decorator_info_unref, NULL);
	g_queue_clear (&priv->item_cache);

	decorator_cache_next_items (decorator);
}

/* This function is called after the caller has completed the
 * GTask given on the TrackerDecoratorInfo, this definitely removes
 * the element being processed from queues.
 */
static void
decorator_task_done (GObject      *object,
                     GAsyncResult *result,
                     gpointer      user_data)
{
	TrackerDecorator *decorator = TRACKER_DECORATOR (object);
	TrackerDecoratorInfo *info = user_data;
	TrackerDecoratorPrivate *priv;
	TrackerExtractInfo *extract_info;
	GError *error = NULL;

	priv = tracker_decorator_get_instance_private (decorator);
	extract_info = g_task_propagate_pointer (G_TASK (result), &error);

	if (!extract_info) {
		if (error) {
			g_warning ("Task for '%s' finished with error: %s\n",
			           info->url, error->message);
			g_error_free (error);
		}
	} else {
		if (!priv->sparql_buffer) {
			priv->sparql_buffer =
				g_ptr_array_new_with_free_func ((GDestroyNotify) tracker_extract_info_unref);
		}

		g_ptr_array_add (priv->sparql_buffer, extract_info);
	}

	if (priv->n_remaining_items > 0)
		priv->n_remaining_items--;
	priv->n_processed_items++;

	if (priv->n_remaining_items == 0) {
		decorator_finish (decorator);
		if (!priv->updating)
			decorator_rebuild_cache (decorator);
	} else if (g_queue_is_empty (&priv->item_cache) &&
	           (!priv->sparql_buffer || !priv->commit_buffer)) {
		decorator_cache_next_items (decorator);
	}
}

static TrackerSparqlStatement *
load_statement (TrackerDecorator *decorator,
                const gchar      *query_filename)
{
	g_autofree gchar *resource_path = NULL;
	TrackerSparqlConnection *conn;

	resource_path =
		g_strconcat ("/org/freedesktop/Tracker3/Extract/queries/",
		             query_filename, NULL);

	conn = tracker_miner_get_connection (TRACKER_MINER (decorator));

	return tracker_sparql_connection_load_statement_from_gresource (conn,
	                                                                resource_path,
	                                                                NULL,
	                                                                NULL);
}

static void
decorator_count_remaining_items_cb (GObject      *object,
                                    GAsyncResult *result,
                                    gpointer      user_data)
{
	TrackerDecorator *decorator = user_data;
	TrackerDecoratorPrivate *priv;
	g_autoptr (TrackerSparqlCursor) cursor = NULL;
	g_autoptr (GError) error = NULL;

	cursor = tracker_sparql_statement_execute_finish (TRACKER_SPARQL_STATEMENT (object),
	                                                  result, &error);

	if (error) {
		g_warning ("Could not get remaining item count: %s", error->message);
		return;
	}

	if (!tracker_sparql_cursor_next (cursor, NULL, NULL))
		return;

	priv = tracker_decorator_get_instance_private (decorator);
	priv->querying = FALSE;

	priv->n_remaining_items = tracker_sparql_cursor_get_integer (cursor, 0);

	TRACKER_NOTE (DECORATOR, g_message ("[Decorator] Found %" G_GSIZE_FORMAT " items to extract", priv->n_remaining_items));

	if (priv->n_remaining_items > 0)
		decorator_cache_next_items (decorator);
	else
		decorator_finish (decorator);
}

static void
decorator_count_remaining_items (TrackerDecorator *decorator)
{
	TrackerDecoratorPrivate *priv;

	priv = tracker_decorator_get_instance_private (decorator);

	if (!priv->item_count_query)
		priv->item_count_query = load_statement (decorator, "get-item-count.rq");

	tracker_sparql_statement_execute_async (priv->item_count_query,
	                                        priv->cancellable,
	                                        decorator_count_remaining_items_cb,
	                                        decorator);
}

static void
decorator_item_cache_remove (TrackerDecorator *decorator,
                             gint              id)
{
	TrackerDecoratorPrivate *priv;
	GList *item;

	priv = tracker_decorator_get_instance_private (decorator);

	for (item = g_queue_peek_head_link (&priv->item_cache);
	     item; item = item->next) {
		TrackerDecoratorInfo *info = item->data;

		if (info->id != id)
			continue;

		g_queue_remove (&priv->item_cache, info);
		tracker_decorator_info_unref (info);
		break;
	}
}

static void
decorator_cache_items_cb (GObject      *object,
                          GAsyncResult *result,
                          gpointer      user_data)
{
	TrackerDecorator *decorator = user_data;
	TrackerDecoratorPrivate *priv;
	g_autoptr (TrackerSparqlCursor) cursor = NULL;
	TrackerDecoratorInfo *info;
	g_autoptr (GError) error = NULL;
	gboolean queue_was_empty;

	cursor = tracker_sparql_statement_execute_finish (TRACKER_SPARQL_STATEMENT (object),
	                                                  result, &error);
	priv = tracker_decorator_get_instance_private (decorator);
	priv->querying = FALSE;

	decorator_commit_info (decorator);

	queue_was_empty = g_queue_is_empty (&priv->item_cache);

	if (error) {
		g_warning ("Could not get unextracted files: %s", error->message);
		return;
	} else {
		while (tracker_sparql_cursor_next (cursor, NULL, NULL)) {
			info = tracker_decorator_info_new (decorator, cursor);
			g_queue_push_tail (&priv->item_cache, info);
		}
	}

	if (!g_queue_is_empty (&priv->item_cache) && !priv->processing) {
		decorator_start (decorator);
	} else if (g_queue_is_empty (&priv->item_cache) && priv->processing) {
		decorator_finish (decorator);
	} else if (queue_was_empty) {
		g_signal_emit (decorator, signals[ITEMS_AVAILABLE], 0);
	}
}

static void
bind_graph_limits (TrackerDecorator *decorator,
                   gboolean          priority)
{
	TrackerDecoratorPrivate *priv;
	const gchar *graphs[][3] = {
		{ "tracker:Audio", "audioHigh", "audioLow" },
		{ "tracker:Pictures", "picturesHigh", "picturesLow" },
		{ "tracker:Video", "videoHigh", "videoLow" },
		{ "tracker:Software", "softwareHigh", "softwareLow" },
		{ "tracker:Documents", "documentsHigh", "documentsLow" },
	};
	guint i;

	priv = tracker_decorator_get_instance_private (decorator);

	for (i = 0; i < G_N_ELEMENTS (graphs); i++) {
		const gchar *graph = graphs[i][0];
		const gchar *high_limit = graphs[i][1];
		const gchar *low_limit = graphs[i][2];
		gboolean is_priority;

		is_priority = priv->priority_graphs &&
			g_strv_contains ((const gchar * const *) priv->priority_graphs, graph);

		/* Graphs with high priority get unbound high limit and 0 low limit,
		 * graphs with regular priority get the opposite.
		 */
		tracker_sparql_statement_bind_int (priv->remaining_items_query,
		                                   high_limit,
		                                   is_priority ? -1 : 0);
		tracker_sparql_statement_bind_int (priv->remaining_items_query,
		                                   low_limit,
		                                   is_priority ? 0 : -1);
	}
}

static void
decorator_query_next_items (TrackerDecorator *decorator)
{
	TrackerDecoratorPrivate *priv;
	gint offset = 0;

	priv = tracker_decorator_get_instance_private (decorator);

	if (priv->sparql_buffer)
		offset += priv->sparql_buffer->len;
	if (priv->commit_buffer)
		offset += priv->commit_buffer->len;

	if (!priv->remaining_items_query)
		priv->remaining_items_query = load_statement (decorator, "get-items.rq");

	bind_graph_limits (decorator, TRUE);
	tracker_sparql_statement_bind_int (priv->remaining_items_query,
	                                   "offset", offset);
	tracker_sparql_statement_bind_int (priv->remaining_items_query,
	                                   "limit", QUERY_BATCH_SIZE);

	tracker_sparql_statement_execute_async (priv->remaining_items_query,
	                                        priv->cancellable,
	                                        decorator_cache_items_cb,
	                                        decorator);
}

static void
decorator_cache_next_items (TrackerDecorator *decorator)
{
	TrackerDecoratorPrivate *priv;

	priv = tracker_decorator_get_instance_private (decorator);

	if (priv->querying ||
	    priv->updating ||
	    !g_queue_is_empty (&priv->item_cache))
		return;

	priv->querying = TRUE;

	if (priv->n_remaining_items == 0) {
		TRACKER_NOTE (DECORATOR, g_message ("[Decorator] Counting items which still need processing"));
		decorator_count_remaining_items (decorator);
	} else {
		TRACKER_NOTE (DECORATOR, g_message ("[Decorator] Querying items which still need processing"));
		decorator_query_next_items (decorator);
	}
}

static void
tracker_decorator_get_property (GObject    *object,
                                guint       param_id,
                                GValue     *value,
                                GParamSpec *pspec)
{
	TrackerDecoratorPrivate *priv;

	priv = tracker_decorator_get_instance_private (TRACKER_DECORATOR (object));

	switch (param_id) {
	case PROP_COMMIT_BATCH_SIZE:
		g_value_set_int (value, priv->batch_size);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, param_id, pspec);
	}
}

static void
tracker_decorator_set_property (GObject      *object,
                                guint         param_id,
                                const GValue *value,
                                GParamSpec   *pspec)
{
	TrackerDecoratorPrivate *priv;

	priv = tracker_decorator_get_instance_private (TRACKER_DECORATOR (object));

	switch (param_id) {
	case PROP_COMMIT_BATCH_SIZE:
		priv->batch_size = g_value_get_int (value);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, param_id, pspec);
	}
}

static void
notifier_events_cb (TrackerDecorator *decorator,
                    const gchar      *service,
                    const gchar      *graph,
                    GPtrArray        *events,
                    TrackerNotifier  *notifier)
{
	TrackerDecoratorPrivate *priv;
	gboolean check_added = FALSE;
	gint64 id;
	gint i;

	priv = tracker_decorator_get_instance_private (decorator);

	for (i = 0; i < events->len; i++) {
		TrackerNotifierEvent *event;

		event = g_ptr_array_index (events, i);
		id = tracker_notifier_event_get_id (event);

		switch (tracker_notifier_event_get_event_type (event)) {
		case TRACKER_NOTIFIER_EVENT_CREATE:
		case TRACKER_NOTIFIER_EVENT_UPDATE:
			/* Merely use this as a hint that there is something
			 * left to be processed.
			 */
			check_added = TRUE;
			break;
		case TRACKER_NOTIFIER_EVENT_DELETE:
			decorator_item_cache_remove (decorator, id);
			break;
		}
	}

	if (check_added && !priv->querying && !priv->updating)
		decorator_cache_next_items (decorator);
}

static void
tracker_decorator_constructed (GObject *object)
{
	TrackerDecorator *decorator;
	TrackerDecoratorPrivate *priv;
	TrackerSparqlConnection *conn;

	G_OBJECT_CLASS (tracker_decorator_parent_class)->constructed (object);

	decorator = TRACKER_DECORATOR (object);
	priv = tracker_decorator_get_instance_private (decorator);

	conn = tracker_miner_get_connection (TRACKER_MINER (decorator));
	priv->notifier = tracker_sparql_connection_create_notifier (conn);
	g_signal_connect_swapped (priv->notifier, "events",
	                          G_CALLBACK (notifier_events_cb),
	                          decorator);

	decorator_update_state (decorator, "Idle", FALSE);
}

static void
tracker_decorator_finalize (GObject *object)
{
	TrackerDecoratorPrivate *priv;
	TrackerDecorator *decorator;

	decorator = TRACKER_DECORATOR (object);
	priv = tracker_decorator_get_instance_private (decorator);

	g_clear_object (&priv->remaining_items_query);
	g_clear_object (&priv->item_count_query);
	g_strfreev (priv->priority_graphs);

	g_cancellable_cancel (priv->cancellable);
	g_clear_object (&priv->cancellable);

	g_cancellable_cancel (priv->task_cancellable);
	g_clear_object (&priv->task_cancellable);

	g_clear_object (&priv->notifier);

	g_queue_foreach (&priv->item_cache,
	                 (GFunc) tracker_decorator_info_unref,
	                 NULL);
	g_queue_clear (&priv->item_cache);

	g_clear_pointer (&priv->sparql_buffer, g_ptr_array_unref);
	g_clear_pointer (&priv->commit_buffer, g_ptr_array_unref);
	g_timer_destroy (priv->timer);

	G_OBJECT_CLASS (tracker_decorator_parent_class)->finalize (object);
}

static void
tracker_decorator_paused (TrackerMiner *miner)
{
	TrackerDecoratorPrivate *priv;

	TRACKER_NOTE (DECORATOR, g_message ("[Decorator] Paused"));

	priv = tracker_decorator_get_instance_private (TRACKER_DECORATOR (miner));
	g_timer_stop (priv->timer);

	g_cancellable_cancel (priv->task_cancellable);
	g_clear_object (&priv->task_cancellable);
	priv->task_cancellable = g_cancellable_new ();
}

static void
tracker_decorator_resumed (TrackerMiner *miner)
{
	TrackerDecoratorPrivate *priv;

	TRACKER_NOTE (DECORATOR, g_message ("[Decorator] Resumed"));
	decorator_cache_next_items (TRACKER_DECORATOR (miner));
	priv = tracker_decorator_get_instance_private (TRACKER_DECORATOR (miner));
	g_timer_continue (priv->timer);
}

static void
tracker_decorator_stopped (TrackerMiner *miner)
{
	TrackerDecoratorPrivate *priv;

	TRACKER_NOTE (DECORATOR, g_message ("[Decorator] Stopped"));
	priv = tracker_decorator_get_instance_private (TRACKER_DECORATOR (miner));
	g_timer_stop (priv->timer);
}

static void
tracker_decorator_started (TrackerMiner *miner)
{
	TrackerDecoratorPrivate *priv;
	TrackerDecorator *decorator;

	decorator = TRACKER_DECORATOR (miner);
	priv = tracker_decorator_get_instance_private (decorator);

	TRACKER_NOTE (DECORATOR, g_message ("[Decorator] Started"));
	g_timer_start (priv->timer);
	decorator_rebuild_cache (decorator);
}

static void
tracker_decorator_class_init (TrackerDecoratorClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	TrackerMinerClass *miner_class = TRACKER_MINER_CLASS (klass);

	object_class->constructed = tracker_decorator_constructed;
	object_class->get_property = tracker_decorator_get_property;
	object_class->set_property = tracker_decorator_set_property;
	object_class->finalize = tracker_decorator_finalize;

	miner_class->paused = tracker_decorator_paused;
	miner_class->resumed = tracker_decorator_resumed;
	miner_class->started = tracker_decorator_started;
	miner_class->stopped = tracker_decorator_stopped;

	g_object_class_install_property (object_class,
	                                 PROP_COMMIT_BATCH_SIZE,
	                                 g_param_spec_int ("commit-batch-size",
	                                                   "Commit batch size",
	                                                   "Number of items per update batch",
	                                                   0, G_MAXINT, DEFAULT_BATCH_SIZE,
	                                                   G_PARAM_READWRITE |
	                                                   G_PARAM_STATIC_STRINGS));
	/**
	 * TrackerDecorator::items-available:
	 * @decorator: the #TrackerDecorator
	 *
	 * The ::items-available signal will be emitted whenever the
	 * #TrackerDecorator sees resources that are available for
	 * extended metadata extraction.
	 *
	 * Since: 0.18
	 **/
	signals[ITEMS_AVAILABLE] =
		g_signal_new ("items-available",
		              G_OBJECT_CLASS_TYPE (object_class),
		              G_SIGNAL_RUN_LAST,
		              G_STRUCT_OFFSET (TrackerDecoratorClass,
		                               items_available),
		              NULL, NULL, NULL,
		              G_TYPE_NONE, 0);
	/**
	 * TrackerDecorator::finished:
	 * @decorator: the #TrackerDecorator
	 *
	 * The ::finished signal will be emitted whenever the
	 * #TrackerDecorator has finished extracted extended metadata
	 * for resources in the database.
	 *
	 * Since: 0.18
	 **/
	signals[FINISHED] =
		g_signal_new ("finished",
		              G_OBJECT_CLASS_TYPE (object_class),
		              G_SIGNAL_RUN_LAST,
		              G_STRUCT_OFFSET (TrackerDecoratorClass, finished),
		              NULL, NULL, NULL,
		              G_TYPE_NONE, 0);
}

static void
tracker_decorator_init (TrackerDecorator *decorator)
{
	TrackerDecoratorPrivate *priv;

	priv = tracker_decorator_get_instance_private (decorator);
	priv->batch_size = DEFAULT_BATCH_SIZE;
	priv->timer = g_timer_new ();
	priv->cancellable = g_cancellable_new ();
	priv->task_cancellable = g_cancellable_new ();

	g_queue_init (&priv->item_cache);
}

/**
 * tracker_decorator_get_n_items:
 * @decorator: a #TrackerDecorator
 *
 * Get the number of items left in the queue to be processed. This
 * indicates content that may already exist in Tracker but is waiting
 * to be further flurished with metadata with a 2nd pass extraction or
 * index.
 *
 * Returns: the number of items queued to be processed, always >= 0.
 *
 * Since: 0.18
 **/
guint
tracker_decorator_get_n_items (TrackerDecorator *decorator)
{
	TrackerDecoratorPrivate *priv;

	g_return_val_if_fail (TRACKER_IS_DECORATOR (decorator), 0);

	priv = tracker_decorator_get_instance_private (decorator);

	return priv->n_remaining_items;
}

TrackerDecoratorInfo *
tracker_decorator_next (TrackerDecorator  *decorator,
                        GError           **error)
{
	TrackerDecoratorPrivate *priv;
	TrackerDecoratorInfo *info;

	g_return_val_if_fail (TRACKER_IS_DECORATOR (decorator), NULL);

	priv = tracker_decorator_get_instance_private (decorator);

	if (tracker_miner_is_paused (TRACKER_MINER (decorator))) {
		g_set_error (error,
		             tracker_decorator_error_quark (),
		             TRACKER_DECORATOR_ERROR_PAUSED,
		             "Decorator is paused");
		return NULL;
	}

	info = g_queue_pop_head (&priv->item_cache);
	if (!info)
		return NULL;

	TRACKER_NOTE (DECORATOR, g_message ("[Decorator] Next item %s", info->url));

	return info;
}

void
tracker_decorator_set_priority_graphs (TrackerDecorator    *decorator,
                                       const gchar * const *graphs)
{
	TrackerDecoratorPrivate *priv;

	priv = tracker_decorator_get_instance_private (decorator);

	g_strfreev (priv->priority_graphs);
	priv->priority_graphs = g_strdupv ((gchar **) graphs);
	decorator_rebuild_cache (decorator);
}

/**
 * tracker_decorator_info_get_url:
 * @info: a #TrackerDecoratorInfo.
 *
 * A URL is a Uniform Resource Locator and should be a location associated
 * with a resource in the database. For example, 'file:///tmp/foo.txt'.
 *
 * Returns: the URL for #TrackerDecoratorInfo on success or #NULL on error.
 *
 * Since: 0.18
 **/
const gchar *
tracker_decorator_info_get_url (TrackerDecoratorInfo *info)
{
	g_return_val_if_fail (info != NULL, NULL);
	return info->url;
}

GCancellable *
tracker_decorator_info_get_cancellable (TrackerDecoratorInfo *info)
{
	g_return_val_if_fail (info != NULL, NULL);
	return g_task_get_cancellable (info->task);
}

/**
 * tracker_decorator_info_complete:
 * @info: a #TrackerDecoratorInfo
 * @sparql: (transfer full): SPARQL string
 *
 * Completes the task associated to this #TrackerDecoratorInfo.
 * Takes ownership of @sparql.
 *
 * Since: 2.0
 **/
void
tracker_decorator_info_complete (TrackerDecoratorInfo *info,
                                 TrackerExtractInfo   *extract_info)
{
	TRACKER_NOTE (DECORATOR, g_message ("[Decorator] Task for %s completed successfully", info->url));
	g_task_return_pointer (info->task,
	                       tracker_extract_info_ref (extract_info),
	                       (GDestroyNotify) tracker_extract_info_unref);
}

/**
 * tracker_decorator_info_complete_error:
 * @info: a #TrackerDecoratorInfo
 * @error: (transfer full): An error occurred during SPARQL generation
 *
 * Completes the task associated to this #TrackerDecoratorInfo,
 * returning the given @error happened during SPARQL generation.
 *
 * Since: 2.0
 **/
void
tracker_decorator_info_complete_error (TrackerDecoratorInfo *info,
                                       GError               *error)
{
	TRACKER_NOTE (DECORATOR, g_message ("[Decorator] Task for %s failed: %s", info->url, error->message));
	g_task_return_error (info->task, error);
}

void
tracker_decorator_invalidate_cache (TrackerDecorator *decorator)
{
	decorator_rebuild_cache (decorator);
}
