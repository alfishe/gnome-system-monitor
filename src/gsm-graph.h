#ifndef _GSM_GRAPH_H_
#define _GSM_GRAPH_H_
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

#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GSM_TYPE_GRAPH  (gsm_graph_get_type ())
#define GSM_GRAPH(obj)  (G_TYPE_CHECK_INSTANCE_CAST ((obj), GSM_TYPE_GRAPH, GsmGraph))
#define GSM_IS_GRAPH(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), GSM_TYPE_GRAPH))

typedef struct _GsmGraph GsmGraph;
typedef struct _GsmGraphClass GsmGraphClass;
typedef struct _GsmGraphPrivate GsmGraphPrivate;

/* The graph is rendered as two cached layers:
 *  - BACKGROUND: frame, grid and labels; covers the whole widget and is only
 *    re-rendered when the size, scale or a display setting changes.
 *  - CURVES: the data lines; re-rendered once per new data sample.
 * Between samples the cached CURVES layer is only blitted at a whole-pixel
 * offset (scrolled), so animation frames don't rasterize anything. */
typedef enum
{
  GSM_GRAPH_LAYER_BACKGROUND,
  GSM_GRAPH_LAYER_CURVES,
} GsmGraphLayer;

typedef struct
{
  /* area the curves are visible in, in widget coordinates */
  graphene_rect_t plot;
  /* horizontal distance between two consecutive samples */
  double x_step;
} GsmGraphLayout;

typedef void (*GsmGraphLayoutFunc) (GsmGraph       *graph,
                                    int             width,
                                    int             height,
                                    GsmGraphLayout *layout,
                                    gpointer        user_data);

/* Paints @layer for a widget of @width x @height into @cr (widget coordinates).
 * The CURVES layer must be painted at scroll phase 0, i.e. with the newest
 * sample one x_step to the right of the plot area. */
typedef void (*GsmGraphPaintFunc) (GsmGraph             *graph,
                                   GsmGraphLayer         layer,
                                   cairo_t              *cr,
                                   int                   width,
                                   int                   height,
                                   const GsmGraphLayout *layout,
                                   gpointer              user_data);

struct _GsmGraph
{
  GtkDrawingArea parent_instance;
};

struct _GsmGraphPrivate
{
  gboolean draw;
  
  // internal usage for animations
  
  // a new data sample is taken every speed * frames_per_unit ms
  guint frames_per_unit;
  guint sample_timeout;
  // frame clock tick driving the smooth scrolling
  guint tick_id;
  // monotonic time (us) of the latest sample
  gint64 last_sample_time;
  // scroll offset (device pixels) of the last queued frame
  double drawn_shift;
  // x_step of the last layout, used by the tick callback
  double x_step;

  // used for displaying, configurable

  // font size for labels
  double fontsize;
  // refresh timeout
  guint speed;
  // draw y axis with logarithmic scale instead of default linear
  gboolean logarithmic_scale;
  // number of chart data points
  guint num_points;
  // draw stacked chart
  gboolean stacked;
  // draw smooth chart
  gboolean smooth;
  // maximum value for y axis
  guint64 max_value;

  // used for displaying, internal

  // right margin
  double rmargin;
  guint indent;

  // render callbacks
  GsmGraphLayoutFunc layout_func;
  GsmGraphPaintFunc paint_func;
  gpointer paint_data;

  // cached layers
  cairo_surface_t *background;
  cairo_surface_t *curves;
  graphene_rect_t curves_bounds;
  guint64 data_generation;
  guint64 curves_generation;
  int cache_width;
  int cache_height;
  double cache_scale;
  
  // function to calculate the latest set of data to display
  GSourceFunc data_function;
  // the data to be passed to the data_function
  gpointer update_data;
};

struct _GsmGraphClass
{
  GtkDrawingAreaClass parent_class;

  void (* css_changed) (GsmGraph *graph);
};

GType               gsm_graph_get_type (void);
GsmGraph *          gsm_graph_new (void);

void                gsm_graph_dispose (GObject*);
void                gsm_graph_finalize (GObject*);

void                gsm_graph_start (GsmGraph*);
void                gsm_graph_stop (GsmGraph*);
void                gsm_graph_force_refresh (GsmGraph*);

// public setter methods
void                gsm_graph_set_logarithmic_scale (GsmGraph*, gboolean);
void                gsm_graph_set_smooth_chart (GsmGraph*, gboolean);
void                gsm_graph_set_stacked_chart (GsmGraph*, gboolean);
void                gsm_graph_set_speed (GsmGraph*, guint);
void                gsm_graph_set_data_function (GsmGraph*, GSourceFunc, gpointer);
void                gsm_graph_set_render_functions (GsmGraph*, GsmGraphLayoutFunc, GsmGraphPaintFunc, gpointer);
void                gsm_graph_set_font_size (GsmGraph*, double);
void                gsm_graph_set_num_points (GsmGraph*, guint);
void                gsm_graph_set_max_value (GsmGraph*, guint64);

// drop the cached layers so they are re-rendered on the next frame
void                gsm_graph_clear_background (GsmGraph*);

// public getter methods
gboolean            gsm_graph_is_logarithmic_scale (GsmGraph*);
guint               gsm_graph_get_speed (GsmGraph*);
double              gsm_graph_get_font_size (GsmGraph*);
gboolean            gsm_graph_is_started (GsmGraph*);
gboolean            gsm_graph_is_smooth_chart (GsmGraph*);
gboolean            gsm_graph_is_stacked_chart (GsmGraph*);
guint               gsm_graph_get_num_points (GsmGraph*);
guint64             gsm_graph_get_max_value (GsmGraph*);

// temporary exported methods for the migration
guint               gsm_graph_get_frames_per_unit (GsmGraph*);
guint               gsm_graph_get_indent (GsmGraph*);
double              gsm_graph_get_right_margin (GsmGraph*);
guint               gsm_graph_get_num_bars (GsmGraph*, gint);


G_END_DECLS

#endif /* _GSM_GRAPH_H_ */
