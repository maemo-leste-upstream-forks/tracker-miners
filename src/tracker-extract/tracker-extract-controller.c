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

#include <gio/gunixfdlist.h>

enum {
	PROP_DECORATOR = 1,
	PROP_CONNECTION,
	PROP_PERSISTENCE,
};

struct TrackerExtractControllerPrivate {
	TrackerDecorator *decorator;
	TrackerExtractPersistence *persistence;
	GCancellable *cancellable;
	GDBusConnection *connection;
	GDBusProxy *miner_proxy;
	guint object_id;
	gint paused;
};

#define OBJECT_PATH "/org/freedesktop/Tracker3/Extract"

static const gchar *introspection_xml =
	"<node>"
	"  <interface name='org.freedesktop.Tracker3.Extract'>"
	"    <signal name='Error'>"
	"      <arg type='a{sv}' name='data' direction='out' />"
	"    </signal>"
	"  </interface>"
	"</node>";

G_DEFINE_TYPE_WITH_PRIVATE (TrackerExtractController, tracker_extract_controller, G_TYPE_OBJECT)

static void
update_extract_config (TrackerExtractController *controller,
                       GDBusProxy               *proxy)
{
	TrackerExtractControllerPrivate *priv;
	GVariantIter iter;
	g_autoptr (GVariant) v = NULL;
	GVariant *value;
	gchar *key;

	priv = tracker_extract_controller_get_instance_private (controller);

	v = g_dbus_proxy_get_cached_property (proxy, "ExtractorConfig");
	if (!v)
		return;

	g_variant_iter_init (&iter, v);

	while (g_variant_iter_next (&iter, "{sv}", &key, &value)) {
		if (g_strcmp0 (key, "max-bytes") == 0 &&
		    g_variant_is_of_type (value, G_VARIANT_TYPE_INT32)) {
			TrackerExtract *extract = NULL;
			gint max_bytes;

			max_bytes = g_variant_get_int32 (value);
			g_object_get (priv->decorator, "extractor", &extract, NULL);

			if (extract) {
				tracker_extract_set_max_text (extract, max_bytes);
				g_object_unref (extract);
			}
		}

		g_free (key);
		g_variant_unref (value);
	}
}

static void
miner_properties_changed_cb (GDBusProxy *proxy,
                             GVariant   *changed_properties,
                             GStrv       invalidated_properties,
                             gpointer    user_data)
{
	update_extract_config (user_data, proxy);
}

static gboolean
set_up_persistence (TrackerExtractController  *controller,
                    GCancellable              *cancellable,
                    GError                   **error)
{
	TrackerExtractControllerPrivate *priv =
		tracker_extract_controller_get_instance_private (controller);
	g_autoptr (GUnixFDList) out_fd_list = NULL;
	g_autoptr (GVariant) variant = NULL;
	int idx, fd;

	variant = g_dbus_proxy_call_with_unix_fd_list_sync (priv->miner_proxy,
	                                                    "GetPersistenceStorage",
	                                                    NULL,
	                                                    G_DBUS_CALL_FLAGS_NO_AUTO_START,
	                                                    -1,
	                                                    NULL,
	                                                    &out_fd_list,
	                                                    cancellable,
	                                                    error);
	if (!variant)
		return FALSE;

	g_variant_get (variant, "(h)", &idx);
	fd = g_unix_fd_list_get (out_fd_list, idx, error);
	if (fd < 0)
		return FALSE;

	tracker_extract_persistence_set_fd (priv->persistence, fd);
	return TRUE;
}

static void
decorator_raise_error_cb (TrackerDecorator         *decorator,
                          GFile                    *file,
                          gchar                    *msg,
                          gchar                    *extra,
                          TrackerExtractController *controller)
{
	TrackerExtractControllerPrivate *priv =
		tracker_extract_controller_get_instance_private (controller);
	g_autoptr (GError) error = NULL;
	g_autofree gchar *uri = NULL;
	GVariantBuilder builder;

	uri = g_file_get_uri (file);

	g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sv}"));
	g_variant_builder_add (&builder, "{sv}", "uri",
	                       g_variant_new_string (uri));
	g_variant_builder_add (&builder, "{sv}", "message",
	                       g_variant_new_string (msg));

	if (extra) {
		g_variant_builder_add (&builder, "{sv}", "extra-info",
		                       g_variant_new_string (extra));
	}

	g_dbus_connection_emit_signal (priv->connection,
	                               NULL,
	                               OBJECT_PATH,
	                               "org.freedesktop.Tracker3.Extract",
	                               "Error",
	                               g_variant_new ("(@a{sv})", g_variant_builder_end (&builder)),
	                               &error);

	if (error)
		g_warning ("Could not emit signal: %s\n", error->message);
}

static void
tracker_extract_controller_constructed (GObject *object)
{
	TrackerExtractController *self = (TrackerExtractController *) object;
	g_autoptr (GDBusNodeInfo) introspection_data = NULL;
	GDBusInterfaceVTable interface_vtable = {
		NULL, NULL, NULL
	};

	G_OBJECT_CLASS (tracker_extract_controller_parent_class)->constructed (object);

	g_assert (self->priv->decorator != NULL);

	g_signal_connect (self->priv->decorator, "raise-error",
	                  G_CALLBACK (decorator_raise_error_cb), object);

	introspection_data = g_dbus_node_info_new_for_xml (introspection_xml, NULL);
	g_assert (introspection_data);
	self->priv->object_id =
		g_dbus_connection_register_object (self->priv->connection,
						   OBJECT_PATH,
		                                   introspection_data->interfaces[0],
		                                   &interface_vtable,
		                                   object,
		                                   NULL, NULL);

	self->priv->miner_proxy = g_dbus_proxy_new_sync (self->priv->connection,
	                                                 G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START,
	                                                 NULL,
	                                                 "org.freedesktop.Tracker3.Miner.Files",
	                                                 "/org/freedesktop/Tracker3/Files",
	                                                 "org.freedesktop.Tracker3.Files",
	                                                 NULL, NULL);
	if (self->priv->miner_proxy) {
		g_signal_connect (self->priv->miner_proxy, "g-properties-changed",
		                  G_CALLBACK (miner_properties_changed_cb), self);
		update_extract_config (self, self->priv->miner_proxy);
	}

	set_up_persistence (self, NULL, NULL);
}

static void
tracker_extract_controller_get_property (GObject    *object,
                                         guint       param_id,
                                         GValue     *value,
                                         GParamSpec *pspec)
{
	TrackerExtractController *self = (TrackerExtractController *) object;

	switch (param_id) {
	case PROP_DECORATOR:
		g_value_set_object (value, self->priv->decorator);
		break;
	case PROP_CONNECTION:
		g_value_set_object (value, self->priv->connection);
		break;
	case PROP_PERSISTENCE:
		g_value_set_object (value, self->priv->persistence);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, param_id, pspec);
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

	switch (param_id) {
	case PROP_DECORATOR:
		g_assert (self->priv->decorator == NULL);
		self->priv->decorator = g_value_dup_object (value);
		break;
	case PROP_CONNECTION:
		self->priv->connection = g_value_dup_object (value);
		break;
	case PROP_PERSISTENCE:
		self->priv->persistence = g_value_dup_object (value);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, param_id, pspec);
		break;
	}
}

static void
tracker_extract_controller_dispose (GObject *object)
{
	TrackerExtractController *self = (TrackerExtractController *) object;

	if (self->priv->connection && self->priv->object_id) {
		g_dbus_connection_unregister_object (self->priv->connection, self->priv->object_id);
		self->priv->object_id = 0;
	}

	g_clear_object (&self->priv->decorator);
	g_clear_object (&self->priv->persistence);

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
	g_object_class_install_property (object_class,
	                                 PROP_PERSISTENCE,
	                                 g_param_spec_object ("persistence",
	                                                      NULL, NULL,
	                                                      TRACKER_TYPE_EXTRACT_PERSISTENCE,
	                                                      G_PARAM_STATIC_STRINGS |
	                                                      G_PARAM_READWRITE |
	                                                      G_PARAM_CONSTRUCT_ONLY));
}

static void
tracker_extract_controller_init (TrackerExtractController *self)
{
	self->priv = tracker_extract_controller_get_instance_private (self);
}

TrackerExtractController *
tracker_extract_controller_new (TrackerDecorator          *decorator,
                                GDBusConnection           *connection,
                                TrackerExtractPersistence *persistence)
{
	g_return_val_if_fail (TRACKER_IS_DECORATOR (decorator), NULL);

	return g_object_new (TRACKER_TYPE_EXTRACT_CONTROLLER,
	                     "decorator", decorator,
	                     "connection", connection,
	                     "persistence", persistence,
	                     NULL);
}
