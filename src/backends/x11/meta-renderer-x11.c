/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/*
 * Copyright (C) 2016 Red Hat
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 *
 * Written by:
 *     Jonas Ådahl <jadahl@gmail.com>
 */

#include "config.h"

#include <glib-object.h>

#include "clutter/x11/clutter-x11.h"
#include "cogl/cogl-mutter-config.h"
#include "cogl/cogl.h"
#include "cogl/cogl-xlib.h"
#include "cogl/winsys/cogl-winsys-glx-private.h"
#include "cogl/winsys/cogl-winsys-egl-x11-private.h"
#include "backends/meta-backend-private.h"
#include "backends/meta-renderer.h"
#include "backends/meta-renderer-view.h"
#include "backends/x11/meta-renderer-x11.h"
#include "core/boxes-private.h"
#include "meta/meta-backend.h"
#include "meta/util.h"

struct _MetaRendererX11
{
  MetaRenderer parent;
};

G_DEFINE_TYPE (MetaRendererX11, meta_renderer_x11, META_TYPE_RENDERER)

static const CoglWinsysVtable *
get_x11_cogl_winsys_vtable (CoglRenderer *renderer)
{
#ifdef HAVE_WAYLAND
  if (meta_is_wayland_compositor ())
    return _cogl_winsys_egl_xlib_get_vtable ();
#endif

  switch (renderer->driver)
    {
    case COGL_DRIVER_GLES1:
    case COGL_DRIVER_GLES2:
      return _cogl_winsys_egl_xlib_get_vtable ();
    case COGL_DRIVER_GL:
    case COGL_DRIVER_GL3:
#if HAVE_COGL_GL
      return _cogl_winsys_glx_get_vtable ();
#endif
    case COGL_DRIVER_ANY:
    case COGL_DRIVER_NOP:
    case COGL_DRIVER_WEBGL:
      break;
    }
  g_assert_not_reached ();
}

static CoglRenderer *
meta_renderer_x11_create_cogl_renderer (MetaRenderer *renderer)
{
  CoglRenderer *cogl_renderer;
  Display *xdisplay = clutter_x11_get_default_display ();

  cogl_renderer = cogl_renderer_new ();
  cogl_renderer_set_custom_winsys (cogl_renderer, get_x11_cogl_winsys_vtable);
  cogl_xlib_renderer_set_foreign_display (cogl_renderer, xdisplay);

  return cogl_renderer;
}

static MetaRendererView *
meta_renderer_x11_create_view (MetaRenderer    *renderer,
                               MetaMonitorInfo *monitor_info)
{
  MetaBackend *backend = meta_get_backend ();
  ClutterBackend *clutter_backend = meta_backend_get_clutter_backend (backend);
  CoglContext *cogl_context = clutter_backend_get_cogl_context (clutter_backend);
  int width, height;
  CoglTexture2D *texture_2d;
  CoglOffscreen *offscreen;
  GError *error = NULL;

  g_assert (meta_is_wayland_compositor ());

  width = monitor_info->rect.width;
  height = monitor_info->rect.height;
  texture_2d = cogl_texture_2d_new_with_size (cogl_context, width, height);
  offscreen = cogl_offscreen_new_with_texture (COGL_TEXTURE (texture_2d));

  if (!cogl_framebuffer_allocate (COGL_FRAMEBUFFER (offscreen), &error))
    meta_fatal ("Couldn't allocate framebuffer: %s", error->message);

  return g_object_new (META_TYPE_RENDERER_VIEW,
                       "layout", &monitor_info->rect,
                       "framebuffer", COGL_FRAMEBUFFER (offscreen),
                       NULL);
}

static void
meta_renderer_x11_init (MetaRendererX11 *renderer_x11)
{
}

static void
meta_renderer_x11_class_init (MetaRendererX11Class *klass)
{
  MetaRendererClass *renderer_class = META_RENDERER_CLASS (klass);

  renderer_class->create_cogl_renderer = meta_renderer_x11_create_cogl_renderer;
  renderer_class->create_view = meta_renderer_x11_create_view;
}
