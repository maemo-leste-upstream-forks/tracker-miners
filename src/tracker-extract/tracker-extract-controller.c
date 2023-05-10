/*
 * Copyright (C) 2014 - Collabora Ltd.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 */

#include "config-miners.h"

#include "tracker-extract-controller.h"

#include "tracker-main.h"

enum {
	PROP_DECORATOR = 1,
	PROP_CONNECTION,
};

typedef struct _TrackerExtractControllerPrivate TrackerExtractControllerPrivate;

struct _TrackerExtractControllerPrivate {
	TrackerDecorator *decorator;
	TrackerConfig *config;
	GCancellable *cancellable;
	GDBusConnection *connection;
	GDBusProxy *index_proxy;
	guint watch_id;
	guint progress_signal_id;
	gint paused;
};

static void tracker_extract_controller_initable_iface_init (GInitableIface *iface);

G_DEFINE_TYPE_WITH_CODE (TrackerExtractController,
                         tracker_extract_controller,
                         G_TYPE_OBJECT,
                         G_ADD_PRIVATE (TrackerExtractController)
                         G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE, tracker_extract_controller_initable_iface_init))

static void
update_graphs_from_proxy (TrackerExtractController *controller,
                          GDBusProxy               *proxy)
{
	TrackerExtractControllerPrivate *priv;
	const gchar **graphs = NULL;
	GVariant *v;

	priv = tracker_extract_controller_get_instance_private (controller);

	v = g_dbus_proxy_get_cached_property (proxy, "Graphs");
	if (v)
		graphs = g_variant_get_strv (v, NULL);

	tracker_decorator_set_priority_graphs (priv->decorator, graphs);
	g_free (graphs);
}

static void
proxy_properties_changed_cb (GDBusProxy *proxy,
                             GVariant   *changed_properties,
                             GStrv       invalidated_properties,
                             gpointer    user_data)
{
	update_graphs_from_proxy (user_data, proxy);
}

static gboolean
tracker_extract_controller_initable_init (GInitable     *initable,
                                          GCancellable  *cancellable,
                                          GError       **error)
{
	TrackerExtractController *controller;
	TrackerExtractControllerPrivate *priv;
	GDBusConnection *conn;

	controller = TRACKER_EXTRACT_CONTROLLER (initable);
	priv = tracker_extract_controller_get_instance_private (controller);
	conn = priv->connection;

	priv->index_proxy = g_dbus_proxy_new_sync (conn,
	                                           G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START,
	                                           NULL,
	                                           "org.freedesktop.Tracker3.Miner.Files.Control",
	                                           "/org/freedesktop/Tracker3/Miner/Files/Proxy",
	                                           "org.freedesktop.Tracker3.Miner.Files.Proxy",
	                                           NULL,
	                                           error);
	if (!priv->index_proxy)
		return FALSE;

	g_signal_connect (priv->index_proxy, "g-properties-changed",
	                  G_CALLBACK (proxy_properties_changed_cb), controller);
	update_graphs_from_proxy (controller, priv->index_proxy);

	return TRUE;
}

static void
tracker_extract_controller_initable_iface_init (GInitableIface *iface)
{
	iface->init = tracker_extract_controller_initable_init;
}

static void
files_miner_idleness_changed (TrackerExtractController *self,
                              gboolean                  idle)
{
	TrackerExtractControllerPrivate *priv;

	priv = tracker_extract_controller_get_instance_private (self);

	if (idle && priv->paused) {
		tracker_miner_resume (TRACKER_MINER (priv->decorator));
		priv->paused = FALSE;
	} else if (!idle && !priv->paused) {
		priv->paused = FALSE;
		tracker_miner_pause (TRACKER_MINER (priv->decorator));
	}
}

static void
files_miner_status_changed (TrackerExtractController *self,
                            const gchar              *status)
{
	files_miner_idleness_changed (self, g_str_equal (status, "Idle"));
}

static void
files_miner_get_status_cb (GObject      *source,
                           GAsyncResult *result,
                           gpointer      user_data)
{
	TrackerExtractController *self = user_data;
	TrackerExtractControllerPrivate *priv;
	GDBusConnection *conn = (GDBusConnection *) source;
	GVariant *reply;
	const gchar *status;
	GError *error = NULL;

	priv = tracker_extract_controller_get_instance_private (self);

	reply = g_dbus_connection_call_finish (conn, result, &error);
	if (!reply) {
		g_debug ("Failed to get tracker-miner-fs status: %s",
		         error->message);
		g_clear_error (&error);
	} else {
		g_variant_get (reply, "(&s)", &status);
		files_miner_status_changed (self, status);
		g_variant_unref (reply);
	}

	g_clear_object (&priv->cancellable);
	g_object_unref (self);
}

static void
appeared_cb (GDBusConnection *connection,
             const gchar     *name,
             const gchar     *name_owner,
             gpointer         user_data)
{
	TrackerExtractController *self = user_data;
	TrackerExtractControllerPrivate *priv;

	priv = tracker_extract_controller_get_instance_private (self);

	/* Get initial status */
	priv->cancellable = g_cancellable_new ();
	g_dbus_connection_call (connection,
	                        "org.freedesktop.Tracker3.Miner.Files",
	                        "/org/freedesktop/Tracker3/Miner/Files",
	                        "org.freedesktop.Tracker3.Miner",
	                        "GetStatus",
	                        NULL,
	                        G_VARIANT_TYPE ("(s)"),
	                        G_DBUS_CALL_FLAGS_NO_AUTO_START,
	                        -1,
	                        priv->cancellable,
	                        files_miner_get_status_cb,
	                        g_object_ref (self));
}

static void
vanished_cb (GDBusConnection *connection,
             const gchar     *name,
             gpointer         user_data)
{
	TrackerExtractController *self = user_data;

	/* tracker-miner-fs vanished, we don't have anything to wait for
	 * anymore. */
	files_miner_idleness_changed (self, TRUE);
}

static void
files_miner_progress_cb (GDBusConnection *connection,
                         const gchar     *sender_name,
                         const gchar     *object_path,
                         const gchar     *interface_name,
                         const gchar     *signal_name,
                         GVariant        *parameters,
                         gpointer         user_data)
{
	TrackerExtractController *self = user_data;
	TrackerExtractControllerPrivate *priv;
	const gchar *status;

	priv = tracker_extract_controller_get_instance_private (self);

	g_return_if_fail (g_variant_is_of_type (parameters, G_VARIANT_TYPE ("(sdi)")));

	/* If we didn't get the initial status yet, ignore Progress signals */
	if (priv->cancellable)
		return;

	g_variant_get (parameters, "(&sdi)", &status, NULL, NULL);
	files_miner_status_changed (self, status);
}

static void
disconnect_all (TrackerExtractController *self)
{
	TrackerExtractControllerPrivate *priv;
	GDBusConnection *conn;

	priv = tracker_extract_controller_get_instance_private (self);
	conn = priv->connection;

	if (priv->watch_id != 0)
		g_bus_unwatch_name (priv->watch_id);
	priv->watch_id = 0;

	if (priv->progress_signal_id != 0)
		g_dbus_connection_signal_unsubscribe (conn,
		                                      priv->progress_signal_id);
	priv->progress_signal_id = 0;

	if (priv->cancellable)
		g_cancellable_cancel (priv->cancellable);
	g_clear_object (&priv->cancellable);
}

static void
update_wait_for_miner_fs (TrackerExtractController *self)
{
	TrackerExtractControllerPrivate *priv;
	GDBusConnection *conn;

	priv = tracker_extract_controller_get_instance_private (self);
	conn = priv->connection;

	if (tracker_config_get_wait_for_miner_fs (priv->config)) {
		priv->progress_signal_id =
			g_dbus_connection_signal_subscribe (conn,
			                                    "org.freedesktop.Tracker3.Miner.Files",
			                                    "org.freedesktop.Tracker3.Miner",
			                                    "Progress",
			                                    "/org/freedesktop/Tracker3/Miner/Files",
			                                    NULL,
			                                    G_DBUS_SIGNAL_FLAGS_NONE,
			                                    files_miner_progress_cb,
			                                    self, NULL);

		/* appeared_cb is guaranteed to be called even if the service
		 * was already running, so we'll start the miner from there. */
		priv->watch_id = g_bus_watch_name_on_connection (conn,
		                                                 "org.freedesktop.Tracker3.Miner.Files",
		                                                 G_BUS_NAME_WATCHER_FLAGS_NONE,
		                                                 appeared_cb,
		                                                 vanished_cb,
		                                                 self, NULL);
	} else {
		disconnect_all (self);
		files_miner_idleness_changed (self, TRUE);
	}
}

static void
tracker_extract_controller_constructed (GObject *object)
{
	TrackerExtractController *self = (TrackerExtractController *) object;
	TrackerExtractControllerPrivate *priv;

	priv = tracker_extract_controller_get_instance_private (self);

	G_OBJECT_CLASS (tracker_extract_controller_parent_class)->constructed (object);

	g_assert (priv->decorator != NULL);

	priv->config = g_object_ref (tracker_main_get_config ());
	g_signal_connect_object (priv->config,
	                         "notify::wait-for-miner-fs",
	                         G_CALLBACK (update_wait_for_miner_fs),
	                         self, G_CONNECT_SWAPPED);
	update_wait_for_miner_fs (self);
}

static void
tracker_extract_controller_get_property (GObject    *object,
                                         guint       param_id,
                                         GValue     *value,
                                         GParamSpec *pspec)
{
	TrackerExtractController *self = (TrackerExtractController *) object;
	TrackerExtractControllerPrivate *priv;

	priv = tracker_extract_controller_get_instance_private (self);

	switch (param_id) {
	case PROP_DECORATOR:
		g_value_set_object (value, priv->decorator);
		break;
	case PROP_CONNECTION:
		g_value_set_object (value, priv->connection);
		break;
	}
}

static void
tracker_extract_controller_set_property (GObject      *object,
                                         guint         param_id,
                                         const GValue *value,
                                         GParamSpec   *pspec)
{
	TrackerExtractController *self = (TrackerExtractController *) object;
	TrackerExtractControllerPrivate *priv;

	priv = tracker_extract_controller_get_instance_private (self);

	switch (param_id) {
	case PROP_DECORATOR:
		g_assert (priv->decorator == NULL);
		priv->decorator = g_value_dup_object (value);
		break;
	case PROP_CONNECTION:
		priv->connection = g_value_dup_object (value);
		break;
	}
}

static void
tracker_extract_controller_dispose (GObject *object)
{
	TrackerExtractController *self = (TrackerExtractController *) object;
	TrackerExtractControllerPrivate *priv;

	priv = tracker_extract_controller_get_instance_private (self);

	disconnect_all (self);
	g_clear_object (&priv->decorator);
	g_clear_object (&priv->config);
	g_clear_object (&priv->index_proxy);

	G_OBJECT_CLASS (tracker_extract_controller_parent_class)->dispose (object);
}

static void
tracker_extract_controller_class_init (TrackerExtractControllerClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	object_class->constructed = tracker_extract_controller_constructed;
	object_class->dispose = tracker_extract_controller_dispose;
	object_class->get_property = tracker_extract_controller_get_property;
	object_class->set_property = tracker_extract_controller_set_property;

	g_object_class_install_property (object_class,
	                                 PROP_DECORATOR,
	                                 g_param_spec_object ("decorator",
	                                                      "Decorator",
	                                                      "Decorator",
	                                                      TRACKER_TYPE_DECORATOR,
	                                                      G_PARAM_STATIC_STRINGS |
	                                                      G_PARAM_READWRITE |
	                                                      G_PARAM_CONSTRUCT_ONLY));
	g_object_class_install_property (object_class,
	                                 PROP_CONNECTION,
	                                 g_param_spec_object ("connection",
	                                                      "Connection",
	                                                      "Connection",
	                                                      G_TYPE_DBUS_CONNECTION,
	                                                      G_PARAM_STATIC_STRINGS |
	                                                      G_PARAM_READWRITE |
	                                                      G_PARAM_CONSTRUCT_ONLY));
}

static void
tracker_extract_controller_init (TrackerExtractController *self)
{
}

TrackerExtractController *
tracker_extract_controller_new (TrackerDecorator  *decorator,
                                GDBusConnection   *connection,
                                GError           **error)
{
	g_return_val_if_fail (TRACKER_IS_DECORATOR (decorator), NULL);

	return g_initable_new (TRACKER_TYPE_EXTRACT_CONTROLLER,
	                       NULL, error,
	                       "decorator", decorator,
	                       "connection", connection,
	                       NULL);
}
