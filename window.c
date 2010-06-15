/*
 * Copyright © 2008 Kristian Høgsberg
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

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <math.h>
#include <time.h>
#include <cairo.h>
#include <glib.h>
#include <glib-object.h>

#define EGL_EGLEXT_PROTOTYPES 1
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <cairo-gl.h>

#include <linux/input.h>
#include "wayland-util.h"
#include "wayland-client.h"
#include "wayland-glib.h"
#include "../cairo-util.h"

#include "window.h"

struct display {
	struct wl_display *display;
	struct wl_compositor *compositor;
	struct wl_output *output;
	struct wl_input_device *input_device;
	struct rectangle screen_allocation;
	EGLDisplay dpy;
	EGLContext ctx;
	cairo_device_t *device;
	int fd;
	GMainLoop *loop;
	GSource *source;
	struct wl_list window_list;
	char *device_name;
};

struct window {
	struct display *display;
	struct wl_surface *surface;
	const char *title;
	struct rectangle allocation, saved_allocation;
	int minimum_width, minimum_height;
	int margin;
	int drag_x, drag_y;
	int state;
	int fullscreen;
	int decoration;
	struct wl_input_device *grab_device;
	struct wl_input_device *keyboard_device;
	uint32_t name;
	uint32_t modifiers;

	EGLImageKHR *image;
	cairo_surface_t *cairo_surface, *pending_surface;
	int new_surface;

	window_resize_handler_t resize_handler;
	window_key_handler_t key_handler;
	window_keyboard_focus_handler_t keyboard_focus_handler;
	window_acknowledge_handler_t acknowledge_handler;
	window_frame_handler_t frame_handler;

	void *user_data;
	struct wl_list link;
};

static void
rounded_rect(cairo_t *cr, int x0, int y0, int x1, int y1, int radius)
{
	cairo_move_to(cr, x0, y0 + radius);
	cairo_arc(cr, x0 + radius, y0 + radius, radius, M_PI, 3 * M_PI / 2);
	cairo_line_to(cr, x1 - radius, y0);
	cairo_arc(cr, x1 - radius, y0 + radius, radius, 3 * M_PI / 2, 2 * M_PI);
	cairo_line_to(cr, x1, y1 - radius);
	cairo_arc(cr, x1 - radius, y1 - radius, radius, 0, M_PI / 2);
	cairo_line_to(cr, x0 + radius, y1);
	cairo_arc(cr, x0 + radius, y1 - radius, radius, M_PI / 2, M_PI);
	cairo_close_path(cr);
}

static const cairo_user_data_key_t surface_data_key;
struct surface_data {
	EGLImageKHR image;
	GLuint texture;
	EGLDisplay dpy;
};

static void
surface_data_destroy(void *p)
{
	struct surface_data *data = p;

	glDeleteTextures(1, &data->texture);
	eglDestroyImageKHR(data->dpy, data->image);
}

cairo_surface_t *
window_create_surface(struct window *window,
		      struct rectangle *rectangle)
{
	struct surface_data *data;
	EGLDisplay dpy = window->display->dpy;
	cairo_surface_t *surface;

	EGLint image_attribs[] = {
		EGL_WIDTH,		0,
		EGL_HEIGHT,		0,
		EGL_IMAGE_FORMAT_MESA,	EGL_IMAGE_FORMAT_ARGB8888_MESA,
		EGL_IMAGE_USE_MESA,	EGL_IMAGE_USE_SCANOUT_MESA,
		EGL_NONE
	};

	data = malloc(sizeof *data);
	image_attribs[1] = rectangle->width;
	image_attribs[3] = rectangle->height;
	data->image = eglCreateDRMImageMESA(dpy, image_attribs);
	glGenTextures(1, &data->texture);
	data->dpy = dpy;
	glBindTexture(GL_TEXTURE_2D, data->texture);
	glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, data->image);

	surface = cairo_gl_surface_create_for_texture(window->display->device,
						      CAIRO_CONTENT_COLOR_ALPHA,
						      data->texture,
						      rectangle->width,
						      rectangle->height);
	
	cairo_surface_set_user_data (surface, &surface_data_key,
				     data, surface_data_destroy);
	
	return surface;
}

static void
window_attach_surface(struct window *window)
{
	struct wl_visual *visual;
	struct surface_data *data;
	EGLint name, stride;

	if (window->pending_surface != NULL)
		return;

	window->pending_surface =
		cairo_surface_reference(window->cairo_surface);

	data = cairo_surface_get_user_data (window->cairo_surface, &surface_data_key);
	eglExportDRMImageMESA(window->display->dpy, data->image, &name, NULL, &stride);

	visual = wl_display_get_premultiplied_argb_visual(window->display->display);
	wl_surface_attach(window->surface,
			  name,
			  window->allocation.width,
			  window->allocation.height,
			  stride,
			  visual);

	wl_surface_map(window->surface,
		       window->allocation.x - window->margin,
		       window->allocation.y - window->margin,
		       window->allocation.width,
		       window->allocation.height);
}

void
window_commit(struct window *window, uint32_t key)
{
	if (window->new_surface) {
		window_attach_surface(window);
		window->new_surface = 0;
	}

	wl_compositor_commit(window->display->compositor, key);
}

static void
window_draw_decorations(struct window *window)
{
	cairo_t *cr;
	int border = 2, radius = 5;
	cairo_text_extents_t extents;
	cairo_pattern_t *gradient, *outline, *bright, *dim;
	int width, height;
	int shadow_dx = 4, shadow_dy = 4;

	window->cairo_surface =
		window_create_surface(window, &window->allocation);

	outline = cairo_pattern_create_rgb(0.1, 0.1, 0.1);
	bright = cairo_pattern_create_rgb(0.8, 0.8, 0.8);
	dim = cairo_pattern_create_rgb(0.4, 0.4, 0.4);

	cr = cairo_create(window->cairo_surface);

	width = window->allocation.width - window->margin * 2;
	height = window->allocation.height - window->margin * 2;

	cairo_translate(cr, window->margin + shadow_dx,
			window->margin + shadow_dy);
	cairo_set_line_width (cr, border);
	cairo_set_source_rgba(cr, 0, 0, 0, 0.7);
	rounded_rect(cr, -1, -1, width + 1, height + 1, radius);
	cairo_fill(cr);

#define SLOW_BUT_PWETTY_not_right_now
#ifdef SLOW_BUT_PWETTY
	/* FIXME: Aw, pretty drop shadows now have to fallback to sw.
	 * Ideally we should have convolution filters in cairo, but we
	 * can also fallback to compositing the shadow image a bunch
	 * of times according to the blur kernel. */
	{
		cairo_surface_t *map;

		map = cairo_drm_surface_map(window->cairo_surface);
		blur_surface(map, 32);
		cairo_drm_surface_unmap(window->cairo_surface, map);
	}
#endif

	cairo_translate(cr, -shadow_dx, -shadow_dy);
	if (window->keyboard_device) {
		rounded_rect(cr, 0, 0, width, height, radius);
		gradient = cairo_pattern_create_linear (0, 0, 0, 100);
		cairo_pattern_add_color_stop_rgb(gradient, 0, 0.6, 0.6, 0.6);
		cairo_pattern_add_color_stop_rgb(gradient, 1, 0.8, 0.8, 0.8);
		cairo_set_source(cr, gradient);
		cairo_fill(cr);
		cairo_pattern_destroy(gradient);
	} else {
		rounded_rect(cr, 0, 0, width, height, radius);
		cairo_set_source_rgba(cr, 0.6, 0.6, 0.6, 1);
		cairo_fill(cr);
	}

	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_move_to(cr, 10, 50);
	cairo_line_to(cr, width - 10, 50);
	cairo_line_to(cr, width - 10, height - 10);
	cairo_line_to(cr, 10, height - 10);
	cairo_close_path(cr);
	cairo_set_source(cr, dim);
	cairo_stroke(cr);

	cairo_move_to(cr, 11, 51);
	cairo_line_to(cr, width - 10, 51);
	cairo_line_to(cr, width - 10, height - 10);
	cairo_line_to(cr, 11, height - 10);
	cairo_close_path(cr);
	cairo_set_source(cr, bright);
	cairo_stroke(cr);

	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
	cairo_set_font_size(cr, 14);
	cairo_text_extents(cr, window->title, &extents);
	cairo_move_to(cr, (width - extents.width) / 2, 10 - extents.y_bearing);
	cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);
	cairo_set_line_join (cr, CAIRO_LINE_JOIN_ROUND);
	cairo_set_line_width (cr, 4);
	cairo_text_path(cr, window->title);
	if (window->keyboard_device) {
		cairo_set_source_rgb(cr, 0.56, 0.56, 0.56);
		cairo_stroke_preserve(cr);
		cairo_set_source_rgb(cr, 1, 1, 1);
		cairo_fill(cr);
	} else {
		cairo_set_source_rgb(cr, 0.8, 0.8, 0.8);
		cairo_fill(cr);
	}
	cairo_destroy(cr);
}

static void
window_draw_fullscreen(struct window *window)
{
	window->cairo_surface =
		window_create_surface(window, &window->allocation);
}

void
window_draw(struct window *window)
{
	if (window->cairo_surface != NULL)
		cairo_surface_destroy(window->cairo_surface);

	if (window->fullscreen || !window->decoration)
		window_draw_fullscreen(window);
	else
		window_draw_decorations(window);

	window->new_surface = 1;
}

cairo_surface_t *
window_get_surface(struct window *window)
{
	return window->cairo_surface;
}

enum window_state {
	WINDOW_MOVING = 0,
	WINDOW_RESIZING_TOP = 1,
	WINDOW_RESIZING_BOTTOM = 2,
	WINDOW_RESIZING_LEFT = 4,
	WINDOW_RESIZING_TOP_LEFT = 5,
	WINDOW_RESIZING_BOTTOM_LEFT = 6,
	WINDOW_RESIZING_RIGHT = 8,
	WINDOW_RESIZING_TOP_RIGHT = 9,
	WINDOW_RESIZING_BOTTOM_RIGHT = 10,
	WINDOW_RESIZING_MASK = 15,
	WINDOW_STABLE = 16,
};

static void
window_handle_motion(void *data, struct wl_input_device *input_device,
		     int32_t x, int32_t y, int32_t sx, int32_t sy)
{
	struct window *window = data;

	switch (window->state) {
	case WINDOW_MOVING:
		if (window->fullscreen)
			break;
		if (window->grab_device != input_device)
			break;
		window->allocation.x = window->drag_x + x;
		window->allocation.y = window->drag_y + y;
		wl_surface_map(window->surface,
			       window->allocation.x - window->margin,
			       window->allocation.y - window->margin,
			       window->allocation.width,
			       window->allocation.height);
		wl_compositor_commit(window->display->compositor, 1);
		break;

	case WINDOW_RESIZING_TOP:
	case WINDOW_RESIZING_BOTTOM:
	case WINDOW_RESIZING_LEFT:
	case WINDOW_RESIZING_RIGHT:
	case WINDOW_RESIZING_TOP_LEFT:
	case WINDOW_RESIZING_TOP_RIGHT:
	case WINDOW_RESIZING_BOTTOM_LEFT:
	case WINDOW_RESIZING_BOTTOM_RIGHT:
		if (window->fullscreen)
			break;
		if (window->grab_device != input_device)
			break;
		if (window->state & WINDOW_RESIZING_LEFT) {
			window->allocation.x = x - window->drag_x + window->saved_allocation.x;
			window->allocation.width = window->drag_x - x + window->saved_allocation.width;
		}
		if (window->state & WINDOW_RESIZING_RIGHT)
			window->allocation.width = x - window->drag_x + window->saved_allocation.width;
		if (window->state & WINDOW_RESIZING_TOP) {
			window->allocation.y = y - window->drag_y + window->saved_allocation.y;
			window->allocation.height = window->drag_y - y + window->saved_allocation.height;
		}
		if (window->state & WINDOW_RESIZING_BOTTOM)
			window->allocation.height = y - window->drag_y + window->saved_allocation.height;

		if (window->resize_handler)
			(*window->resize_handler)(window,
						  window->user_data);

		break;
	}
}

static void window_handle_button(void *data, struct wl_input_device *input_device,
				 uint32_t button, uint32_t state,
				 int32_t x, int32_t y, int32_t sx, int32_t sy)
{
	struct window *window = data;
	int grip_size = 8, vlocation, hlocation;

	if (window->margin <= sx && sx < window->margin + grip_size)
		hlocation = WINDOW_RESIZING_LEFT;
	else if (sx < window->allocation.width - window->margin - grip_size)
		hlocation = WINDOW_MOVING;
	else if (sx < window->allocation.width - window->margin)
		hlocation = WINDOW_RESIZING_RIGHT;
	else
		hlocation = WINDOW_STABLE;

	if (window->margin <= sy && sy < window->margin + grip_size)
		vlocation = WINDOW_RESIZING_TOP;
	else if (sy < window->allocation.height - window->margin - grip_size)
		vlocation = WINDOW_MOVING;
	else if (sy < window->allocation.height - window->margin)
		vlocation = WINDOW_RESIZING_BOTTOM;
	else
		vlocation = WINDOW_STABLE;

	if (button == BTN_LEFT && state == 1) {
		switch (hlocation | vlocation) {
		case WINDOW_MOVING:
			window->drag_x = window->allocation.x - x;
			window->drag_y = window->allocation.y - y;
			window->state = WINDOW_MOVING;
			window->grab_device = input_device;
			break;
		case WINDOW_RESIZING_TOP:
		case WINDOW_RESIZING_BOTTOM:
		case WINDOW_RESIZING_LEFT:
		case WINDOW_RESIZING_RIGHT:
		case WINDOW_RESIZING_TOP_LEFT:
		case WINDOW_RESIZING_TOP_RIGHT:
		case WINDOW_RESIZING_BOTTOM_LEFT:
		case WINDOW_RESIZING_BOTTOM_RIGHT:
			window->drag_x = x;
			window->drag_y = y;
			window->saved_allocation = window->allocation;
			window->state = hlocation | vlocation;
			window->grab_device = input_device;
			break;
		default:
			window->state = WINDOW_STABLE;
			break;
		}
	} else if (button == BTN_LEFT &&
		   state == 0 && window->grab_device == input_device) {
		window->state = WINDOW_STABLE;
	}
}


struct key {
	uint32_t code[4];
} evdev_keymap[] = {
	{ { 0, 0 } },		/* 0 */
	{ { 0x1b, 0x1b } },
	{ { '1', '!' } },
	{ { '2', '@' } },
	{ { '3', '#' } },
	{ { '4', '$' } },
	{ { '5', '%' } },
	{ { '6', '^' } },
	{ { '7', '&' } },
	{ { '8', '*' } },
	{ { '9', '(' } },
	{ { '0', ')' } },
	{ { '-', '_' } },
	{ { '=', '+' } },
	{ { '\b', '\b' } },
	{ { '\t', '\t' } },

	{ { 'q', 'Q', 0x11 } },		/* 16 */
	{ { 'w', 'W', 0x17 } },
	{ { 'e', 'E', 0x05 } },
	{ { 'r', 'R', 0x12 } },
	{ { 't', 'T', 0x14 } },
	{ { 'y', 'Y', 0x19 } },
	{ { 'u', 'U', 0x15 } },
	{ { 'i', 'I', 0x09 } },
	{ { 'o', 'O', 0x0f } },
	{ { 'p', 'P', 0x10 } },
	{ { '[', '{', 0x1b } },
	{ { ']', '}', 0x1d } },
	{ { '\n', '\n' } },
	{ { 0, 0 } },
	{ { 'a', 'A', 0x01} },
	{ { 's', 'S', 0x13 } },

	{ { 'd', 'D', 0x04 } },		/* 32 */
	{ { 'f', 'F', 0x06 } },
	{ { 'g', 'G', 0x07 } },
	{ { 'h', 'H', 0x08 } },
	{ { 'j', 'J', 0x0a } },
	{ { 'k', 'K', 0x0b } },
	{ { 'l', 'L', 0x0c } },
	{ { ';', ':' } },
	{ { '\'', '"' } },
	{ { '`', '~' } },
	{ { 0, 0 } },
	{ { '\\', '|', 0x1c } },
	{ { 'z', 'Z', 0x1a } },
	{ { 'x', 'X', 0x18 } },
	{ { 'c', 'C', 0x03 } },
	{ { 'v', 'V', 0x16 } },

	{ { 'b', 'B', 0x02 } },		/* 48 */
	{ { 'n', 'N', 0x0e } },
	{ { 'm', 'M', 0x0d } },
	{ { ',', '<' } },
	{ { '.', '>' } },
	{ { '/', '?' } },
	{ { 0, 0 } },
	{ { '*', '*' } },
	{ { 0, 0 } },
	{ { ' ', ' ' } },
	{ { 0, 0 } }

	/* 59 */
};

#define ARRAY_LENGTH(a) (sizeof (a) / sizeof (a)[0])

static void
window_update_modifiers(struct window *window, uint32_t key, uint32_t state)
{
	uint32_t mod = 0;

	switch (key) {
	case KEY_LEFTSHIFT:
	case KEY_RIGHTSHIFT:
		mod = WINDOW_MODIFIER_SHIFT;
		break;
	case KEY_LEFTCTRL:
	case KEY_RIGHTCTRL:
		mod = WINDOW_MODIFIER_CONTROL;
		break;
	case KEY_LEFTALT:
	case KEY_RIGHTALT:
		mod = WINDOW_MODIFIER_ALT;
		break;
	}

	if (state)
		window->modifiers |= mod;
	else
		window->modifiers &= ~mod;
}

static void
window_handle_key(void *data, struct wl_input_device *input_device,
		  uint32_t key, uint32_t state)
{
	struct window *window = data;
	uint32_t unicode = 0;

	if (window->keyboard_device != input_device)
		return;

	window_update_modifiers(window, key, state);

	if (key < ARRAY_LENGTH(evdev_keymap)) {
		if (window->modifiers & WINDOW_MODIFIER_CONTROL)
			unicode = evdev_keymap[key].code[2];
		else if (window->modifiers & WINDOW_MODIFIER_SHIFT)
			unicode = evdev_keymap[key].code[1];
		else
			unicode = evdev_keymap[key].code[0];
	}

	if (window->key_handler)
		(*window->key_handler)(window, key, unicode,
				       state, window->modifiers, window->user_data);
}

static void
window_handle_pointer_focus(void *data,
			    struct wl_input_device *input_device,
			    struct wl_surface *surface)
{
}

static void
window_handle_keyboard_focus(void *data,
			     struct wl_input_device *input_device,
			     struct wl_surface *surface,
			     struct wl_array *keys)
{
	struct window *window = data;
	uint32_t *k, *end;

	if (window->keyboard_device == input_device && surface != window->surface)
		window->keyboard_device = NULL;
	else if (window->keyboard_device == NULL && surface == window->surface)
		window->keyboard_device = input_device;
	else
		return;

	if (window->keyboard_device) {
		end = keys->data + keys->size;
		for (k = keys->data; k < end; k++)
			window_update_modifiers(window, *k, 1);
	} else {
		window->modifiers = 0;
	}

	if (window->keyboard_focus_handler)
		(*window->keyboard_focus_handler)(window,
						  window->keyboard_device,
						  window->user_data);
}

static const struct wl_input_device_listener input_device_listener = {
	window_handle_motion,
	window_handle_button,
	window_handle_key,
	window_handle_pointer_focus,
	window_handle_keyboard_focus,
};

void
window_get_child_rectangle(struct window *window,
			   struct rectangle *rectangle)
{
	if (window->fullscreen && !window->decoration) {
		*rectangle = window->allocation;
	} else {
		rectangle->x = window->margin + 10;
		rectangle->y = window->margin + 50;
		rectangle->width = window->allocation.width - 20 - window->margin * 2;
		rectangle->height = window->allocation.height - 60 - window->margin * 2;
	}
}

void
window_set_child_size(struct window *window,
		      struct rectangle *rectangle)
{
	int32_t width, height;

	if (!window->fullscreen) {
		width = rectangle->width + 20 + window->margin * 2;
		height = rectangle->height + 60 + window->margin * 2;
		if (window->state & WINDOW_RESIZING_LEFT)
			window->allocation.x -=
				width - window->allocation.width;
		if (window->state & WINDOW_RESIZING_TOP)
			window->allocation.y -=
				height - window->allocation.height;
		window->allocation.width = width;
		window->allocation.height = height;
	}
}

void
window_copy_image(struct window *window,
		  struct rectangle *rectangle, EGLImageKHR image)
{
	/* set image as read buffer, copy pixels or something... */
}

void
window_copy_surface(struct window *window,
		    struct rectangle *rectangle,
		    cairo_surface_t *surface)
{
	cairo_t *cr;

	cr = cairo_create (window->cairo_surface);

	cairo_set_source_surface (cr,
				  surface,
				  rectangle->x, rectangle->y);

	cairo_paint (cr);
	cairo_destroy (cr);
}

void
window_set_fullscreen(struct window *window, int fullscreen)
{
	window->fullscreen = fullscreen;
	if (window->fullscreen) {
		window->saved_allocation = window->allocation;
		window->allocation = window->display->screen_allocation;
	} else {
		window->allocation = window->saved_allocation;
	}
}

void
window_set_decoration(struct window *window, int decoration)
{
	window->decoration = decoration;
}

void
window_set_resize_handler(struct window *window,
			  window_resize_handler_t handler, void *data)
{
	window->resize_handler = handler;
	window->user_data = data;
}

void
window_set_key_handler(struct window *window,
		       window_key_handler_t handler, void *data)
{
	window->key_handler = handler;
	window->user_data = data;
}

void
window_set_acknowledge_handler(struct window *window,
			       window_acknowledge_handler_t handler, void *data)
{
	window->acknowledge_handler = handler;
	window->user_data = data;
}

void
window_set_frame_handler(struct window *window,
			 window_frame_handler_t handler, void *data)
{
	window->frame_handler = handler;
	window->user_data = data;
}

void
window_set_keyboard_focus_handler(struct window *window,
				  window_keyboard_focus_handler_t handler, void *data)
{
	window->keyboard_focus_handler = handler;
	window->user_data = data;
}

void
window_move(struct window *window, int32_t x, int32_t y)
{
	window->allocation.x = x;
	window->allocation.y = y;

	wl_surface_map(window->surface,
		       window->allocation.x - window->margin,
		       window->allocation.y - window->margin,
		       window->allocation.width,
		       window->allocation.height);
}

struct window *
window_create(struct display *display, const char *title,
	      int32_t x, int32_t y, int32_t width, int32_t height)
{
	struct window *window;

	window = malloc(sizeof *window);
	if (window == NULL)
		return NULL;

	memset(window, 0, sizeof *window);
	window->display = display;
	window->title = strdup(title);
	window->surface = wl_compositor_create_surface(display->compositor);
	window->allocation.x = x;
	window->allocation.y = y;
	window->allocation.width = width;
	window->allocation.height = height;
	window->saved_allocation = window->allocation;
	window->margin = 16;
	window->state = WINDOW_STABLE;
	window->decoration = 1;

	wl_list_insert(display->window_list.prev, &window->link);

	wl_input_device_add_listener(display->input_device,
				     &input_device_listener, window);

	return window;
}

static void
display_handle_device(void *data,
		      struct wl_compositor *compositor,
		      const char *device)
{
	struct display *d = data;

	d->device_name = strdup(device);
}

static void
display_handle_acknowledge(void *data,
			   struct wl_compositor *compositor,
			   uint32_t key, uint32_t frame)
{
	struct display *d = data;
	struct window *window;
	cairo_surface_t *pending;
		
	/* The acknowledge event means that the server processed our
	 * last commit request and we can now safely free the old
	 * window buffer if we resized and render the next frame into
	 * our back buffer.. */
	wl_list_for_each(window, &d->window_list, link) {
		pending = window->pending_surface;
		window->pending_surface = NULL;
		if (pending != window->cairo_surface)
			window_attach_surface(window);
		cairo_surface_destroy(pending);
		if (window->acknowledge_handler)
			(*window->acknowledge_handler)(window, key, frame, window->user_data);
	}
}

static void
display_handle_frame(void *data,
		     struct wl_compositor *compositor,
		     uint32_t frame, uint32_t timestamp)
{
	struct display *d = data;
	struct window *window;

	wl_list_for_each(window, &d->window_list, link) {
		if (window->frame_handler)
			(*window->frame_handler)(window, frame,
						 timestamp, window->user_data);
	}
}

static const struct wl_compositor_listener compositor_listener = {
	display_handle_device,
	display_handle_acknowledge,
	display_handle_frame,
};

static void
display_handle_geometry(void *data,
			struct wl_output *output,
			int32_t width, int32_t height)
{
	struct display *display = data;

	display->screen_allocation.x = 0;
	display->screen_allocation.y = 0;
	display->screen_allocation.width = width;
	display->screen_allocation.height = height;
}

static const struct wl_output_listener output_listener = {
	display_handle_geometry,
};

static void
display_handle_global(struct wl_display *display,
		     struct wl_object *object, void *data)
{
	struct display *d = data;

	if (wl_object_implements(object, "compositor", 1)) { 
		d->compositor = (struct wl_compositor *) object;
		wl_compositor_add_listener(d->compositor, &compositor_listener, d);
	} else if (wl_object_implements(object, "output", 1)) {
		d->output = (struct wl_output *) object;
		wl_output_add_listener(d->output, &output_listener, d);
	} else if (wl_object_implements(object, "input_device", 1)) {
		d->input_device =(struct wl_input_device *) object;
	}
}

static const char socket_name[] = "\0wayland";

struct display *
display_create(int *argc, char **argv[], const GOptionEntry *option_entries)
{
	PFNEGLGETTYPEDDISPLAYMESA get_typed_display_mesa;
	struct display *d;
	EGLint major, minor, count;
	EGLConfig config;
	int fd;
	GOptionContext *context;
	GError *error;

	static const EGLint config_attribs[] = {
		EGL_SURFACE_TYPE,		0,
		EGL_NO_SURFACE_CAPABLE_MESA,	EGL_OPENGL_BIT,
		EGL_RENDERABLE_TYPE,		EGL_OPENGL_BIT,
		EGL_NONE
	};

	g_type_init();

	context = g_option_context_new(NULL);
	if (option_entries) {
		g_option_context_add_main_entries(context, option_entries, "Wayland View");
		if (!g_option_context_parse(context, argc, argv, &error)) {
			fprintf(stderr, "option parsing failed: %s\n", error->message);
			exit(EXIT_FAILURE);
		}
	}

	d = malloc(sizeof *d);
	if (d == NULL)
		return NULL;

	d->display = wl_display_create(socket_name, sizeof socket_name);
	if (d->display == NULL) {
		fprintf(stderr, "failed to create display: %m\n");
		return NULL;
	}

	/* Set up listener so we'll catch all events. */
	wl_display_add_global_listener(d->display,
				       display_handle_global, d);

	/* Process connection events. */
	wl_display_iterate(d->display, WL_DISPLAY_READABLE);

	fd = open(d->device_name, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "drm open failed: %m\n");
		return NULL;
	}

	get_typed_display_mesa =
		(PFNEGLGETTYPEDDISPLAYMESA) eglGetProcAddress("eglGetTypedDisplayMESA");
	if (get_typed_display_mesa == NULL) {
		fprintf(stderr, "eglGetDisplayMESA() not found\n");
		return NULL;
	}

	d->dpy = get_typed_display_mesa(EGL_DRM_DISPLAY_TYPE_MESA,
					(void *) fd);
	if (!eglInitialize(d->dpy, &major, &minor)) {
		fprintf(stderr, "failed to initialize display\n");
		return NULL;
	}

	if (!eglChooseConfig(d->dpy, config_attribs, &config, 1, &count) ||
	    count == 0) {
		fprintf(stderr, "eglChooseConfig() failed\n");
		return NULL;
	}

	eglBindAPI(EGL_OPENGL_API);

	d->ctx = eglCreateContext(d->dpy, config, EGL_NO_CONTEXT, NULL);
	if (d->ctx == NULL) {
		fprintf(stderr, "failed to create context\n");
		return NULL;
	}

	if (!eglMakeCurrent(d->dpy, NULL, NULL, d->ctx)) {
		fprintf(stderr, "faile to make context current\n");
		return NULL;
	}

	d->device = cairo_egl_device_create(d->dpy, d->ctx);
	if (d->device == NULL) {
		fprintf(stderr, "failed to get cairo drm device\n");
		return NULL;
	}

	d->loop = g_main_loop_new(NULL, FALSE);
	d->source = wl_glib_source_new(d->display);
	g_source_attach(d->source, NULL);

	wl_list_init(&d->window_list);

	return d;
}

struct wl_compositor *
display_get_compositor(struct display *display)
{
	return display->compositor;
}

EGLDisplay
display_get_egl_display(struct display *d)
{
	return d->dpy;
}

void
display_run(struct display *d)
{
	g_main_loop_run(d->loop);
}
