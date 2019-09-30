/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*- */

/* e-book-backend-decsync.h - DecSync contact backend.
 * Based on the file contact backend.
 *
 * Copyright (C) 1999-2008 Novell, Inc. (www.novell.com)
 * Copyright (C) 2012 Intel Corporation
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
 *
 * Authors: Nat Friedman <nat@novell.com>
 *          Chris Toshok <toshok@ximian.com>
 *          Hans Petter Jansson <hpj@novell.com>
 *          Tristan Van Berkom <tristanvb@openismus.com>
 */

#ifndef E_BOOK_BACKEND_DECSYNC_H
#define E_BOOK_BACKEND_DECSYNC_H

#include <libedata-book/libedata-book.h>

/* Standard GObject macros */
#define E_TYPE_BOOK_BACKEND_DECSYNC \
	(e_book_backend_decsync_get_type ())
#define E_BOOK_BACKEND_DECSYNC(obj) \
	(G_TYPE_CHECK_INSTANCE_CAST \
	((obj), E_TYPE_BOOK_BACKEND_DECSYNC, EBookBackendDecsync))
#define E_BOOK_BACKEND_DECSYNC_CLASS(cls) \
	(G_TYPE_CHECK_CLASS_CAST \
	((cls), E_TYPE_BOOK_BACKEND_DECSYNC, EBookBackendDecsyncClass))
#define E_IS_BOOK_BACKEND_DECSYNC(obj) \
	(G_TYPE_CHECK_INSTANCE_TYPE \
	((obj), E_TYPE_BOOK_BACKEND_DECSYNC))
#define E_IS_BOOK_BACKEND_DECSYNC_CLASS(cls) \
	(G_TYPE_CHECK_CLASS_TYPE \
	((cls), E_TYPE_BOOK_BACKEND_DECSYNC))
#define E_BOOK_BACKEND_DECSYNC_GET_CLASS(obj) \
	(G_TYPE_INSTANCE_GET_CLASS \
	((obj), E_TYPE_BOOK_BACKEND_DECSYNC, EBookBackendDecsyncClass))

G_BEGIN_DECLS

typedef struct _EBookBackendDecsync EBookBackendDecsync;
typedef struct _EBookBackendDecsyncClass EBookBackendDecsyncClass;
typedef struct _EBookBackendDecsyncPrivate EBookBackendDecsyncPrivate;

struct _EBookBackendDecsync {
	EBookBackendSync parent;
	EBookBackendDecsyncPrivate *priv;
};

struct _EBookBackendDecsyncClass {
	EBookBackendSyncClass parent_class;
};

GType		e_book_backend_decsync_get_type	(void);

G_END_DECLS

#endif /* E_BOOK_BACKEND_DECSYNC_H */

