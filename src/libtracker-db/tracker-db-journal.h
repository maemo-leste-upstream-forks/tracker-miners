/*
 * Copyright (C) 2009, Nokia
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
 *
 * Author: Philip Van Hoof <philip@codeminded.be>
 */

#ifndef __LIBTRACKER_DB_JOURNAL_H__
#define __LIBTRACKER_DB_JOURNAL_H__

#include <glib.h>
#include <gio/gio.h>

G_BEGIN_DECLS

#define TRACKER_DB_JOURNAL_ERROR_DOMAIN "TrackerDBJournal"
#define TRACKER_DB_JOURNAL_ERROR        tracker_db_journal_error_quark()

typedef enum {
	TRACKER_DB_JOURNAL_START,
	TRACKER_DB_JOURNAL_START_TRANSACTION,
	TRACKER_DB_JOURNAL_END_TRANSACTION,
	TRACKER_DB_JOURNAL_RESOURCE,
	TRACKER_DB_JOURNAL_INSERT_STATEMENT,
	TRACKER_DB_JOURNAL_INSERT_STATEMENT_ID,
	TRACKER_DB_JOURNAL_DELETE_STATEMENT,
	TRACKER_DB_JOURNAL_DELETE_STATEMENT_ID
} TrackerDBJournalEntryType;

GQuark       tracker_db_journal_error_quark                  (void);

/*
 * Writer API
 */
gboolean     tracker_db_journal_init                         (const gchar *filename);
gboolean     tracker_db_journal_shutdown                     (void);

const gchar* tracker_db_journal_get_filename                 (void);
gsize        tracker_db_journal_get_size                     (void);

gboolean     tracker_db_journal_start_transaction            (void);
gboolean     tracker_db_journal_append_delete_statement      (guint32      s_id,
                                                              guint32      p_id,
                                                              const gchar *object);
gboolean     tracker_db_journal_append_delete_statement_id   (guint32      s_id,
                                                              guint32      p_id,
                                                              guint32      o_id);
gboolean     tracker_db_journal_append_insert_statement      (guint32      s_id, 
                                                              guint32      p_id, 
                                                              const gchar *object);
gboolean     tracker_db_journal_append_insert_statement_id   (guint32      s_id,
                                                              guint32      p_id,
                                                              guint32      o_id);
gboolean     tracker_db_journal_append_resource              (guint32      s_id,
                                                              const gchar *uri);

gboolean     tracker_db_journal_rollback_transaction         (void);
gboolean     tracker_db_journal_commit_transaction           (void);

gboolean     tracker_db_journal_fsync                        (void);

/*
 * Reader API
 */
gboolean     tracker_db_journal_reader_init                  (const gchar  *filename);
gboolean     tracker_db_journal_reader_shutdown              (void);
TrackerDBJournalEntryType
             tracker_db_journal_reader_get_type              (void);

gboolean     tracker_db_journal_reader_next                  (GError      **error);
gboolean     tracker_db_journal_reader_get_resource          (guint32      *id,
                                                              const gchar **uri);
gboolean     tracker_db_journal_reader_get_statement         (guint32      *s_id,
                                                              guint32      *p_id,
                                                              const gchar **object);
gboolean     tracker_db_journal_reader_get_statement_id      (guint32      *s_id,
                                                              guint32      *p_id,
                                                              guint32      *o_id);

G_END_DECLS

#endif /* __LIBTRACKER_DB_JOURNAL_H__ */
