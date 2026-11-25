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

/* A cairo node showing the part of @surface (placed at @origin) inside @rect.
 * Drawing it is a single blit, unfiltered as long as it lands on device
 * pixels. */
static GskRenderNode *
node_for_surface (cairo_surface_t        *surface,
                  const graphene_point_t *origin,
                  const graphene_rect_t  *rect)
{
  GskRenderNode *node = gsk_cairo_node_new (rect);
  cairo_t *cr = gsk_cairo_node_get_draw_context (node);

  cairo_set_source_surface (cr, surface, origin->x, origin->y);
  cairo_paint (cr);
  cairo_destroy (cr);

  return node;
}

static GskRenderNode *
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

  graphene_rect_t rect = GRAPHENE_RECT_INIT (bounds->origin.x, bounds->origin.y,
                                             pixel_width / scale, pixel_height / scale);
  GskRenderNode *node = node_for_surface (surface, &rect.origin, &rect);
  cairo_surface_destroy (surface);

  return node;
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

/* Rate of the scrolling animation */
#define GSM_GRAPH_MAX_FPS 30

/* One animation clock shared by all shown graphs, so they all move in the
 * same frames and the window repaints at most GSM_GRAPH_MAX_FPS times per
 * second; between steps nothing runs. */
static GList *animated_graphs;
static guint animation_source;
static gint64 animation_time;

static void gsm_graph_set_animated (GsmGraph *self, gboolean animated);
static void gsm_graph_set_sampling (GsmGraph *self, gboolean sampling);

/* Fraction [0, 1] of the way from the latest sample to the next one at the
 * current animation step, so the scrolling speed is independent of the frame
 * rate and of timer jitter. */
static double
gsm_graph_get_scroll_phase (GsmGraph *self)
{
  GsmGraphPrivate *priv = gsm_graph_get_instance_private (self);
  gint64 now = animation_time ? animation_time : g_get_monotonic_time ();
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
      g_clear_pointer (&priv->curves, gsk_render_node_unref);
      priv->curves_bounds = GRAPHENE_RECT_INIT (floor (layout.plot.origin.x * scale) / scale,
                                                floor (layout.plot.origin.y * scale) / scale,
                                                layout.plot.size.width + layout.x_step + 2,
                                                layout.plot.size.height + 1);
      priv->curves = gsm_graph_render_layer (self, GSM_GRAPH_LAYER_CURVES,
                                             &priv->curves_bounds, scale, width, height, &layout);
      priv->curves_generation = priv->data_generation;
    }

  if (priv->background)
    gtk_snapshot_append_node (snapshot, priv->background);

  if (priv->curves)
    {
      /* Smooth scrolling between two samples, in whole device pixels */
      double shift = round (layout.x_step * gsm_graph_get_scroll_phase (self) * scale);

      priv->x_step = layout.x_step;
      priv->drawn_shift = shift;

      gtk_snapshot_push_clip (snapshot, &layout.plot);
      gtk_snapshot_save (snapshot);
      gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (-shift / scale, 0));
      gtk_snapshot_append_node (snapshot, priv->curves);
      gtk_snapshot_restore (snapshot);
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
  gsm_graph_clear_background (GSM_GRAPH (self));
  gsm_graph_set_sampling (GSM_GRAPH (self), FALSE);
  gsm_graph_set_animated (GSM_GRAPH (self), FALSE);

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

static void
_gsm_graph_sample (GsmGraph *self,
                   gint64    now)
{
  GsmGraphPrivate *priv = gsm_graph_get_instance_private (self);

  priv->data_function (priv->update_data);
  priv->data_generation++;
  priv->last_sample_time = now;

  if (gsm_graph_is_started (self))
    gtk_widget_queue_draw (GTK_WIDGET (self));
}

/* One sampling clock shared by all graphs: they all get new data with the
 * same timestamp, so their scroll offsets advance in lockstep and the window
 * repaints once per pixel of movement instead of once per graph. */
static GList *sampled_graphs;
static guint sampling_source;
static guint sampling_period;

static gboolean
gsm_graph_sample_all (gpointer)
{
  gint64 now = g_get_monotonic_time ();

  for (GList *l = sampled_graphs; l != NULL; l = l->next)
    _gsm_graph_sample (GSM_GRAPH (l->data), now);

  return G_SOURCE_CONTINUE;
}

static void
gsm_graph_set_sampling (GsmGraph *self,
                        gboolean  sampling)
{
  GsmGraphPrivate *priv = gsm_graph_get_instance_private (self);

  if (sampling && !priv->sampling)
    sampled_graphs = g_list_prepend (sampled_graphs, self);
  else if (!sampling && priv->sampling)
    sampled_graphs = g_list_remove (sampled_graphs, self);
  priv->sampling = sampling;

  guint period = sampled_graphs ? gsm_graph_get_sample_period (GSM_GRAPH (sampled_graphs->data)) : 0;

  if (period != sampling_period || (period != 0 && sampling_source == 0))
    {
      g_clear_handle_id (&sampling_source, g_source_remove);
      sampling_period = period;
      if (period != 0)
        sampling_source = g_timeout_add (period, gsm_graph_sample_all, NULL);
    }
}

/* One animation step. A graph is only redrawn when its scroll offset moved
 * by a whole device pixel, so the redraw rate follows the scrolling speed
 * (~20/s with default settings), at most GSM_GRAPH_MAX_FPS. */
static gboolean
gsm_graph_animation_step (gpointer)
{
  animation_time = g_get_monotonic_time ();

  for (GList *l = animated_graphs; l != NULL; l = l->next)
    {
      GsmGraph *self = GSM_GRAPH (l->data);
      GsmGraphPrivate *priv = gsm_graph_get_instance_private (self);
      double scale = gsm_graph_get_surface_scale (GTK_WIDGET (self));
      double shift = round (priv->x_step * gsm_graph_get_scroll_phase (self) * scale);

      if (shift != priv->drawn_shift)
        gtk_widget_queue_draw (GTK_WIDGET (self));
    }

  return G_SOURCE_CONTINUE;
}

static void
gsm_graph_set_animated (GsmGraph *self,
                        gboolean  animated)
{
  GList *link = g_list_find (animated_graphs, self);

  if (animated && link == NULL)
    animated_graphs = g_list_prepend (animated_graphs, self);
  else if (!animated && link != NULL)
    animated_graphs = g_list_delete_link (animated_graphs, link);

  if (animated_graphs != NULL && animation_source == 0)
    animation_source = g_timeout_add (1000 / GSM_GRAPH_MAX_FPS, gsm_graph_animation_step, NULL);
  else if (animated_graphs == NULL)
    {
      g_clear_handle_id (&animation_source, g_source_remove);
      animation_time = 0;
    }
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
    if (priv->sampling) {
      /* restarts the shared clock with the new period */
      gsm_graph_set_sampling (self, FALSE);
      gsm_graph_set_sampling (self, TRUE);
      gsm_graph_clear_background (self);
    }
  }
}

void gsm_graph_set_data_function (GsmGraph *self, GSourceFunc function, gpointer data)
{
  g_return_if_fail (GSM_IS_GRAPH (self));
  GsmGraphPrivate *priv = gsm_graph_get_instance_private (self);

  priv->data_function = function;
  priv->update_data = data;
}

void
gsm_graph_start (GsmGraph *self)
{
  _gsm_graph_set_draw (self, TRUE);
  GsmGraphPrivate *priv = gsm_graph_get_instance_private (self);
  g_return_if_fail ( priv->data_function != NULL);

  if (!priv->sampling)
    {
      // Update the data two times so the graph
      // doesn't wait one cycle to start drawing.
      priv->data_function (priv->update_data);
      _gsm_graph_sample (self, g_get_monotonic_time ());
      gsm_graph_set_sampling (self, TRUE);
    }

  gsm_graph_set_animated (self, TRUE);
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

  g_clear_pointer (&priv->background, gsk_render_node_unref);
  g_clear_pointer (&priv->curves, gsk_render_node_unref);
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
  _gsm_graph_set_draw (self, FALSE);
  gsm_graph_set_animated (self, FALSE);
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
