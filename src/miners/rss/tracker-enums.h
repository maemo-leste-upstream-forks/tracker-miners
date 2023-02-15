/*
 * Copyright (C) 2011, Nokia <ivan.frade@nokia.com>
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
 * Author: Carlos Garnacho  <carlosg@gnome.org>
 */

#ifndef __TRACKER_MINER_RSS_ENUMS_H__
#define __TRACKER_MINER_RSS_ENUMS_H__

G_BEGIN_DECLS

/**
 * TrackerNetworkType:
 * @TRACKER_NETWORK_TYPE_NONE: Network is disconnected
 * @TRACKER_NETWORK_TYPE_UNKNOWN: Network status is unknown
 * @TRACKER_NETWORK_TYPE_GPRS: Network is connected over a GPRS
 * connection
 * @TRACKER_NETWORK_TYPE_EDGE: Network is connected over an EDGE
 * connection
 * @TRACKER_NETWORK_TYPE_3G: Network is connected over a 3G or
 * faster (HSDPA, UMTS, ...) connection
 * @TRACKER_NETWORK_TYPE_LAN: Network is connected over a local
 * network connection. This can be ethernet, wifi, etc.
 *
 * Enumerates the different types of connections that the device might
 * use when connected to internet. Note that not all providers might
 * provide this information.
 *
 * Since: 0.18
 **/
typedef enum {
	TRACKER_NETWORK_TYPE_NONE,
	TRACKER_NETWORK_TYPE_UNKNOWN,
	TRACKER_NETWORK_TYPE_GPRS,
	TRACKER_NETWORK_TYPE_EDGE,
	TRACKER_NETWORK_TYPE_3G,
	TRACKER_NETWORK_TYPE_LAN
} TrackerNetworkType;

G_END_DECLS

#endif /* __TRACKER_MINER_RSS_ENUMS_H__ */
