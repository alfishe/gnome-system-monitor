/*
 * GNOME System Monitor graph
 * Copyright (C) 2022 Ondřej Míchal <harrymichal@seznam.cz>
 * All rights reserved.
 *
 * This Library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with the software; see the file COPYING. If not,
 * see <http://www.gnu.org/licenses/>.
 */

#include "gsm-graph.h"

#include <glib/gi18n.h>
#include <math.h>

enum
{
  CSS_CHANGED,
  NUM_SIGNALS
};

enum
{
  PROP_LOGARITHMIC_SCALE = 1,
  PROP_FONT_SIZE,
  PROP_NUM_POINTS,
  PROP_SMOOTH_CHART,
  PROP_STACKED_CHART,
  PROP_MAX_VALUE,
  NUM_PROPS
};

static guint signals[NUM_SIGNALS] = {
  0
};

static GParamSpec* obj_properties[NUM_PROPS] = {
  NULL,
};

G_DEFINE_TYPE_WITH_CODE (GsmGraph, gsm_graph, GTK_TYPE_DRAWING_AREA, G_ADD_PRIVATE(GsmGraph))

static void
gsm_graph_css_changed (GtkWidget *widget,
                       GtkCssStyleChange*)
{
  g_signal_emit (G_OBJECT (widget), signals[CSS_CHANGED], 0);
}

static void
gsm_graph_set_property (GObject      *object,
                        guint         property_id,
                        const GValue *value,
                        GParamSpec   *pspec)
{
  GsmGraph *self = GSM_GRAPH (object);

  switch (property_id)
    {
    case PROP_LOGARITHMIC_SCALE:
      gsm_graph_set_logarithmic_scale (self, g_value_get_boolean (value));
      break;
    case PROP_SMOOTH_CHART:
      gsm_graph_set_smooth_chart (self, g_value_get_boolean (value));
      break;
    case PROP_STACKED_CHART:
      gsm_graph_set_stacked_chart (self, g_value_get_boolean (value));
      break;
    case PROP_FONT_SIZE:
      gsm_graph_set_font_size (self, g_value_get_double (value));
      break;
    case PROP_NUM_POINTS:
      gsm_graph_set_num_points (self, g_value_get_uint (value));
      break;
    case PROP_MAX_VALUE:
      gsm_graph_set_max_value (self, g_value_get_uint64 (value));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
    }
}

static void
gsm_graph_get_property (GObject    *object,
                        guint       property_id,
                        GValue     *value,
                        GParamSpec *pspec)
{
  GsmGraph *self = GSM_GRAPH (object);

  switch (property_id)
    {
    case PROP_LOGARITHMIC_SCALE:
      g_value_set_boolean (value, gsm_graph_is_logarithmic_scale (self));
      break;
    case PROP_SMOOTH_CHART:
      g_value_set_boolean (value, gsm_graph_is_smooth_chart (self));
      break;
    case PROP_STACKED_CHART:
      g_value_set_boolean (value, gsm_graph_is_stacked_chart (self));
      break;
    case PROP_FONT_SIZE:
      g_value_set_double (value, gsm_graph_get_font_size (self));
      break;
    case PROP_NUM_POINTS:
      g_value_set_double (value, gsm_graph_get_font_size (self));
      break;
    case PROP_MAX_VALUE:
      g_value_set_uint64 (value, gsm_graph_get_max_value (self));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
    }
}

static cairo_surface_t *
gsm_graph_render_layer (GsmGraph             *self,
                        GsmGraphLayer         layer,
                        const graphene_rect_t*bounds,
                        double                scale,
                        int                   width,
                        int                   height,
                        const GsmGraphLayout *layout)
{
  GsmGraphPrivate *priv = gsm_graph_get_instance_private (self);
  int pixel_width = (int) ceil (bounds->size.width * scale);
  int pixel_height = (int) ceil (bounds->size.height * scale);

  if (pixel_width <= 0 || pixel_height <= 0)
    return NULL;

  cairo_surface_t *surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, pixel_width, pixel_height);
  cairo_surface_set_device_scale (surface, scale, scale);

  cairo_t *cr = cairo_create (surface);
  cairo_translate (cr, -bounds->origin.x, -bounds->origin.y);
  priv->paint_func (self, layer, cr, width, height, layout, priv->paint_data);
  cairo_destroy (cr);

  return surface;
}

/* Blits a cached layer; x/y must be on device pixels so pixman takes its
 * unfiltered fast path. */
static void
append_layer (GtkSnapshot     *snapshot,
              cairo_surface_t *surface,
              double           x,
              double           y,
              double           scale)
{
  cairo_t *cr = gtk_snapshot_append_cairo (snapshot,
                                           &GRAPHENE_RECT_INIT (x, y,
                                                                cairo_image_surface_get_width (surface) / scale,
                                                                cairo_image_surface_get_height (surface) / scale));
  cairo_set_source_surface (cr, surface, x, y);
  cairo_paint (cr);
  cairo_destroy (cr);
}

static double
gsm_graph_get_surface_scale (GtkWidget *widget)
{
  GtkNative *native = gtk_widget_get_native (widget);
  GdkSurface *surface = native ? gtk_native_get_surface (native) : NULL;

  if (surface)
    return gdk_surface_get_scale (surface);

  return gtk_widget_get_scale_factor (widget);
}

static guint
gsm_graph_get_sample_period (GsmGraph *self)
{
  GsmGraphPrivate *priv = gsm_graph_get_instance_private (self);

  return MAX (priv->speed * priv->frames_per_unit, 1);
}

/* Upper bound for the scrolling animation frame rate */
#define GSM_GRAPH_MAX_FPS 30

/* Fraction [0, 1] of the way from the latest sample to the next one, based on
 * the current frame time, so the scrolling speed is independent of the frame
 * rate and of timer jitter. The time is quantized to 1/GSM_GRAPH_MAX_FPS
 * slots shared by all graphs (they use the same frame clock), so they all
 * move in the same frames and the window repaints at most GSM_GRAPH_MAX_FPS
 * times per second. */
static double
gsm_graph_get_scroll_phase (GsmGraph *self)
{
  GsmGraphPrivate *priv = gsm_graph_get_instance_private (self);
  GdkFrameClock *clock = gtk_widget_get_frame_clock (GTK_WIDGET (self));
  gint64 now = clock ? gdk_frame_clock_get_frame_time (clock) : g_get_monotonic_time ();
  const gint64 slot = G_USEC_PER_SEC / GSM_GRAPH_MAX_FPS;

  now -= now % slot;
  double phase = (now - priv->last_sample_time) / (1000.0 * gsm_graph_get_sample_period (self));

  return CLAMP (phase, 0.0, 1.0);
}

static void
gsm_graph_snapshot (GtkWidget   *widget,
                    GtkSnapshot *snapshot)
{
  GsmGraph *self = GSM_GRAPH (widget);
  GsmGraphPrivate *priv = gsm_graph_get_instance_private (self);
  int width = gtk_widget_get_width (widget);
  int height = gtk_widget_get_height (widget);
  double scale = gsm_graph_get_surface_scale (widget);
  GsmGraphLayout layout;

  if (width <= 0 || height <= 0 || priv->layout_func == NULL || priv->paint_func == NULL)
    return;

  if (width != priv->cache_width || height != priv->cache_height || scale != priv->cache_scale)
    {
      gsm_graph_clear_background (self);
      priv->cache_width = width;
      priv->cache_height = height;
      priv->cache_scale = scale;
    }

  priv->layout_func (self, width, height, &layout, priv->paint_data);

  if (priv->background == NULL)
    {
      graphene_rect_t bounds = GRAPHENE_RECT_INIT (0, 0, width, height);
      priv->background = gsm_graph_render_layer (self, GSM_GRAPH_LAYER_BACKGROUND,
                                                 &bounds, scale, width, height, &layout);
    }

  if (priv->curves == NULL || priv->curves_generation != priv->data_generation)
    {
      /* The layer is one sample wider than the plot (plus room for the line
       * caps), so it can be scrolled left by up to one x_step. */
      g_clear_pointer (&priv->curves, cairo_surface_destroy);
      priv->curves_bounds = GRAPHENE_RECT_INIT (floor (layout.plot.origin.x * scale) / scale,
                                                floor (layout.plot.origin.y * scale) / scale,
                                                layout.plot.size.width + layout.x_step + 2,
                                                layout.plot.size.height + 1);
      priv->curves = gsm_graph_render_layer (self, GSM_GRAPH_LAYER_CURVES,
                                             &priv->curves_bounds, scale, width, height, &layout);
      priv->curves_generation = priv->data_generation;
    }

  if (priv->background)
    append_layer (snapshot, priv->background, 0, 0, scale);

  if (priv->curves)
    {
      /* Smooth scrolling between two samples, in whole device pixels */
      double shift = round (layout.x_step * gsm_graph_get_scroll_phase (self) * scale);

      priv->x_step = layout.x_step;
      priv->drawn_shift = shift;

      gtk_snapshot_push_clip (snapshot, &layout.plot);
      append_layer (snapshot, priv->curves,
                    priv->curves_bounds.origin.x - shift / scale,
                    priv->curves_bounds.origin.y, scale);
      gtk_snapshot_pop (snapshot);
    }
}

static void
gsm_graph_class_init (GsmGraphClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  widget_class->css_changed = gsm_graph_css_changed;
  widget_class->snapshot = gsm_graph_snapshot;

  object_class->set_property = gsm_graph_set_property;
  object_class->get_property = gsm_graph_get_property;

  object_class->dispose = gsm_graph_dispose;
  object_class->finalize = gsm_graph_finalize;

  /**
   * GsmGraph::css-changed:
   * @self:
   */
  signals[CSS_CHANGED] = g_signal_new ("css-changed",
                                       G_TYPE_FROM_CLASS (object_class),
                                       G_SIGNAL_RUN_LAST,
                                       G_STRUCT_OFFSET (GsmGraphClass, css_changed),
                                       NULL, NULL,
                                       NULL,
                                       G_TYPE_NONE,
                                       0);
  obj_properties[PROP_LOGARITHMIC_SCALE] =
                        g_param_spec_boolean ("logarithmic-scale", NULL, NULL, FALSE, G_PARAM_READWRITE);
  obj_properties[PROP_FONT_SIZE] =
                        g_param_spec_double ("font-size", NULL, NULL, 8.0, 48.0, 8.0, G_PARAM_READWRITE);
  obj_properties[PROP_NUM_POINTS] =
                        g_param_spec_uint ("num-points", NULL, NULL, 10, 600, 60, G_PARAM_READWRITE);
  obj_properties[PROP_SMOOTH_CHART] =
                        g_param_spec_boolean ("smooth-chart", NULL, NULL, TRUE, G_PARAM_READWRITE);
  obj_properties[PROP_STACKED_CHART] =
                        g_param_spec_boolean ("stacked-chart", NULL, NULL, FALSE, G_PARAM_READWRITE);
  obj_properties[PROP_MAX_VALUE] =
                        g_param_spec_uint64 ("max-value", NULL, NULL, 0, G_MAXUINT64, 100, G_PARAM_READWRITE);

  g_object_class_install_properties (object_class,
                                     G_N_ELEMENTS (obj_properties),
                                     obj_properties);

}

void
gsm_graph_dispose (GObject *self)
{
  GsmGraphPrivate *priv = gsm_graph_get_instance_private (GSM_GRAPH (self));

  gsm_graph_clear_background (GSM_GRAPH (self));
  g_clear_handle_id (&priv->sample_timeout, g_source_remove);
  if (priv->tick_id)
    {
      gtk_widget_remove_tick_callback (GTK_WIDGET (self), priv->tick_id);
      priv->tick_id = 0;
    }

  G_OBJECT_CLASS (gsm_graph_parent_class)->dispose (G_OBJECT (self));
}

void
gsm_graph_finalize (GObject *self)
{
  G_OBJECT_CLASS (gsm_graph_parent_class)->finalize (G_OBJECT (self));
}

static void
_gsm_graph_state_flags_changed (GtkWidget *widget,
                                GtkStateFlags*,
                                gpointer   self)
{
  GsmGraph *graph = GSM_GRAPH (self);

  gsm_graph_force_refresh (graph);
  if (gtk_widget_is_visible (widget))
    gsm_graph_start (graph);
  else
    gsm_graph_stop (graph);
}

static gboolean
_gsm_graph_sample (GsmGraph *self)
{
  GsmGraphPrivate *priv = gsm_graph_get_instance_private (self);

  priv->data_function (priv->update_data);
  priv->data_generation++;
  priv->last_sample_time = g_get_monotonic_time ();

  if (gsm_graph_is_started (self))
    gtk_widget_queue_draw (GTK_WIDGET (self));

  return G_SOURCE_CONTINUE;
}

static void
_gsm_graph_restart_sampling (GsmGraph *self)
{
  GsmGraphPrivate *priv = gsm_graph_get_instance_private (self);

  g_clear_handle_id (&priv->sample_timeout, g_source_remove);
  priv->sample_timeout = g_timeout_add (gsm_graph_get_sample_period (self),
                                        (GSourceFunc) _gsm_graph_sample,
                                        self);
}

/* Runs once per display frame while the graph is shown. The picture only
 * changes when the scroll offset moves by a whole device pixel, so the redraw
 * rate follows the scrolling speed (~20/s with default settings), and the
 * time quantization in gsm_graph_get_scroll_phase() caps it at
 * GSM_GRAPH_MAX_FPS. */
static gboolean
_gsm_graph_tick (GtkWidget     *widget,
                 GdkFrameClock *,
                 gpointer)
{
  GsmGraph *self = GSM_GRAPH (widget);
  GsmGraphPrivate *priv = gsm_graph_get_instance_private (self);
  double scale = gsm_graph_get_surface_scale (widget);
  double shift = round (priv->x_step * gsm_graph_get_scroll_phase (self) * scale);

  if (shift != priv->drawn_shift)
    gtk_widget_queue_draw (widget);

  return G_SOURCE_CONTINUE;
}

static void
gsm_graph_init (GsmGraph *self)
{
  GsmGraphPrivate *priv = gsm_graph_get_instance_private (self);
  priv->draw = FALSE;
  priv->logarithmic_scale = FALSE;
  
  // FIXME:
  // on configure, frames_per_unit = graph->draw_width/num_points;
  // knock FRAMES down to 10 until cairo gets faster
  priv->frames_per_unit = 10;
  priv->fontsize = 8.0;
  priv->num_points = 60;
  priv->indent = 18;
  priv->rmargin = 6 * priv->fontsize;
  priv->smooth = TRUE;
  priv->stacked = FALSE;
  priv->max_value = 100;

  g_signal_connect (G_OBJECT (self), "resize",
                    G_CALLBACK (gsm_graph_force_refresh), self);
  g_signal_connect (G_OBJECT (self), "css-changed",
                    G_CALLBACK (gsm_graph_force_refresh), self);
  g_signal_connect (G_OBJECT (self), "state-flags-changed",
                    G_CALLBACK (_gsm_graph_state_flags_changed), self);
}

GsmGraph *
gsm_graph_new (void)
{
  return g_object_new (GSM_TYPE_GRAPH, NULL);
}

void _gsm_graph_set_draw (GsmGraph *self, gboolean draw)
{
  g_return_if_fail (GSM_IS_GRAPH (self));
  GsmGraphPrivate *priv = gsm_graph_get_instance_private (self);

  if (priv->draw != draw)
    priv->draw = draw;
}

void gsm_graph_set_font_size (GsmGraph *self, double fontsize)
{
  g_return_if_fail (GSM_IS_GRAPH (self));
  GsmGraphPrivate *priv = gsm_graph_get_instance_private (self);

  if (priv->fontsize != fontsize)
  {
    priv->fontsize = fontsize;
    priv->rmargin = 6 * fontsize;
    gsm_graph_force_refresh (self);
  }
}

void gsm_graph_set_num_points (GsmGraph *self, guint num_points)
{
  g_return_if_fail (GSM_IS_GRAPH (self));
  GsmGraphPrivate *priv = gsm_graph_get_instance_private (self);

  if (priv->num_points != num_points)
  {
    priv->num_points = num_points;
    gsm_graph_force_refresh (self);
  }
}

void gsm_graph_set_max_value (GsmGraph *self, guint64 max_value)
{
  g_return_if_fail (GSM_IS_GRAPH (self));
  GsmGraphPrivate *priv = gsm_graph_get_instance_private (self);

  if (priv->max_value != max_value)
  {
    priv->max_value = max_value;
    gsm_graph_force_refresh (self);
  }
}

void gsm_graph_set_speed (GsmGraph *self, guint speed)
{
  g_return_if_fail (GSM_IS_GRAPH (self));
  GsmGraphPrivate *priv = gsm_graph_get_instance_private (self);

  if (priv->speed != speed) {
    priv->speed = speed;
    if (priv->sample_timeout) {
      _gsm_graph_restart_sampling (self);
      gsm_graph_clear_background (self);
    }
  }
}

void gsm_graph_set_data_function (GsmGraph *self, GSourceFunc function, gpointer data)
{
  g_return_if_fail (GSM_IS_GRAPH (self));
  GsmGraphPrivate *priv = gsm_graph_get_instance_private (self);

  g_clear_handle_id (&priv->sample_timeout, g_source_remove);

  priv->data_function = function;
  priv->update_data = data;

  if (gsm_graph_is_started (self))
    _gsm_graph_restart_sampling (self);
}

void
gsm_graph_start (GsmGraph *self)
{
  _gsm_graph_set_draw (self, TRUE);
  GsmGraphPrivate *priv = gsm_graph_get_instance_private (self);
  g_return_if_fail ( priv->data_function != NULL);

  if (priv->sample_timeout == 0)
    {
      // Update the data two times so the graph
      // doesn't wait one cycle to start drawing.
      priv->data_function (priv->update_data);
      _gsm_graph_sample (self);
      _gsm_graph_restart_sampling (self);
    }

  if (priv->tick_id == 0)
    priv->tick_id = gtk_widget_add_tick_callback (GTK_WIDGET (self), _gsm_graph_tick, NULL, NULL);
}

void
gsm_graph_force_refresh (GsmGraph *self)
{
  gsm_graph_clear_background (self);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
gsm_graph_clear_background (GsmGraph *self)
{
  g_return_if_fail (GSM_IS_GRAPH (self));
  GsmGraphPrivate *priv = gsm_graph_get_instance_private (self);

  g_clear_pointer (&priv->background, cairo_surface_destroy);
  g_clear_pointer (&priv->curves, cairo_surface_destroy);
}

void
gsm_graph_set_render_functions (GsmGraph          *self,
                                GsmGraphLayoutFunc layout_func,
                                GsmGraphPaintFunc  paint_func,
                                gpointer           user_data)
{
  g_return_if_fail (GSM_IS_GRAPH (self));
  GsmGraphPrivate *priv = gsm_graph_get_instance_private (self);

  priv->layout_func = layout_func;
  priv->paint_func = paint_func;
  priv->paint_data = user_data;
  gsm_graph_force_refresh (self);
}

void
gsm_graph_set_logarithmic_scale (GsmGraph *self, gboolean logarithmic)
{
  g_return_if_fail (GSM_IS_GRAPH (self));
  GsmGraphPrivate *priv = gsm_graph_get_instance_private (self);
  if (priv->logarithmic_scale != logarithmic) {
    priv->logarithmic_scale = logarithmic;
  }
}

void
gsm_graph_set_smooth_chart (GsmGraph *self, gboolean smooth)
{
  g_return_if_fail (GSM_IS_GRAPH (self));
  GsmGraphPrivate *priv = gsm_graph_get_instance_private (self);
  if (priv->smooth != smooth) {
    priv->smooth = smooth;
  }
}

void
gsm_graph_set_stacked_chart (GsmGraph *self, gboolean stacked)
{
  g_return_if_fail (GSM_IS_GRAPH (self));
  GsmGraphPrivate *priv = gsm_graph_get_instance_private (self);
  if (priv->stacked != stacked) {
    priv->stacked = stacked;
  }
}


void
gsm_graph_stop (GsmGraph *self)
{
  /* stop drawing, the data function keeps being polled */
  GsmGraphPrivate *priv = gsm_graph_get_instance_private (self);

  _gsm_graph_set_draw (self, FALSE);
  if (priv->tick_id)
    {
      gtk_widget_remove_tick_callback (GTK_WIDGET (self), priv->tick_id);
      priv->tick_id = 0;
    }
}

gboolean
gsm_graph_is_started (GsmGraph *self)
{   
  g_return_val_if_fail (GSM_IS_GRAPH (self), FALSE);
  GsmGraphPrivate *priv = gsm_graph_get_instance_private (self);

  return priv->draw;
}

guint
gsm_graph_get_speed (GsmGraph *self)
{   
  g_return_val_if_fail (GSM_IS_GRAPH (self), 0);
  GsmGraphPrivate *priv = gsm_graph_get_instance_private (self);

  return priv->speed;
}

guint
gsm_graph_get_frames_per_unit (GsmGraph *self)
{   
  g_return_val_if_fail (GSM_IS_GRAPH (self), 0);
  GsmGraphPrivate *priv = gsm_graph_get_instance_private (self);

  return priv->frames_per_unit;
}

double
gsm_graph_get_font_size (GsmGraph *self)
{
  g_return_val_if_fail (GSM_IS_GRAPH (self), 0);
  GsmGraphPrivate *priv = gsm_graph_get_instance_private (self);

  return priv->fontsize;
}

guint
gsm_graph_get_num_points (GsmGraph *self)
{
  g_return_val_if_fail (GSM_IS_GRAPH (self), 0);
  GsmGraphPrivate *priv = gsm_graph_get_instance_private (self);

  return priv->num_points;
}

guint64
gsm_graph_get_max_value (GsmGraph *self)
{
  g_return_val_if_fail (GSM_IS_GRAPH (self), 0);
  GsmGraphPrivate *priv = gsm_graph_get_instance_private (self);

  return priv->max_value;
}

guint
gsm_graph_get_indent (GsmGraph *self)
{
  g_return_val_if_fail (GSM_IS_GRAPH (self), 0);
  GsmGraphPrivate *priv = gsm_graph_get_instance_private (self);

  return priv->indent;
}

double
gsm_graph_get_right_margin (GsmGraph *self)
{
  g_return_val_if_fail (GSM_IS_GRAPH (self), 0);
  GsmGraphPrivate *priv = gsm_graph_get_instance_private (self);

  return priv->rmargin;
}

gboolean
gsm_graph_is_logarithmic_scale (GsmGraph *self)
{
  g_return_val_if_fail (GSM_IS_GRAPH (self), FALSE);
  GsmGraphPrivate *priv = gsm_graph_get_instance_private (self);

  return priv->logarithmic_scale;
}

gboolean
gsm_graph_is_smooth_chart (GsmGraph *self)
{
  g_return_val_if_fail (GSM_IS_GRAPH (self), FALSE);
  GsmGraphPrivate *priv = gsm_graph_get_instance_private (self);

  return priv->smooth;
}

gboolean
gsm_graph_is_stacked_chart (GsmGraph *self)
{
  g_return_val_if_fail (GSM_IS_GRAPH (self), FALSE);
  GsmGraphPrivate *priv = gsm_graph_get_instance_private (self);

  return priv->stacked;
}

guint
gsm_graph_get_num_bars (GsmGraph *self, int height)
{
  guint n;

  g_return_val_if_fail (GSM_IS_GRAPH (self), 0);
  GsmGraphPrivate *priv = gsm_graph_get_instance_private (self);

  // keep 100 % num_bars == 0
  switch ((int)(height / (priv->fontsize + 14)))
    {
      case 0:
      case 1:
        n = 1;
        break;

      case 2:
      case 3:
        n = 2;
        break;

      case 4:
        n = 4;
        break;

      case 5:
        n = 5;
        if (priv->logarithmic_scale)
          n = 4;
        break;

      default:
        n = 5;
        if (priv->logarithmic_scale)
          n = 6;
    }

  return n;
}
