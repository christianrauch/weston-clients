/*
 * Copyright © 2008 Kristian Høgsberg
 * Copyright © 2013 Collabora, Ltd.
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting documentation, and
 * that the name of the copyright holders not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission.  The copyright holders make no representations
 * about the suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
 * OF THIS SOFTWARE.
 */

#include "config.h"

#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <cairo.h>

#include <linux/input.h>

#include "window.h"
#include "scaler-client-protocol.h"

#define BUFFER_SCALE 2
static const int BUFFER_WIDTH = 421 * BUFFER_SCALE;
static const int BUFFER_HEIGHT = 337 * BUFFER_SCALE;
static const int SURFACE_WIDTH = 55 * 4;
static const int SURFACE_HEIGHT = 77 * 4;
static const double RECT_X = 21 * BUFFER_SCALE; /* buffer coords */
static const double RECT_Y = 25 * BUFFER_SCALE;
static const double RECT_W = 55 * BUFFER_SCALE;
static const double RECT_H = 77 * BUFFER_SCALE;

struct box {
	struct display *display;
	struct window *window;
	struct widget *widget;
	int width, height;

	struct wl_scaler *scaler;
	int scaler_version;
	struct wl_viewport *viewport;

	enum {
		MODE_NO_VIEWPORT,
		MODE_SRC_ONLY,
		MODE_DST_ONLY,
		MODE_SRC_DST
	} mode;
};

static void
set_my_viewport(struct box *box)
{
	wl_fixed_t src_x, src_y, src_width, src_height;
	int32_t dst_width = SURFACE_WIDTH;
	int32_t dst_height = SURFACE_HEIGHT;

	if (box->mode == MODE_NO_VIEWPORT)
		return;

	/* Cut the green border in half, take white border fully in,
	 * and black border fully out. The borders are 1px wide in buffer.
	 *
	 * The gl-renderer uses linear texture sampling, this means the
	 * top and left edges go to 100% green, bottom goes to 50% blue/black,
	 * right edge has thick white sliding to 50% red.
	 */
	src_x = wl_fixed_from_double((RECT_X + 0.5) / BUFFER_SCALE);
	src_y = wl_fixed_from_double((RECT_Y + 0.5) / BUFFER_SCALE);
	src_width = wl_fixed_from_double((RECT_W - 0.5) / BUFFER_SCALE);
	src_height = wl_fixed_from_double((RECT_H - 0.5) / BUFFER_SCALE);

	if (box->scaler_version < 2 && box->mode != MODE_SRC_DST) {
		fprintf(stderr, "Error: server's wl_scaler interface version "
			"%d does not support this mode.\n",
			box->scaler_version);
		exit(1);
	}

	switch (box->mode){
	case MODE_SRC_ONLY:
		wl_viewport_set_source(box->viewport, src_x, src_y,
				       src_width, src_height);
		break;
	case MODE_DST_ONLY:
		wl_viewport_set_destination(box->viewport,
					    dst_width, dst_height);
		break;
	case MODE_SRC_DST:
		if (box->scaler_version < 2) {
			wl_viewport_set(box->viewport,
					src_x, src_y, src_width, src_height,
					dst_width, dst_height);
		} else {
			wl_viewport_set_source(box->viewport, src_x, src_y,
					       src_width, src_height);
			wl_viewport_set_destination(box->viewport,
						    dst_width, dst_height);
		}
		break;
	default:
		assert(!"not reached");
	}
}

static void
resize_handler(struct widget *widget,
	       int32_t width, int32_t height, void *data)
{
	struct box *box = data;

	/* Dont resize me */
	widget_set_size(box->widget, box->width, box->height);
}

static void
redraw_handler(struct widget *widget, void *data)
{
	struct box *box = data;
	cairo_surface_t *surface;
	cairo_t *cr;

	surface = window_get_surface(box->window);
	if (surface == NULL ||
	    cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
		fprintf(stderr, "failed to create cairo egl surface\n");
		return;
	}

	cr = cairo_create(surface);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_line_width(cr, 1.0);
	cairo_translate(cr, RECT_X, RECT_Y);

	/* red background */
	cairo_set_source_rgba(cr, 255, 0, 0, 255);
	cairo_paint(cr);

	/* blue box */
	cairo_set_source_rgba(cr, 0, 0, 255, 255);
	cairo_rectangle(cr, 0, 0, RECT_W, RECT_H);
	cairo_fill(cr);

	/* black border outside the box */
	cairo_set_source_rgb(cr, 0, 0, 0);
	cairo_move_to(cr, 0, RECT_H + 0.5);
	cairo_line_to(cr, RECT_W, RECT_H + 0.5);
	cairo_stroke(cr);

	/* white border inside the box */
	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_move_to(cr, RECT_W - 0.5, 0);
	cairo_line_to(cr, RECT_W - 0.5, RECT_H);
	cairo_stroke(cr);

	/* the green border on inside the box, to be split half by crop */
	cairo_set_source_rgb(cr, 0, 1, 0);
	cairo_move_to(cr, 0.5, RECT_H);
	cairo_line_to(cr, 0.5, 0);
	cairo_move_to(cr, 0, 0.5);
	cairo_line_to(cr, RECT_W, 0.5);
	cairo_stroke(cr);

	cairo_destroy(cr);

	/* TODO: buffer_transform */

	cairo_surface_destroy(surface);
}

static void
global_handler(struct display *display, uint32_t name,
	       const char *interface, uint32_t version, void *data)
{
	struct box *box = data;

	if (strcmp(interface, "wl_scaler") == 0) {
		box->scaler_version = version < 2 ? version : 2;

		box->scaler = display_bind(display, name,
					   &wl_scaler_interface,
					   box->scaler_version);

		box->viewport = wl_scaler_get_viewport(box->scaler,
			widget_get_wl_surface(box->widget));

		set_my_viewport(box);
	}
}

static void
button_handler(struct widget *widget,
	       struct input *input, uint32_t time,
	       uint32_t button, enum wl_pointer_button_state state, void *data)
{
	struct box *box = data;

	if (button != BTN_LEFT)
		return;

	if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
		window_move(box->window, input,
			    display_get_serial(box->display));
	}
}

static void
touch_down_handler(struct widget *widget, struct input *input,
		   uint32_t serial, uint32_t time, int32_t id,
		   float x, float y, void *data)
{
	struct box *box = data;
	window_move(box->window, input,
		    display_get_serial(box->display));
}

static void
usage(const char *progname)
{
	fprintf(stderr, "Usage: %s [mode]\n"
		"where 'mode' is one of\n"
		"  -b\tset both src and dst in viewport (default)\n"
		"  -d\tset only dst in viewport\n"
		"  -s\tset only src in viewport\n"
		"  -n\tdo not set viewport at all\n\n",
		progname);

	fprintf(stderr, "Expected output with output_scale=1:\n");

	fprintf(stderr, "Mode -n:\n"
		"  window size %dx%d px\n"
		"  Red box with a blue box in the upper left part.\n"
		"  The blue box has white right edge, black bottom edge,\n"
		"  and thin green left and top edges that can really\n"
		"  be seen only when zoomed in.\n\n",
		BUFFER_WIDTH / BUFFER_SCALE, BUFFER_HEIGHT / BUFFER_SCALE);

	fprintf(stderr, "Mode -b:\n"
		"  window size %dx%d px\n"
		"  Blue box with green top and left edge,\n"
		"  thick white right edge with a hint of red,\n"
		"  and a hint of black in bottom edge.\n\n",
		SURFACE_WIDTH, SURFACE_HEIGHT);

	fprintf(stderr, "Mode -s:\n"
		"  window size %.0fx%.0f px\n"
		"  The same as mode -b, but scaled a lot smaller.\n\n",
		RECT_W / BUFFER_SCALE, RECT_H / BUFFER_SCALE);

	fprintf(stderr, "Mode -d:\n"
		"  window size %dx%d px\n"
		"  This is horizontally squashed version of the -n mode.\n\n",
		SURFACE_WIDTH, SURFACE_HEIGHT);
}

int
main(int argc, char *argv[])
{
	struct box box;
	struct display *d;
	struct timeval tv;
	int i;

	d = display_create(&argc, argv);
	if (d == NULL) {
		fprintf(stderr, "failed to create display: %m\n");
		return -1;
	}

	box.mode = MODE_SRC_DST;

	for (i = 1; i < argc; i++) {
		if (strcmp("-s", argv[i]) == 0)
			box.mode = MODE_SRC_ONLY;
		else if (strcmp("-d", argv[i]) == 0)
			box.mode = MODE_DST_ONLY;
		else if (strcmp("-b", argv[i]) == 0)
			box.mode = MODE_SRC_DST;
		else if (strcmp("-n", argv[i]) == 0)
			box.mode = MODE_NO_VIEWPORT;
		else {
			usage(argv[0]);
			exit(1);
		}
	}

	gettimeofday(&tv, NULL);
	srandom(tv.tv_usec);

	box.width = BUFFER_WIDTH / BUFFER_SCALE;
	box.height = BUFFER_HEIGHT / BUFFER_SCALE;
	box.display = d;
	box.window = window_create(d);
	box.widget = window_add_widget(box.window, &box);
	window_set_title(box.window, "Scaler Test Box");
	window_set_buffer_scale(box.window, BUFFER_SCALE);

	widget_set_resize_handler(box.widget, resize_handler);
	widget_set_redraw_handler(box.widget, redraw_handler);
	widget_set_button_handler(box.widget, button_handler);
	widget_set_default_cursor(box.widget, CURSOR_HAND1);
	widget_set_touch_down_handler(box.widget, touch_down_handler);

	window_schedule_resize(box.window, box.width, box.height);

	display_set_user_data(box.display, &box);
	display_set_global_handler(box.display, global_handler);

	display_run(d);

	widget_destroy(box.widget);
	window_destroy(box.window);
	display_destroy(d);

	return 0;
}
