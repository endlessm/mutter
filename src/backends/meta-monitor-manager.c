/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/*
 * Copyright (C) 2001, 2002 Havoc Pennington
 * Copyright (C) 2002, 2003 Red Hat Inc.
 * Some ICCCM manager selection code derived from fvwm2,
 * Copyright (C) 2001 Dominik Vogt, Matthias Clasen, and fvwm2 team
 * Copyright (C) 2003 Rob Adams
 * Copyright (C) 2004-2006 Elijah Newren
 * Copyright (C) 2013 Red Hat Inc.
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
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include "meta-monitor-manager-private.h"

#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <clutter/clutter.h>
#include <eosmetrics/eosmetrics.h>

#include <meta/main.h>
#include "util-private.h"
#include <meta/errors.h>
#include "edid.h"
#include "meta-monitor-config.h"
#include "backends/meta-logical-monitor.h"
#include "backends/meta-monitor.h"
#include "backends/meta-monitor-config-manager.h"
#include "backends/x11/meta-monitor-manager-xrandr.h"
#include "meta-backend-private.h"

#define DEFAULT_DISPLAY_CONFIGURATION_TIMEOUT 20

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

enum {
  CONFIRM_DISPLAY_CHANGE,
  LID_IS_CLOSED_CHANGED,
  SIGNALS_LAST
};

/* Array index matches MetaMonitorTransform */
static gfloat transform_matrices[][6] = {
  {  1,  0,  0,  0,  1,  0 }, /* normal */
  {  0, -1,  1,  1,  0,  0 }, /* 90° */
  { -1,  0,  1,  0, -1,  1 }, /* 180° */
  {  0,  1,  0, -1,  0,  1 }, /* 270° */
  { -1,  0,  1,  0,  1,  0 }, /* normal flipped */
  {  0,  1,  0,  1,  0,  0 }, /* 90° flipped */
  {  1,  0,  0,  0, -1,  1 }, /* 180° flipped */
  {  0, -1,  1, -1,  0,  1 }, /* 270° flipped */
};

static int signals[SIGNALS_LAST];

static void meta_monitor_manager_display_config_init (MetaDBusDisplayConfigIface *iface);

G_DEFINE_ABSTRACT_TYPE_WITH_CODE (MetaMonitorManager, meta_monitor_manager, META_DBUS_TYPE_DISPLAY_CONFIG_SKELETON,
                                  G_IMPLEMENT_INTERFACE (META_DBUS_TYPE_DISPLAY_CONFIG, meta_monitor_manager_display_config_init));

static void initialize_dbus_interface (MetaMonitorManager *manager);

static void
meta_monitor_manager_init (MetaMonitorManager *manager)
{
}

static void
meta_monitor_manager_set_primary_logical_monitor (MetaMonitorManager *manager,
                                                  MetaLogicalMonitor *logical_monitor)
{
  manager->primary_logical_monitor = logical_monitor;
  if (logical_monitor)
    meta_logical_monitor_make_primary (logical_monitor);
}

static gboolean
is_main_tiled_monitor_output (MetaOutput *output)
{
  return output->tile_info.loc_h_tile == 0 && output->tile_info.loc_v_tile == 0;
}

static MetaLogicalMonitor *
logical_monitor_from_layout (MetaMonitorManager *manager,
                             GList              *logical_monitors,
                             MetaRectangle      *layout)
{
  GList *l;

  for (l = logical_monitors; l; l = l->next)
    {
      MetaLogicalMonitor *logical_monitor = l->data;

      if (meta_rectangle_equal (layout, &logical_monitor->rect))
        return logical_monitor;
    }

  return NULL;
}

static void
meta_monitor_manager_rebuild_logical_monitors (MetaMonitorManager *manager,
                                               MetaMonitorsConfig *config)
{
  GList *logical_monitor_configs;
  GList *logical_monitors = NULL;
  GList *l;
  int monitor_number = 0;
  MetaLogicalMonitor *primary_logical_monitor = NULL;

  logical_monitor_configs = config ? config->logical_monitor_configs : NULL;
  for (l = logical_monitor_configs; l; l = l->next)
    {
      MetaLogicalMonitorConfig *logical_monitor_config = l->data;
      MetaLogicalMonitor *logical_monitor;

      logical_monitor = meta_logical_monitor_new (manager,
                                                  logical_monitor_config,
                                                  monitor_number);
      monitor_number++;

      if (logical_monitor_config->is_primary)
        primary_logical_monitor = logical_monitor;

      logical_monitors = g_list_append (logical_monitors, logical_monitor);
    }

  /*
   * If no monitor was marked as primary, fall back on marking the first
   * logical monitor the primary one.
   */
  if (!primary_logical_monitor && logical_monitors)
    primary_logical_monitor = g_list_first (logical_monitors)->data;

  manager->logical_monitors = logical_monitors;
  meta_monitor_manager_set_primary_logical_monitor (manager,
                                                    primary_logical_monitor);
}

static void
meta_monitor_manager_rebuild_logical_monitors_derived (MetaMonitorManager *manager)
{
  GList *logical_monitors = NULL;
  GList *l;
  int monitor_number;
  MetaLogicalMonitor *primary_logical_monitor = NULL;

  monitor_number = 0;

  for (l = manager->monitors; l; l = l->next)
    {
      MetaMonitor *monitor = l->data;
      MetaLogicalMonitor *logical_monitor;
      MetaRectangle layout;

      if (!meta_monitor_is_active (monitor))
        continue;

      meta_monitor_derive_layout (monitor, &layout);
      logical_monitor = logical_monitor_from_layout (manager, logical_monitors,
                                                     &layout);
      if (logical_monitor)
        {
          meta_logical_monitor_add_monitor (logical_monitor, monitor);
        }
      else
        {
          logical_monitor = meta_logical_monitor_new_derived (manager,
                                                              monitor,
                                                              &layout,
                                                              monitor_number);
          logical_monitors = g_list_append (logical_monitors, logical_monitor);
          monitor_number++;
        }

      if (meta_monitor_is_primary (monitor))
        primary_logical_monitor = logical_monitor;
    }

  manager->logical_monitors = logical_monitors;

  /*
   * If no monitor was marked as primary, fall back on marking the first
   * logical monitor the primary one.
   */
  if (!primary_logical_monitor && manager->logical_monitors)
    primary_logical_monitor = g_list_first (manager->logical_monitors)->data;

  meta_monitor_manager_set_primary_logical_monitor (manager,
                                                    primary_logical_monitor);
}

static void
power_save_mode_changed (MetaMonitorManager *manager,
                         GParamSpec         *pspec,
                         gpointer            user_data)
{
  MetaMonitorManagerClass *klass;
  int mode = meta_dbus_display_config_get_power_save_mode (META_DBUS_DISPLAY_CONFIG (manager));

  if (mode == META_POWER_SAVE_UNSUPPORTED)
    return;

  /* If DPMS is unsupported, force the property back. */
  if (manager->power_save_mode == META_POWER_SAVE_UNSUPPORTED)
    {
      meta_dbus_display_config_set_power_save_mode (META_DBUS_DISPLAY_CONFIG (manager), META_POWER_SAVE_UNSUPPORTED);
      return;
    }

  klass = META_MONITOR_MANAGER_GET_CLASS (manager);
  if (klass->set_power_save_mode)
    klass->set_power_save_mode (manager, mode);

  manager->power_save_mode = mode;
}

void
meta_monitor_manager_lid_is_closed_changed (MetaMonitorManager *manager)
{
  if (manager->config_manager)
    meta_monitor_manager_ensure_configured (manager);

  g_signal_emit (manager, signals[LID_IS_CLOSED_CHANGED], 0);
}

static void
lid_is_closed_changed (UpClient   *client,
                       GParamSpec *pspec,
                       gpointer    user_data)
{
  MetaMonitorManager *manager = user_data;

  meta_monitor_manager_lid_is_closed_changed (manager);
}

static gboolean
meta_monitor_manager_real_is_lid_closed (MetaMonitorManager *manager)
{
  if (!manager->up_client)
    return FALSE;

  return up_client_get_lid_is_closed (manager->up_client);
}

gboolean
meta_monitor_manager_is_lid_closed (MetaMonitorManager *manager)
{
  return META_MONITOR_MANAGER_GET_CLASS (manager)->is_lid_closed (manager);
}

gboolean
meta_monitor_manager_is_headless (MetaMonitorManager *manager)
{
  return !manager->logical_monitors;
}

static void
meta_monitor_manager_ensure_initial_config (MetaMonitorManager *manager)
{
  META_MONITOR_MANAGER_GET_CLASS (manager)->ensure_initial_config (manager);
}

static gboolean
meta_monitor_manager_apply_monitors_config (MetaMonitorManager *manager,
                                            MetaMonitorsConfig *config,
                                            GError            **error)
{
  MetaMonitorManagerClass *manager_class =
    META_MONITOR_MANAGER_GET_CLASS (manager);

  return manager_class->apply_monitors_config (manager, config, error);
}

gboolean
meta_monitor_manager_has_hotplug_mode_update (MetaMonitorManager *manager)
{
  unsigned int i;

  for (i = 0; i < manager->n_outputs; i++)
    {
      MetaOutput *output = &manager->outputs[i];

      if (output->hotplug_mode_update)
        return TRUE;
    }

  return FALSE;
}

static gboolean
should_use_stored_config (MetaMonitorManager *manager)
{
  return !meta_monitor_manager_has_hotplug_mode_update (manager);
}

static void
legacy_ensure_configured (MetaMonitorManager *manager)
{
  if (!meta_monitor_config_apply_stored (manager->legacy_config, manager))
    meta_monitor_config_make_default (manager->legacy_config, manager);
}

MetaMonitorsConfig *
meta_monitor_manager_ensure_configured (MetaMonitorManager *manager)
{
  MetaMonitorsConfig *config = NULL;
  GError *error = NULL;

  if (!manager->config_manager)
    {
      legacy_ensure_configured (manager);
      return NULL;
    }

  if (should_use_stored_config (manager))
    {
      config = meta_monitor_config_manager_get_stored (manager->config_manager);
      if (config)
        {
          if (!meta_monitor_manager_apply_monitors_config (manager, config,
                                                           &error))
            {
              config = NULL;
              g_warning ("Failed to use stored monitor configuration: %s",
                         error->message);
              g_clear_error (&error);
            }
          else
            {
              g_object_ref (config);
              goto done;
            }
        }
    }

  config = meta_monitor_config_manager_create_suggested (manager->config_manager);
  if (config)
    {
      if (!meta_monitor_manager_apply_monitors_config (manager, config, &error))
        {
          g_clear_object (&config);
          g_warning ("Failed to use suggested monitor configuration: %s",
                     error->message);
          g_clear_error (&error);
        }
      else
        {
          goto done;
        }
    }

  config = meta_monitor_config_manager_create_linear (manager->config_manager);
  if (config)
    {
      if (!meta_monitor_manager_apply_monitors_config (manager, config, &error))
        {
          g_clear_object (&config);
          g_warning ("Failed to use linear monitor configuration: %s",
                     error->message);
          g_clear_error (&error);
        }
      else
        {
          goto done;
        }
    }

  config = meta_monitor_config_manager_create_fallback (manager->config_manager);
  if (config)
    {
      if (!meta_monitor_manager_apply_monitors_config (manager, config, &error))
        {
          g_clear_object (&config);
          g_warning ("Failed to use fallback monitor configuration: %s",
                 error->message);
          g_clear_error (&error);
        }
      else
        {
          goto done;
        }
    }

done:
  meta_monitor_config_manager_set_current (manager->config_manager, config);

  if (!config)
    {
      meta_monitor_manager_rebuild (manager, NULL);
      return NULL;
    }

  g_object_unref (config);

  return config;
}

static void
meta_monitor_manager_constructed (GObject *object)
{
  MetaMonitorManager *manager = META_MONITOR_MANAGER (object);
  MetaMonitorManagerClass *manager_class =
    META_MONITOR_MANAGER_GET_CLASS (manager);

  if (manager_class->is_lid_closed == meta_monitor_manager_real_is_lid_closed)
    {
      manager->up_client = up_client_new ();
      g_signal_connect_object (manager->up_client, "notify::lid-is-closed",
                               G_CALLBACK (lid_is_closed_changed), manager, 0);
    }

  g_signal_connect_object (manager, "notify::power-save-mode",
                           G_CALLBACK (power_save_mode_changed), manager, 0);

  manager->in_init = TRUE;

  if (g_strcmp0 (g_getenv ("MUTTER_USE_CONFIG_MANAGER"), "1") == 0)
    manager->config_manager = meta_monitor_config_manager_new (manager);
  else
    manager->legacy_config = meta_monitor_config_new (manager);

  meta_monitor_manager_read_current_state (manager);

  meta_monitor_manager_ensure_initial_config (manager);

  initialize_dbus_interface (manager);

  manager->in_init = FALSE;
}

void
meta_monitor_manager_clear_output (MetaOutput *output)
{
  g_free (output->name);
  g_free (output->vendor);
  g_free (output->product);
  g_free (output->serial);
  g_free (output->modes);
  g_free (output->possible_crtcs);
  g_free (output->possible_clones);
  g_free (output->underscan_value);

  if (output->driver_notify)
    output->driver_notify (output);

  memset (output, 0, sizeof (*output));
}

static void
meta_monitor_manager_free_output_array (MetaOutput *old_outputs,
                                        int         n_old_outputs)
{
  int i;

  for (i = 0; i < n_old_outputs; i++)
    meta_monitor_manager_clear_output (&old_outputs[i]);

  g_free (old_outputs);
}

void
meta_monitor_manager_clear_mode (MetaCrtcMode *mode)
{
  g_free (mode->name);

  if (mode->driver_notify)
    mode->driver_notify (mode);

  memset (mode, 0, sizeof (*mode));
}

static void
meta_monitor_manager_free_mode_array (MetaCrtcMode *old_modes,
                                      int           n_old_modes)
{
  int i;

  for (i = 0; i < n_old_modes; i++)
    meta_monitor_manager_clear_mode (&old_modes[i]);

  g_free (old_modes);
}

void
meta_monitor_manager_clear_crtc (MetaCrtc *crtc)
{
  if (crtc->driver_notify)
    crtc->driver_notify (crtc);

  memset (crtc, 0, sizeof (*crtc));
}

static void
meta_monitor_manager_free_crtc_array (MetaCrtc *old_crtcs,
                                      int       n_old_crtcs)
{
  int i;

  for (i = 0; i < n_old_crtcs; i++)
    meta_monitor_manager_clear_crtc (&old_crtcs[i]);

  g_free (old_crtcs);
}

static void
meta_monitor_manager_finalize (GObject *object)
{
  MetaMonitorManager *manager = META_MONITOR_MANAGER (object);

  meta_monitor_manager_free_output_array (manager->outputs, manager->n_outputs);
  meta_monitor_manager_free_mode_array (manager->modes, manager->n_modes);
  meta_monitor_manager_free_crtc_array (manager->crtcs, manager->n_crtcs);
  g_list_free_full (manager->logical_monitors, g_object_unref);

  G_OBJECT_CLASS (meta_monitor_manager_parent_class)->finalize (object);
}

static void
meta_monitor_manager_dispose (GObject *object)
{
  MetaMonitorManager *manager = META_MONITOR_MANAGER (object);

  if (manager->dbus_name_id != 0)
    {
      g_bus_unown_name (manager->dbus_name_id);
      manager->dbus_name_id = 0;
    }

  g_clear_object (&manager->config_manager);
  g_clear_object (&manager->up_client);

  G_OBJECT_CLASS (meta_monitor_manager_parent_class)->dispose (object);
}

static GBytes *
meta_monitor_manager_real_read_edid (MetaMonitorManager *manager,
                                     MetaOutput         *output)
{
  return NULL;
}

static char *
meta_monitor_manager_real_get_edid_file (MetaMonitorManager *manager,
                                         MetaOutput         *output)
{
  return NULL;
}

static void
meta_monitor_manager_class_init (MetaMonitorManagerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = meta_monitor_manager_constructed;
  object_class->dispose = meta_monitor_manager_dispose;
  object_class->finalize = meta_monitor_manager_finalize;

  klass->get_edid_file = meta_monitor_manager_real_get_edid_file;
  klass->read_edid = meta_monitor_manager_real_read_edid;
  klass->is_lid_closed = meta_monitor_manager_real_is_lid_closed;

  signals[CONFIRM_DISPLAY_CHANGE] =
    g_signal_new ("confirm-display-change",
		  G_TYPE_FROM_CLASS (object_class),
		  G_SIGNAL_RUN_LAST,
		  0,
                  NULL, NULL, NULL,
		  G_TYPE_NONE, 0);

  signals[LID_IS_CLOSED_CHANGED] =
    g_signal_new ("lid-is-closed-changed",
		  G_TYPE_FROM_CLASS (object_class),
		  G_SIGNAL_RUN_LAST,
		  0,
                  NULL, NULL, NULL,
		  G_TYPE_NONE, 0);
}

static const double known_diagonals[] = {
    12.1,
    13.3,
    15.6
};

static char *
diagonal_to_str (double d)
{
  unsigned int i;

  for (i = 0; i < G_N_ELEMENTS (known_diagonals); i++)
    {
      double delta;

      delta = fabs(known_diagonals[i] - d);
      if (delta < 0.1)
        return g_strdup_printf ("%0.1lf\"", known_diagonals[i]);
    }

  return g_strdup_printf ("%d\"", (int) (d + 0.5));
}

static char *
make_display_name (MetaMonitorManager *manager,
                   MetaOutput         *output)
{
  g_autofree char *inches = NULL;
  g_autofree char *vendor_name = NULL;

  if (meta_output_is_laptop (output))
      return g_strdup (_("Built-in display"));

  if (output->width_mm > 0 && output->height_mm > 0)
    {
      double d = sqrt (output->width_mm * output->width_mm +
                       output->height_mm * output->height_mm);
      inches = diagonal_to_str (d / 25.4);
    }

  if (g_strcmp0 (output->vendor, "unknown") != 0)
    {
      if (!manager->pnp_ids)
        manager->pnp_ids = gnome_pnp_ids_new ();

      vendor_name = gnome_pnp_ids_get_pnp_id (manager->pnp_ids,
                                              output->vendor);

      if (!vendor_name)
        vendor_name = g_strdup (output->vendor);
    }
  else
    {
      if (inches != NULL)
        vendor_name = g_strdup (_("Unknown"));
      else
        vendor_name = g_strdup (_("Unknown Display"));
    }

  if (inches != NULL)
    {
      /* TRANSLATORS: this is a monitor vendor name, followed by a
       * size in inches, like 'Dell 15"'
       */
      return g_strdup_printf (_("%s %s"), vendor_name, inches);
    }
  else
    {
      return g_strdup (vendor_name);
    }
}

static const char *
get_connector_type_name (MetaConnectorType connector_type)
{
  switch (connector_type)
    {
    case META_CONNECTOR_TYPE_Unknown: return "Unknown";
    case META_CONNECTOR_TYPE_VGA: return "VGA";
    case META_CONNECTOR_TYPE_DVII: return "DVII";
    case META_CONNECTOR_TYPE_DVID: return "DVID";
    case META_CONNECTOR_TYPE_DVIA: return "DVIA";
    case META_CONNECTOR_TYPE_Composite: return "Composite";
    case META_CONNECTOR_TYPE_SVIDEO: return "SVIDEO";
    case META_CONNECTOR_TYPE_LVDS: return "LVDS";
    case META_CONNECTOR_TYPE_Component: return "Component";
    case META_CONNECTOR_TYPE_9PinDIN: return "9PinDIN";
    case META_CONNECTOR_TYPE_DisplayPort: return "DisplayPort";
    case META_CONNECTOR_TYPE_HDMIA: return "HDMIA";
    case META_CONNECTOR_TYPE_HDMIB: return "HDMIB";
    case META_CONNECTOR_TYPE_TV: return "TV";
    case META_CONNECTOR_TYPE_eDP: return "eDP";
    case META_CONNECTOR_TYPE_VIRTUAL: return "VIRTUAL";
    case META_CONNECTOR_TYPE_DSI: return "DSI";
    default: g_assert_not_reached ();
    }
}

static gboolean
meta_monitor_manager_handle_get_resources (MetaDBusDisplayConfig *skeleton,
                                           GDBusMethodInvocation *invocation)
{
  MetaMonitorManager *manager = META_MONITOR_MANAGER (skeleton);
  MetaMonitorManagerClass *manager_class = META_MONITOR_MANAGER_GET_CLASS (skeleton);
  GVariantBuilder crtc_builder, output_builder, mode_builder;
  unsigned int i, j;

  g_variant_builder_init (&crtc_builder, G_VARIANT_TYPE ("a(uxiiiiiuaua{sv})"));
  g_variant_builder_init (&output_builder, G_VARIANT_TYPE ("a(uxiausauaua{sv})"));
  g_variant_builder_init (&mode_builder, G_VARIANT_TYPE ("a(usxuudu)"));

  for (i = 0; i < manager->n_crtcs; i++)
    {
      MetaCrtc *crtc = &manager->crtcs[i];
      GVariantBuilder transforms;

      g_variant_builder_init (&transforms, G_VARIANT_TYPE ("au"));
      for (j = 0; j <= META_MONITOR_TRANSFORM_FLIPPED_270; j++)
        if (crtc->all_transforms & (1 << j))
          g_variant_builder_add (&transforms, "u", j);

      g_variant_builder_add (&crtc_builder, "(uxiiiiiuaua{sv})",
                             i, /* ID */
                             (gint64)crtc->crtc_id,
                             (int)crtc->rect.x,
                             (int)crtc->rect.y,
                             (int)crtc->rect.width,
                             (int)crtc->rect.height,
                             (int)(crtc->current_mode ? crtc->current_mode - manager->modes : -1),
                             (guint32)crtc->transform,
                             &transforms,
                             NULL /* properties */);
    }

  for (i = 0; i < manager->n_outputs; i++)
    {
      MetaOutput *output = &manager->outputs[i];
      GVariantBuilder crtcs, modes, clones, properties;
      GBytes *edid;
      char *edid_file;

      g_variant_builder_init (&crtcs, G_VARIANT_TYPE ("au"));
      for (j = 0; j < output->n_possible_crtcs; j++)
        g_variant_builder_add (&crtcs, "u",
                               (unsigned)(output->possible_crtcs[j] - manager->crtcs));

      g_variant_builder_init (&modes, G_VARIANT_TYPE ("au"));
      for (j = 0; j < output->n_modes; j++)
        g_variant_builder_add (&modes, "u",
                               (unsigned)(output->modes[j] - manager->modes));

      g_variant_builder_init (&clones, G_VARIANT_TYPE ("au"));
      for (j = 0; j < output->n_possible_clones; j++)
        g_variant_builder_add (&clones, "u",
                               (unsigned)(output->possible_clones[j] - manager->outputs));

      g_variant_builder_init (&properties, G_VARIANT_TYPE ("a{sv}"));
      g_variant_builder_add (&properties, "{sv}", "vendor",
                             g_variant_new_string (output->vendor));
      g_variant_builder_add (&properties, "{sv}", "product",
                             g_variant_new_string (output->product));
      g_variant_builder_add (&properties, "{sv}", "serial",
                             g_variant_new_string (output->serial));
      g_variant_builder_add (&properties, "{sv}", "width-mm",
                             g_variant_new_int32 (output->width_mm));
      g_variant_builder_add (&properties, "{sv}", "height-mm",
                             g_variant_new_int32 (output->height_mm));
      g_variant_builder_add (&properties, "{sv}", "display-name",
                             g_variant_new_take_string (make_display_name (manager, output)));
      g_variant_builder_add (&properties, "{sv}", "backlight",
                             g_variant_new_int32 (output->backlight));
      g_variant_builder_add (&properties, "{sv}", "min-backlight-step",
                             g_variant_new_int32 ((output->backlight_max - output->backlight_min) ?
                                                  100 / (output->backlight_max - output->backlight_min) : -1));
      g_variant_builder_add (&properties, "{sv}", "primary",
                             g_variant_new_boolean (output->is_primary));
      g_variant_builder_add (&properties, "{sv}", "presentation",
                             g_variant_new_boolean (output->is_presentation));
      g_variant_builder_add (&properties, "{sv}", "connector-type",
                             g_variant_new_string (get_connector_type_name (output->connector_type)));
      g_variant_builder_add (&properties, "{sv}", "underscanning",
                             g_variant_new_boolean (output->is_underscanning));
      g_variant_builder_add (&properties, "{sv}", "supports-underscanning",
                             g_variant_new_boolean (output->supports_underscanning));

      edid_file = manager_class->get_edid_file (manager, output);
      if (edid_file)
        {
          g_variant_builder_add (&properties, "{sv}", "edid-file",
                                 g_variant_new_take_string (edid_file));
        }
      else
        {
          edid = manager_class->read_edid (manager, output);

          if (edid)
            {
              g_variant_builder_add (&properties, "{sv}", "edid",
                                     g_variant_new_from_bytes (G_VARIANT_TYPE ("ay"),
                                                               edid, TRUE));
              g_bytes_unref (edid);
            }
        }

      if (output->tile_info.group_id)
        {
          g_variant_builder_add (&properties, "{sv}", "tile",
                                 g_variant_new ("(uuuuuuuu)",
                                                output->tile_info.group_id,
                                                output->tile_info.flags,
                                                output->tile_info.max_h_tiles,
                                                output->tile_info.max_v_tiles,
                                                output->tile_info.loc_h_tile,
                                                output->tile_info.loc_v_tile,
                                                output->tile_info.tile_w,
                                                output->tile_info.tile_h));
        }

      g_variant_builder_add (&output_builder, "(uxiausauaua{sv})",
                             i, /* ID */
                             (gint64)output->winsys_id,
                             (int)(output->crtc ? output->crtc - manager->crtcs : -1),
                             &crtcs,
                             output->name,
                             &modes,
                             &clones,
                             &properties);
    }

  for (i = 0; i < manager->n_modes; i++)
    {
      MetaCrtcMode *mode = &manager->modes[i];

      g_variant_builder_add (&mode_builder, "(usxuudu)",
                             i, /* ID */
                             mode->name,
                             (gint64)mode->mode_id,
                             (guint32)mode->width,
                             (guint32)mode->height,
                             (double)mode->refresh_rate,
                             (guint32)mode->flags);
    }

  meta_dbus_display_config_complete_get_resources (skeleton,
                                                   invocation,
                                                   manager->serial,
                                                   g_variant_builder_end (&crtc_builder),
                                                   g_variant_builder_end (&output_builder),
                                                   g_variant_builder_end (&mode_builder),
                                                   manager->max_screen_width,
                                                   manager->max_screen_height);
  return TRUE;
}

static gboolean
output_can_config (MetaOutput   *output,
                   MetaCrtc     *crtc,
                   MetaCrtcMode *mode)
{
  unsigned int i;
  gboolean ok = FALSE;

  for (i = 0; i < output->n_possible_crtcs && !ok; i++)
    ok = output->possible_crtcs[i] == crtc;

  if (!ok)
    return FALSE;

  if (mode == NULL)
    return TRUE;

  ok = FALSE;
  for (i = 0; i < output->n_modes && !ok; i++)
    ok = output->modes[i] == mode;

  return ok;
}

static gboolean
output_can_clone (MetaOutput *output,
                  MetaOutput *clone)
{
  unsigned int i;
  gboolean ok = FALSE;

  for (i = 0; i < output->n_possible_clones && !ok; i++)
    ok = output->possible_clones[i] == clone;

  return ok;
}

void
meta_monitor_manager_apply_configuration (MetaMonitorManager *manager,
                                          MetaCrtcInfo       **crtcs,
                                          unsigned int         n_crtcs,
                                          MetaOutputInfo     **outputs,
                                          unsigned int         n_outputs)
{
  META_MONITOR_MANAGER_GET_CLASS (manager)->apply_configuration (manager,
                                                                 crtcs, n_crtcs,
                                                                 outputs, n_outputs);
}

static void
legacy_restore_previous_config (MetaMonitorManager *manager)
{
  meta_monitor_config_restore_previous (manager->legacy_config, manager);
}

gint
meta_monitor_manager_get_display_configuration_timeout (void)
{
  return DEFAULT_DISPLAY_CONFIGURATION_TIMEOUT;
}

static gboolean
save_config_timeout (gpointer user_data)
{
  MetaMonitorManager *manager = user_data;

  if (manager->config_manager)
    g_assert (!"missing implementation");
  else
    legacy_restore_previous_config (manager);

  manager->persistent_timeout_id = 0;
  return G_SOURCE_REMOVE;
}

static gboolean
meta_monitor_manager_legacy_handle_apply_configuration  (MetaDBusDisplayConfig *skeleton,
                                                         GDBusMethodInvocation *invocation,
                                                         guint                  serial,
                                                         gboolean               persistent,
                                                         GVariant              *crtcs,
                                                         GVariant              *outputs)
{
  MetaMonitorManager *manager = META_MONITOR_MANAGER (skeleton);
  GVariantIter crtc_iter, output_iter, *nested_outputs;
  GVariant *properties;
  guint crtc_id;
  int new_mode, x, y;
  int new_screen_width, new_screen_height;
  guint transform;
  guint output_index;
  GPtrArray *crtc_infos, *output_infos;

  if (manager->config_manager)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Used old configuration API with new configuration system");
      return TRUE;
    }

  if (serial != manager->serial)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "The requested configuration is based on stale information");
      return TRUE;
    }

  crtc_infos = g_ptr_array_new_full (g_variant_n_children (crtcs),
                                     (GDestroyNotify) meta_crtc_info_free);
  output_infos = g_ptr_array_new_full (g_variant_n_children (outputs),
                                       (GDestroyNotify) meta_output_info_free);

  /* Validate all arguments */
  new_screen_width = 0; new_screen_height = 0;
  g_variant_iter_init (&crtc_iter, crtcs);
  while (g_variant_iter_loop (&crtc_iter, "(uiiiuaua{sv})",
                              &crtc_id, &new_mode, &x, &y, &transform,
                              &nested_outputs, NULL))
    {
      MetaCrtcInfo *crtc_info;
      MetaOutput *first_output;
      MetaCrtc *crtc;
      MetaCrtcMode *mode;

      crtc_info = g_slice_new (MetaCrtcInfo);
      crtc_info->outputs = g_ptr_array_new ();

      if (crtc_id >= manager->n_crtcs)
        {
          g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                                 G_DBUS_ERROR_INVALID_ARGS,
                                                 "Invalid CRTC id");
          return TRUE;
        }
      crtc = &manager->crtcs[crtc_id];
      crtc_info->crtc = crtc;

      if (new_mode != -1 && (new_mode < 0 || (unsigned)new_mode >= manager->n_modes))
        {
          g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                                 G_DBUS_ERROR_INVALID_ARGS,
                                                 "Invalid mode id");
          return TRUE;
        }
      mode = new_mode != -1 ? &manager->modes[new_mode] : NULL;
      crtc_info->mode = mode;

      if (mode)
        {
          int width, height;

          if (meta_monitor_transform_is_rotated (transform))
            {
              width = mode->height;
              height = mode->width;
            }
          else
            {
              width = mode->width;
              height = mode->height;
            }

          if (x < 0 ||
              x + width > manager->max_screen_width ||
              y < 0 ||
              y + height > manager->max_screen_height)
            {
              g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                                     G_DBUS_ERROR_INVALID_ARGS,
                                                     "Invalid CRTC geometry");
              return TRUE;
            }

          new_screen_width = MAX (new_screen_width, x + width);
          new_screen_height = MAX (new_screen_height, y + height);
          crtc_info->x = x;
          crtc_info->y = y;
        }
      else
        {
          crtc_info->x = 0;
          crtc_info->y = 0;
        }

      if (transform > META_MONITOR_TRANSFORM_FLIPPED_270 ||
          ((crtc->all_transforms & (1 << transform)) == 0))
        {
          g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                                 G_DBUS_ERROR_INVALID_ARGS,
                                                 "Invalid transform");
          return TRUE;
        }
      crtc_info->transform = transform;

      first_output = NULL;
      while (g_variant_iter_loop (nested_outputs, "u", &output_index))
        {
          MetaOutput *output;

          if (output_index >= manager->n_outputs)
            {
              g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                                     G_DBUS_ERROR_INVALID_ARGS,
                                                     "Invalid output id");
              return TRUE;
            }
          output = &manager->outputs[output_index];

          if (!output_can_config (output, crtc, mode))
            {
              g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                                     G_DBUS_ERROR_INVALID_ARGS,
                                                     "Output cannot be assigned to this CRTC or mode");
              return TRUE;
            }
          g_ptr_array_add (crtc_info->outputs, output);

          if (first_output)
            {
              if (!output_can_clone (output, first_output))
                {
                  g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                                         G_DBUS_ERROR_INVALID_ARGS,
                                                         "Outputs cannot be cloned");
                  return TRUE;
                }
            }
          else
            first_output = output;
        }

      if (!first_output && mode)
        {
          g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                                 G_DBUS_ERROR_INVALID_ARGS,
                                                 "Mode specified without outputs?");
          return TRUE;
        }

      g_ptr_array_add (crtc_infos, crtc_info);
    }

  if (new_screen_width == 0 || new_screen_height == 0)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                             G_DBUS_ERROR_INVALID_ARGS,
                                             "Refusing to disable all outputs");
      return TRUE;
    }

  g_variant_iter_init (&output_iter, outputs);
  while (g_variant_iter_loop (&output_iter, "(u@a{sv})", &output_index, &properties))
    {
      MetaOutput *output;
      MetaOutputInfo *output_info;
      gboolean primary, presentation, underscanning;

      if (output_index >= manager->n_outputs)
        {
          g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                                 G_DBUS_ERROR_INVALID_ARGS,
                                                 "Invalid output id");
          return TRUE;
        }

      output = &manager->outputs[output_index];
      underscanning = FALSE;

      if (g_variant_lookup (properties, "underscanning", "b", &underscanning))
        {
          if (underscanning && !output->supports_underscanning)
            {
              g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                                     G_DBUS_ERROR_INVALID_ARGS,
                                                     "Underscanning requested but unsupported");
              return TRUE;
            }
        }

      output_info = g_slice_new0 (MetaOutputInfo);
      output_info->output = output;
      output_info->is_underscanning = underscanning;

      if (g_variant_lookup (properties, "primary", "b", &primary))
        output_info->is_primary = primary;

      if (g_variant_lookup (properties, "presentation", "b", &presentation))
        output_info->is_presentation = presentation;

      g_ptr_array_add (output_infos, output_info);
    }

  /* If we were in progress of making a persistent change and we see a
     new request, it's likely that the old one failed in some way, so
     don't save it, but also don't queue for restoring it.
  */
  if (manager->persistent_timeout_id && persistent)
    {
      g_source_remove (manager->persistent_timeout_id);
      manager->persistent_timeout_id = 0;
    }

  meta_monitor_manager_apply_configuration (manager,
                                            (MetaCrtcInfo**)crtc_infos->pdata,
                                            crtc_infos->len,
                                            (MetaOutputInfo**)output_infos->pdata,
                                            output_infos->len);

  g_ptr_array_unref (crtc_infos);
  g_ptr_array_unref (output_infos);

  /* Update MetaMonitorConfig data structures immediately so that we
     don't revert the change at the next XRandR event, then ask the plugin
     manager (through MetaScreen) to confirm the display change with the
     appropriate UI. Then wait 20 seconds and if not confirmed, revert the
     configuration.
  */
  meta_monitor_config_update_current (manager->legacy_config, manager);
  if (persistent)
    {
      manager->persistent_timeout_id = g_timeout_add_seconds (meta_monitor_manager_get_display_configuration_timeout (),
                                                              save_config_timeout,
                                                              manager);
      g_source_set_name_by_id (manager->persistent_timeout_id, "[mutter] save_config_timeout");
      g_signal_emit (manager, signals[CONFIRM_DISPLAY_CHANGE], 0);
    }

  meta_dbus_display_config_complete_apply_configuration (skeleton, invocation);
  return TRUE;
}

static void
legacy_confirm_configuration (MetaMonitorManager *manager,
                              gboolean            confirmed)
{
  if (confirmed)
    meta_monitor_config_make_persistent (manager->legacy_config);
  else
    meta_monitor_config_restore_previous (manager->legacy_config, manager);
}

void
meta_monitor_manager_confirm_configuration (MetaMonitorManager *manager,
                                            gboolean            ok)
{
  if (!manager->persistent_timeout_id)
    {
      /* too late */
      return;
    }

  g_source_remove (manager->persistent_timeout_id);
  manager->persistent_timeout_id = 0;

  if (manager->config_manager)
    g_assert (!"not implemented");
  else
    legacy_confirm_configuration (manager, ok);
}

static gboolean
meta_monitor_manager_handle_change_backlight  (MetaDBusDisplayConfig *skeleton,
                                               GDBusMethodInvocation *invocation,
                                               guint                  serial,
                                               guint                  output_index,
                                               gint                   value)
{
  MetaMonitorManager *manager = META_MONITOR_MANAGER (skeleton);
  MetaOutput *output;

  if (serial != manager->serial)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "The requested configuration is based on stale information");
      return TRUE;
    }

  if (output_index >= manager->n_outputs)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                             G_DBUS_ERROR_INVALID_ARGS,
                                             "Invalid output id");
      return TRUE;
    }
  output = &manager->outputs[output_index];

  if (value < 0 || value > 100)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                             G_DBUS_ERROR_INVALID_ARGS,
                                             "Invalid backlight value");
      return TRUE;
    }

  if (output->backlight == -1 ||
      (output->backlight_min == 0 && output->backlight_max == 0))
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                             G_DBUS_ERROR_INVALID_ARGS,
                                             "Output does not support changing backlight");
      return TRUE;
    }

  META_MONITOR_MANAGER_GET_CLASS (manager)->change_backlight (manager, output, value);

  meta_dbus_display_config_complete_change_backlight (skeleton, invocation, output->backlight);
  return TRUE;
}

static gboolean
meta_monitor_manager_handle_get_crtc_gamma  (MetaDBusDisplayConfig *skeleton,
                                             GDBusMethodInvocation *invocation,
                                             guint                  serial,
                                             guint                  crtc_id)
{
  MetaMonitorManager *manager = META_MONITOR_MANAGER (skeleton);
  MetaMonitorManagerClass *klass;
  MetaCrtc *crtc;
  gsize size;
  unsigned short *red;
  unsigned short *green;
  unsigned short *blue;
  GBytes *red_bytes, *green_bytes, *blue_bytes;
  GVariant *red_v, *green_v, *blue_v;

  if (serial != manager->serial)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "The requested configuration is based on stale information");
      return TRUE;
    }

  if (crtc_id >= manager->n_crtcs)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                             G_DBUS_ERROR_INVALID_ARGS,
                                             "Invalid crtc id");
      return TRUE;
    }
  crtc = &manager->crtcs[crtc_id];

  klass = META_MONITOR_MANAGER_GET_CLASS (manager);
  if (klass->get_crtc_gamma)
    klass->get_crtc_gamma (manager, crtc, &size, &red, &green, &blue);
  else
    {
      size = 0;
      red = green = blue = NULL;
    }

  red_bytes = g_bytes_new_take (red, size * sizeof (unsigned short));
  green_bytes = g_bytes_new_take (green, size * sizeof (unsigned short));
  blue_bytes = g_bytes_new_take (blue, size * sizeof (unsigned short));

  red_v = g_variant_new_from_bytes (G_VARIANT_TYPE ("aq"), red_bytes, TRUE);
  green_v = g_variant_new_from_bytes (G_VARIANT_TYPE ("aq"), green_bytes, TRUE);
  blue_v = g_variant_new_from_bytes (G_VARIANT_TYPE ("aq"), blue_bytes, TRUE);

  meta_dbus_display_config_complete_get_crtc_gamma (skeleton, invocation,
                                                    red_v, green_v, blue_v);

  g_bytes_unref (red_bytes);
  g_bytes_unref (green_bytes);
  g_bytes_unref (blue_bytes);

  return TRUE;
}

static gboolean
meta_monitor_manager_handle_set_crtc_gamma  (MetaDBusDisplayConfig *skeleton,
                                             GDBusMethodInvocation *invocation,
                                             guint                  serial,
                                             guint                  crtc_id,
                                             GVariant              *red_v,
                                             GVariant              *green_v,
                                             GVariant              *blue_v)
{
  MetaMonitorManager *manager = META_MONITOR_MANAGER (skeleton);
  MetaMonitorManagerClass *klass;
  MetaCrtc *crtc;
  gsize size, dummy;
  unsigned short *red;
  unsigned short *green;
  unsigned short *blue;
  GBytes *red_bytes, *green_bytes, *blue_bytes;

  if (serial != manager->serial)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "The requested configuration is based on stale information");
      return TRUE;
    }

  if (crtc_id >= manager->n_crtcs)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                             G_DBUS_ERROR_INVALID_ARGS,
                                             "Invalid crtc id");
      return TRUE;
    }
  crtc = &manager->crtcs[crtc_id];

  red_bytes = g_variant_get_data_as_bytes (red_v);
  green_bytes = g_variant_get_data_as_bytes (green_v);
  blue_bytes = g_variant_get_data_as_bytes (blue_v);

  size = g_bytes_get_size (red_bytes) / sizeof (unsigned short);
  red = (unsigned short*) g_bytes_get_data (red_bytes, &dummy);
  green = (unsigned short*) g_bytes_get_data (green_bytes, &dummy);
  blue = (unsigned short*) g_bytes_get_data (blue_bytes, &dummy);

  klass = META_MONITOR_MANAGER_GET_CLASS (manager);
  if (klass->set_crtc_gamma)
    klass->set_crtc_gamma (manager, crtc, size, red, green, blue);

  meta_dbus_display_config_complete_set_crtc_gamma (skeleton, invocation);

  g_bytes_unref (red_bytes);
  g_bytes_unref (green_bytes);
  g_bytes_unref (blue_bytes);

  return TRUE;
}

static void
meta_monitor_manager_display_config_init (MetaDBusDisplayConfigIface *iface)
{
  iface->handle_get_resources = meta_monitor_manager_handle_get_resources;
  iface->handle_apply_configuration = meta_monitor_manager_legacy_handle_apply_configuration;
  iface->handle_change_backlight = meta_monitor_manager_handle_change_backlight;
  iface->handle_get_crtc_gamma = meta_monitor_manager_handle_get_crtc_gamma;
  iface->handle_set_crtc_gamma = meta_monitor_manager_handle_set_crtc_gamma;
}

static void
on_bus_acquired (GDBusConnection *connection,
                 const char      *name,
                 gpointer         user_data)
{
  MetaMonitorManager *manager = user_data;

  g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (manager),
                                    connection,
                                    "/org/gnome/Mutter/DisplayConfig",
                                    NULL);
}

static void
on_name_acquired (GDBusConnection *connection,
                  const char      *name,
                  gpointer         user_data)
{
  meta_topic (META_DEBUG_DBUS, "Acquired name %s\n", name);
}

static void
on_name_lost (GDBusConnection *connection,
              const char      *name,
              gpointer         user_data)
{
  meta_topic (META_DEBUG_DBUS, "Lost or failed to acquire name %s\n", name);
}

static void
initialize_dbus_interface (MetaMonitorManager *manager)
{
  manager->dbus_name_id = g_bus_own_name (G_BUS_TYPE_SESSION,
                                          "org.gnome.Mutter.DisplayConfig",
                                          G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT |
                                          (meta_get_replace_current_wm () ?
                                           G_BUS_NAME_OWNER_FLAGS_REPLACE : 0),
                                          on_bus_acquired,
                                          on_name_acquired,
                                          on_name_lost,
                                          g_object_ref (manager),
                                          g_object_unref);
}

/**
 * meta_monitor_manager_get:
 *
 * Accessor for the singleton MetaMonitorManager.
 *
 * Returns: (transfer none): The only #MetaMonitorManager there is.
 */
MetaMonitorManager *
meta_monitor_manager_get (void)
{
  MetaBackend *backend = meta_get_backend ();

  return meta_backend_get_monitor_manager (backend);
}

int
meta_monitor_manager_get_num_logical_monitors (MetaMonitorManager *manager)
{
  return g_list_length (manager->logical_monitors);
}

GList *
meta_monitor_manager_get_logical_monitors (MetaMonitorManager *manager)
{
  return manager->logical_monitors;
}

MetaLogicalMonitor *
meta_monitor_manager_get_logical_monitor_from_number (MetaMonitorManager *manager,
                                                      int                 number)
{
  g_return_val_if_fail ((unsigned int) number < g_list_length (manager->logical_monitors), NULL);

  return g_list_nth (manager->logical_monitors, number)->data;
}

MetaLogicalMonitor *
meta_monitor_manager_get_primary_logical_monitor (MetaMonitorManager *manager)
{
  return manager->primary_logical_monitor;
}

static MetaMonitor *
find_monitor (MetaMonitorManager *monitor_manager,
              gboolean (*match_func) (MetaMonitor *monitor))
{
  GList *monitors;
  GList *l;

  monitors = meta_monitor_manager_get_monitors (monitor_manager);
  for (l = monitors; l; l = l->next)
    {
      MetaMonitor *monitor = l->data;

      if (match_func (monitor))
        return monitor;
    }

  return NULL;
}

MetaMonitor *
meta_monitor_manager_get_primary_monitor (MetaMonitorManager *manager)
{
  return find_monitor (manager, meta_monitor_is_primary);
}

MetaMonitor *
meta_monitor_manager_get_laptop_panel (MetaMonitorManager *manager)
{
  return find_monitor (manager, meta_monitor_is_laptop_panel);
}

MetaMonitor *
meta_monitor_manager_get_monitor_from_spec (MetaMonitorManager *manager,
                                            MetaMonitorSpec    *monitor_spec)
{
  GList *l;

  for (l = manager->monitors; l; l = l->next)
    {
      MetaMonitor *monitor = l->data;

      if (meta_monitor_spec_equals (meta_monitor_get_spec (monitor),
                                    monitor_spec))
        return monitor;
    }

  return NULL;
}

MetaLogicalMonitor *
meta_monitor_manager_get_logical_monitor_at (MetaMonitorManager *manager,
                                             float               x,
                                             float               y)
{
  GList *l;

  for (l = manager->logical_monitors; l; l = l->next)
    {
      MetaLogicalMonitor *logical_monitor = l->data;

      if (POINT_IN_RECT (x, y, logical_monitor->rect))
        return logical_monitor;
    }

  return NULL;
}

MetaLogicalMonitor *
meta_monitor_manager_get_logical_monitor_from_rect (MetaMonitorManager *manager,
                                                    MetaRectangle      *rect)
{
  MetaLogicalMonitor *best_logical_monitor;
  int best_logical_monitor_area;
  GList *l;

  best_logical_monitor = NULL;
  best_logical_monitor_area = 0;

  for (l = manager->logical_monitors; l; l = l->next)
    {
      MetaLogicalMonitor *logical_monitor = l->data;
      MetaRectangle intersection;
      int intersection_area;

      if (!meta_rectangle_intersect (&logical_monitor->rect,
                                     rect,
                                     &intersection))
        continue;

      intersection_area = meta_rectangle_area (&intersection);

      if (intersection_area > best_logical_monitor_area)
        {
          best_logical_monitor = logical_monitor;
          best_logical_monitor_area = intersection_area;
        }
    }

  if (!best_logical_monitor && (rect->width == 0 || rect->height == 0))
    best_logical_monitor =
      meta_monitor_manager_get_logical_monitor_at (manager, rect->x, rect->y);

  if (!best_logical_monitor)
    best_logical_monitor = manager->primary_logical_monitor;

  return best_logical_monitor;
}

MetaLogicalMonitor *
meta_monitor_manager_get_logical_monitor_neighbor (MetaMonitorManager *manager,
                                                   MetaLogicalMonitor *logical_monitor,
                                                   MetaScreenDirection direction)
{
  GList *l;

  for (l = manager->logical_monitors; l; l = l->next)
    {
      MetaLogicalMonitor *other = l->data;

      if (meta_logical_monitor_has_neighbor (logical_monitor, other, direction))
        return other;
    }

  return NULL;
}

GList *
meta_monitor_manager_get_monitors (MetaMonitorManager *manager)
{
  return manager->monitors;
}

MetaOutput *
meta_monitor_manager_get_outputs (MetaMonitorManager *manager,
                                  unsigned int       *n_outputs)
{
  *n_outputs = manager->n_outputs;
  return manager->outputs;
}

void
meta_monitor_manager_get_resources (MetaMonitorManager  *manager,
                                    MetaCrtcMode       **modes,
                                    unsigned int        *n_modes,
                                    MetaCrtc           **crtcs,
                                    unsigned int        *n_crtcs,
                                    MetaOutput         **outputs,
                                    unsigned int        *n_outputs)
{
  if (modes)
    {
      *modes = manager->modes;
      *n_modes = manager->n_modes;
    }
  if (crtcs)
    {
      *crtcs = manager->crtcs;
      *n_crtcs = manager->n_crtcs;
    }
  if (outputs)
    {
      *outputs = manager->outputs;
      *n_outputs = manager->n_outputs;
    }
}

void
meta_monitor_manager_get_screen_size (MetaMonitorManager *manager,
                                      int                *width,
                                      int                *height)
{
  *width = manager->screen_width;
  *height = manager->screen_height;
}

void
meta_monitor_manager_get_screen_limits (MetaMonitorManager *manager,
                                        int                *width,
                                        int                *height)
{
  *width = manager->max_screen_width;
  *height = manager->max_screen_height;
}

static void
rebuild_monitors (MetaMonitorManager *manager)
{
  unsigned int i;

  if (manager->monitors)
    {
      g_list_free_full (manager->monitors, g_object_unref);
      manager->monitors = NULL;
    }

  for (i = 0; i < manager->n_outputs; i++)
    {
      MetaOutput *output = &manager->outputs[i];

      if (output->tile_info.group_id)
        {
          if (is_main_tiled_monitor_output (output))
            {
              MetaMonitorTiled *monitor_tiled;

              monitor_tiled = meta_monitor_tiled_new (manager, output);
              manager->monitors = g_list_append (manager->monitors,
                                                 monitor_tiled);
            }
        }
      else
        {
          MetaMonitorNormal *monitor_normal;

          monitor_normal = meta_monitor_normal_new (manager, output);
          manager->monitors = g_list_append (manager->monitors,
                                             monitor_normal);
        }
    }
}

void
meta_monitor_manager_tiled_monitor_added (MetaMonitorManager *manager,
                                          MetaMonitor        *monitor)
{
  MetaMonitorManagerClass *manager_class =
    META_MONITOR_MANAGER_GET_CLASS (manager);

  if (manager_class->tiled_monitor_added)
    manager_class->tiled_monitor_added (manager, monitor);
}

void
meta_monitor_manager_tiled_monitor_removed (MetaMonitorManager *manager,
                                            MetaMonitor        *monitor)
{
  MetaMonitorManagerClass *manager_class =
    META_MONITOR_MANAGER_GET_CLASS (manager);

  if (manager_class->tiled_monitor_removed)
    manager_class->tiled_monitor_removed (manager, monitor);
}

gboolean
meta_monitor_manager_is_transform_handled (MetaMonitorManager  *manager,
                                           MetaCrtc            *crtc,
                                           MetaMonitorTransform transform)
{
  MetaMonitorManagerClass *manager_class =
    META_MONITOR_MANAGER_GET_CLASS (manager);

  return manager_class->is_transform_handled (manager, crtc, transform);
}

static GVariant *
meta_monitor_manager_get_output_auxiliary_payload (MetaMonitorManager *manager,
                                                   MetaOutput         *output)
{
  GBytes *edid;
  GVariant *edid_variant;
  const guint8 *raw_edid = NULL;
  gsize edid_length = 0;
  MetaMonitorManagerClass *manager_class = META_MONITOR_MANAGER_GET_CLASS (manager);

  edid = manager_class->read_edid (manager, output);
  if (edid != NULL)
    {
      raw_edid = g_bytes_get_data (edid, &edid_length);
      g_bytes_unref (edid);
    }

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
meta_monitor_manager_record_connect_events (MetaMonitorManager *manager,
                                            MetaOutput         *old_outputs,
                                            gsize               n_old_outputs)
{
  gsize new_output_index;
  for (new_output_index = 0; new_output_index < manager->n_outputs;
       new_output_index++)
    {
      MetaOutput *new_output = manager->outputs + new_output_index;
      gsize old_output_index;

      for (old_output_index = 0; old_output_index < n_old_outputs;
           old_output_index++)
        {
          MetaOutput *old_output = old_outputs + old_output_index;
          if (new_output->winsys_id == old_output->winsys_id)
            break;
        }

      if (old_output_index == n_old_outputs)
        {
          /* Output is connected now but wasn't previously. */
          GVariant *auxiliary_payload =
            meta_monitor_manager_get_output_auxiliary_payload (manager,
                                                               new_output);
          emtr_event_recorder_record_event (emtr_event_recorder_get_default (),
                                            MONITOR_CONNECTED,
                                            auxiliary_payload);
        }
    }
}

static void
meta_monitor_manager_record_disconnect_events (MetaMonitorManager *manager,
                                               MetaOutput         *old_outputs,
                                               gsize               n_old_outputs)
{
  gsize old_output_index;
  for (old_output_index = 0; old_output_index < n_old_outputs;
       old_output_index++)
    {
      MetaOutput *old_output = old_outputs + old_output_index;
      gsize new_output_index;

      for (new_output_index = 0; new_output_index < manager->n_outputs;
           new_output_index++)
        {
          MetaOutput *new_output = manager->outputs + new_output_index;
          if (old_output->winsys_id == new_output->winsys_id)
            break;
        }

      if (new_output_index == manager->n_outputs)
        {
          /* Output was connected previously but isn't now. */
          GVariant *auxiliary_payload =
            meta_monitor_manager_get_output_auxiliary_payload (manager,
                                                               old_output);
          emtr_event_recorder_record_event (emtr_event_recorder_get_default (),
                                            MONITOR_DISCONNECTED,
                                            auxiliary_payload);
        }
    }
}

static void
meta_monitor_manager_record_connection_changes (MetaMonitorManager *manager,
                                                MetaOutput         *old_outputs,
                                                gsize               n_old_outputs)
{
  meta_monitor_manager_record_connect_events (manager, old_outputs,
                                              n_old_outputs);
  meta_monitor_manager_record_disconnect_events (manager, old_outputs,
                                                 n_old_outputs);
}

void
meta_monitor_manager_read_current_state (MetaMonitorManager *manager)
{
  MetaOutput *old_outputs;
  MetaCrtc *old_crtcs;
  MetaCrtcMode *old_modes;
  unsigned int n_old_outputs, n_old_crtcs, n_old_modes;

  /* Some implementations of read_current use the existing information
   * we have available, so don't free the old configuration until after
   * read_current finishes. */
  old_outputs = manager->outputs;
  n_old_outputs = manager->n_outputs;
  old_crtcs = manager->crtcs;
  n_old_crtcs = manager->n_crtcs;
  old_modes = manager->modes;
  n_old_modes = manager->n_modes;

  manager->serial++;
  META_MONITOR_MANAGER_GET_CLASS (manager)->read_current (manager);

  rebuild_monitors (manager);
  meta_monitor_manager_record_connection_changes (manager, old_outputs, n_old_outputs);

  meta_monitor_manager_free_output_array (old_outputs, n_old_outputs);
  meta_monitor_manager_free_mode_array (old_modes, n_old_modes);
  meta_monitor_manager_free_crtc_array (old_crtcs, n_old_crtcs);
}

static void
meta_monitor_manager_notify_monitors_changed (MetaMonitorManager *manager)
{
  MetaBackend *backend = meta_get_backend ();

  meta_backend_monitors_changed (backend);
  g_signal_emit_by_name (manager, "monitors-changed");
}

static void
set_logical_monitor_modes (MetaMonitorManager       *manager,
                           MetaLogicalMonitorConfig *logical_monitor_config)
{
  GList *l;

  for (l = logical_monitor_config->monitor_configs; l; l = l->next)
    {
      MetaMonitorConfig *monitor_config = l->data;
      MetaMonitorSpec *monitor_spec;
      MetaMonitor *monitor;
      MetaMonitorModeSpec *monitor_mode_spec;
      MetaMonitorMode *monitor_mode;

      monitor_spec = monitor_config->monitor_spec;
      monitor = meta_monitor_manager_get_monitor_from_spec (manager,
                                                            monitor_spec);
      monitor_mode_spec = monitor_config->mode_spec;
      monitor_mode = meta_monitor_get_mode_from_spec (monitor,
                                                      monitor_mode_spec);

      meta_monitor_set_current_mode (monitor, monitor_mode);
    }
}

static void
meta_monitor_manager_update_monitor_modes (MetaMonitorManager *manager,
                                           MetaMonitorsConfig *config)
{
  GList *logical_monitor_configs;
  GList *l;

  logical_monitor_configs = config ? config->logical_monitor_configs : NULL;
  for (l = logical_monitor_configs; l; l = l->next)
    {
      MetaLogicalMonitorConfig *logical_monitor_config = l->data;

      set_logical_monitor_modes (manager, logical_monitor_config);
    }

  for (l = manager->monitors; l; l = l->next)
    {
      MetaMonitor *monitor = l->data;

      if (!meta_monitor_get_logical_monitor (monitor))
        meta_monitor_set_current_mode (monitor, NULL);
    }
}

void
meta_monitor_manager_update_logical_state (MetaMonitorManager *manager,
                                           MetaMonitorsConfig *config)
{
  meta_monitor_manager_rebuild_logical_monitors (manager, config);
  meta_monitor_manager_update_monitor_modes (manager, config);
}

void
meta_monitor_manager_rebuild (MetaMonitorManager *manager,
                              MetaMonitorsConfig *config)
{
  GList *old_logical_monitors;

  if (manager->in_init)
    return;

  old_logical_monitors = manager->logical_monitors;

  meta_monitor_manager_update_logical_state (manager, config);

  meta_monitor_manager_notify_monitors_changed (manager);

  g_list_free_full (old_logical_monitors, g_object_unref);
}

static void
meta_monitor_manager_update_monitor_modes_derived (MetaMonitorManager *manager)
{
  GList *l;

  for (l = manager->monitors; l; l = l->next)
    {
      MetaMonitor *monitor = l->data;

      meta_monitor_derive_current_mode (monitor);
    }
}

void
meta_monitor_manager_update_logical_state_derived (MetaMonitorManager *manager)
{
  meta_monitor_manager_rebuild_logical_monitors_derived (manager);
  meta_monitor_manager_update_monitor_modes_derived (manager);
}

void
meta_monitor_manager_rebuild_derived (MetaMonitorManager *manager)
{
  GList *old_logical_monitors;

  if (manager->in_init)
    return;

  old_logical_monitors = manager->logical_monitors;

  meta_monitor_manager_update_logical_state_derived (manager);

  meta_monitor_manager_notify_monitors_changed (manager);

  g_list_free_full (old_logical_monitors, g_object_unref);
}

void
meta_output_parse_edid (MetaOutput *meta_output,
                        GBytes     *edid)
{
  MonitorInfo *parsed_edid;
  gsize len;

  if (!edid)
    goto out;

  parsed_edid = decode_edid (g_bytes_get_data (edid, &len));

  if (parsed_edid)
    {
      meta_output->vendor = g_strndup (parsed_edid->manufacturer_code, 4);
      if (!g_utf8_validate (meta_output->vendor, -1, NULL))
        g_clear_pointer (&meta_output->vendor, g_free);

      meta_output->product = g_strndup (parsed_edid->dsc_product_name, 14);
      if (!g_utf8_validate (meta_output->product, -1, NULL) ||
          meta_output->product[0] == '\0')
        {
          g_clear_pointer (&meta_output->product, g_free);
          meta_output->product = g_strdup_printf ("0x%04x", (unsigned) parsed_edid->product_code);
        }

      meta_output->serial = g_strndup (parsed_edid->dsc_serial_number, 14);
      if (!g_utf8_validate (meta_output->serial, -1, NULL) ||
          meta_output->serial[0] == '\0')
        {
          g_clear_pointer (&meta_output->serial, g_free);
          meta_output->serial = g_strdup_printf ("0x%08x", parsed_edid->serial_number);
        }

      g_free (parsed_edid);
    }

 out:
  if (!meta_output->vendor)
    meta_output->vendor = g_strdup ("unknown");
  if (!meta_output->product)
    meta_output->product = g_strdup ("unknown");
  if (!meta_output->serial)
    meta_output->serial = g_strdup ("unknown");
}

gboolean
meta_output_is_laptop (MetaOutput *output)
{
  /* FIXME: extend with better heuristics */
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

static gboolean
is_hdtv_resolution (int width,
                    int height)
{
  return ((width == 1920 && height == 1080) ||
          (width == 1440 && height == 1080) ||
          (width == 1280 && height == 720));
}

gboolean
meta_output_supports_underscan (MetaOutput *output)
{
  if (!g_str_has_prefix (get_connector_type_name (output->connector_type), "HDMI"))
    return FALSE;

  if (!output->crtc || !output->crtc->current_mode)
    return FALSE;

  return is_hdtv_resolution (output->crtc->current_mode->width, output->crtc->current_mode->height);
}

static void
legacy_on_hotplug (MetaMonitorManager *manager)
{
  gboolean applied_config = FALSE;

  /* If the monitor has hotplug_mode_update (which is used by VMs), don't bother
   * applying our stored configuration, because it's likely the user just resizing
   * the window.
   */
  if (!meta_monitor_manager_has_hotplug_mode_update (manager))
    {
      if (meta_monitor_config_apply_stored (manager->legacy_config, manager))
        applied_config = TRUE;
    }

  /* If we haven't applied any configuration, apply the default configuration. */
  if (!applied_config)
    meta_monitor_config_make_default (manager->legacy_config, manager);
}

void
meta_monitor_manager_on_hotplug (MetaMonitorManager *manager)
{
  if (manager->legacy_config)
    {
      legacy_on_hotplug (manager);
      return;
    }

  meta_monitor_manager_ensure_configured (manager);
}

static gboolean
calculate_viewport_matrix (MetaMonitorManager *manager,
                           MetaLogicalMonitor *logical_monitor,
                           gfloat              viewport[6])
{
  gfloat x, y, width, height;

  x = (float) logical_monitor->rect.x / manager->screen_width;
  y = (float) logical_monitor->rect.y / manager->screen_height;
  width  = (float) logical_monitor->rect.width / manager->screen_width;
  height = (float) logical_monitor->rect.height / manager->screen_height;

  viewport[0] = width;
  viewport[1] = 0.0f;
  viewport[2] = x;
  viewport[3] = 0.0f;
  viewport[4] = height;
  viewport[5] = y;

  return TRUE;
}

static inline void
multiply_matrix (float a[6],
		 float b[6],
		 float res[6])
{
  res[0] = a[0] * b[0] + a[1] * b[3];
  res[1] = a[0] * b[1] + a[1] * b[4];
  res[2] = a[0] * b[2] + a[1] * b[5] + a[2];
  res[3] = a[3] * b[0] + a[4] * b[3];
  res[4] = a[3] * b[1] + a[4] * b[4];
  res[5] = a[3] * b[2] + a[4] * b[5] + a[5];
}

gboolean
meta_monitor_manager_get_monitor_matrix (MetaMonitorManager *manager,
                                         MetaLogicalMonitor *logical_monitor,
                                         gfloat              matrix[6])
{
  MetaMonitor *main_monitor;
  MetaOutput *main_output;
  MetaMonitorTransform transform;
  gfloat viewport[9];

  if (!calculate_viewport_matrix (manager, logical_monitor, viewport))
    return FALSE;

  main_monitor = meta_logical_monitor_get_monitors (logical_monitor)->data;
  main_output = meta_monitor_get_main_output (main_monitor);
  transform = main_output->crtc->transform;
  multiply_matrix (viewport, transform_matrices[transform],
                   matrix);
  return TRUE;
}

/**
 * meta_monitor_manager_get_output_geometry:
 * @manager: A #MetaMonitorManager
 * @id: A valid #MetaOutput id
 *
 * Returns: The monitor index or -1 if @id isn't valid or the output
 * isn't associated with a logical monitor.
 */
gint
meta_monitor_manager_get_monitor_for_output (MetaMonitorManager *manager,
                                             guint               id)
{
  MetaOutput *output;
  GList *l;

  g_return_val_if_fail (META_IS_MONITOR_MANAGER (manager), -1);
  g_return_val_if_fail (id < manager->n_outputs, -1);

  output = &manager->outputs[id];
  if (!output || !output->crtc)
    return -1;

  for (l = manager->logical_monitors; l; l = l->next)
    {
      MetaLogicalMonitor *logical_monitor = l->data;

      if (meta_rectangle_contains_rect (&logical_monitor->rect,
                                        &output->crtc->rect))
        return logical_monitor->number;
    }

  return -1;
}

gboolean
meta_monitor_manager_get_is_builtin_display_on (MetaMonitorManager *manager)
{
  MetaMonitor *laptop_panel;

  g_return_val_if_fail (META_IS_MONITOR_MANAGER (manager), FALSE);

  laptop_panel = meta_monitor_manager_get_laptop_panel (manager);
  if (!laptop_panel)
    return FALSE;

  return meta_monitor_is_active (laptop_panel);
}
