/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/*
 * Copyright (C) 2017 Red Hat
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

#include <eosmetrics/eosmetrics.h>

#include "backends/meta-gpu.h"

#include "backends/meta-output.h"

/*
 * Recorded when a monitor is connected to a machine. The auxiliary payload
 * is a 7-tuple composed of the monitor's name as a string, vendor as a string,
 * product as a string, serial code as a string, width (mm) as an integer,
 * height (mm) as an integer, and EDID as an array of unsigned bytes (or NULL if
 * the EDID couldn't be obtained; a GVariant NULL array is treated as a
 * semantically empty array of the given type.)
 */
#define MONITOR_CONNECTED "fa82f422-a685-46e4-91a7-7b7bfb5b289f"

/*
 * Recorded when a monitor is disconnected from a machine. The auxiliary
 * payload is of the same format as for MONITOR_CONNECTED events.
 */
#define MONITOR_DISCONNECTED "5e8c3f40-22a2-4d5d-82f3-e3bf927b5b74"

enum
{
  PROP_0,

  PROP_MONITOR_MANAGER,

  PROP_LAST
};

static GParamSpec *obj_props[PROP_LAST];

typedef struct _MetaGpuPrivate
{
  MetaMonitorManager *monitor_manager;

  GList *outputs;
  GList *crtcs;
  GList *modes;
} MetaGpuPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (MetaGpu, meta_gpu, G_TYPE_OBJECT)


static GVariant *
get_output_auxiliary_payload (MetaGpu    *gpu,
                              MetaOutput *output)
{
  MetaGpuPrivate *priv = meta_gpu_get_instance_private (gpu);
  g_autoptr(GBytes) edid = NULL;
  GVariant *edid_variant;
  const guint8 *raw_edid = NULL;
  gsize edid_length = 0;
  MetaMonitorManagerClass *manager_class =
    META_MONITOR_MANAGER_GET_CLASS (priv->monitor_manager);

  edid = manager_class->read_edid (priv->monitor_manager, output);
  if (edid != NULL)
    raw_edid = g_bytes_get_data (edid, &edid_length);

  edid_variant =
    g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE,
                               raw_edid,
                               edid_length,
                               sizeof (guint8));

  return g_variant_new ("(ssssii@ay)", output->name, output->vendor,
                        output->product, output->serial, output->width_mm,
                        output->height_mm, edid_variant);
}

static void
record_connect_events (MetaGpu *gpu,
                       GList   *old_outputs)
{
  MetaGpuPrivate *priv = meta_gpu_get_instance_private (gpu);
  GList *l;

  for (l = priv->outputs; l != NULL; l = l->next)
    {
      MetaOutput *new_output = l->data;
      GList *k;

      for (k = old_outputs; k != NULL; k = k->next)
        {
          MetaOutput *old_output = k->data;
          if (new_output->winsys_id == old_output->winsys_id)
            break;
        }

      if (k == NULL)
        {
          /* Output is connected now but wasn't previously. */
          GVariant *auxiliary_payload =
            get_output_auxiliary_payload (gpu, new_output);

          emtr_event_recorder_record_event (emtr_event_recorder_get_default (),
                                            MONITOR_CONNECTED,
                                            auxiliary_payload);
        }
    }
}

static void
record_disconnect_events (MetaGpu *gpu,
                          GList   *old_outputs)
{
  MetaGpuPrivate *priv = meta_gpu_get_instance_private (gpu);
  GList *l;

  for (l = old_outputs; l != NULL; l = l->next)
    {
      MetaOutput *old_output = l->data;
      GList *k;

      for (k = priv->outputs; k != NULL; k = k->next)
        {
          MetaOutput *new_output = k->data;
          if (old_output->winsys_id == new_output->winsys_id)
            break;
        }

      if (k == NULL)
        {
          /* Output was connected previously but isn't now. */
          GVariant *auxiliary_payload =
            get_output_auxiliary_payload (gpu, old_output);

          emtr_event_recorder_record_event (emtr_event_recorder_get_default (),
                                            MONITOR_DISCONNECTED,
                                            auxiliary_payload);
        }
    }
}

static void
record_connection_changes (MetaGpu *gpu,
                           GList   *old_outputs)
{
  record_connect_events (gpu, old_outputs);
  record_disconnect_events (gpu, old_outputs);
}

gboolean
meta_gpu_has_hotplug_mode_update (MetaGpu *gpu)
{
  MetaGpuPrivate *priv = meta_gpu_get_instance_private (gpu);
  GList *l;

  for (l = priv->outputs; l; l = l->next)
    {
      MetaOutput *output = l->data;

      if (output->hotplug_mode_update)
        return TRUE;
    }

  return FALSE;
}

gboolean
meta_gpu_read_current (MetaGpu  *gpu,
                       GError  **error)
{
  MetaGpuPrivate *priv = meta_gpu_get_instance_private (gpu);
  gboolean ret;
  GList *old_outputs;
  GList *old_crtcs;
  GList *old_modes;

  /* TODO: Get rid of this when objects incref:s what they need instead */
  old_outputs = priv->outputs;
  old_crtcs = priv->crtcs;
  old_modes = priv->modes;

  ret = META_GPU_GET_CLASS (gpu)->read_current (gpu, error);

  record_connection_changes (gpu, old_outputs);

  g_list_free_full (old_outputs, g_object_unref);
  g_list_free_full (old_modes, g_object_unref);
  g_list_free_full (old_crtcs, g_object_unref);

  return ret;
}

MetaMonitorManager *
meta_gpu_get_monitor_manager (MetaGpu *gpu)
{
  MetaGpuPrivate *priv = meta_gpu_get_instance_private (gpu);

  return priv->monitor_manager;
}

GList *
meta_gpu_get_outputs (MetaGpu *gpu)
{
  MetaGpuPrivate *priv = meta_gpu_get_instance_private (gpu);

  return priv->outputs;
}

GList *
meta_gpu_get_crtcs (MetaGpu *gpu)
{
  MetaGpuPrivate *priv = meta_gpu_get_instance_private (gpu);

  return priv->crtcs;
}

GList *
meta_gpu_get_modes (MetaGpu *gpu)
{
  MetaGpuPrivate *priv = meta_gpu_get_instance_private (gpu);

  return priv->modes;
}

void
meta_gpu_take_outputs (MetaGpu *gpu,
                       GList   *outputs)
{
  MetaGpuPrivate *priv = meta_gpu_get_instance_private (gpu);

  priv->outputs = outputs;
}

void
meta_gpu_take_crtcs (MetaGpu *gpu,
                    GList   *crtcs)
{
  MetaGpuPrivate *priv = meta_gpu_get_instance_private (gpu);

  priv->crtcs = crtcs;
}

void
meta_gpu_take_modes (MetaGpu *gpu,
                     GList   *modes)
{
  MetaGpuPrivate *priv = meta_gpu_get_instance_private (gpu);

  priv->modes = modes;
}

static void
meta_gpu_set_property (GObject      *object,
                       guint         prop_id,
                       const GValue *value,
                       GParamSpec   *pspec)
{
  MetaGpu *gpu = META_GPU (object);
  MetaGpuPrivate *priv = meta_gpu_get_instance_private (gpu);

  switch (prop_id)
    {
    case PROP_MONITOR_MANAGER:
      priv->monitor_manager = g_value_get_object (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
meta_gpu_get_property (GObject    *object,
                       guint       prop_id,
                       GValue     *value,
                       GParamSpec *pspec)
{
  MetaGpu *gpu = META_GPU (object);
  MetaGpuPrivate *priv = meta_gpu_get_instance_private (gpu);

  switch (prop_id)
    {
    case PROP_MONITOR_MANAGER:
      g_value_set_object (value, priv->monitor_manager);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
meta_gpu_finalize (GObject *object)
{
  MetaGpu *gpu = META_GPU (object);
  MetaGpuPrivate *priv = meta_gpu_get_instance_private (gpu);

  g_list_free_full (priv->outputs, g_object_unref);
  g_list_free_full (priv->modes, g_object_unref);
  g_list_free_full (priv->crtcs, g_object_unref);

  G_OBJECT_CLASS (meta_gpu_parent_class)->finalize (object);
}

static void
meta_gpu_init (MetaGpu *gpu)
{
}

static void
meta_gpu_class_init (MetaGpuClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->set_property = meta_gpu_set_property;
  object_class->get_property = meta_gpu_get_property;
  object_class->finalize = meta_gpu_finalize;

  obj_props[PROP_MONITOR_MANAGER] =
    g_param_spec_object ("monitor-manager",
                         "monitor-manager",
                         "MetaMonitorManager",
                         META_TYPE_MONITOR_MANAGER,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);
  g_object_class_install_properties (object_class, PROP_LAST, obj_props);
}
