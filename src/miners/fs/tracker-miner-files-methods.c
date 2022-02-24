/*
 * Copyright (C) 2008, Nokia <ivan.frade@nokia.com>
 * Copyright (C) 2021, Red Hat Inc.
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

#include "tracker-miner-files-methods.h"

#include <libtracker-extract/tracker-extract.h>
#include <libtracker-miners-common/tracker-common.h>

#define DEFAULT_GRAPH "tracker:FileSystem"

static void
miner_files_add_to_datasource (TrackerMinerFiles *mf,
                               GFile             *file,
                               TrackerResource   *resource,
                               TrackerResource   *element_resource)
{
	TrackerIndexingTree *indexing_tree;
	TrackerMinerFS *fs = TRACKER_MINER_FS (mf);

	indexing_tree = tracker_miner_fs_get_indexing_tree (fs);

	if (tracker_indexing_tree_file_is_root (indexing_tree, file)) {
		tracker_resource_set_relation (resource, "nie:dataSource", element_resource);
	} else {
		const gchar *identifier = NULL;
		GFile *root;

		root = tracker_indexing_tree_get_root (indexing_tree, file, NULL);

		if (root)
			identifier = tracker_miner_fs_get_identifier (fs, root);

		if (identifier)
			tracker_resource_set_uri (resource, "nie:dataSource", identifier);
	}
}

static void
miner_files_add_mount_info (TrackerMinerFiles *miner,
                            TrackerResource   *resource,
                            GFile             *file)
{
	TrackerStorage *storage;
	TrackerStorageType storage_type;
	const gchar *uuid;

	storage = tracker_miner_files_get_storage (miner);
	uuid = tracker_storage_get_uuid_for_file (storage, file);
	if (!uuid)
		return;

	storage_type = tracker_storage_get_type_for_uuid (storage, uuid);

	tracker_resource_set_boolean (resource, "tracker:isRemovable",
	                              (storage_type & TRACKER_STORAGE_REMOVABLE) != 0);
	tracker_resource_set_boolean (resource, "tracker:isOptical",
	                              (storage_type & TRACKER_STORAGE_OPTICAL) != 0);
}

static TrackerResource *
miner_files_create_folder_information_element (TrackerMinerFiles *miner,
                                               GFile             *file,
                                               const gchar       *mime_type,
                                               gboolean           create)
{
	TrackerResource *resource, *file_resource;
	TrackerIndexingTree *indexing_tree;
	const gchar *urn;
	gchar *uri;

	/* Preserve URN for nfo:Folders */
	urn = tracker_miner_fs_get_identifier (TRACKER_MINER_FS (miner),
	                                       file);
	resource = tracker_resource_new (urn);

	tracker_resource_set_string (resource, "nie:mimeType", mime_type);
	tracker_resource_add_uri (resource, "rdf:type", "nie:InformationElement");

	tracker_resource_add_uri (resource, "rdf:type", "nfo:Folder");
	indexing_tree = tracker_miner_fs_get_indexing_tree (TRACKER_MINER_FS (miner));

	if (tracker_indexing_tree_file_is_root (indexing_tree, file)) {
		tracker_resource_add_uri (resource, "rdf:type", "tracker:IndexedFolder");
		tracker_resource_set_boolean (resource, "tracker:available", TRUE);
		tracker_resource_set_uri (resource, "nie:rootElementOf",
		                          tracker_resource_get_identifier (resource));

		miner_files_add_mount_info (miner, resource, file);
	}

	uri = g_file_get_uri (file);
	file_resource = tracker_resource_new (uri);
	tracker_resource_add_uri (file_resource, "rdf:type", "nfo:FileDataObject");
	g_free (uri);

	/* Laying the link between the IE and the DO */
	tracker_resource_add_take_relation (resource, "nie:isStoredAs", file_resource);
	tracker_resource_add_uri (file_resource, "nie:interpretedAs",
				  tracker_resource_get_identifier (resource));

	return resource;
}

void
tracker_miner_files_process_file (TrackerMinerFS      *fs,
                                  GFile               *file,
                                  GFileInfo           *file_info,
                                  TrackerSparqlBuffer *buffer,
                                  gboolean             create)
{
	TrackerIndexingTree *indexing_tree;
	TrackerResource *resource = NULL, *folder_resource = NULL, *graph_file = NULL;
	const gchar *mime_type, *graph;
	const gchar *parent_urn;
	gchar *delete_properties_sparql = NULL;
	GFile *parent;
	gchar *uri;
	gboolean is_directory;
	GDateTime *modified;
#ifdef GIO_SUPPORTS_CREATION_TIME
	GDateTime *accessed, *created;
#else
	time_t time_;
	gchar *time_str;
#endif

	uri = g_file_get_uri (file);
	indexing_tree = tracker_miner_fs_get_indexing_tree (fs);
	mime_type = g_file_info_get_content_type (file_info);

	is_directory = (g_file_info_get_file_type (file_info) == G_FILE_TYPE_DIRECTORY ?
	                TRUE : FALSE);

	modified = g_file_info_get_modification_date_time (file_info);
	if (!modified)
		modified = g_date_time_new_from_unix_utc (0);

	if (!create && !is_directory) {
		/* In case of update: delete all information elements for the given data object
		 * and delete extractorHash, so we ensure the file is extracted again.
		 */
		delete_properties_sparql =
			g_strdup_printf ("DELETE {"
			                 "  GRAPH ?g {"
			                 "    <%s> nie:interpretedAs ?ie . "
			                 "    ?ie a rdfs:Resource . "
			                 "  }"
			                 "} WHERE {"
			                 "  GRAPH ?g {"
			                 "    <%s> nie:interpretedAs ?ie ."
			                 "  }"
			                 "}; "
					 "DELETE WHERE {"
					 "  GRAPH " DEFAULT_GRAPH " {"
					 "    <%s> tracker:extractorHash ?h ."
					 "  }"
					 "}",
			                 uri, uri, uri);
	}

	resource = tracker_resource_new (uri);

	tracker_resource_add_uri (resource, "rdf:type", "nfo:FileDataObject");

	parent = g_file_get_parent (file);
	parent_urn = tracker_miner_fs_get_identifier (fs, parent);
	g_object_unref (parent);

	if (parent_urn) {
		tracker_resource_set_uri (resource, "nfo:belongsToContainer", parent_urn);
	}

	tracker_resource_set_string (resource, "nfo:fileName",
	                             g_file_info_get_display_name (file_info));
	tracker_resource_set_int64 (resource, "nfo:fileSize",
	                            g_file_info_get_size (file_info));

	tracker_resource_set_datetime (resource, "nfo:fileLastModified", modified);

#ifdef GIO_SUPPORTS_CREATION_TIME
	accessed = g_file_info_get_access_date_time (file_info);
	if (!accessed)
		accessed = g_date_time_new_from_unix_utc (0);

	tracker_resource_set_datetime (resource, "nfo:fileLastAccessed", accessed);
	g_date_time_unref (accessed);

	created = g_file_info_get_creation_date_time (file_info);
	if (created) {
		tracker_resource_set_datetime (resource, "nfo:fileCreated", created);
		g_date_time_unref (created);
	}
#else
	time_ = (time_t) g_file_info_get_attribute_uint64 (file_info, G_FILE_ATTRIBUTE_TIME_ACCESS);
	time_str = tracker_date_to_string (time_);
	tracker_resource_set_string (resource, "nfo:fileLastAccessed", time_str);
	g_free (time_str);
#endif

	/* The URL of the DataObject (because IE = DO, this is correct) */
	tracker_resource_set_string (resource, "nie:url", uri);

	if (is_directory || tracker_indexing_tree_file_is_root (indexing_tree, file)) {
		folder_resource =
			miner_files_create_folder_information_element (TRACKER_MINER_FILES (fs),
								       file,
								       mime_type,
								       create);
		/* Add indexing roots also to content specific graphs to provide the availability information */
		if (tracker_indexing_tree_file_is_root (indexing_tree, file)) {
			const gchar *special_graphs[] = {
				"tracker:Audio",
				"tracker:Documents",
				"tracker:Pictures",
				"tracker:Software",
				"tracker:Video"
			};
			gint i;

			for (i = 0; i < G_N_ELEMENTS (special_graphs); i++) {
				tracker_sparql_buffer_push (buffer, file, special_graphs[i], folder_resource);
			}
		}

		/* Always use inode/directory here, we don't really care if it's a symlink */
		tracker_resource_set_string (resource, "tracker:extractorHash",
		                             tracker_extract_module_manager_get_hash ("inode/directory"));
	}

	miner_files_add_to_datasource (TRACKER_MINER_FILES (fs), file, resource, folder_resource);

	graph = tracker_extract_module_manager_get_graph (mime_type);

	if (graph && g_file_info_get_size (file_info) > 0) {
		/* This mimetype will be extracted by some module, pre-fill the
		 * nfo:FileDataObject in that graph.
		 * Empty files skipped as mime-type for those cannot be trusted.
		 */
		graph_file = tracker_resource_new (uri);
		tracker_resource_add_uri (graph_file, "rdf:type", "nfo:FileDataObject");

		tracker_resource_set_string (graph_file, "nfo:fileName",
		                             g_file_info_get_display_name (file_info));

		tracker_resource_set_datetime (graph_file, "nfo:fileLastModified", modified);

		tracker_resource_set_int64 (graph_file, "nfo:fileSize",
		                            g_file_info_get_size (file_info));
		miner_files_add_to_datasource (TRACKER_MINER_FILES (fs), file, graph_file, NULL);
	}

	if (delete_properties_sparql)
		tracker_sparql_buffer_push_sparql (buffer, file, delete_properties_sparql);

	tracker_sparql_buffer_push (buffer, file, DEFAULT_GRAPH, resource);

	if (graph_file)
		tracker_sparql_buffer_push (buffer, file, graph, graph_file);
	if (folder_resource)
		tracker_sparql_buffer_push (buffer, file, DEFAULT_GRAPH, folder_resource);

	g_date_time_unref (modified);
	g_object_unref (resource);
	g_clear_object (&folder_resource);
	g_clear_object (&graph_file);
	g_free (delete_properties_sparql);
	g_free (uri);
}

void
tracker_miner_files_process_file_attributes (TrackerMinerFS      *fs,
                                             GFile               *file,
                                             GFileInfo           *info,
                                             TrackerSparqlBuffer *buffer)
{
	TrackerResource *resource, *graph_file;
	gchar *uri;
	const gchar *mime_type, *graph;
	GDateTime *modified;
#ifdef GIO_SUPPORTS_CREATION_TIME
	GDateTime *accessed, *created;
#else
	gchar *time_str;
	time_t time_;
#endif

	uri = g_file_get_uri (file);
	resource = tracker_resource_new (uri);
	tracker_resource_add_uri (resource, "rdf:type", "nfo:FileDataObject");

	if (!info) {
		info = g_file_query_info (file,
		                          G_FILE_ATTRIBUTE_TIME_MODIFIED ","
		                          G_FILE_ATTRIBUTE_TIME_ACCESS ","
					  G_FILE_ATTRIBUTE_TIME_CREATED,
		                          G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
		                          NULL, NULL);
	}

	modified = g_file_info_get_modification_date_time (info);
	if (!modified)
		modified = g_date_time_new_from_unix_utc (0);

	mime_type = g_file_info_get_content_type (info);
	graph = tracker_extract_module_manager_get_graph (mime_type);

	/* Update nfo:fileLastModified */
	tracker_resource_set_datetime (resource, "nfo:fileLastModified", modified);
	if (graph) {
		graph_file = tracker_resource_new (uri);
		tracker_resource_add_uri (graph_file, "rdf:type", "nfo:FileDataObject");
		tracker_resource_set_datetime (graph_file, "nfo:fileLastModified", modified);
		tracker_sparql_buffer_push (buffer, file, graph, graph_file);
		g_clear_object (&graph_file);
	}
	g_date_time_unref (modified);

#ifdef GIO_SUPPORTS_CREATION_TIME
	/* Update nfo:fileLastAccessed */
	accessed = g_file_info_get_access_date_time (info);
	tracker_resource_set_datetime (resource, "nfo:fileLastAccessed", accessed);
	g_date_time_unref (accessed);

	/* Update nfo:fileCreated */
	created = g_file_info_get_creation_date_time (info);

	if (created) {
		tracker_resource_set_datetime (resource, "nfo:fileCreated", created);
		g_date_time_unref (created);
	}
#else
	time_ = (time_t) g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_ACCESS);
	time_str = tracker_date_to_string (time_);
	tracker_resource_set_string (resource, "nfo:fileLastAccessed", time_str);
	g_free (time_str);
#endif

	g_free (uri);

	tracker_sparql_buffer_push (buffer, file, DEFAULT_GRAPH, resource);
	g_object_unref (resource);
}
