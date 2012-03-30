/*
 * Copyright © 2011 Tim Wiederhake
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

/**
 * \file eventdemo.c
 * \brief Demonstrate the use of Wayland's toytoolkit.
 *
 * Heavily commented demo program that can report all events that are
 * dispatched to the window. For other functionality, eg. opengl/egl,
 * drag and drop, etc. have a look at the other demos.
 * \author Tim Wiederhake
 */

#include <stdio.h>
#include <stdlib.h>

#include <cairo.h>
#include <glib.h>

#include "window.h"

/** window title */
static char *title = "EventDemo";

/** window width */
static int width = 500;

/** window height */
static int height = 400;

/** set if window has no borders */
static int noborder = 0;

/** if non-zero, maximum window width */
static int width_max = 0;

/** if non-zero, maximum window height */
static int height_max = 0;

/** set to log redrawing */
static int log_redraw = 0;

/** set to log resizing */
static int log_resize = 0;

/** set to log keyboard focus */
static int log_focus = 0;

/** set to log key events */
static int log_key = 0;

/** set to log button events */
static int log_button = 0;

/** set to log motion events */
static int log_motion = 0;

/**
 * \struct eventdemo
 * \brief Holds all data the program needs per window
 *
 * In this demo the struct holds the position of a
 * red rectangle that is drawn in the window's area.
 */
struct eventdemo {
	struct window *window;
	struct widget *widget;
	struct display *display;

	int x, y, w, h;
};

/**
 * \brief CALLBACK function, Wayland requests the window to redraw.
 * \param window window to be redrawn
 * \param data user data associated to the window
 *
 * Draws a red rectangle as demonstration of per-window data.
 */
static void
redraw_handler(struct widget *widget, void *data)
{
	struct eventdemo *e = data;
	cairo_surface_t *surface;
	cairo_t *cr;
	struct rectangle rect;

	if (log_redraw)
		printf("redraw\n");

	widget_get_allocation(e->widget, &rect);
	surface = window_get_surface(e->window);

	cr = cairo_create(surface);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);

	cairo_rectangle(cr, rect.x, rect.y, rect.width, rect.height);
	cairo_set_source_rgba(cr, 0, 0, 0, 0.8);
	cairo_fill(cr);

	cairo_rectangle(cr, e->x, e->y, e->w, e->h);
	cairo_set_source_rgba(cr, 1.0, 0, 0, 1);
	cairo_fill(cr);

	cairo_destroy(cr);
	cairo_surface_destroy(surface);
}

/**
 * \brief CALLBACK function, Wayland requests the window to resize.
 * \param window window to be resized
 * \param width desired width
 * \param height desired height
 * \param data user data associated to the window
 */

static void
resize_handler(struct widget *widget,
	       int32_t width, int32_t height, void *data)
{
	struct eventdemo *e = data;
	if (log_resize)
		printf("resize width: %d, height: %d\n", width, height);

	/* if a maximum width is set, constrain to it */
	if (width_max && width_max < width)
		width = width_max;

	/* if a maximum height is set, constrain to it */
	if (height_max && height_max < height)
		height = height_max;

	/* set the new window dimensions */
	widget_set_size(e->widget, width, height);
}

/**
 * \brief CALLBACK function, Wayland informs about keyboard focus change
 * \param window window
 * \param device device that caused the focus change
 * \param data user data associated to the window
 */
static void
keyboard_focus_handler(struct window *window,
		       struct input *device, void *data)
{
	int32_t x, y;
	struct eventdemo *e = data;

	if(log_focus) {
		if(device) {
			input_get_position(device, &x, &y);
			printf("focus x: %d, y: %d\n", x, y);
		} else {
			printf("focus lost\n");
		}
	}

	window_schedule_redraw(e->window);
}

/**
 * \brief CALLBACK function, Wayland informs about key event
 * \param window window
 * \param key keycode
 * \param unicode associated character
 * \param state pressed or released
 * \param modifiers modifiers: ctrl, alt, meta etc.
 * \param data user data associated to the window
 */
static void
key_handler(struct window *window, struct input *input, uint32_t time,
            uint32_t key, uint32_t unicode, uint32_t state, void *data)
{
	uint32_t modifiers = input_get_modifiers(input);

	if(!log_key)
		return;

	printf("key key: %d, unicode: %d, state: %d, modifiers: %d\n",
	       key, unicode, state, modifiers);
}

/**
 * \brief CALLBACK function, Wayland informs about button event
 * \param window window
 * \param input input device that caused the button event
 * \param time time the event happend
 * \param button button
 * \param state pressed or released
 * \param data user data associated to the window
 */
static void
button_handler(struct widget *widget, struct input *input, uint32_t time,
	       int button, int state, void *data)
{
	int32_t x, y;

	if (!log_button)
		return;

	input_get_position(input, &x, &y);
	printf("button time: %d, button: %d, state: %d, x: %d, y: %d\n",
	       time, button, state, x, y);
}

/**
 * \brief CALLBACK function, Waylands informs about pointer motion
 * \param window window
 * \param input input device that caused the motion event
 * \param time time the event happend
 * \param x absolute x position
 * \param y absolute y position
 * \param sx x position relative to the window
 * \param sy y position relative to the window
 * \param data user data associated to the window
 *
 * Demonstrates the use of different cursors
 */
static int
motion_handler(struct widget *widget, struct input *input, uint32_t time,
	       int32_t x, int32_t y, void *data)
{
	struct eventdemo *e = data;

	if (log_motion) {
		printf("motion time: %d, x: %d, y: %d\n", time, x, y);
	}

	if (x > e->x && x < e->x + e->w)
		if (y > e->y && y < e->y + e->h)
			return POINTER_HAND1;

	return POINTER_LEFT_PTR;
}

/**
 * \brief Create and initialise a new eventdemo window.
 * \param d associated display
 */
static struct eventdemo *
eventdemo_create(struct display *d)
{
	struct eventdemo *e;

	e = malloc(sizeof (struct eventdemo));
	if(e == NULL)
		return NULL;

	e->window = window_create(d);
	e->widget = frame_create(e->window, e);
	window_set_title(e->window, title);
	e->display = d;

	/* The eventdemo window draws a red rectangle as a demonstration
	 * of per-window data. The dimensions of that rectangle are set
	 * here.
	 */
	e->x = width * 1.0 / 4.0;
	e->w = width * 2.0 / 4.0;
	e->y = height * 1.0 / 4.0;
	e->h = height * 2.0 / 4.0;

	/* Connect the user data to the window */
	window_set_user_data(e->window, e);

	/* Set the callback redraw handler for the window */
	widget_set_redraw_handler(e->widget, redraw_handler);

	/* Set the callback resize handler for the window */
	widget_set_resize_handler(e->widget, resize_handler);

	/* Set the callback focus handler for the window */
	window_set_keyboard_focus_handler(e->window,
					  keyboard_focus_handler);

	/* Set the callback key handler for the window */
	window_set_key_handler(e->window, key_handler);

	/* Set the callback button handler for the window */
	widget_set_button_handler(e->widget, button_handler);

	/* Set the callback motion handler for the window */
	widget_set_motion_handler(e->widget, motion_handler);

	/* Demonstrate how to create a borderless window.
	   Move windows with META + left mouse button.
	 */
	if (noborder) {
	}

	/* Initial drawing of the window */
	window_schedule_resize(e->window, width, height);

	return e;
}
/**
 * \brief command line options for eventdemo
 */
static const struct weston_option eventdemo_options[] = {
	{ WESTON_OPTION_STRING, "title", 0, &title },
	{ WESTON_OPTION_INTEGER, "width", 'w', &width },
	{ WESTON_OPTION_INTEGER, "height", 'h', &height },
	{ WESTON_OPTION_INTEGER, "max-width", 0, &width_max },
	{ WESTON_OPTION_INTEGER, "max-height", 0, &height_max },
	{ WESTON_OPTION_BOOLEAN, "no-border", 'b', &noborder },
	{ WESTON_OPTION_BOOLEAN, "log-redraw", '0', &log_redraw },
	{ WESTON_OPTION_BOOLEAN, "log-resize", '0', &log_resize },
	{ WESTON_OPTION_BOOLEAN, "log-focus", '0', &log_focus },
	{ WESTON_OPTION_BOOLEAN, "log-key", '0', &log_key },
	{ WESTON_OPTION_BOOLEAN, "log-button", '0', &log_button },
	{ WESTON_OPTION_BOOLEAN, "log-motion", '0', &log_motion },
};

/**
 * \brief Connects to the display, creates the window and hands over
 * to the main loop.
 */
int
main(int argc, char *argv[])
{
	struct display *d;
	struct eventdemo *e;

	argc = parse_options(eventdemo_options,
			     ARRAY_LENGTH(eventdemo_options), argc, argv);

	/* Connect to the display and have the arguments parsed */
	d = display_create(argc, argv);
	if (d == NULL) {
		fprintf(stderr, "failed to create display: %m\n");
		return -1;
	}

	/* Create new eventdemo window */
	e = eventdemo_create(d);
	if (e == NULL) {
		fprintf(stderr, "failed to create eventdemo: %m\n");
		return -1;
	}

	display_run(d);

	return 0;
}
