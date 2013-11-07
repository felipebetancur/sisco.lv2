/* simple scope -- example pipe raw audio data to UI
 *
 * Copyright (C) 2013 Robin Gareus <robin@gareus.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>

#include <gtk/gtk.h>
#include <cairo.h>

#include "lv2/lv2plug.in/ns/extensions/ui/ui.h"
#include "./uris.h"

#define DAWIDTH  (640)
#define DAHEIGHT (200)
#define MAX_CAIRO_PATH (128)

/* note: a cairo-pixel at 0 spans -.5 .. +.5, hence (DAHEIGHT / 2.0 -.5)
 * also the cairo Y-axis points upwards
 */
#define CYPOS(CHN, GAIN, VAL) (DAHEIGHT * (CHN) + 99.5f - (VAL) * 100.0f * (GAIN))

typedef struct {
  float data_min[DAWIDTH];
  float data_max[DAWIDTH];

  uint32_t idx;
  uint32_t sub;
  pthread_mutex_t lock;
} ScoChan;

typedef struct {
  LV2_Atom_Forge forge;
  LV2_URID_Map*  map;
  ScoLV2URIs     uris;

  LV2UI_Write_Function write;
  LV2UI_Controller     controller;


  GtkWidget *hbox, *vbox;
  GtkWidget *sep[2];
  GtkWidget *darea;

  GtkWidget *btn_pause;
  GtkWidget *lbl_speed, *lbl_amp;
  GtkWidget *spb_speed, *spb_amp;
  GtkAdjustment *spb_speed_adj, *spb_amp_adj;

  ScoChan  chn[2];
  uint32_t stride;
  uint32_t n_channels;
  bool     paused;
  float    rate;
} SiScoUI;


/* send current settings to backend */
static void ui_state(LV2UI_Handle handle)
{
  SiScoUI* ui = (SiScoUI*)handle;
  const float gain = gtk_spin_button_get_value(GTK_SPIN_BUTTON(ui->spb_amp));

  uint8_t obj_buf[1024];
  lv2_atom_forge_set_buffer(&ui->forge, obj_buf, 1024);
  LV2_Atom_Forge_Frame frame;
  lv2_atom_forge_frame_time(&ui->forge, 0);
  LV2_Atom* msg = (LV2_Atom*)lv2_atom_forge_blank(&ui->forge, &frame, 1, ui->uris.ui_state);
  lv2_atom_forge_property_head(&ui->forge, ui->uris.ui_spp, 0); lv2_atom_forge_int(&ui->forge, ui->stride);
  lv2_atom_forge_property_head(&ui->forge, ui->uris.ui_amp, 0); lv2_atom_forge_float(&ui->forge, gain);
  lv2_atom_forge_pop(&ui->forge, &frame);
  ui->write(ui->controller, 0, lv2_atom_total_size(msg), ui->uris.atom_eventTransfer, msg);
}

/* notfiy backend that UI is closed */
static void ui_disable(LV2UI_Handle handle)
{
  SiScoUI* ui = (SiScoUI*)handle;
  ui_state(handle);

  uint8_t obj_buf[64];
  lv2_atom_forge_set_buffer(&ui->forge, obj_buf, 64);
  LV2_Atom_Forge_Frame frame;
  lv2_atom_forge_frame_time(&ui->forge, 0);
  LV2_Atom* msg = (LV2_Atom*)lv2_atom_forge_blank(&ui->forge, &frame, 1, ui->uris.ui_off);
  lv2_atom_forge_pop(&ui->forge, &frame);
  ui->write(ui->controller, 0, lv2_atom_total_size(msg), ui->uris.atom_eventTransfer, msg);
}

/* notfiy backend that UI is active,
 * request state and enable data-transmission */
static void ui_enable(LV2UI_Handle handle)
{
  SiScoUI* ui = (SiScoUI*)handle;
  uint8_t obj_buf[64];
  lv2_atom_forge_set_buffer(&ui->forge, obj_buf, 64);
  LV2_Atom_Forge_Frame frame;
  lv2_atom_forge_frame_time(&ui->forge, 0);
  LV2_Atom* msg = (LV2_Atom*)lv2_atom_forge_blank(&ui->forge, &frame, 1, ui->uris.ui_on);
  lv2_atom_forge_pop(&ui->forge, &frame);
  ui->write(ui->controller, 0, lv2_atom_total_size(msg), ui->uris.atom_eventTransfer, msg);
}


gboolean cfg_changed (GtkWidget *widget, gpointer data)
{
  ui_state(data);
  return TRUE;
}


gboolean expose_event_callback (GtkWidget *widget, GdkEventExpose *ev, gpointer data)
{
  /* this runs in gtk's main thread
   * TODO: read from ringbuffer or blit cairo surface instead of [b]locking
   */
  SiScoUI* ui = (SiScoUI*) data;
  const float gain = gtk_spin_button_get_value(GTK_SPIN_BUTTON(ui->spb_amp));

  cairo_t *cr;
  cr = gdk_cairo_create(ui->darea->window);

  cairo_rectangle (cr, ev->area.x, ev->area.y, ev->area.width, ev->area.height);
  cairo_clip(cr);

  cairo_set_source_rgba (cr, .0, .0, .0, 1.0);
  cairo_rectangle(cr, 0, 0, DAWIDTH, DAHEIGHT * ui->n_channels);
  cairo_fill(cr);

  cairo_set_line_width(cr, 1.0);

  const uint32_t start = ev->area.x;
  const uint32_t end = ev->area.x + ev->area.width;

  assert(start >= 0);
  assert(start < DAWIDTH);
  assert(end >= 0);
  assert(end <= DAWIDTH);
  assert(start < end);

  for(uint32_t c = 0 ; c < ui->n_channels; ++c) {
    ScoChan *chn = &ui->chn[c];

    cairo_save(cr);
    cairo_rectangle (cr, 0, DAHEIGHT * c, DAWIDTH, DAHEIGHT);
    cairo_clip(cr);
    cairo_set_source_rgba (cr, .0, 1.0, .0, 1.0);

    pthread_mutex_lock(&chn->lock);

    if (start == chn->idx) {
      cairo_move_to(cr, start - .5, CYPOS(c, gain, 0));
    } else {
      cairo_move_to(cr, start - .5, CYPOS(c, gain, chn->data_max[start]));
    }

    uint32_t pathlength = 0;
    for (uint32_t i = start ; i < end; ++i) {
      if (i == chn->idx) {
	continue;
      } else if (i%2) {
	cairo_line_to(cr, i - .5, CYPOS(c, gain, chn->data_min[i]));
	cairo_line_to(cr, i - .5, CYPOS(c, gain, chn->data_max[i]));
	++pathlength;
      } else {
	cairo_line_to(cr, i - .5, CYPOS(c, gain, chn->data_max[i]));
	cairo_line_to(cr, i - .5, CYPOS(c, gain, chn->data_min[i]));
	++pathlength;
      }

      if (pathlength > MAX_CAIRO_PATH) {
	pathlength = 0;
	cairo_stroke (cr);
	if (i%2) {
	  cairo_move_to(cr, i - .5, CYPOS(c, gain, chn->data_max[i]));
	} else {
	  cairo_move_to(cr, i - .5, CYPOS(c, gain, chn->data_min[i]));
	}
      }
    }
    if (pathlength > 0) {
      cairo_stroke (cr);
    }

    /* current position */
    if (ui->stride >= ui->rate / 4800.0f || ui->paused) {
      cairo_set_source_rgba (cr, .9, .2, .2, .6);
      cairo_move_to(cr, chn->idx - .5, DAHEIGHT * c);
      cairo_line_to(cr, chn->idx - .5, DAHEIGHT * (c+1));
      cairo_stroke (cr);
    }
    cairo_restore(cr);
    pthread_mutex_unlock(&chn->lock);

    /* channel separator */
    if (c > 0) {
      cairo_set_source_rgba (cr, .5, .5, .5, 1.0);
      cairo_move_to(cr, 0, DAHEIGHT * c - .5);
      cairo_line_to(cr, DAWIDTH, DAHEIGHT * c - .5);
      cairo_stroke (cr);
    }

    /* zero line */
    cairo_set_source_rgba (cr, .3, .3, .7, .5);
    cairo_move_to(cr, 0, DAHEIGHT * (c + .5) - .5);
    cairo_line_to(cr, DAWIDTH, DAHEIGHT * (c + .5) - .5);
    cairo_stroke (cr);
  }

  cairo_destroy (cr);
  return TRUE;
}

static void update_scope(SiScoUI* ui, const int channel, const size_t n_elem, float const *data)
{
  /* this callback runs in the "communication" thread of the LV2-host
   * usually a g_timeout() at ~25fps
   */
  if (channel > ui->n_channels || channel < 0) {
    return;
  }
  /* update state in sync with 1st channel */
  if (channel == 0) {
    ui->stride = gtk_spin_button_get_value(GTK_SPIN_BUTTON(ui->spb_speed));
    bool paused = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ui->btn_pause));
    if (paused != ui->paused) {
      ui->paused = paused;
      gtk_widget_queue_draw(ui->darea);
    }
  }
  if (ui->paused) {
    return;
  }

  ScoChan *chn = &ui->chn[channel];

  /* TODO: process/filter data depending on speed || trigger
   * TODO: write into ringbuffer OR draw a cairo-surface here
   * instead of locking data.
   */
  pthread_mutex_lock(&chn->lock);
  int overflow = 0;
  const uint32_t idx_start = chn->idx;
  for (int i = 0; i < n_elem; ++i) {
    if (data[i] < chn->data_min[chn->idx]) { chn->data_min[chn->idx] = data[i]; }
    if (data[i] > chn->data_max[chn->idx]) { chn->data_max[chn->idx] = data[i]; }
    if (chn->sub++ >= ui->stride) {
      chn->sub = 0;
      chn->idx = (chn->idx + 1) % DAWIDTH;
      chn->data_min[chn->idx] = 1.0;
      chn->data_max[chn->idx] = -1.0;
      if (chn->idx == 0) ++overflow;
    }
  }
  const uint32_t idx_end = chn->idx;
  pthread_mutex_unlock(&chn->lock);

  /* signal gtk's main thread to redraw the widget after the last channel */
  if (channel + 1 == ui->n_channels) {
    if (overflow > 1) {
      gtk_widget_queue_draw(ui->darea);
    } else if (idx_end > idx_start) {
      gtk_widget_queue_draw_area(ui->darea, idx_start - 2, 0, 3 + idx_end - idx_start, DAHEIGHT * ui->n_channels);
    } else if (idx_end < idx_start) {
      gtk_widget_queue_draw_area(ui->darea, idx_start - 2, 0, 3 + DAWIDTH - idx_start, DAHEIGHT * ui->n_channels);
      gtk_widget_queue_draw_area(ui->darea, 0, 0, idx_end + 1, DAHEIGHT * ui->n_channels);
    }
  }
}

static LV2UI_Handle
instantiate(const LV2UI_Descriptor*   descriptor,
            const char*               plugin_uri,
            const char*               bundle_path,
            LV2UI_Write_Function      write_function,
            LV2UI_Controller          controller,
            LV2UI_Widget*             widget,
            const LV2_Feature* const* features)
{
  SiScoUI* ui = (SiScoUI*)malloc(sizeof(SiScoUI));
  ui->map        = NULL;
  ui->write      = write_function;
  ui->controller = controller;
  ui->vbox       = NULL;
  ui->hbox       = NULL;
  ui->darea      = NULL;
  ui->stride     = 25;
  ui->paused     = false;
  ui->rate       = 48000;

  ui->chn[0].idx = 0;
  ui->chn[0].sub = 0;
  ui->chn[1].idx = 0;
  ui->chn[1].sub = 0;
  memset(ui->chn[0].data_min, 0, sizeof(float) * DAWIDTH);
  memset(ui->chn[0].data_max, 0, sizeof(float) * DAWIDTH);
  memset(ui->chn[1].data_min, 0, sizeof(float) * DAWIDTH);
  memset(ui->chn[1].data_max, 0, sizeof(float) * DAWIDTH);
  pthread_mutex_init(&ui->chn[0].lock, NULL);
  pthread_mutex_init(&ui->chn[1].lock, NULL);

  *widget = NULL;

  if (!strcmp(plugin_uri, SCO_URI "#Mono")) {
    ui->n_channels = 1;
  } else if (!strcmp(plugin_uri, SCO_URI "#Stereo")) {
    ui->n_channels = 2;
  } else {
    free(ui);
    return NULL;
  }

  for (int i = 0; features[i]; ++i) {
    if (!strcmp(features[i]->URI, LV2_URID_URI "#map")) {
      ui->map = (LV2_URID_Map*)features[i]->data;
    }
  }

  if (!ui->map) {
    fprintf(stderr, "SiSco.lv2 UI: Host does not support urid:map\n");
    free(ui);
    return NULL;
  }

  map_sco_uris(ui->map, &ui->uris);
  lv2_atom_forge_init(&ui->forge, ui->map);

  ui->hbox = gtk_hbox_new(FALSE, 0);
  ui->vbox = gtk_vbox_new(FALSE, 0);

  ui->darea = gtk_drawing_area_new();
  gtk_widget_set_size_request(ui->darea, DAWIDTH, DAHEIGHT * ui->n_channels);

  ui->lbl_speed = gtk_label_new("Samples/Pixel");
  ui->lbl_amp = gtk_label_new("Amplitude");

  ui->sep[0] = gtk_hseparator_new();
  ui->sep[1] = gtk_label_new("");
  ui->btn_pause = gtk_toggle_button_new_with_label("Pause");

  ui->spb_speed_adj = (GtkAdjustment *) gtk_adjustment_new(25.0, 1.0, 1000.0, 1.0, 5.0, 0.0);
  ui->spb_speed = gtk_spin_button_new(ui->spb_speed_adj, 1.0, 0);

  ui->spb_amp_adj = (GtkAdjustment *) gtk_adjustment_new(1.0, 0.1, 6.0, 0.1, 1.0, 0.0);
  ui->spb_amp = gtk_spin_button_new(ui->spb_amp_adj, 0.1, 1);

  gtk_box_pack_start(GTK_BOX(ui->hbox), ui->darea, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(ui->hbox), ui->vbox, FALSE, FALSE, 4);

  gtk_box_pack_start(GTK_BOX(ui->vbox), ui->lbl_speed, FALSE, FALSE, 2);
  gtk_box_pack_start(GTK_BOX(ui->vbox), ui->spb_speed, FALSE, FALSE, 2);
  gtk_box_pack_start(GTK_BOX(ui->vbox), ui->sep[0], FALSE, FALSE, 8);
  gtk_box_pack_start(GTK_BOX(ui->vbox), ui->lbl_amp, FALSE, FALSE, 2);
  gtk_box_pack_start(GTK_BOX(ui->vbox), ui->spb_amp, FALSE, FALSE, 2);
  gtk_box_pack_start(GTK_BOX(ui->vbox), ui->sep[1], TRUE, FALSE, 8);
  gtk_box_pack_start(GTK_BOX(ui->vbox), ui->btn_pause, FALSE, FALSE, 2);

  g_signal_connect(G_OBJECT(ui->darea), "expose_event", G_CALLBACK(expose_event_callback), ui);
  g_signal_connect(G_OBJECT(ui->spb_amp), "value-changed", G_CALLBACK(cfg_changed), ui);
  g_signal_connect(G_OBJECT(ui->spb_speed), "value-changed", G_CALLBACK(cfg_changed), ui);

  *widget = ui->hbox;
  ui_enable(ui);

  return ui;
}

static void
cleanup(LV2UI_Handle handle)
{
  SiScoUI* ui = (SiScoUI*)handle;
  ui_disable(ui);
  pthread_mutex_destroy(&ui->chn[0].lock);
  pthread_mutex_destroy(&ui->chn[1].lock);
  gtk_widget_destroy(ui->darea);
  free(ui);
}


static inline void parse_atom_vector(SiScoUI* ui, const int32_t channel, LV2_Atom *atom) {
  LV2_Atom_Vector* vof = (LV2_Atom_Vector*)LV2_ATOM_BODY(atom);
  if (vof->atom.type == ui->uris.atom_Float) {
    const size_t n_elem = (atom->size - sizeof(LV2_Atom_Vector_Body)) / vof->atom.size;
    const float *data = (float*) LV2_ATOM_BODY(&vof->atom);
    update_scope(ui, channel, n_elem, data);
  }
}

static void
port_event(LV2UI_Handle handle,
           uint32_t     port_index,
           uint32_t     buffer_size,
           uint32_t     format,
           const void*  buffer)
{
  SiScoUI* ui = (SiScoUI*)handle;
  LV2_Atom* atom = (LV2_Atom*)buffer;

  if (format == ui->uris.atom_eventTransfer
      && atom->type == ui->uris.atom_Blank
      )
  {
    LV2_Atom_Object* obj = (LV2_Atom_Object*)atom;
    LV2_Atom *a0 = NULL;
    LV2_Atom *a1 = NULL;
    LV2_Atom *a2 = NULL;
    if (obj->body.otype == ui->uris.rawaudio
	&& 2 == lv2_atom_object_get(obj, ui->uris.channelid, &a0, ui->uris.audiodata, &a1, NULL)
	&& a0
	&& a1
	&& a0->type == ui->uris.atom_Int
	&& a1->type == ui->uris.atom_Vector
	)
    {
      const int32_t chn = ((LV2_Atom_Int*)a0)->body;
      parse_atom_vector(ui, chn, a1);
    }
    else if (obj->body.otype == ui->uris.ui_state
	&& 3 == lv2_atom_object_get(obj,
	  ui->uris.ui_spp, &a0,
	  ui->uris.ui_amp, &a1,
	  ui->uris.samplerate, &a2, NULL)
	&& a0 && a1 && a2
	&& a0->type == ui->uris.atom_Int
	&& a1->type == ui->uris.atom_Float
	&& a2->type == ui->uris.atom_Float
	)
    {
      int spp = ((LV2_Atom_Int*)a0)->body;
      float amp = ((LV2_Atom_Float*)a1)->body;
      ui->rate = ((LV2_Atom_Float*)a2)->body;
      gtk_spin_button_set_value(GTK_SPIN_BUTTON(ui->spb_speed), spp);
      gtk_spin_button_set_value(GTK_SPIN_BUTTON(ui->spb_amp), amp);
    }
  }
}

static const LV2UI_Descriptor descriptor = {
  SCO_URI "#ui",
  instantiate,
  cleanup,
  port_event,
  NULL
};

LV2_SYMBOL_EXPORT
const LV2UI_Descriptor*
lv2ui_descriptor(uint32_t index)
{
  switch (index) {
  case 0:
    return &descriptor;
  default:
    return NULL;
  }
}

/* vi:set ts=8 sts=2 sw=2: */
