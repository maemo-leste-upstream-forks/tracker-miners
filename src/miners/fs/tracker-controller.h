#ifndef __TRACKER_CONTROLLER_H__
#define __TRACKER_CONTROLLER_H__

#include <glib-object.h>

#include "tracker-indexing-tree.h"
#include "tracker-storage.h"

G_BEGIN_DECLS

#define TRACKER_TYPE_CONTROLLER (tracker_controller_get_type ())
G_DECLARE_FINAL_TYPE (TrackerController, tracker_controller, TRACKER, CONTROLLER, GObject)

TrackerController * tracker_controller_new (TrackerIndexingTree *tree,
                                            TrackerStorage      *storage);

G_END_DECLS

#endif /* __TRACKER_CONTROLLER_H__ */
