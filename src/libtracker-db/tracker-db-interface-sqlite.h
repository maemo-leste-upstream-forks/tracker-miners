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

#ifndef __LIBTRACKER_DB_INTERFACE_SQLITE_H__
#define __LIBTRACKER_DB_INTERFACE_SQLITE_H__

#include "tracker-db-interface.h"

G_BEGIN_DECLS

#if !defined (__LIBTRACKER_DB_INSIDE__) && !defined (TRACKER_COMPILATION)
#error "only <libtracker-db/tracker-db.h> must be included directly."
#endif

typedef gint (* TrackerDBCollationFunc) (gchar *str1,
                                         gint   len1,
                                         gchar *str2,
                                         gint   len2);

TrackerDBInterface *tracker_db_interface_sqlite_new                    (const gchar              *filename);
TrackerDBInterface *tracker_db_interface_sqlite_new_ro                 (const gchar              *filename);
gboolean            tracker_db_interface_sqlite_set_collation_function (TrackerDBInterface       *interface,
                                                                        const gchar              *name,
                                                                        TrackerDBCollationFunc    func);
gint64              tracker_db_interface_sqlite_get_last_insert_id     (TrackerDBInterface       *interface);
void                tracker_db_interface_sqlite_enable_shared_cache    (void);
void                tracker_db_interface_sqlite_fts_init               (TrackerDBInterface       *interface,
                                                                        gboolean                  create);

G_END_DECLS

#endif /* __LIBTRACKER_DB_INTERFACE_SQLITE_H__ */
