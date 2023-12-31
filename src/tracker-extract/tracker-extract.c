/*
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

#include "config-miners.h"

#include <string.h>
#include <unistd.h>

#include <gmodule.h>
#include <glib/gi18n.h>
#include <gio/gio.h>

#include <gio/gunixoutputstream.h>
#include <gio/gunixinputstream.h>
#include <gio/gunixfdlist.h>

#include <libtracker-miners-common/tracker-common.h>

#include <libtracker-extract/tracker-extract.h>

#include "tracker-extract.h"
#include "tracker-main.h"

#ifdef THREAD_ENABLE_TRACE
#warning Main thread traces enabled
#endif /* THREAD_ENABLE_TRACE */

#define TRACKER_EXTRACT_GET_PRIVATE(obj) (tracker_extract_get_instance_private (TRACKER_EXTRACT (obj)))

G_DEFINE_QUARK (TrackerExtractError, tracker_extract_error)

#define DEADLINE_SECONDS 30

extern gboolean debug;

typedef struct {
	gint extracted_count;
	gint failed_count;
} StatisticsData;

typedef struct {
	GHashTable *statistics_data;
	GList *running_tasks;

	/* used to maintain the running tasks
	 * and stats from different threads
	 */
	GMutex task_mutex;

	/* Thread pool for multi-threaded extractors */
	GThreadPool *thread_pool;

	/* module -> async queue hashtable
	 * for single-threaded extractors
	 */
	GHashTable *single_thread_extractors;

	gboolean disable_shutdown;
	gboolean disable_summary_on_finalize;

	gchar *force_module;

	gint unhandled_count;
} TrackerExtractPrivate;

typedef struct {
	TrackerExtract *extract;
	GCancellable *cancellable;
	GAsyncResult *res;
	gchar *file;
	gchar *mimetype;

	TrackerMimetypeInfo *mimetype_handlers;

	/* to be fed from mimetype_handlers */
	TrackerExtractMetadataFunc cur_func;
	GModule *cur_module;

	guint signal_id;
	guint timeout_id;
	guint success : 1;
} TrackerExtractTask;

static void tracker_extract_finalize (GObject *object);
static void report_statistics        (GObject *object);
static gboolean get_metadata         (TrackerExtractTask *task);
static gboolean dispatch_task_cb     (TrackerExtractTask *task);


G_DEFINE_TYPE_WITH_PRIVATE(TrackerExtract, tracker_extract, G_TYPE_OBJECT)

static void
tracker_extract_class_init (TrackerExtractClass *klass)
{
	GObjectClass *object_class;

	object_class = G_OBJECT_CLASS (klass);

	object_class->finalize = tracker_extract_finalize;
}

static void
statistics_data_free (StatisticsData *data)
{
	g_slice_free (StatisticsData, data);
}

static void
tracker_extract_init (TrackerExtract *object)
{
	TrackerExtractPrivate *priv;

	priv = TRACKER_EXTRACT_GET_PRIVATE (object);
	priv->statistics_data = g_hash_table_new_full (NULL, NULL, NULL,
	                                               (GDestroyNotify) statistics_data_free);
	priv->single_thread_extractors = g_hash_table_new (NULL, NULL);
	priv->thread_pool = g_thread_pool_new ((GFunc) get_metadata,
	                                       NULL, 10, TRUE, NULL);

	g_mutex_init (&priv->task_mutex);
}

static void
tracker_extract_finalize (GObject *object)
{
	TrackerExtractPrivate *priv;

	priv = TRACKER_EXTRACT_GET_PRIVATE (object);

	/* FIXME: Shutdown modules? */

	g_hash_table_destroy (priv->single_thread_extractors);
	g_thread_pool_free (priv->thread_pool, TRUE, FALSE);

	if (!priv->disable_summary_on_finalize) {
		report_statistics (object);
	}

	g_hash_table_destroy (priv->statistics_data);

	g_mutex_clear (&priv->task_mutex);

	G_OBJECT_CLASS (tracker_extract_parent_class)->finalize (object);
}

static void
report_statistics (GObject *object)
{
	TrackerExtractPrivate *priv;
	GHashTableIter iter;
	gpointer key, value;

	priv = TRACKER_EXTRACT_GET_PRIVATE (object);

	g_mutex_lock (&priv->task_mutex);

	g_message ("--------------------------------------------------");
	g_message ("Statistics:");

	g_hash_table_iter_init (&iter, priv->statistics_data);

	while (g_hash_table_iter_next (&iter, &key, &value)) {
		GModule *module = key;
		StatisticsData *data = value;

		if (data->extracted_count > 0 || data->failed_count > 0) {
			const gchar *name, *name_without_path;

			name = g_module_name (module);
			name_without_path = strrchr (name, G_DIR_SEPARATOR) + 1;

			g_message ("    Module:'%s', extracted:%d, failures:%d",
			           name_without_path,
			           data->extracted_count,
			           data->failed_count);
		}
	}

	g_message ("Unhandled files: %d", priv->unhandled_count);

	if (priv->unhandled_count == 0 &&
	    g_hash_table_size (priv->statistics_data) < 1) {
		g_message ("    No files handled");
	}

	g_message ("--------------------------------------------------");

	g_mutex_unlock (&priv->task_mutex);
}

TrackerExtract *
tracker_extract_new (gboolean     disable_shutdown,
                     const gchar *force_module)
{
	TrackerExtract *object;
	TrackerExtractPrivate *priv;

	if (!tracker_extract_module_manager_init ()) {
		return NULL;
	}

	/* Set extractors */
	object = g_object_new (TRACKER_TYPE_EXTRACT, NULL);

	priv = TRACKER_EXTRACT_GET_PRIVATE (object);

	priv->disable_shutdown = disable_shutdown;
	priv->force_module = g_strdup (force_module);

	return object;
}

static void
notify_task_finish (TrackerExtractTask *task,
                    gboolean            success)
{
	TrackerExtract *extract;
	TrackerExtractPrivate *priv;
	StatisticsData *stats_data;

	extract = task->extract;
	priv = TRACKER_EXTRACT_GET_PRIVATE (extract);

	/* Reports and ongoing tasks may be
	 * accessed from other threads.
	 */
	g_mutex_lock (&priv->task_mutex);

	if (task->cur_module) {
		stats_data = g_hash_table_lookup (priv->statistics_data,
						  task->cur_module);

		if (!stats_data) {
			stats_data = g_slice_new0 (StatisticsData);
			g_hash_table_insert (priv->statistics_data,
					     task->cur_module,
					     stats_data);
		}

		stats_data->extracted_count++;

		if (!success) {
			stats_data->failed_count++;
		}
	} else {
		priv->unhandled_count++;
	}

	priv->running_tasks = g_list_remove (priv->running_tasks, task);

	g_mutex_unlock (&priv->task_mutex);
}

static gboolean
get_file_metadata (TrackerExtractTask  *task,
                   TrackerExtractInfo **info_out)
{
	TrackerExtractInfo *info;
	GFile *file;
	gchar *mime_used = NULL;

	*info_out = NULL;

	file = g_file_new_for_uri (task->file);
	info = tracker_extract_info_new (file, task->mimetype);
	g_object_unref (file);

	if (task->mimetype && *task->mimetype) {
		/* We know the mime */
		mime_used = g_strdup (task->mimetype);
	} else {
		tracker_extract_info_unref (info);
		return FALSE;
	}

	/* Now we have sanity checked everything, actually get the
	 * data we need from the extractors.
	 */
	if (mime_used) {
		if (task->cur_func) {
			g_debug ("Using %s...",
				 task->cur_module ?
				 g_module_name (task->cur_module) :
				 "Dummy extraction");

			task->success = (task->cur_func) (info);
		}

		g_free (mime_used);
	}

	if (!task->success) {
		tracker_extract_info_unref (info);
		info = NULL;
	}

	*info_out = info;

	return task->success;
}

/* This function is called on the thread calling g_cancellable_cancel() */
static void
task_cancellable_cancelled_cb (GCancellable       *cancellable,
                               TrackerExtractTask *task)
{
	TrackerExtractPrivate *priv;
	TrackerExtract *extract;

	extract = task->extract;
	priv = TRACKER_EXTRACT_GET_PRIVATE (extract);

	g_mutex_lock (&priv->task_mutex);

	if (g_list_find (priv->running_tasks, task)) {
		g_message ("Cancelled task for '%s' was currently being "
		           "processed, _exit()ing immediately",
		           task->file);
		_exit (0);
	}

	g_mutex_unlock (&priv->task_mutex);
}

static gboolean
task_deadline_cb (gpointer user_data)
{
	TrackerExtractTask *task = user_data;

	g_warning ("File '%s' took too long to process. Shutting down everything",
	           task->file);

	if (task->cancellable)
		g_cancellable_cancel (task->cancellable);
	_exit (0);
}

static TrackerExtractTask *
extract_task_new (TrackerExtract *extract,
                  const gchar    *uri,
                  const gchar    *mimetype,
                  GCancellable   *cancellable,
                  GAsyncResult   *res,
                  GError        **error)
{
	TrackerExtractTask *task;
	gchar *mimetype_used;

	if (!mimetype || !*mimetype) {
		GFile *file;
		GFileInfo *info;
		GError *internal_error = NULL;

		file = g_file_new_for_uri (uri);
		info = g_file_query_info (file,
		                          G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE,
		                          G_FILE_QUERY_INFO_NONE,
		                          NULL,
		                          &internal_error);

		g_object_unref (file);

		if (internal_error) {
			g_propagate_error (error, internal_error);
			return NULL;
		}

		mimetype_used = g_strdup (g_file_info_get_content_type (info));
		g_object_unref (info);
		g_message ("MIME type guessed as '%s' (from GIO)", mimetype_used);
	} else {
		mimetype_used = g_strdup (mimetype);
		g_message ("MIME type passed to us as '%s'", mimetype_used);
	}

	task = g_slice_new0 (TrackerExtractTask);
	task->cancellable = (cancellable) ? g_object_ref (cancellable) : NULL;
	task->res = (res) ? g_object_ref (res) : NULL;
	task->file = g_strdup (uri);
	task->mimetype = mimetype_used;
	task->extract = extract;

	if (task->res) {
		GSource *source;

		source = g_timeout_source_new_seconds (DEADLINE_SECONDS);
		g_source_set_callback (source, task_deadline_cb, task, NULL);
		task->timeout_id =
			g_source_attach (source, g_task_get_context (G_TASK (task->res)));
	}

	if (task->cancellable) {
		task->signal_id = g_cancellable_connect (cancellable,
		                                         G_CALLBACK (task_cancellable_cancelled_cb),
		                                         task, NULL);
	}

	return task;
}

static void
extract_task_free (TrackerExtractTask *task)
{
	if (task->cancellable && task->signal_id != 0) {
		g_cancellable_disconnect (task->cancellable, task->signal_id);
	}

	notify_task_finish (task, task->success);

	if (task->timeout_id)
		g_source_remove (task->timeout_id);

	if (task->res) {
		g_object_unref (task->res);
	}

	if (task->cancellable) {
		g_object_unref (task->cancellable);
	}

	if (task->mimetype_handlers) {
		tracker_mimetype_info_free (task->mimetype_handlers);
	}

	g_free (task->mimetype);
	g_free (task->file);

	g_slice_free (TrackerExtractTask, task);
}

static gboolean
filter_module (TrackerExtract *extract,
               GModule        *module)
{
	TrackerExtractPrivate *priv;
	gchar *module_basename, *filter_name;
	gboolean filter;

	if (!module) {
		return FALSE;
	}

	priv = TRACKER_EXTRACT_GET_PRIVATE (extract);

	if (!priv->force_module) {
		return FALSE;
	}

	/* Module name is the full path to it */
	module_basename = g_path_get_basename (g_module_name (module));

	if (g_str_has_prefix (priv->force_module, "lib") &&
	    g_str_has_suffix (priv->force_module, "." G_MODULE_SUFFIX)) {
		filter_name = g_strdup (priv->force_module);
	} else {
		filter_name = g_strdup_printf ("libextract-%s.so",
		                               priv->force_module);
	}

	filter = strcmp (module_basename, filter_name) != 0;

	if (filter) {
		g_debug ("Module filtered out '%s' (due to --force-module='%s')",
		         module_basename,
		         filter_name);
	} else {
		g_debug ("Module used '%s' (due to --force-module='%s')",
		         module_basename,
		         filter_name);
	}

	g_free (module_basename);
	g_free (filter_name);

	return filter;
}

static gboolean
get_metadata (TrackerExtractTask *task)
{
	TrackerExtractInfo *info;

#ifdef THREAD_ENABLE_TRACE
	g_debug ("Thread:%p --> '%s': Collected metadata",
	         g_thread_self (),
	         task->file);
#endif /* THREAD_ENABLE_TRACE */

	if (g_task_return_error_if_cancelled (G_TASK (task->res))) {
		extract_task_free (task);
		return FALSE;
	}

	if (!filter_module (task->extract, task->cur_module) &&
	    get_file_metadata (task, &info)) {
		g_task_return_pointer (G_TASK (task->res), info,
		                       (GDestroyNotify) tracker_extract_info_unref);
		extract_task_free (task);
	} else {
		/* Reinject the task into the main thread
		 * queue, so the next module kicks in.
		 */
		g_idle_add ((GSourceFunc) dispatch_task_cb, task);
	}

	return FALSE;
}

static gpointer
single_thread_get_metadata (GAsyncQueue *queue)
{
	if (!tracker_seccomp_init ())
		g_assert_not_reached ();

	while (TRUE) {
		TrackerExtractTask *task;

		task = g_async_queue_pop (queue);
#ifdef THREAD_ENABLE_TRACE
		g_debug ("Thread:%p --> '%s': Dispatching in dedicated thread",
		         g_thread_self(), task->file);
#endif /* THREAD_ENABLE_TRACE */
		get_metadata (task);
	}

	return NULL;
}

/* This function is executed in the main thread, decides the
 * module that's going to be run for a given task, and dispatches
 * the task according to the threading strategy of that module.
 */
static gboolean
dispatch_task_cb (TrackerExtractTask *task)
{
	TrackerExtractPrivate *priv;
	GError *error = NULL;
	GAsyncQueue *async_queue;
	GModule *module;

#ifdef THREAD_ENABLE_TRACE
	g_debug ("Thread:%p (Main) <-- '%s': Handling task...\n",
	         g_thread_self (),
	         task->file);
#endif /* THREAD_ENABLE_TRACE */

	priv = TRACKER_EXTRACT_GET_PRIVATE (task->extract);

	if (!task->mimetype) {
		error = g_error_new (tracker_extract_error_quark (),
		                     TRACKER_EXTRACT_ERROR_NO_MIMETYPE,
		                     "No mimetype for '%s'", task->file);
	} else {
		if (!task->mimetype_handlers) {
			/* First iteration for task, get the mimetype handlers */
			task->mimetype_handlers = tracker_extract_module_manager_get_mimetype_handlers (task->mimetype);

			if (!task->mimetype_handlers) {
				error = g_error_new (tracker_extract_error_quark (),
				                     TRACKER_EXTRACT_ERROR_NO_EXTRACTOR,
				                     "No mimetype extractor handlers for uri:'%s' and mime:'%s'",
				                     task->file, task->mimetype);
			}
		} else {
			/* Any further iteration, should happen rarely if
			 * most specific handlers know nothing about the file
			 */
			if (!tracker_mimetype_info_iter_next (task->mimetype_handlers)) {
				g_message ("There's no next extractor");

				error = g_error_new (tracker_extract_error_quark (),
				                     TRACKER_EXTRACT_ERROR_NO_EXTRACTOR,
				                     "Could not get any metadata for uri:'%s' and mime:'%s'",
				                     task->file, task->mimetype);
			} else {
				g_message ("Trying next extractor for '%s'", task->file);
			}
		}
	}

	if (error) {
		g_task_return_error (G_TASK (task->res), error);
		extract_task_free (task);

		return FALSE;
	}

	task->cur_module = module = tracker_mimetype_info_get_module (task->mimetype_handlers, &task->cur_func);

	if (!task->cur_func) {
		g_warning ("Discarding task, no module able to handle '%s'", task->file);
		priv->unhandled_count++;
		extract_task_free (task);
		return FALSE;
	}

	async_queue = g_hash_table_lookup (priv->single_thread_extractors, module);

	if (!async_queue) {
		GThread *thread;

		/* No thread created yet for this module, create it
		 * together with the async queue used to pass data to it
		 */
		async_queue = g_async_queue_new ();
		thread = g_thread_try_new ("single",
		                           (GThreadFunc) single_thread_get_metadata,
		                           g_async_queue_ref (async_queue),
		                           &error);
		if (!thread) {
			g_task_return_error (G_TASK (task->res), error);
			extract_task_free (task);
			return FALSE;
		}

		/* We won't join the thread, so just unref it here */
		g_thread_unref (thread);

		g_hash_table_insert (priv->single_thread_extractors, module, async_queue);
	}

	g_async_queue_push (async_queue, task);

	return FALSE;
}

/* This function can be called in any thread */
void
tracker_extract_file (TrackerExtract      *extract,
                      const gchar         *file,
                      const gchar         *mimetype,
                      GCancellable        *cancellable,
                      GAsyncReadyCallback  cb,
                      gpointer             user_data)
{
	GError *error = NULL;
	TrackerExtractTask *task;
	GTask *async_task;

	g_return_if_fail (TRACKER_IS_EXTRACT (extract));
	g_return_if_fail (file != NULL);
	g_return_if_fail (cb != NULL);

#ifdef THREAD_ENABLE_TRACE
	g_debug ("Thread:%p <-- '%s': Processing file\n",
	         g_thread_self (),
	         file);
#endif /* THREAD_ENABLE_TRACE */

	async_task = g_task_new (extract, cancellable, cb, user_data);

	task = extract_task_new (extract, file, mimetype, cancellable,
	                         G_ASYNC_RESULT (async_task), &error);

	if (error) {
		g_warning ("Could not get mimetype, %s", error->message);
		g_task_return_error (async_task, error);
	} else {
		TrackerExtractPrivate *priv;

		priv = TRACKER_EXTRACT_GET_PRIVATE (task->extract);

		g_mutex_lock (&priv->task_mutex);
		priv->running_tasks = g_list_prepend (priv->running_tasks, task);
		g_mutex_unlock (&priv->task_mutex);

		g_idle_add ((GSourceFunc) dispatch_task_cb, task);
	}

	/* Task takes a ref and if this fails, we want to unref anyway */
	g_object_unref (async_task);
}

void
tracker_extract_get_metadata_by_cmdline (TrackerExtract             *object,
                                         const gchar                *uri,
                                         const gchar                *mime,
                                         TrackerSerializationFormat  output_format)
{
	GError *error = NULL;
	TrackerExtractPrivate *priv;
	TrackerExtractTask *task;
	TrackerExtractInfo *info;
	gboolean no_data_or_modules = TRUE;

	priv = TRACKER_EXTRACT_GET_PRIVATE (object);
	priv->disable_summary_on_finalize = TRUE;

	g_return_if_fail (uri != NULL);

	task = extract_task_new (object, uri, mime, NULL, NULL, &error);

	if (error) {
		g_printerr ("%s, %s\n",
		            _("Metadata extraction failed"),
		            error->message);
		g_error_free (error);

		return;
	}

	task->mimetype_handlers = tracker_extract_module_manager_get_mimetype_handlers (task->mimetype);
	if (task->mimetype_handlers) {
		task->cur_module = tracker_mimetype_info_get_module (task->mimetype_handlers, &task->cur_func);
	}

	while (task->cur_func) {
		if (!filter_module (object, task->cur_module) &&
		    get_file_metadata (task, &info)) {
			TrackerResource *resource = tracker_extract_info_get_resource (info);

			if (resource == NULL)
				break;

			no_data_or_modules = FALSE;

			if (output_format == TRACKER_SERIALIZATION_FORMAT_SPARQL) {
				char *text;

				/* If this was going into the tracker-store we'd generate a unique ID
				 * here, so that the data persisted across file renames.
				 */
				tracker_resource_set_identifier (resource, uri);

				text = tracker_resource_print_sparql_update (resource, NULL, NULL);

				g_print ("%s\n", text);

				g_free (text);
			} else if (output_format == TRACKER_SERIALIZATION_FORMAT_TURTLE) {
				char *turtle;

				/* If this was going into the tracker-store we'd generate a unique ID
				 * here, so that the data persisted across file renames.
				 */
				tracker_resource_set_identifier (resource, uri);

				turtle = tracker_resource_print_turtle (resource, NULL);

				if (turtle) {
					g_print ("%s\n", turtle);
					g_free (turtle);
				}
			} else {
				/* JSON-LD extraction */
				char *json;

				/* If this was going into the tracker-store we'd generate a unique ID
				 * here, so that the data persisted across file renames.
				 */
				tracker_resource_set_identifier (resource, uri);

				json = tracker_resource_print_jsonld (resource, NULL);
				if (json) {
					g_print ("%s\n", json);
					g_free (json);
				}
			}

			tracker_extract_info_unref (info);
			break;
		} else {
			if (!tracker_mimetype_info_iter_next (task->mimetype_handlers)) {
				break;
			}

			task->cur_module = tracker_mimetype_info_get_module (task->mimetype_handlers,
			                                                     &task->cur_func);
		}
	}

	if (no_data_or_modules) {
		g_printerr ("%s: %s\n",
		         uri,
		         _("No metadata or extractor modules found to handle this file"));
	}

	extract_task_free (task);
}

TrackerExtractInfo *
tracker_extract_file_finish (TrackerExtract  *extract,
                             GAsyncResult    *res,
                             GError         **error)
{
	g_return_val_if_fail (TRACKER_IS_EXTRACT (extract), NULL);
	g_return_val_if_fail (G_IS_ASYNC_RESULT (res), NULL);
	g_return_val_if_fail (!error || !*error, NULL);

	return g_task_propagate_pointer (G_TASK (res), error);
}
