/**
 * Evolution-DecSync - e-source-decsync.h
 *
 * Copyright (C) 2018 Aldo Gunsing
 *
 * This library is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License
 * for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef E_SOURCE_DECSYNC_H
#define E_SOURCE_DECSYNC_H

#include <libedataserver/libedataserver.h>

/* Standard GObject macros */
#define E_TYPE_SOURCE_DECSYNC \
	(e_source_decsync_get_type ())
#define E_SOURCE_DECSYNC(obj) \
	(G_TYPE_CHECK_INSTANCE_CAST \
	((obj), E_TYPE_SOURCE_DECSYNC, ESourceDecsync))
#define E_SOURCE_DECSYNC_CLASS(cls) \
	(G_TYPE_CHECK_CLASS_CAST \
	((cls), E_TYPE_SOURCE_DECSYNC, ESourceDecsyncClass))
#define E_IS_SOURCE_DECSYNC(obj) \
	(G_TYPE_CHECK_INSTANCE_TYPE \
	((obj), E_TYPE_SOURCE_DECSYNC))
#define E_IS_SOURCE_DECSYNC_CLASS(cls) \
	(G_TYPE_CHECK_CLASS_TYPE \
	((cls), E_TYPE_SOURCE_DECSYNC))
#define E_SOURCE_DECSYNC_GET_CLASS(obj) \
	(G_TYPE_INSTANCE_GET_CLASS \
	((obj), E_TYPE_SOURCE_DECSYNC, ESourceDecsyncClass))

/**
 * E_SOURCE_EXTENSION_DECSYNC_BACKEND:
 *
 * Pass this extension name to e_source_get_extension() to access
 * #ESourceDecsync.  This is also used as a group name in key files.
 *
 * Since: 3.18
 **/
#define E_SOURCE_EXTENSION_DECSYNC_BACKEND "DecSync Backend"

G_BEGIN_DECLS

typedef struct _ESourceDecsync ESourceDecsync;
typedef struct _ESourceDecsyncClass ESourceDecsyncClass;
typedef struct _ESourceDecsyncPrivate ESourceDecsyncPrivate;

struct _ESourceDecsync {
	/*< private >*/
	ESourceExtension parent;
	ESourceDecsyncPrivate *priv;
};

struct _ESourceDecsyncClass {
	ESourceExtensionClass parent_class;
};

GType		e_source_decsync_get_type		(void);
const gchar *	e_source_decsync_get_decsync_dir	(ESourceDecsync *extension);
gchar *		e_source_decsync_dup_decsync_dir	(ESourceDecsync *extension);
void		e_source_decsync_set_decsync_dir	(ESourceDecsync *extension, const gchar *decsync_dir);
const gchar *	e_source_decsync_get_collection	(ESourceDecsync *extension);
gchar *		e_source_decsync_dup_collection	(ESourceDecsync *extension);
void		e_source_decsync_set_collection	(ESourceDecsync *extension, const gchar *collection);
const gchar *	e_source_decsync_get_appid	(ESourceDecsync *extension);
gchar *		e_source_decsync_dup_appid	(ESourceDecsync *extension);
void		e_source_decsync_set_appid	(ESourceDecsync *extension, const gchar *appid);

G_END_DECLS

#endif /* E_SOURCE_DECSYNC_H */
