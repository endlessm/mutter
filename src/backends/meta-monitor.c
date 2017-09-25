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
 */

#include "config.h"

#include "backends/meta-monitor.h"

#include "backends/meta-backend-private.h"
#include "backends/meta-monitor-manager-private.h"

typedef struct _MetaMonitorMode
{
  MetaMonitorModeSpec spec;
  MetaMonitorCrtcMode *crtc_modes;
} MetaMonitorMode;

typedef struct _MetaMonitorPrivate
{
  MetaMonitorManager *monitor_manager;

  GList *outputs;
  GList *modes;

  MetaMonitorMode *preferred_mode;
  MetaMonitorMode *current_mode;

  MetaMonitorSpec *spec;

  /*
   * The primary or first output for this monitor, 0 if we can't figure out.
   * It can be matched to a winsys_id of a MetaOutput.
   *
   * This is used as an opaque token on reconfiguration when switching from
   * clone to extened, to decide on what output the windows should go next
   * (it's an attempt to keep windows on the same monitor, and preferably on
   * the primary one).
   */
  long winsys_id;
} MetaMonitorPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (MetaMonitor, meta_monitor, G_TYPE_OBJECT)

struct _MetaMonitorNormal
{
  MetaMonitor parent;
};

G_DEFINE_TYPE (MetaMonitorNormal, meta_monitor_normal, META_TYPE_MONITOR)

struct _MetaMonitorTiled
{
  MetaMonitor parent;

  uint32_t tile_group_id;

  /* The tile (0, 0) output. */
  MetaOutput *origin_output;

  /* The output enabled even when a non-tiled mode is used. */
  MetaOutput *main_output;
};

G_DEFINE_TYPE (MetaMonitorTiled, meta_monitor_tiled, META_TYPE_MONITOR)

static void
meta_monitor_mode_free (MetaMonitorMode *mode);

MetaMonitorSpec *
meta_monitor_spec_clone (MetaMonitorSpec *monitor_spec)
{
  MetaMonitorSpec *new_monitor_spec;

  new_monitor_spec = g_new0 (MetaMonitorSpec, 1);
  *new_monitor_spec = (MetaMonitorSpec) {
    .connector = g_strdup (monitor_spec->connector),
    .vendor = g_strdup (monitor_spec->vendor),
    .product = g_strdup (monitor_spec->product),
    .serial = g_strdup (monitor_spec->serial),
  };

  return new_monitor_spec;
}

gboolean
meta_monitor_spec_equals (MetaMonitorSpec *monitor_spec,
                          MetaMonitorSpec *other_monitor_spec)
{
  return (g_str_equal (monitor_spec->connector, other_monitor_spec->connector) &&
          g_str_equal (monitor_spec->vendor, other_monitor_spec->vendor) &&
          g_str_equal (monitor_spec->product, other_monitor_spec->product) &&
          g_str_equal (monitor_spec->serial, other_monitor_spec->serial));
}

int
meta_monitor_spec_compare (MetaMonitorSpec *monitor_spec_a,
                           MetaMonitorSpec *monitor_spec_b)
{
  int ret;

  ret = strcmp (monitor_spec_a->connector, monitor_spec_b->connector);
  if (ret != 0)
    return ret;

  ret = strcmp (monitor_spec_a->vendor, monitor_spec_b->vendor);
  if (ret != 0)
    return ret;

  ret = strcmp (monitor_spec_a->product, monitor_spec_b->product);
  if (ret != 0)
    return ret;

  return strcmp (monitor_spec_a->serial, monitor_spec_b->serial);
}

void
meta_monitor_spec_free (MetaMonitorSpec *monitor_spec)
{
  g_free (monitor_spec->connector);
  g_free (monitor_spec->vendor);
  g_free (monitor_spec->product);
  g_free (monitor_spec->serial);
  g_free (monitor_spec);
}

static void
meta_monitor_generate_spec (MetaMonitor *monitor)
{
  MetaMonitorPrivate *priv = meta_monitor_get_instance_private (monitor);
  MetaOutput *output = meta_monitor_get_main_output (monitor);
  MetaMonitorSpec *monitor_spec;

  monitor_spec = g_new0 (MetaMonitorSpec, 1);
  *monitor_spec = (MetaMonitorSpec) {
    .connector = g_strdup (output->name),
    .vendor = g_strdup (output->vendor),
    .product = g_strdup (output->product),
    .serial = g_strdup (output->serial),
  };

  priv->spec = monitor_spec;
}

GList *
meta_monitor_get_outputs (MetaMonitor *monitor)
{
  MetaMonitorPrivate *priv = meta_monitor_get_instance_private (monitor);

  return priv->outputs;
}

MetaOutput *
meta_monitor_get_main_output (MetaMonitor *monitor)
{
  return META_MONITOR_GET_CLASS (monitor)->get_main_output (monitor);
}

gboolean
meta_monitor_is_active (MetaMonitor *monitor)
{
  MetaOutput *output;

  output = meta_monitor_get_main_output (monitor);

  return output->crtc && output->crtc->current_mode;
}

gboolean
meta_monitor_is_primary (MetaMonitor *monitor)
{
  MetaOutput *output;

  output = meta_monitor_get_main_output (monitor);

  return output->is_primary;
}

gboolean
meta_monitor_is_underscanning (MetaMonitor *monitor)
{
  MetaOutput *output;

  output = meta_monitor_get_main_output (monitor);

  return output->is_underscanning;
}

gboolean
meta_monitor_is_laptop_panel (MetaMonitor *monitor)
{
  MetaOutput *output;

  output = meta_monitor_get_main_output (monitor);

  switch (output->connector_type)
    {
    case META_CONNECTOR_TYPE_eDP:
    case META_CONNECTOR_TYPE_LVDS:
    case META_CONNECTOR_TYPE_DSI:
      return TRUE;
    default:
      return FALSE;
    }
}

void
meta_monitor_get_current_resolution (MetaMonitor *monitor,
                                     int         *width,
                                     int         *height)
{
  MetaMonitorMode *mode = meta_monitor_get_current_mode (monitor);

  *width = mode->spec.width;
  *height = mode->spec.height;
}

void
meta_monitor_derive_layout (MetaMonitor   *monitor,
                            MetaRectangle *layout)
{
  META_MONITOR_GET_CLASS (monitor)->derive_layout (monitor, layout);
}

void
meta_monitor_get_physical_dimensions (MetaMonitor *monitor,
                                      int         *width_mm,
                                      int         *height_mm)
{
  MetaOutput *output;

  output = meta_monitor_get_main_output (monitor);
  *width_mm = output->width_mm;
  *height_mm = output->height_mm;
}

CoglSubpixelOrder
meta_monitor_get_subpixel_order (MetaMonitor *monitor)
{
  MetaOutput *output;

  output = meta_monitor_get_main_output (monitor);
  return output->subpixel_order;
}

const char *
meta_monitor_get_vendor (MetaMonitor *monitor)
{
  MetaOutput *output;

  output = meta_monitor_get_main_output (monitor);
  return output->vendor;
}

const char *
meta_monitor_get_product (MetaMonitor *monitor)
{
  MetaOutput *output;

  output = meta_monitor_get_main_output (monitor);
  return output->product;
}

const char *
meta_monitor_get_serial (MetaMonitor *monitor)
{
  MetaOutput *output;

  output = meta_monitor_get_main_output (monitor);
  return output->serial;
}

static void
meta_monitor_finalize (GObject *object)
{
  MetaMonitor *monitor = META_MONITOR (object);
  MetaMonitorPrivate *priv = meta_monitor_get_instance_private (monitor);

  g_list_free_full (priv->modes, (GDestroyNotify) meta_monitor_mode_free);
  g_clear_pointer (&priv->outputs, g_list_free);
  meta_monitor_spec_free (priv->spec);
}

static void
meta_monitor_init (MetaMonitor *monitor)
{
}

static void
meta_monitor_class_init (MetaMonitorClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = meta_monitor_finalize;
}

static void
meta_monitor_normal_generate_modes (MetaMonitorNormal *monitor_normal)
{
  MetaMonitor *monitor = META_MONITOR (monitor_normal);
  MetaMonitorPrivate *monitor_priv =
    meta_monitor_get_instance_private (monitor);
  MetaOutput *output;
  unsigned int i;

  output = meta_monitor_get_main_output (monitor);
  for (i = 0; i < output->n_modes; i++)
    {
      MetaCrtcMode *crtc_mode = output->modes[i];
      MetaMonitorMode *mode;

      mode = g_new0 (MetaMonitorMode, 1);
      mode->spec = (MetaMonitorModeSpec) {
        .width = crtc_mode->width,
        .height = crtc_mode->height,
        .refresh_rate = crtc_mode->refresh_rate
      },
      mode->crtc_modes = g_new (MetaMonitorCrtcMode, 1);
      mode->crtc_modes[0] = (MetaMonitorCrtcMode) {
        .x = 0,
        .y = 0,
        .output = output,
        .crtc_mode = crtc_mode
      };

      if (crtc_mode == output->preferred_mode)
        monitor_priv->preferred_mode = mode;
      if (output->crtc && crtc_mode == output->crtc->current_mode)
        monitor_priv->current_mode = mode;

      monitor_priv->modes = g_list_append (monitor_priv->modes, mode);
    }
}

MetaMonitorNormal *
meta_monitor_normal_new (MetaMonitorManager *monitor_manager,
                         MetaOutput         *output)
{
  MetaMonitorNormal *monitor_normal;
  MetaMonitor *monitor;
  MetaMonitorPrivate *monitor_priv;

  monitor_normal = g_object_new (META_TYPE_MONITOR_NORMAL, NULL);
  monitor = META_MONITOR (monitor_normal);
  monitor_priv = meta_monitor_get_instance_private (monitor);

  monitor_priv->monitor_manager = monitor_manager;

  monitor_priv->outputs = g_list_append (NULL, output);
  monitor_priv->winsys_id = output->winsys_id;
  meta_monitor_generate_spec (monitor);

  meta_monitor_normal_generate_modes (monitor_normal);

  return monitor_normal;
}

static MetaOutput *
meta_monitor_normal_get_main_output (MetaMonitor *monitor)
{
  MetaMonitorPrivate *monitor_priv =
    meta_monitor_get_instance_private (monitor);

  return monitor_priv->outputs->data;
}

static void
meta_monitor_normal_derive_layout (MetaMonitor   *monitor,
                                   MetaRectangle *layout)
{
  MetaOutput *output;

  output = meta_monitor_get_main_output (monitor);
  *layout = (MetaRectangle) {
    .x = output->crtc->rect.x,
    .y = output->crtc->rect.y,
    .width = output->crtc->rect.width,
    .height = output->crtc->rect.height
  };
}

static gboolean
meta_monitor_normal_get_suggested_position (MetaMonitor *monitor,
                                            int         *x,
                                            int         *y)
{
  MetaOutput *output;

  output = meta_monitor_get_main_output (monitor);
  if (output->suggested_x < 0 && output->suggested_y < 0)
    return FALSE;

  *x = output->suggested_x;
  *y = output->suggested_y;

  return TRUE;
}

static void
meta_monitor_normal_init (MetaMonitorNormal *monitor)
{
}

static void
meta_monitor_normal_class_init (MetaMonitorNormalClass *klass)
{
  MetaMonitorClass *monitor_class = META_MONITOR_CLASS (klass);

  monitor_class->get_main_output = meta_monitor_normal_get_main_output;
  monitor_class->derive_layout = meta_monitor_normal_derive_layout;
  monitor_class->get_suggested_position = meta_monitor_normal_get_suggested_position;
}

uint32_t
meta_monitor_tiled_get_tile_group_id (MetaMonitorTiled *monitor_tiled)
{
  return monitor_tiled->tile_group_id;
}

gboolean
meta_monitor_get_suggested_position (MetaMonitor *monitor,
                                     int         *x,
                                     int         *y)
{
  return META_MONITOR_GET_CLASS (monitor)->get_suggested_position (monitor,
                                                                   x, y);
}

static void
add_tiled_monitor_outputs (MetaMonitorManager *monitor_manager,
                           MetaMonitorTiled   *monitor_tiled)
{
  MetaMonitorPrivate *monitor_priv =
    meta_monitor_get_instance_private (META_MONITOR (monitor_tiled));
  unsigned int i;

  for (i = 0; i < monitor_manager->n_outputs; i++)
    {
      MetaOutput *output = &monitor_manager->outputs[i];

      if (output->tile_info.group_id != monitor_tiled->tile_group_id)
        continue;

      g_warn_if_fail (output->subpixel_order ==
                      monitor_tiled->origin_output->subpixel_order);

      monitor_priv->outputs = g_list_append (monitor_priv->outputs, output);
    }
}

static void
calculate_tile_coordinate (MetaMonitor *monitor,
                           MetaOutput  *output,
                           int         *out_x,
                           int         *out_y)
{
  MetaMonitorPrivate *monitor_priv =
    meta_monitor_get_instance_private (monitor);
  GList *l;
  int x = 0;
  int y = 0;

  for (l = monitor_priv->outputs; l; l = l->next)
    {
      MetaOutput *other_output = l->data;

      if (other_output->tile_info.loc_v_tile == output->tile_info.loc_v_tile &&
          other_output->tile_info.loc_h_tile < output->tile_info.loc_h_tile)
        x += other_output->tile_info.tile_w;
      if (other_output->tile_info.loc_h_tile == output->tile_info.loc_h_tile &&
          other_output->tile_info.loc_v_tile < output->tile_info.loc_v_tile)
        y += other_output->tile_info.tile_h;
    }

  *out_x = x;
  *out_y = y;
}

static void
meta_monitor_tiled_calculate_tiled_size (MetaMonitor *monitor,
                                         int         *out_width,
                                         int         *out_height)
{
  MetaMonitorPrivate *monitor_priv =
    meta_monitor_get_instance_private (monitor);
  GList *l;
  int width;
  int height;

  width = 0;
  height = 0;
  for (l = monitor_priv->outputs; l; l = l->next)
    {
      MetaOutput *output = l->data;

      if (output->tile_info.loc_v_tile == 0)
        width += output->tile_info.tile_w;

      if (output->tile_info.loc_h_tile == 0)
        height += output->tile_info.tile_h;
    }

  *out_width = width;
  *out_height = height;
}

static gboolean
is_monitor_mode_assigned (MetaMonitor     *monitor,
                          MetaMonitorMode *mode)
{
  MetaMonitorPrivate *priv = meta_monitor_get_instance_private (monitor);
  GList *l;
  int i;

  for (l = priv->outputs, i = 0; l; l = l->next, i++)
    {
      MetaOutput *output = l->data;
      MetaMonitorCrtcMode *monitor_crtc_mode = &mode->crtc_modes[i];

      if (monitor_crtc_mode->crtc_mode &&
          (!output->crtc ||
           output->crtc->current_mode != monitor_crtc_mode->crtc_mode))
        return FALSE;
      else if (!monitor_crtc_mode->crtc_mode && output->crtc)
        return FALSE;
    }

  return TRUE;
}

static gboolean
is_crtc_mode_tiled (MetaOutput   *output,
                    MetaCrtcMode *crtc_mode)
{
  return (crtc_mode->width == (int) output->tile_info.tile_w &&
          crtc_mode->height == (int) output->tile_info.tile_h);
}

static MetaCrtcMode *
find_tiled_crtc_mode (MetaOutput *output,
                      float       refresh_rate)
{
  MetaCrtcMode *crtc_mode;
  unsigned int i;

  crtc_mode = output->preferred_mode;
  if (is_crtc_mode_tiled (output, crtc_mode))
    return crtc_mode;

  for (i = 0; i < output->n_modes; i++)
    {
      crtc_mode = output->modes[i];

      if (!is_crtc_mode_tiled (output, crtc_mode))
        continue;

      if (crtc_mode->refresh_rate != refresh_rate)
        continue;

      return crtc_mode;
    }

  return NULL;
}

static MetaMonitorMode *
create_tiled_monitor_mode (MetaMonitorTiled *monitor_tiled,
                           float             refresh_rate,
                           gboolean         *out_is_preferred)
{
  MetaMonitor *monitor = META_MONITOR (monitor_tiled);
  MetaMonitorPrivate *monitor_priv =
    meta_monitor_get_instance_private (monitor);
  MetaMonitorMode *mode;
  GList *l;
  unsigned int i;
  gboolean is_preferred = TRUE;

  mode = g_new0 (MetaMonitorMode, 1);
  meta_monitor_tiled_calculate_tiled_size (monitor,
                                           &mode->spec.width,
                                           &mode->spec.height);
  mode->crtc_modes = g_new0 (MetaMonitorCrtcMode,
                             g_list_length (monitor_priv->outputs));
  for (l = monitor_priv->outputs, i = 0; l; l = l->next, i++)
    {
      MetaOutput *output = l->data;
      MetaCrtcMode *tiled_crtc_mode;
      int x;
      int y;

      tiled_crtc_mode = find_tiled_crtc_mode (output, refresh_rate);
      if (!tiled_crtc_mode)
        {
          g_warning ("No tiled mode with refresh rate %f on %s",
                     refresh_rate, output->name);
          meta_monitor_mode_free (mode);
          return NULL;
        }

      calculate_tile_coordinate (monitor, output, &x, &y);

      mode->crtc_modes[i] = (MetaMonitorCrtcMode) {
        .x = x,
        .y = y,
        .output = output,
        .crtc_mode = tiled_crtc_mode
      };

      mode->spec.refresh_rate = refresh_rate;

      is_preferred = is_preferred && tiled_crtc_mode == output->preferred_mode;
    }

  *out_is_preferred = is_preferred;

  return mode;
}

static void
generate_tiled_monitor_modes (MetaMonitorTiled *monitor_tiled)
{
  MetaMonitor *monitor = META_MONITOR (monitor_tiled);
  MetaMonitorPrivate *monitor_priv =
    meta_monitor_get_instance_private (monitor);
  MetaOutput *main_output;
  GList *tiled_modes = NULL;
  unsigned int i;

  main_output = meta_monitor_get_main_output (META_MONITOR (monitor_tiled));

  for (i = 0; i < main_output->n_modes; i++)
    {
      MetaCrtcMode *crtc_mode = main_output->modes[i];
      MetaMonitorMode *mode;
      gboolean is_preferred;

      if (!is_crtc_mode_tiled (main_output, crtc_mode))
        continue;

      mode = create_tiled_monitor_mode (monitor_tiled, crtc_mode->refresh_rate,
                                        &is_preferred);
      if (!mode)
        continue;

      tiled_modes = g_list_append (tiled_modes, mode);

      if (is_monitor_mode_assigned (monitor, mode))
        monitor_priv->current_mode = mode;

      if (is_preferred)
        monitor_priv->preferred_mode = mode;
    }

  if (!monitor_priv->preferred_mode)
    {
      MetaMonitorMode *best_mode = NULL;
      GList *l;

      for (l = tiled_modes; l; l = l->next)
        {
          MetaMonitorMode *mode = l->data;

          if (!best_mode ||
              mode->spec.refresh_rate > best_mode->spec.refresh_rate)
            best_mode = mode;
        }

      monitor_priv->preferred_mode = best_mode;
    }

  monitor_priv->modes = g_list_concat (monitor_priv->modes, tiled_modes);
}

static MetaMonitorMode *
create_untiled_monitor_mode (MetaMonitorTiled *monitor_tiled,
                             MetaOutput       *main_output,
                             MetaCrtcMode     *crtc_mode)
{
  MetaMonitor *monitor = META_MONITOR (monitor_tiled);
  MetaMonitorPrivate *monitor_priv =
    meta_monitor_get_instance_private (monitor);
  MetaMonitorMode *mode;
  GList *l;
  int i;

  if (is_crtc_mode_tiled (main_output, crtc_mode))
    return NULL;

  mode = g_new0 (MetaMonitorMode, 1);

  mode->spec = (MetaMonitorModeSpec) {
    .width = crtc_mode->width,
    .height = crtc_mode->height,
    .refresh_rate = crtc_mode->refresh_rate
  };
  mode->crtc_modes = g_new0 (MetaMonitorCrtcMode,
                             g_list_length (monitor_priv->outputs));

  for (l = monitor_priv->outputs, i = 0; l; l = l->next, i++)
    {
      MetaOutput *output = l->data;

      if (output == main_output)
        {
          mode->crtc_modes[i] = (MetaMonitorCrtcMode) {
            .x = 0,
            .y = 0,
            .output = output,
            .crtc_mode = crtc_mode
          };
        }
      else
        {
          mode->crtc_modes[i] = (MetaMonitorCrtcMode) {
            .output = output,
            .crtc_mode = NULL
          };
        }
    }

  return mode;
}

static int
count_untiled_crtc_modes (MetaOutput *output)
{
  int count;
  unsigned int i;

  count = 0;
  for (i = 0; i < output->n_modes; i++)
    {
      MetaCrtcMode *crtc_mode = output->modes[i];

      if (!is_crtc_mode_tiled (output, crtc_mode))
        count++;
    }

  return count;
}

static MetaOutput *
find_untiled_output (MetaMonitorTiled *monitor_tiled)
{
  MetaMonitor *monitor = META_MONITOR (monitor_tiled);
  MetaMonitorPrivate *monitor_priv =
    meta_monitor_get_instance_private (monitor);
  MetaOutput *best_output;
  int best_untiled_crtc_mode_count;
  GList *l;

  best_output = monitor_tiled->origin_output;
  best_untiled_crtc_mode_count =
    count_untiled_crtc_modes (monitor_tiled->origin_output);

  for (l = monitor_priv->outputs; l; l = l->next)
    {
      MetaOutput *output = l->data;
      int untiled_crtc_mode_count;

      if (output == monitor_tiled->origin_output)
        continue;

      untiled_crtc_mode_count = count_untiled_crtc_modes (output);
      if (untiled_crtc_mode_count > best_untiled_crtc_mode_count)
        {
          best_untiled_crtc_mode_count = untiled_crtc_mode_count;
          best_output = output;
        }
    }

  return best_output;
}

static void
generate_untiled_monitor_modes (MetaMonitorTiled *monitor_tiled)
{
  MetaMonitor *monitor = META_MONITOR (monitor_tiled);
  MetaMonitorPrivate *monitor_priv =
    meta_monitor_get_instance_private (monitor);
  MetaOutput *main_output;
  unsigned int i;

  main_output = meta_monitor_get_main_output (monitor);

  for (i = 0; i < main_output->n_modes; i++)
    {
      MetaCrtcMode *crtc_mode = main_output->modes[i];
      MetaMonitorMode *mode;

      mode = create_untiled_monitor_mode (monitor_tiled,
                                          main_output,
                                          crtc_mode);
      if (!mode)
        continue;

      monitor_priv->modes = g_list_append (monitor_priv->modes, mode);

      if (is_monitor_mode_assigned (monitor, mode))
        {
          g_assert (!monitor_priv->current_mode);
          monitor_priv->current_mode = mode;
        }

      if (!monitor_priv->preferred_mode &&
          crtc_mode == main_output->preferred_mode)
        monitor_priv->preferred_mode = mode;
    }
}

static MetaMonitorMode *
find_best_mode (MetaMonitor *monitor)
{
  MetaMonitorPrivate *monitor_priv =
    meta_monitor_get_instance_private (monitor);
  MetaMonitorMode *best_mode = NULL;
  GList *l;

  for (l = monitor_priv->modes; l; l = l->next)
    {
      MetaMonitorMode *mode = l->data;
      int area, best_area;

      if (!best_mode)
        {
          best_mode = mode;
          continue;
        }

      area = mode->spec.width * mode->spec.height;
      best_area = best_mode->spec.width * best_mode->spec.height;
      if (area > best_area)
        {
          best_mode = mode;
          continue;
        }

      if (mode->spec.refresh_rate > best_mode->spec.refresh_rate)
        {
          best_mode = mode;
          continue;
        }
    }

  return best_mode;
}

static void
meta_monitor_tiled_generate_modes (MetaMonitorTiled *monitor_tiled)
{
  MetaMonitor *monitor = META_MONITOR (monitor_tiled);
  MetaMonitorPrivate *monitor_priv =
    meta_monitor_get_instance_private (monitor);

  /*
   * Tiled monitors may look a bit different from each other, depending on the
   * monitor itself, the driver, etc.
   *
   * On some, the tiled modes will be the preferred CRTC modes, and running
   * untiled is done by only enabling (0, 0) tile. In this case, things are
   * pretty straight forward.
   *
   * Other times a monitor may have some bogus mode preferred on the main tile,
   * and an untiled mode preferred on the non-main tile, and there seems to be
   * no guarantee that the (0, 0) tile is the one that should drive the
   * non-tiled mode.
   *
   * To handle both these cases, the following hueristics are implemented:
   *
   *  1) Find all the tiled CRTC modes of the (0, 0) tile, and create tiled
   *     monitor modes for all tiles based on these.
   *  2) If there is any tiled monitor mode combination where all CRTC modes
   *     are the preferred ones, that one is marked as preferred.
   *  3) If there is no preferred mode determined so far, assume the tiled
   *     monitor mode with the highest refresh rate is preferred.
   *  4) Find the tile with highest number of untiled CRTC modes available,
   *     assume this is the one driving the monitor in untiled mode, and
   *     create monitor modes for all untiled CRTC modes of that tile. If
   *     there is still no preferred mode, set any untiled mode as preferred
   *     if the CRTC mode is marked as such.
   *  5) If at this point there is still no preferred mode, just pick the one
   *     with the highest number of pixels and highest refresh rate.
   *
   * Note that this ignores the preference if the preference is a non-tiled
   * mode. This seems to be the case on some systems, where the user tends to
   * manually set up the tiled mode anyway.
   */

  generate_tiled_monitor_modes (monitor_tiled);

  if (!monitor_priv->preferred_mode)
    g_warning ("Tiled monitor on %s didn't have any tiled modes",
               monitor_priv->spec->connector);

  generate_untiled_monitor_modes (monitor_tiled);

  if (!monitor_priv->preferred_mode)
    {
      g_warning ("Tiled monitor on %s didn't have a valid preferred mode",
                 monitor_priv->spec->connector);
      monitor_priv->preferred_mode = find_best_mode (monitor);
    }
}

MetaMonitorTiled *
meta_monitor_tiled_new (MetaMonitorManager *monitor_manager,
                        MetaOutput         *output)
{
  MetaMonitorTiled *monitor_tiled;
  MetaMonitor *monitor;
  MetaMonitorPrivate *monitor_priv;

  monitor_tiled = g_object_new (META_TYPE_MONITOR_TILED, NULL);
  monitor = META_MONITOR (monitor_tiled);
  monitor_priv = meta_monitor_get_instance_private (monitor);

  monitor_priv->monitor_manager = monitor_manager;

  monitor_tiled->tile_group_id = output->tile_info.group_id;
  monitor_priv->winsys_id = output->winsys_id;

  monitor_tiled->origin_output = output;
  add_tiled_monitor_outputs (monitor_manager, monitor_tiled);

  monitor_tiled->main_output = find_untiled_output (monitor_tiled);

  meta_monitor_generate_spec (monitor);

  meta_monitor_manager_tiled_monitor_added (monitor_manager,
                                            META_MONITOR (monitor_tiled));

  meta_monitor_tiled_generate_modes (monitor_tiled);

  return monitor_tiled;
}

static MetaOutput *
meta_monitor_tiled_get_main_output (MetaMonitor *monitor)
{
  MetaMonitorTiled *monitor_tiled = META_MONITOR_TILED (monitor);

  return monitor_tiled->main_output;
}

static void
meta_monitor_derived_derive_layout (MetaMonitor   *monitor,
                                    MetaRectangle *layout)
{
  MetaMonitorPrivate *monitor_priv =
    meta_monitor_get_instance_private (monitor);
  GList *l;
  int min_x, min_y, max_x, max_y;

  min_x = INT_MAX;
  min_y = INT_MAX;
  max_x = 0;
  max_y = 0;
  for (l = monitor_priv->outputs; l; l = l->next)
    {
      MetaOutput *output = l->data;

      if (!output->crtc)
        continue;

      min_x = MIN (output->crtc->rect.x, min_x);
      min_y = MIN (output->crtc->rect.y, min_y);
      max_x = MAX (output->crtc->rect.x + output->crtc->rect.width, max_x);
      max_y = MAX (output->crtc->rect.y + output->crtc->rect.height, max_y);
    }

  *layout = (MetaRectangle) {
    .x = min_x,
    .y = min_y,
    .width = max_x - min_x,
    .height = max_y - min_y
  };
}

static gboolean
meta_monitor_tiled_get_suggested_position (MetaMonitor *monitor,
                                           int         *x,
                                           int         *y)
{
  return FALSE;
}

static void
meta_monitor_tiled_finalize (GObject *object)
{
  MetaMonitor *monitor = META_MONITOR (object);
  MetaMonitorPrivate *monitor_priv =
    meta_monitor_get_instance_private (monitor);

  meta_monitor_manager_tiled_monitor_removed (monitor_priv->monitor_manager,
                                              monitor);
}

static void
meta_monitor_tiled_init (MetaMonitorTiled *monitor)
{
}

static void
meta_monitor_tiled_class_init (MetaMonitorTiledClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  MetaMonitorClass *monitor_class = META_MONITOR_CLASS (klass);

  object_class->finalize = meta_monitor_tiled_finalize;

  monitor_class->get_main_output = meta_monitor_tiled_get_main_output;
  monitor_class->derive_layout = meta_monitor_derived_derive_layout;
  monitor_class->get_suggested_position = meta_monitor_tiled_get_suggested_position;
}

static void
meta_monitor_mode_free (MetaMonitorMode *monitor_mode)
{
  g_free (monitor_mode->crtc_modes);
  g_free (monitor_mode);
}

MetaMonitorSpec *
meta_monitor_get_spec (MetaMonitor *monitor)
{
  MetaMonitorPrivate *priv = meta_monitor_get_instance_private (monitor);

  return priv->spec;
}

MetaLogicalMonitor *
meta_monitor_get_logical_monitor (MetaMonitor *monitor)
{
  MetaOutput *output = meta_monitor_get_main_output (monitor);

  if (output->crtc)
    return output->crtc->logical_monitor;
  else
    return NULL;
}

static gboolean
meta_monitor_mode_spec_equals (MetaMonitorModeSpec *monitor_mode_spec,
                               MetaMonitorModeSpec *other_monitor_mode_spec)
{
  return (monitor_mode_spec->width == other_monitor_mode_spec->width &&
          monitor_mode_spec->height == other_monitor_mode_spec->height &&
          (monitor_mode_spec->refresh_rate ==
           other_monitor_mode_spec->refresh_rate));
}

MetaMonitorMode *
meta_monitor_get_mode_from_spec (MetaMonitor         *monitor,
                                 MetaMonitorModeSpec *monitor_mode_spec)
{
  MetaMonitorPrivate *priv = meta_monitor_get_instance_private (monitor);
  GList *l;

  for (l = priv->modes; l; l = l->next)
    {
      MetaMonitorMode *monitor_mode = l->data;

      if (meta_monitor_mode_spec_equals (monitor_mode_spec,
                                         &monitor_mode->spec))
        return monitor_mode;
    }

  return NULL;
}

MetaMonitorMode *
meta_monitor_get_preferred_mode (MetaMonitor *monitor)
{
  MetaMonitorPrivate *priv = meta_monitor_get_instance_private (monitor);

  return priv->preferred_mode;
}

MetaMonitorMode *
meta_monitor_get_current_mode (MetaMonitor *monitor)
{
  MetaMonitorPrivate *priv = meta_monitor_get_instance_private (monitor);

  return priv->current_mode;
}

void
meta_monitor_derive_current_mode (MetaMonitor *monitor)
{
  MetaMonitorPrivate *priv = meta_monitor_get_instance_private (monitor);
  MetaMonitorMode *current_mode = NULL;
  GList *l;

  for (l = priv->modes; l; l = l->next)
    {
      MetaMonitorMode *mode = l->data;

      if (is_monitor_mode_assigned (monitor, mode))
        {
          current_mode = mode;
          break;
        }
    }

  priv->current_mode = current_mode;
}

void
meta_monitor_set_current_mode (MetaMonitor     *monitor,
                               MetaMonitorMode *mode)
{
  MetaMonitorPrivate *priv = meta_monitor_get_instance_private (monitor);

  priv->current_mode = mode;
}

GList *
meta_monitor_get_modes (MetaMonitor *monitor)
{
  MetaMonitorPrivate *priv = meta_monitor_get_instance_private (monitor);

  return priv->modes;
}

MetaMonitorModeSpec *
meta_monitor_mode_get_spec (MetaMonitorMode *monitor_mode)
{
  return &monitor_mode->spec;
}

void
meta_monitor_mode_get_resolution (MetaMonitorMode *monitor_mode,
                                  int             *width,
                                  int             *height)
{
  *width = monitor_mode->spec.width;
  *height = monitor_mode->spec.height;
}

float
meta_monitor_mode_get_refresh_rate (MetaMonitorMode *monitor_mode)
{
  return monitor_mode->spec.refresh_rate;
}

gboolean
meta_monitor_mode_foreach_crtc (MetaMonitor        *monitor,
                                MetaMonitorMode    *mode,
                                MetaMonitorModeFunc func,
                                gpointer            user_data,
                                GError            **error)
{
  MetaMonitorPrivate *monitor_priv =
    meta_monitor_get_instance_private (monitor);
  GList *l;
  int i;

  for (l = monitor_priv->outputs, i = 0; l; l = l->next, i++)
    {
      MetaMonitorCrtcMode *monitor_crtc_mode = &mode->crtc_modes[i];

      if (!monitor_crtc_mode->crtc_mode)
        continue;

      if (!func (monitor, mode, monitor_crtc_mode, user_data, error))
        return FALSE;
    }

  return TRUE;
}

gboolean
meta_monitor_mode_foreach_output (MetaMonitor        *monitor,
                                  MetaMonitorMode    *mode,
                                  MetaMonitorModeFunc func,
                                  gpointer            user_data,
                                  GError            **error)
{
  MetaMonitorPrivate *monitor_priv =
    meta_monitor_get_instance_private (monitor);
  GList *l;
  int i;

  for (l = monitor_priv->outputs, i = 0; l; l = l->next, i++)
    {
      MetaMonitorCrtcMode *monitor_crtc_mode = &mode->crtc_modes[i];

      if (!func (monitor, mode, monitor_crtc_mode, user_data, error))
        return FALSE;
    }

  return TRUE;
}
