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

#include "../config.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <math.h>
#include <assert.h>
#include <time.h>
#include <cairo.h>
#include <glib.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <sys/mman.h>
#include <sys/epoll.h>

#include <wayland-egl.h>

#include <GL/gl.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#ifdef HAVE_CAIRO_EGL
#include <cairo-gl.h>
#endif

#include <X11/extensions/XKBcommon.h>

#include <linux/input.h>
#include "wayland-util.h"
#include "wayland-client.h"
#include "cairo-util.h"

#include "window.h"

struct display {
	struct wl_display *display;
	struct wl_compositor *compositor;
	struct wl_shell *shell;
	struct wl_shm *shm;
	struct wl_output *output;
	struct rectangle screen_allocation;
	EGLDisplay dpy;
	EGLConfig rgb_config;
	EGLConfig premultiplied_argb_config;
	EGLContext rgb_ctx;
	EGLContext argb_ctx;
	cairo_device_t *rgb_device;
	cairo_device_t *argb_device;

	int display_fd;
	uint32_t mask;
	struct task display_task;

	int epoll_fd;
	struct wl_list deferred_list;

	struct wl_list window_list;
	struct wl_list input_list;
	char *device_name;
	cairo_surface_t *active_frame, *inactive_frame, *shadow;
	struct xkb_desc *xkb;
	cairo_surface_t **pointer_surfaces;

	PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target_texture_2d;
	PFNEGLCREATEIMAGEKHRPROC create_image;
	PFNEGLDESTROYIMAGEKHRPROC destroy_image;
};

enum {
	TYPE_TOPLEVEL,
	TYPE_FULLSCREEN,
	TYPE_TRANSIENT,
	TYPE_CUSTOM
};
       
struct window {
	struct display *display;
	struct window *parent;
	struct wl_surface *surface;
	char *title;
	struct rectangle allocation, saved_allocation, server_allocation;
	int x, y;
	int resize_edges;
	int redraw_scheduled;
	struct task redraw_task;
	int minimum_width, minimum_height;
	int margin;
	int type;
	int decoration;
	int transparent;
	struct input *grab_device;
	struct input *keyboard_device;
	uint32_t name;
	enum window_buffer_type buffer_type;

	EGLImageKHR *image;
	cairo_surface_t *cairo_surface, *pending_surface;

	window_resize_handler_t resize_handler;
	window_redraw_handler_t redraw_handler;
	window_key_handler_t key_handler;
	window_button_handler_t button_handler;
	window_keyboard_focus_handler_t keyboard_focus_handler;
	window_motion_handler_t motion_handler;
	window_enter_handler_t enter_handler;
	window_leave_handler_t leave_handler;
	window_item_focus_handler_t item_focus_handler;

	struct wl_list item_list;
	struct item *focus_item;
	uint32_t item_grab_button;

	void *user_data;
	struct wl_list link;
};

struct item {
	struct wl_list link;
	struct rectangle allocation;
	void *user_data;
};

struct input {
	struct display *display;
	struct wl_input_device *input_device;
	struct window *pointer_focus;
	struct window *keyboard_focus;
	struct selection_offer *offer;
	uint32_t current_pointer_image;
	uint32_t modifiers;
	int32_t x, y, sx, sy;
	struct wl_list link;
};

enum {
	POINTER_DEFAULT = 100,
	POINTER_UNSET
};

enum window_location {
	WINDOW_INTERIOR = 0,
	WINDOW_RESIZING_TOP = 1,
	WINDOW_RESIZING_BOTTOM = 2,
	WINDOW_RESIZING_LEFT = 4,
	WINDOW_RESIZING_TOP_LEFT = 5,
	WINDOW_RESIZING_BOTTOM_LEFT = 6,
	WINDOW_RESIZING_RIGHT = 8,
	WINDOW_RESIZING_TOP_RIGHT = 9,
	WINDOW_RESIZING_BOTTOM_RIGHT = 10,
	WINDOW_RESIZING_MASK = 15,
	WINDOW_EXTERIOR = 16,
	WINDOW_TITLEBAR = 17,
	WINDOW_CLIENT_AREA = 18,
};

const char *option_xkb_layout = "us";
const char *option_xkb_variant = "";
const char *option_xkb_options = "";

static const GOptionEntry xkb_option_entries[] = {
	{ "xkb-layout", 0, 0, G_OPTION_ARG_STRING,
	  &option_xkb_layout, "XKB Layout" },
	{ "xkb-variant", 0, 0, G_OPTION_ARG_STRING,
	  &option_xkb_variant, "XKB Variant" },
	{ "xkb-options", 0, 0, G_OPTION_ARG_STRING,
	  &option_xkb_options, "XKB Options" },
	{ NULL }
};

static const cairo_user_data_key_t surface_data_key;
struct surface_data {
	struct wl_buffer *buffer;
};

#define MULT(_d,c,a,t) \
	do { t = c * a + 0x7f; _d = ((t >> 8) + t) >> 8; } while (0)

#ifdef HAVE_CAIRO_EGL

struct egl_window_surface_data {
	struct display *display;
	struct wl_surface *surface;
	struct wl_egl_window *window;
	EGLSurface surf;
};

static void
egl_window_surface_data_destroy(void *p)
{
	struct egl_window_surface_data *data = p;
	struct display *d = data->display;

	eglDestroySurface(d->dpy, data->surf);
	wl_egl_window_destroy(data->window);
	data->surface = NULL;

	free(p);
}

static cairo_surface_t *
display_create_egl_window_surface(struct display *display,
				  struct wl_surface *surface,
				  uint32_t flags,
				  struct rectangle *rectangle)
{
	cairo_surface_t *cairo_surface;
	struct egl_window_surface_data *data;
	EGLConfig config;
	const EGLint *attribs;
	cairo_device_t *device;

	static const EGLint premul_attribs[] = {
		EGL_ALPHA_FORMAT, EGL_ALPHA_FORMAT_PRE,
		EGL_NONE
	};
	
	data = malloc(sizeof *data);
	if (data == NULL)
		return NULL;

	data->display = display;
	data->surface = surface;

	if (flags & SURFACE_OPAQUE) {
		config = display->rgb_config;
		device = display->rgb_device;
		attribs = NULL;
	} else {
		config = display->premultiplied_argb_config;
		device = display->argb_device;
		attribs = premul_attribs;
	}

	data->window = wl_egl_window_create(surface,
					    rectangle->width,
					    rectangle->height);

	data->surf = eglCreateWindowSurface(display->dpy, config,
					    data->window, attribs);

	cairo_surface = cairo_gl_surface_create_for_egl(device,
							data->surf,
							rectangle->width,
							rectangle->height);

	cairo_surface_set_user_data(cairo_surface, &surface_data_key,
				    data, egl_window_surface_data_destroy);

	return cairo_surface;
}

struct egl_image_surface_data {
	struct surface_data data;
	cairo_device_t *device;
	EGLImageKHR image;
	GLuint texture;
	struct display *display;
	struct wl_egl_pixmap *pixmap;
};

static void
egl_image_surface_data_destroy(void *p)
{
	struct egl_image_surface_data *data = p;
	struct display *d = data->display;

	cairo_device_acquire(data->device);
	glDeleteTextures(1, &data->texture);
	cairo_device_release(data->device);

	d->destroy_image(d->dpy, data->image);
	wl_buffer_destroy(data->data.buffer);
	wl_egl_pixmap_destroy(data->pixmap);
	free(p);
}

EGLImageKHR
display_get_image_for_egl_image_surface(struct display *display,
					cairo_surface_t *surface)
{
	struct egl_image_surface_data *data;

	data = cairo_surface_get_user_data (surface, &surface_data_key);

	return data->image;
}

static cairo_surface_t *
display_create_egl_image_surface(struct display *display,
				 uint32_t flags,
				 struct rectangle *rectangle)
{
	struct egl_image_surface_data *data;
	EGLDisplay dpy = display->dpy;
	cairo_surface_t *surface;
	EGLConfig config;
	cairo_content_t content;

	data = malloc(sizeof *data);
	if (data == NULL)
		return NULL;

	data->display = display;

	data->pixmap = wl_egl_pixmap_create(rectangle->width,
					    rectangle->height, 0);
	if (data->pixmap == NULL) {
		free(data);
		return NULL;
	}

	if (flags & SURFACE_OPAQUE) {
		data->device = display->rgb_device;
		config = display->rgb_config;
		content = CAIRO_CONTENT_COLOR;
	} else {
		data->device = display->argb_device;
		config = display->premultiplied_argb_config;
		content = CAIRO_CONTENT_COLOR_ALPHA;
	}

	data->image = display->create_image(dpy, NULL,
					    EGL_NATIVE_PIXMAP_KHR,
					    (EGLClientBuffer) data->pixmap,
					    NULL);
	if (data->image == EGL_NO_IMAGE_KHR) {
		wl_egl_pixmap_destroy(data->pixmap);
		free(data);
		return NULL;
	}

	data->data.buffer =
		wl_egl_pixmap_create_buffer(data->pixmap);

	cairo_device_acquire(data->device);
	glGenTextures(1, &data->texture);
	glBindTexture(GL_TEXTURE_2D, data->texture);
	display->image_target_texture_2d(GL_TEXTURE_2D, data->image);
	cairo_device_release(data->device);

	surface = cairo_gl_surface_create_for_texture(data->device,
						      content,
						      data->texture,
						      rectangle->width,
						      rectangle->height);

	cairo_surface_set_user_data (surface, &surface_data_key,
				     data, egl_image_surface_data_destroy);

	return surface;
}

static cairo_surface_t *
display_create_egl_image_surface_from_file(struct display *display,
					   const char *filename,
					   struct rectangle *rect)
{
	cairo_surface_t *surface;
	GdkPixbuf *pixbuf;
	GError *error = NULL;
	int stride, i;
	unsigned char *pixels, *p, *end;
	struct egl_image_surface_data *data;

	pixbuf = gdk_pixbuf_new_from_file_at_scale(filename,
						   rect->width, rect->height,
						   FALSE, &error);
	if (error != NULL)
		return NULL;

	if (!gdk_pixbuf_get_has_alpha(pixbuf) ||
	    gdk_pixbuf_get_n_channels(pixbuf) != 4) {
		g_object_unref(pixbuf);
		return NULL;
	}


	stride = gdk_pixbuf_get_rowstride(pixbuf);
	pixels = gdk_pixbuf_get_pixels(pixbuf);

	for (i = 0; i < rect->height; i++) {
		p = pixels + i * stride;
		end = p + rect->width * 4;
		while (p < end) {
			unsigned int t;

			MULT(p[0], p[0], p[3], t);
			MULT(p[1], p[1], p[3], t);
			MULT(p[2], p[2], p[3], t);
			p += 4;

		}
	}

	surface = display_create_egl_image_surface(display, 0, rect);
	if (surface == NULL) {
		g_object_unref(pixbuf);
		return NULL;
	}

	data = cairo_surface_get_user_data(surface, &surface_data_key);

	cairo_device_acquire(display->argb_device);
	glBindTexture(GL_TEXTURE_2D, data->texture);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, rect->width, rect->height,
			GL_RGBA, GL_UNSIGNED_BYTE, pixels);
	cairo_device_release(display->argb_device);

	g_object_unref(pixbuf);

	return surface;
}

#endif

struct wl_buffer *
display_get_buffer_for_surface(struct display *display,
			       cairo_surface_t *surface)
{
	struct surface_data *data;

	data = cairo_surface_get_user_data (surface, &surface_data_key);

	return data->buffer;
}

struct shm_surface_data {
	struct surface_data data;
	void *map;
	size_t length;
};

static void
shm_surface_data_destroy(void *p)
{
	struct shm_surface_data *data = p;

	wl_buffer_destroy(data->data.buffer);
	munmap(data->map, data->length);
}

static cairo_surface_t *
display_create_shm_surface(struct display *display,
			   struct rectangle *rectangle, uint32_t flags)
{
	struct shm_surface_data *data;
	uint32_t format;
	cairo_surface_t *surface;
	int stride, fd;
	char filename[] = "/tmp/wayland-shm-XXXXXX";

	data = malloc(sizeof *data);
	if (data == NULL)
		return NULL;

	stride = cairo_format_stride_for_width (CAIRO_FORMAT_ARGB32,
						rectangle->width);
	data->length = stride * rectangle->height;
	fd = mkstemp(filename);
	if (fd < 0) {
		fprintf(stderr, "open %s failed: %m\n", filename);
		return NULL;
	}
	if (ftruncate(fd, data->length) < 0) {
		fprintf(stderr, "ftruncate failed: %m\n");
		close(fd);
		return NULL;
	}

	data->map = mmap(NULL, data->length,
			 PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	unlink(filename);

	if (data->map == MAP_FAILED) {
		fprintf(stderr, "mmap failed: %m\n");
		close(fd);
		return NULL;
	}

	surface = cairo_image_surface_create_for_data (data->map,
						       CAIRO_FORMAT_ARGB32,
						       rectangle->width,
						       rectangle->height,
						       stride);

	cairo_surface_set_user_data (surface, &surface_data_key,
				     data, shm_surface_data_destroy);

	if (flags & SURFACE_OPAQUE)
		format = WL_SHM_FORMAT_XRGB32;
	else
		format = WL_SHM_FORMAT_PREMULTIPLIED_ARGB32;

	data->data.buffer = wl_shm_create_buffer(display->shm,
						 fd,
						 rectangle->width,
						 rectangle->height,
						 stride, format);

	close(fd);

	return surface;
}

static cairo_surface_t *
display_create_shm_surface_from_file(struct display *display,
				     const char *filename,
				     struct rectangle *rect)
{
	cairo_surface_t *surface;
	GdkPixbuf *pixbuf;
	GError *error = NULL;
	int stride, i;
	unsigned char *pixels, *p, *end, *dest_data;
	int dest_stride;
	uint32_t *d;

	pixbuf = gdk_pixbuf_new_from_file_at_scale(filename,
						   rect->width, rect->height,
						   FALSE, &error);
	if (error != NULL)
		return NULL;

	if (!gdk_pixbuf_get_has_alpha(pixbuf) ||
	    gdk_pixbuf_get_n_channels(pixbuf) != 4) {
		g_object_unref(pixbuf);
		return NULL;
	}

	stride = gdk_pixbuf_get_rowstride(pixbuf);
	pixels = gdk_pixbuf_get_pixels(pixbuf);

	surface = display_create_shm_surface(display, rect, 0);
	if (surface == NULL) {
		g_object_unref(pixbuf);
		return NULL;
	}

	dest_data = cairo_image_surface_get_data (surface);
	dest_stride = cairo_image_surface_get_stride (surface);

	for (i = 0; i < rect->height; i++) {
		d = (uint32_t *) (dest_data + i * dest_stride);
		p = pixels + i * stride;
		end = p + rect->width * 4;
		while (p < end) {
			unsigned int t;
			unsigned char a, r, g, b;

			a = p[3];
			MULT(r, p[0], a, t);
			MULT(g, p[1], a, t);
			MULT(b, p[2], a, t);
			p += 4;
			*d++ = (a << 24) | (r << 16) | (g << 8) | b;
		}
	}

	g_object_unref(pixbuf);

	return surface;
}

static int
check_size(struct rectangle *rect)
{
	if (rect->width && rect->height)
		return 0;

	fprintf(stderr, "tried to create surface of "
		"width: %d, height: %d\n", rect->width, rect->height);
	return -1;
}

cairo_surface_t *
display_create_surface(struct display *display,
		       struct wl_surface *surface,
		       struct rectangle *rectangle,
		       uint32_t flags)
{
	if (check_size(rectangle) < 0)
		return NULL;
#ifdef HAVE_CAIRO_EGL
	if (display->dpy) {
		if (surface)
			return display_create_egl_window_surface(display,
								 surface,
								 flags,
								 rectangle);
		else
			return display_create_egl_image_surface(display,
								flags,
								rectangle);
	}
#endif
	return display_create_shm_surface(display, rectangle, flags);
}

static cairo_surface_t *
display_create_surface_from_file(struct display *display,
				 const char *filename,
				 struct rectangle *rectangle)
{
	if (check_size(rectangle) < 0)
		return NULL;
#ifdef HAVE_CAIRO_EGL
	if (display->dpy) {
		return display_create_egl_image_surface_from_file(display,
								  filename,
								  rectangle);
	}
#endif
	return display_create_shm_surface_from_file(display, filename, rectangle);
}
 static const struct {
	const char *filename;
	int hotspot_x, hotspot_y;
} pointer_images[] = {
	{ DATADIR "/wayland/bottom_left_corner.png",	 6, 30 },
	{ DATADIR "/wayland/bottom_right_corner.png",	28, 28 },
	{ DATADIR "/wayland/bottom_side.png",		16, 20 },
	{ DATADIR "/wayland/grabbing.png",		20, 17 },
	{ DATADIR "/wayland/left_ptr.png",		10,  5 },
	{ DATADIR "/wayland/left_side.png",		10, 20 },
	{ DATADIR "/wayland/right_side.png",		30, 19 },
	{ DATADIR "/wayland/top_left_corner.png",	 8,  8 },
	{ DATADIR "/wayland/top_right_corner.png",	26,  8 },
	{ DATADIR "/wayland/top_side.png",		18,  8 },
	{ DATADIR "/wayland/xterm.png",			15, 15 },
	{ DATADIR "/wayland/hand1.png",			18, 11 }
};

static void
create_pointer_surfaces(struct display *display)
{
	int i, count;
	const int width = 32, height = 32;
	struct rectangle rect;

	count = ARRAY_LENGTH(pointer_images);
	display->pointer_surfaces =
		malloc(count * sizeof *display->pointer_surfaces);
	rect.width = width;
	rect.height = height;
	for (i = 0; i < count; i++) {
		display->pointer_surfaces[i] =
			display_create_surface_from_file(display,
							 pointer_images[i].filename,
							 &rect);
	}

}

cairo_surface_t *
display_get_pointer_surface(struct display *display, int pointer,
			    int *width, int *height,
			    int *hotspot_x, int *hotspot_y)
{
	cairo_surface_t *surface;

	surface = display->pointer_surfaces[pointer];
#if HAVE_CAIRO_EGL
	*width = cairo_gl_surface_get_width(surface);
	*height = cairo_gl_surface_get_height(surface);
#else
	*width = cairo_image_surface_get_width(surface);
	*height = cairo_image_surface_get_height(surface);
#endif
	*hotspot_x = pointer_images[pointer].hotspot_x;
	*hotspot_y = pointer_images[pointer].hotspot_y;

	return cairo_surface_reference(surface);
}


static void
window_attach_surface(struct window *window);

static void
free_surface(void *data, struct wl_callback *callback, uint32_t time)
{
	struct window *window = data;

	wl_callback_destroy(callback);
	cairo_surface_destroy(window->pending_surface);
	window->pending_surface = NULL;
	if (window->cairo_surface)
		window_attach_surface(window);
}

static const struct wl_callback_listener free_surface_listener = {
	free_surface
};

static void
window_get_resize_dx_dy(struct window *window, int *x, int *y)
{
	if (window->resize_edges & WINDOW_RESIZING_LEFT)
		*x = window->server_allocation.width - window->allocation.width;
	else
		*x = 0;

	if (window->resize_edges & WINDOW_RESIZING_TOP)
		*y = window->server_allocation.height -
			window->allocation.height;
	else
		*y = 0;

	window->resize_edges = 0;
}

static void
window_set_type(struct window *window)
{
	struct display *display = window->display;

	switch (window->type) {
	case TYPE_FULLSCREEN:
		wl_shell_set_fullscreen(display->shell, window->surface);
		break;
	case TYPE_TOPLEVEL:
		wl_shell_set_toplevel(display->shell, window->surface);
		break;
	case TYPE_TRANSIENT:
		wl_shell_set_transient(display->shell, window->surface,
				       window->parent->surface,
				       window->x, window->y, 0);
		break;
	case TYPE_CUSTOM:
		break;
	}
}

static void
window_attach_surface(struct window *window)
{
	struct display *display = window->display;
	struct wl_buffer *buffer;
	struct wl_callback *cb;
#ifdef HAVE_CAIRO_EGL
	struct egl_window_surface_data *data;
#endif
	int32_t x, y;

	if (display->shell)
		window_set_type(window);

	switch (window->buffer_type) {
#ifdef HAVE_CAIRO_EGL
	case WINDOW_BUFFER_TYPE_EGL_WINDOW:
		data = cairo_surface_get_user_data(window->cairo_surface,
						   &surface_data_key);

		cairo_gl_surface_swapbuffers(window->cairo_surface);
		wl_egl_window_get_attached_size(data->window,
				&window->server_allocation.width,
				&window->server_allocation.height);
		break;
	case WINDOW_BUFFER_TYPE_EGL_IMAGE:
#endif
	case WINDOW_BUFFER_TYPE_SHM:
		window_get_resize_dx_dy(window, &x, &y);

		if (window->pending_surface != NULL)
			return;

		window->pending_surface = window->cairo_surface;
		window->cairo_surface = NULL;

		buffer =
			display_get_buffer_for_surface(display,
						       window->pending_surface);

		wl_surface_attach(window->surface, buffer, x, y);
		window->server_allocation = window->allocation;
		cb = wl_display_sync(display->display);
		wl_callback_add_listener(cb, &free_surface_listener, window);
		break;
	default:
		return;
	}

	wl_surface_damage(window->surface, 0, 0,
			  window->allocation.width,
			  window->allocation.height);
}

void
window_flush(struct window *window)
{
	if (window->cairo_surface) {
		switch (window->buffer_type) {
		case WINDOW_BUFFER_TYPE_EGL_IMAGE:
		case WINDOW_BUFFER_TYPE_SHM:
			display_surface_damage(window->display,
					       window->cairo_surface,
					       0, 0,
					       window->allocation.width,
					       window->allocation.height);
			break;
		default:
			break;
		}
		window_attach_surface(window);
	}
}

void
window_set_surface(struct window *window, cairo_surface_t *surface)
{
	cairo_surface_reference(surface);

	if (window->cairo_surface != NULL)
		cairo_surface_destroy(window->cairo_surface);

	window->cairo_surface = surface;
}

#ifdef HAVE_CAIRO_EGL
static void
window_resize_cairo_window_surface(struct window *window)
{
	struct egl_window_surface_data *data;
	int x, y;

	data = cairo_surface_get_user_data(window->cairo_surface,
					   &surface_data_key);

	window_get_resize_dx_dy(window, &x, &y),
	wl_egl_window_resize(data->window,
			     window->allocation.width,
			     window->allocation.height,
			     x,y);

	cairo_gl_surface_set_size(window->cairo_surface,
				  window->allocation.width,
				  window->allocation.height);
}
#endif

struct display *
window_get_display(struct window *window)
{
	return window->display;
}

void
window_create_surface(struct window *window)
{
	cairo_surface_t *surface;
	uint32_t flags = 0;
	
	if (!window->transparent)
		flags = SURFACE_OPAQUE;
	
	switch (window->buffer_type) {
#ifdef HAVE_CAIRO_EGL
	case WINDOW_BUFFER_TYPE_EGL_WINDOW:
		if (window->cairo_surface) {
			window_resize_cairo_window_surface(window);
			return;
		}
		surface = display_create_surface(window->display,
						 window->surface,
						 &window->allocation, flags);
		break;
	case WINDOW_BUFFER_TYPE_EGL_IMAGE:
		surface = display_create_surface(window->display,
						 NULL,
						 &window->allocation, flags);
		break;
#endif
	case WINDOW_BUFFER_TYPE_SHM:
		surface = display_create_shm_surface(window->display,
						     &window->allocation, flags);
		break;
        default:
		surface = NULL;
		break;
	}

	window_set_surface(window, surface);
	cairo_surface_destroy(surface);
}

static void
window_draw_menu(struct window *window)
{
	cairo_t *cr;
	int width, height, r = 5;

	window_create_surface(window);

	cr = cairo_create(window->cairo_surface);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.0);
	cairo_paint(cr);

	width = window->allocation.width;
	height = window->allocation.height;
	rounded_rect(cr, r, r, width - r, height - r, r);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_rgba(cr, 1.0, 1.0, 0.0, 0.5);
	cairo_fill(cr);
	cairo_destroy(cr);
}

static void
window_draw_decorations(struct window *window)
{
	cairo_t *cr;
	cairo_text_extents_t extents;
	cairo_surface_t *frame;
	int width, height, shadow_dx = 3, shadow_dy = 3;

	window_create_surface(window);

	width = window->allocation.width;
	height = window->allocation.height;

	cr = cairo_create(window->cairo_surface);

	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_rgba(cr, 0, 0, 0, 0);
	cairo_paint(cr);

	cairo_set_source_rgba(cr, 0, 0, 0, 0.6);
	tile_mask(cr, window->display->shadow,
		  shadow_dx, shadow_dy, width, height,
		  window->margin + 10 - shadow_dx,
		  window->margin + 10 - shadow_dy);

	if (window->keyboard_device)
		frame = window->display->active_frame;
	else
		frame = window->display->inactive_frame;

	tile_source(cr, frame, 0, 0, width, height,
		    window->margin + 10, window->margin + 50);

	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
	cairo_set_font_size(cr, 14);
	cairo_text_extents(cr, window->title, &extents);
	cairo_move_to(cr, (width - extents.width) / 2, 32 - extents.y_bearing);
	if (window->keyboard_device)
		cairo_set_source_rgb(cr, 0, 0, 0);
	else
		cairo_set_source_rgb(cr, 0.8, 0.8, 0.8);
	cairo_show_text(cr, window->title);

	cairo_destroy(cr);

	/* FIXME: this breakes gears, fix cairo? */
#if 0 
	cairo_device_flush (window->display->device);
#endif
}

void
window_destroy(struct window *window)
{
	wl_surface_destroy(window->surface);
	wl_list_remove(&window->link);
	free(window);
}

static struct item *
window_find_item(struct window *window, int32_t x, int32_t y)
{
	struct item *item;

	wl_list_for_each(item, &window->item_list, link) {
		if (item->allocation.x <= x &&
		    x < item->allocation.x + item->allocation.width &&
		    item->allocation.y <= y &&
		    y < item->allocation.y + item->allocation.height) {
			return item;
		}
	}

	return NULL;
}

struct item *
window_add_item(struct window *window, void *data)
{
	struct item *item;

	item = malloc(sizeof *item);
	memset(item, 0, sizeof *item);
	item->user_data = data;
	wl_list_insert(window->item_list.prev, &item->link);

	return item;
}

void
window_for_each_item(struct window *window, item_func_t func, void *data)
{
	struct item *item;

	wl_list_for_each(item, &window->item_list, link)
		func(item, data);
}

struct item *
window_get_focus_item(struct window *window)
{
	return window->focus_item;
}

void
item_get_allocation(struct item *item, struct rectangle *allocation)
{
	*allocation = item->allocation;
}

void
item_set_allocation(struct item *item,
		    int32_t x, int32_t y, int32_t width, int32_t height)
{
	item->allocation.x = x;
	item->allocation.y = y;
	item->allocation.width = width;
	item->allocation.height = height;
}

void *
item_get_user_data(struct item *item)
{
	return item->user_data;
}

void
window_draw(struct window *window)
{
	if (window->parent)
		window_draw_menu(window);
	else if (!window->decoration)
		window_create_surface(window);
	else
		window_draw_decorations(window);
}

cairo_surface_t *
window_get_surface(struct window *window)
{
	return cairo_surface_reference(window->cairo_surface);
}

struct wl_surface *
window_get_wl_surface(struct window *window)
{
	return window->surface;
}

static int
get_pointer_location(struct window *window, int32_t x, int32_t y)
{
	int vlocation, hlocation, location;
	const int grip_size = 8;

	if (!window->decoration)
		return WINDOW_CLIENT_AREA;

	if (x < window->margin)
		hlocation = WINDOW_EXTERIOR;
	else if (window->margin <= x && x < window->margin + grip_size)
		hlocation = WINDOW_RESIZING_LEFT;
	else if (x < window->allocation.width - window->margin - grip_size)
		hlocation = WINDOW_INTERIOR;
	else if (x < window->allocation.width - window->margin)
		hlocation = WINDOW_RESIZING_RIGHT;
	else
		hlocation = WINDOW_EXTERIOR;

	if (y < window->margin)
		vlocation = WINDOW_EXTERIOR;
	else if (window->margin <= y && y < window->margin + grip_size)
		vlocation = WINDOW_RESIZING_TOP;
	else if (y < window->allocation.height - window->margin - grip_size)
		vlocation = WINDOW_INTERIOR;
	else if (y < window->allocation.height - window->margin)
		vlocation = WINDOW_RESIZING_BOTTOM;
	else
		vlocation = WINDOW_EXTERIOR;

	location = vlocation | hlocation;
	if (location & WINDOW_EXTERIOR)
		location = WINDOW_EXTERIOR;
	if (location == WINDOW_INTERIOR && y < window->margin + 50)
		location = WINDOW_TITLEBAR;
	else if (location == WINDOW_INTERIOR)
		location = WINDOW_CLIENT_AREA;

	return location;
}

static void
set_pointer_image(struct input *input, uint32_t time, int pointer)
{
	struct display *display = input->display;
	struct wl_buffer *buffer;
	cairo_surface_t *surface;
	int location;

	location = get_pointer_location(input->pointer_focus,
					input->sx, input->sy);
	switch (location) {
	case WINDOW_RESIZING_TOP:
		pointer = POINTER_TOP;
		break;
	case WINDOW_RESIZING_BOTTOM:
		pointer = POINTER_BOTTOM;
		break;
	case WINDOW_RESIZING_LEFT:
		pointer = POINTER_LEFT;
		break;
	case WINDOW_RESIZING_RIGHT:
		pointer = POINTER_RIGHT;
		break;
	case WINDOW_RESIZING_TOP_LEFT:
		pointer = POINTER_TOP_LEFT;
		break;
	case WINDOW_RESIZING_TOP_RIGHT:
		pointer = POINTER_TOP_RIGHT;
		break;
	case WINDOW_RESIZING_BOTTOM_LEFT:
		pointer = POINTER_BOTTOM_LEFT;
		break;
	case WINDOW_RESIZING_BOTTOM_RIGHT:
		pointer = POINTER_BOTTOM_RIGHT;
		break;
	case WINDOW_EXTERIOR:
	case WINDOW_TITLEBAR:
		if (input->current_pointer_image == POINTER_DEFAULT)
			return;

		wl_input_device_attach(input->input_device, time, NULL, 0, 0);
		input->current_pointer_image = POINTER_DEFAULT;
		return;
	default:
		break;
	}

	if (pointer == input->current_pointer_image)
		return;

	input->current_pointer_image = pointer;
	surface = display->pointer_surfaces[pointer];
	buffer = display_get_buffer_for_surface(display, surface);
	wl_input_device_attach(input->input_device, time, buffer,
			       pointer_images[pointer].hotspot_x,
			       pointer_images[pointer].hotspot_y);
}

static void
window_set_focus_item(struct window *window, struct item *focus)
{
	void *data;

	if (focus == window->focus_item)
		return;

	window->focus_item = focus;
	data = focus ? focus->user_data : NULL;
	if (window->item_focus_handler)
		window->item_focus_handler(window, focus, data);
}

static void
window_handle_motion(void *data, struct wl_input_device *input_device,
		     uint32_t time,
		     int32_t x, int32_t y, int32_t sx, int32_t sy)
{
	struct input *input = data;
	struct window *window = input->pointer_focus;
	struct item *item;
	int pointer = POINTER_LEFT_PTR;

	input->x = x;
	input->y = y;
	input->sx = sx;
	input->sy = sy;

	if (!window->focus_item || !window->item_grab_button) {
		item = window_find_item(window, sx, sy);
		window_set_focus_item(window, item);
	}

	if (window->motion_handler)
		pointer = (*window->motion_handler)(window, input, time,
						    x, y, sx, sy,
						    window->user_data);

	set_pointer_image(input, time, pointer);
}

static void
window_handle_button(void *data,
		     struct wl_input_device *input_device,
		     uint32_t time, uint32_t button, uint32_t state)
{
	struct input *input = data;
	struct window *window = input->pointer_focus;
	struct item *item;
	int location;

	if (window->focus_item && window->item_grab_button == 0 && state)
		window->item_grab_button = button;

	location = get_pointer_location(window, input->sx, input->sy);

	if (window->display->shell &&
	    button == BTN_LEFT && state == 1) {
		switch (location) {
		case WINDOW_TITLEBAR:
			wl_shell_move(window->display->shell,
				      window->surface, input_device, time);
			break;
		case WINDOW_RESIZING_TOP:
		case WINDOW_RESIZING_BOTTOM:
		case WINDOW_RESIZING_LEFT:
		case WINDOW_RESIZING_RIGHT:
		case WINDOW_RESIZING_TOP_LEFT:
		case WINDOW_RESIZING_TOP_RIGHT:
		case WINDOW_RESIZING_BOTTOM_LEFT:
		case WINDOW_RESIZING_BOTTOM_RIGHT:
			wl_shell_resize(window->display->shell,
					window->surface, input_device, time,
					location);
			break;
		case WINDOW_CLIENT_AREA:
			if (window->button_handler)
				(*window->button_handler)(window,
							  input, time,
							  button, state,
							  window->user_data);
			break;
		}
	} else {
		if (window->button_handler)
			(*window->button_handler)(window,
						  input, time,
						  button, state,
						  window->user_data);
	}

	if (window->focus_item &&
	    window->item_grab_button == button && !state) {
		window->item_grab_button = 0;
		item = window_find_item(window, input->sx, input->sy);
		window_set_focus_item(window, item);
	}
}

static void
window_handle_key(void *data, struct wl_input_device *input_device,
		  uint32_t time, uint32_t key, uint32_t state)
{
	struct input *input = data;
	struct window *window = input->keyboard_focus;
	struct display *d = window->display;
	uint32_t code, sym, level;

	code = key + d->xkb->min_key_code;
	if (window->keyboard_device != input)
		return;

	level = 0;
	if (input->modifiers & XKB_COMMON_SHIFT_MASK &&
	    XkbKeyGroupWidth(d->xkb, code, 0) > 1)
		level = 1;

	sym = XkbKeySymEntry(d->xkb, code, level, 0);

	if (state)
		input->modifiers |= d->xkb->map->modmap[code];
	else
		input->modifiers &= ~d->xkb->map->modmap[code];

	if (window->key_handler)
		(*window->key_handler)(window, input, time, key, sym, state,
				       window->user_data);
}

static void
window_handle_pointer_focus(void *data,
			    struct wl_input_device *input_device,
			    uint32_t time, struct wl_surface *surface,
			    int32_t x, int32_t y, int32_t sx, int32_t sy)
{
	struct input *input = data;
	struct window *window;
	struct item *item;
	int pointer;

	window = input->pointer_focus;
	if (window && window->surface != surface) {
		window_set_focus_item(window, NULL);

		if (window->leave_handler)
			window->leave_handler(window, input,
					      time, window->user_data);
		input->pointer_focus = NULL;
		input->current_pointer_image = POINTER_UNSET;
	}

	if (surface) {
		input->pointer_focus = wl_surface_get_user_data(surface);
		window = input->pointer_focus;

		input->x = x;
		input->y = y;
		input->sx = sx;
		input->sy = sy;

		pointer = POINTER_LEFT_PTR;
		if (window->enter_handler)
			pointer = window->enter_handler(window, input,
							time, sx, sy,
							window->user_data);

		item = window_find_item(window, x, y);
		window_set_focus_item(window, item);

		set_pointer_image(input, time, pointer);
	}
}

static void
window_handle_keyboard_focus(void *data,
			     struct wl_input_device *input_device,
			     uint32_t time,
			     struct wl_surface *surface,
			     struct wl_array *keys)
{
	struct input *input = data;
	struct window *window = input->keyboard_focus;
	struct display *d = input->display;
	uint32_t *k, *end;

	window = input->keyboard_focus;
	if (window) {
		window->keyboard_device = NULL;
		if (window->keyboard_focus_handler)
			(*window->keyboard_focus_handler)(window, NULL,
							  window->user_data);
	}

	if (surface)
		input->keyboard_focus = wl_surface_get_user_data(surface);
	else
		input->keyboard_focus = NULL;

	end = keys->data + keys->size;
	input->modifiers = 0;
	for (k = keys->data; k < end; k++)
		input->modifiers |= d->xkb->map->modmap[*k];

	window = input->keyboard_focus;
	if (window) {
		window->keyboard_device = input;
		if (window->keyboard_focus_handler)
			(*window->keyboard_focus_handler)(window,
							  window->keyboard_device,
							  window->user_data);
	}
}

static const struct wl_input_device_listener input_device_listener = {
	window_handle_motion,
	window_handle_button,
	window_handle_key,
	window_handle_pointer_focus,
	window_handle_keyboard_focus,
};

void
input_get_position(struct input *input, int32_t *x, int32_t *y)
{
	*x = input->sx;
	*y = input->sy;
}

struct wl_input_device *
input_get_input_device(struct input *input)
{
	return input->input_device;
}

uint32_t
input_get_modifiers(struct input *input)
{
	return input->modifiers;
}

struct wl_drag *
window_create_drag(struct window *window)
{
	cairo_device_flush (window->display->rgb_device);
	cairo_device_flush (window->display->argb_device);

	return wl_shell_create_drag(window->display->shell);
}

void
window_move(struct window *window, struct input *input, uint32_t time)
{
	if (window->display->shell)
		wl_shell_move(window->display->shell,
			      window->surface, input->input_device, time);
}

void
window_activate_drag(struct wl_drag *drag, struct window *window,
		     struct input *input, uint32_t time)
{
	wl_drag_activate(drag, window->surface, input->input_device, time);
}

static void
handle_configure(void *data, struct wl_shell *shell,
		 uint32_t time, uint32_t edges,
		 struct wl_surface *surface, int32_t width, int32_t height)
{
	struct window *window = wl_surface_get_user_data(surface);
	int32_t child_width, child_height;

	/* FIXME: this is probably the wrong place to check for width
	 * or height <= 0, but it prevents the compositor from crashing
	 */
	if (width <= 0 || height <= 0)
		return;

	window->resize_edges = edges;

	if (window->resize_handler) {
		child_width = width - 20 - window->margin * 2;
		child_height = height - 60 - window->margin * 2;

		(*window->resize_handler)(window,
					  child_width, child_height,
					  window->user_data);
	} else {
		window->allocation.width = width;
		window->allocation.height = height;

		if (window->redraw_handler)
			window_schedule_redraw(window);
	}
}

static const struct wl_shell_listener shell_listener = {
	handle_configure,
};

void
window_get_allocation(struct window *window,
		      struct rectangle *allocation)
{
	*allocation = window->allocation;
}

void
window_get_child_allocation(struct window *window,
			    struct rectangle *allocation)
{
	if (!window->decoration) {
		*allocation = window->allocation;
	} else {
		allocation->x = window->margin + 10;
		allocation->y = window->margin + 50;
		allocation->width =
			window->allocation.width - 20 - window->margin * 2;
		allocation->height =
			window->allocation.height - 60 - window->margin * 2;
	}
}

void
window_set_child_size(struct window *window, int32_t width, int32_t height)
{
	if (window->decoration) {
		window->allocation.x = 20 + window->margin;
		window->allocation.y = 60 + window->margin;
		window->allocation.width = width + 20 + window->margin * 2;
		window->allocation.height = height + 60 + window->margin * 2;
	} else {
		window->allocation.x = 0;
		window->allocation.y = 0;
		window->allocation.width = width;
		window->allocation.height = height;
	}
}

static void
idle_redraw(struct task *task, uint32_t events)
{
	struct window *window =
		container_of(task, struct window, redraw_task);

	window->redraw_handler(window, window->user_data);
	window->redraw_scheduled = 0;
}

void
window_schedule_redraw(struct window *window)
{
	if (!window->redraw_scheduled) {
		window->redraw_task.run = idle_redraw;
		display_defer(window->display, &window->redraw_task);
		window->redraw_scheduled = 1;
	}
}

void
window_set_custom(struct window *window)
{
	window->type = TYPE_CUSTOM;
}

void
window_set_fullscreen(struct window *window, int fullscreen)
{
	int32_t width, height;

	if ((window->type == TYPE_FULLSCREEN) == fullscreen)
		return;

	if (fullscreen) {
		window->type = TYPE_FULLSCREEN;
		window->saved_allocation = window->allocation;
		width = window->display->screen_allocation.width;
		height = window->display->screen_allocation.height;
		window->decoration = 0;
	} else {
		window->type = TYPE_TOPLEVEL;
		width = window->saved_allocation.width - 20 - window->margin * 2;
		height = window->saved_allocation.height - 60 - window->margin * 2;
		window->decoration = 1;
	}

	(*window->resize_handler)(window, width, height, window->user_data);
}

void
window_set_decoration(struct window *window, int decoration)
{
	window->decoration = decoration;
}

void
window_set_user_data(struct window *window, void *data)
{
	window->user_data = data;
}

void *
window_get_user_data(struct window *window)
{
	return window->user_data;
}

void
window_set_resize_handler(struct window *window,
			  window_resize_handler_t handler)
{
	window->resize_handler = handler;
}

void
window_set_redraw_handler(struct window *window,
			  window_redraw_handler_t handler)
{
	window->redraw_handler = handler;
}

void
window_set_key_handler(struct window *window,
		       window_key_handler_t handler)
{
	window->key_handler = handler;
}

void
window_set_button_handler(struct window *window,
			  window_button_handler_t handler)
{
	window->button_handler = handler;
}

void
window_set_motion_handler(struct window *window,
			  window_motion_handler_t handler)
{
	window->motion_handler = handler;
}

void
window_set_enter_handler(struct window *window,
			  window_enter_handler_t handler)
{
	window->enter_handler = handler;
}

void
window_set_leave_handler(struct window *window,
			  window_leave_handler_t handler)
{
	window->leave_handler = handler;
}

void
window_set_keyboard_focus_handler(struct window *window,
				  window_keyboard_focus_handler_t handler)
{
	window->keyboard_focus_handler = handler;
}

void
window_set_item_focus_handler(struct window *window,
			      window_item_focus_handler_t handler)
{
	window->item_focus_handler = handler;
}

void
window_set_transparent(struct window *window, int transparent)
{
	window->transparent = transparent;
}

void
window_set_title(struct window *window, const char *title)
{
	free(window->title);
	window->title = strdup(title);
}

const char *
window_get_title(struct window *window)
{
	return window->title;
}

void
display_surface_damage(struct display *display, cairo_surface_t *cairo_surface,
		       int32_t x, int32_t y, int32_t width, int32_t height)
{
	struct wl_buffer *buffer;

	buffer = display_get_buffer_for_surface(display, cairo_surface);

	wl_buffer_damage(buffer, x, y, width, height);
}

void
window_damage(struct window *window, int32_t x, int32_t y,
	      int32_t width, int32_t height)
{
	wl_surface_damage(window->surface, x, y, width, height);
}

static struct window *
window_create_internal(struct display *display, struct window *parent,
			int32_t width, int32_t height)
{
	struct window *window;

	window = malloc(sizeof *window);
	if (window == NULL)
		return NULL;

	memset(window, 0, sizeof *window);
	window->display = display;
	window->parent = parent;
	window->surface = wl_compositor_create_surface(display->compositor);
	window->allocation.x = 0;
	window->allocation.y = 0;
	window->allocation.width = width;
	window->allocation.height = height;
	window->saved_allocation = window->allocation;
	window->margin = 16;
	window->decoration = 1;
	window->transparent = 1;
	wl_list_init(&window->item_list);

	if (display->dpy)
#ifdef HAVE_CAIRO_EGL
		/* FIXME: make TYPE_EGL_IMAGE choosable for testing */
		window->buffer_type = WINDOW_BUFFER_TYPE_EGL_WINDOW;
#else
		window->buffer_type = WINDOW_BUFFER_TYPE_SHM;
#endif
	else
		window->buffer_type = WINDOW_BUFFER_TYPE_SHM;

	wl_surface_set_user_data(window->surface, window);
	wl_list_insert(display->window_list.prev, &window->link);

	return window;
}

struct window *
window_create(struct display *display, int32_t width, int32_t height)
{
	struct window *window;

	window = window_create_internal(display, NULL, width, height);
	if (!window)
		return NULL;

	return window;
}

struct window *
window_create_transient(struct display *display, struct window *parent,
			int32_t x, int32_t y, int32_t width, int32_t height)
{
	struct window *window;

	window = window_create_internal(parent->display,
					parent, width, height);
	if (!window)
		return NULL;

	window->type = TYPE_TRANSIENT;
	window->x = x;
	window->y = y;

	return window;
}

void
window_set_buffer_type(struct window *window, enum window_buffer_type type)
{
	window->buffer_type = type;
}


static void
display_handle_geometry(void *data,
			struct wl_output *wl_output,
			int x, int y,
			int physical_width,
			int physical_height,
			int subpixel,
			const char *make,
			const char *model)
{
	struct display *display = data;

	display->screen_allocation.x = x;
	display->screen_allocation.y = y;
}

static void
display_handle_mode(void *data,
		    struct wl_output *wl_output,
		    uint32_t flags,
		    int width,
		    int height,
		    int refresh)
{
	struct display *display = data;

	display->screen_allocation.width = width;
	display->screen_allocation.height = height;
}

static const struct wl_output_listener output_listener = {
	display_handle_geometry,
	display_handle_mode
};

static void
display_add_input(struct display *d, uint32_t id)
{
	struct input *input;

	input = malloc(sizeof *input);
	if (input == NULL)
		return;

	memset(input, 0, sizeof *input);
	input->display = d;
	input->input_device =
		wl_display_bind(d->display, id, &wl_input_device_interface);
	input->pointer_focus = NULL;
	input->keyboard_focus = NULL;
	wl_list_insert(d->input_list.prev, &input->link);

	wl_input_device_add_listener(input->input_device,
				     &input_device_listener, input);
	wl_input_device_set_user_data(input->input_device, input);
}

struct selection_offer {
	struct display *display;
	struct wl_selection_offer *offer;
	struct wl_array types;
	struct input *input;
};

int
input_offers_mime_type(struct input *input, const char *type)
{
	struct selection_offer *offer = input->offer;
	char **p, **end;

	if (offer == NULL)
		return 0;

	end = offer->types.data + offer->types.size;
	for (p = offer->types.data; p < end; p++)
		if (strcmp(*p, type) == 0)
			return 1;

	return 0;
}

void
input_receive_mime_type(struct input *input, const char *type, int fd)
{
	struct selection_offer *offer = input->offer;

	/* FIXME: A number of things can go wrong here: the object may
	 * not be the current selection offer any more (which could
	 * still work, but the source may have gone away or just
	 * destroyed its wl_selection) or the offer may not have the
	 * requested type after all (programmer/client error,
	 * typically) */
	wl_selection_offer_receive(offer->offer, type, fd);
}

static void
selection_offer_offer(void *data,
		      struct wl_selection_offer *selection_offer,
		      const char *type)
{
	struct selection_offer *offer = data;

	char **p;

	p = wl_array_add(&offer->types, sizeof *p);
	if (p)
		*p = strdup(type);
};

static void
selection_offer_keyboard_focus(void *data,
			       struct wl_selection_offer *selection_offer,
			       struct wl_input_device *input_device)
{
	struct selection_offer *offer = data;
	struct input *input;
	char **p, **end;

	if (input_device == NULL) {
		printf("selection offer retracted %p\n", selection_offer);
		input = offer->input;
		input->offer = NULL;
		wl_selection_offer_destroy(selection_offer);
		wl_array_release(&offer->types);
		free(offer);
		return;
	}

	input = wl_input_device_get_user_data(input_device);
	printf("new selection offer %p:", selection_offer);

	offer->input = input;
	input->offer = offer;
	end = offer->types.data + offer->types.size;
	for (p = offer->types.data; p < end; p++)
		printf(" %s", *p);

	printf("\n");
}

struct wl_selection_offer_listener selection_offer_listener = {
	selection_offer_offer,
	selection_offer_keyboard_focus
};

static void
add_selection_offer(struct display *d, uint32_t id)
{
	struct selection_offer *offer;

	offer = malloc(sizeof *offer);
	if (offer == NULL)
		return;

	offer->offer =
		wl_display_bind(d->display, id, &wl_selection_offer_interface);
	offer->display = d;
	wl_array_init(&offer->types);
	offer->input = NULL;

	wl_selection_offer_add_listener(offer->offer,
					&selection_offer_listener, offer);
}

static void
display_handle_global(struct wl_display *display, uint32_t id,
		      const char *interface, uint32_t version, void *data)
{
	struct display *d = data;

	if (strcmp(interface, "wl_compositor") == 0) {
		d->compositor =
			wl_display_bind(display, id, &wl_compositor_interface);
	} else if (strcmp(interface, "wl_output") == 0) {
		d->output = wl_display_bind(display, id, &wl_output_interface);
		wl_output_add_listener(d->output, &output_listener, d);
	} else if (strcmp(interface, "wl_input_device") == 0) {
		display_add_input(d, id);
	} else if (strcmp(interface, "wl_shell") == 0) {
		d->shell = wl_display_bind(display, id, &wl_shell_interface);
		wl_shell_add_listener(d->shell, &shell_listener, d);
	} else if (strcmp(interface, "wl_shm") == 0) {
		d->shm = wl_display_bind(display, id, &wl_shm_interface);
	} else if (strcmp(interface, "wl_selection_offer") == 0) {
		add_selection_offer(d, id);
	}
}

static void
display_render_frame(struct display *d)
{
	int radius = 8;
	cairo_t *cr;

	d->shadow = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, 128, 128);
	cr = cairo_create(d->shadow);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
	cairo_set_source_rgba(cr, 0, 0, 0, 1);
	rounded_rect(cr, 16, 16, 112, 112, radius);
	cairo_fill(cr);
	cairo_destroy(cr);
	blur_surface(d->shadow, 64);

	d->active_frame =
		cairo_image_surface_create (CAIRO_FORMAT_ARGB32, 128, 128);
	cr = cairo_create(d->active_frame);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
	cairo_set_source_rgba(cr, 0.8, 0.8, 0.4, 1);
	rounded_rect(cr, 16, 16, 112, 112, radius);
	cairo_fill(cr);
	cairo_destroy(cr);

	d->inactive_frame =
		cairo_image_surface_create (CAIRO_FORMAT_ARGB32, 128, 128);
	cr = cairo_create(d->inactive_frame);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
	cairo_set_source_rgba(cr, 0.6, 0.6, 0.6, 1);
	rounded_rect(cr, 16, 16, 112, 112, radius);
	cairo_fill(cr);
	cairo_destroy(cr);
}

static void
init_xkb(struct display *d)
{
	struct xkb_rule_names names;

	names.rules = "evdev";
	names.model = "pc105";
	names.layout = option_xkb_layout;
	names.variant = option_xkb_variant;
	names.options = option_xkb_options;

	d->xkb = xkb_compile_keymap_from_rules(&names);
	if (!d->xkb) {
		fprintf(stderr, "Failed to compile keymap\n");
		exit(1);
	}
}

static int
init_egl(struct display *d)
{
	EGLint major, minor;
	EGLint n;

	static const EGLint premul_argb_cfg_attribs[] = {
		EGL_SURFACE_TYPE,
			EGL_WINDOW_BIT | EGL_PIXMAP_BIT |
			EGL_VG_ALPHA_FORMAT_PRE_BIT,
		EGL_RED_SIZE, 1,
		EGL_GREEN_SIZE, 1,
		EGL_BLUE_SIZE, 1,
		EGL_ALPHA_SIZE, 1,
		EGL_DEPTH_SIZE, 1,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
		EGL_NONE
	};

	static const EGLint rgb_cfg_attribs[] = {
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT | EGL_PIXMAP_BIT,
		EGL_RED_SIZE, 1,
		EGL_GREEN_SIZE, 1,
		EGL_BLUE_SIZE, 1,
		EGL_ALPHA_SIZE, 0,
		EGL_DEPTH_SIZE, 1,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
		EGL_NONE
	};

	d->dpy = eglGetDisplay(d->display);
	if (!eglInitialize(d->dpy, &major, &minor)) {
		fprintf(stderr, "failed to initialize display\n");
		return -1;
	}

	if (!eglBindAPI(EGL_OPENGL_API)) {
		fprintf(stderr, "failed to bind api EGL_OPENGL_API\n");
		return -1;
	}

	if (!eglChooseConfig(d->dpy, premul_argb_cfg_attribs,
			     &d->premultiplied_argb_config, 1, &n) || n != 1) {
		fprintf(stderr, "failed to choose premul argb config\n");
		return -1;
	}

	if (!eglChooseConfig(d->dpy, rgb_cfg_attribs,
			     &d->rgb_config, 1, &n) || n != 1) {
		fprintf(stderr, "failed to choose rgb config\n");
		return -1;
	}

	d->rgb_ctx = eglCreateContext(d->dpy, d->rgb_config, EGL_NO_CONTEXT, NULL);
	if (d->rgb_ctx == NULL) {
		fprintf(stderr, "failed to create context\n");
		return -1;
	}
	d->argb_ctx = eglCreateContext(d->dpy, d->premultiplied_argb_config,
				       EGL_NO_CONTEXT, NULL);
	if (d->argb_ctx == NULL) {
		fprintf(stderr, "failed to create context\n");
		return -1;
	}

	if (!eglMakeCurrent(d->dpy, NULL, NULL, d->rgb_ctx)) {
		fprintf(stderr, "failed to make context current\n");
		return -1;
	}

#ifdef HAVE_CAIRO_EGL
	d->rgb_device = cairo_egl_device_create(d->dpy, d->rgb_ctx);
	if (cairo_device_status(d->rgb_device) != CAIRO_STATUS_SUCCESS) {
		fprintf(stderr, "failed to get cairo egl device\n");
		return -1;
	}
	d->argb_device = cairo_egl_device_create(d->dpy, d->argb_ctx);
	if (cairo_device_status(d->argb_device) != CAIRO_STATUS_SUCCESS) {
		fprintf(stderr, "failed to get cairo egl argb device\n");
		return -1;
	}
#endif

	return 0;
}

static int
event_mask_update(uint32_t mask, void *data)
{
	struct display *d = data;

	d->mask = mask;

	return 0;
}

static void
handle_display_data(struct task *task, uint32_t events)
{
	struct display *display =
		container_of(task, struct display, display_task);
	
	wl_display_iterate(display->display, display->mask);
}

struct display *
display_create(int *argc, char **argv[], const GOptionEntry *option_entries)
{
	struct display *d;
	GOptionContext *context;
	GOptionGroup *xkb_option_group;
	GError *error;

	g_type_init();

	context = g_option_context_new(NULL);
	if (option_entries)
		g_option_context_add_main_entries(context, option_entries, "Wayland View");

	xkb_option_group = g_option_group_new("xkb",
					      "Keyboard options",
					      "Show all XKB options",
					      NULL, NULL);
	g_option_group_add_entries(xkb_option_group, xkb_option_entries);
	g_option_context_add_group (context, xkb_option_group);

	if (!g_option_context_parse(context, argc, argv, &error)) {
		fprintf(stderr, "option parsing failed: %s\n", error->message);
		exit(EXIT_FAILURE);
	}

        g_option_context_free(context);

	d = malloc(sizeof *d);
	if (d == NULL)
		return NULL;

        memset(d, 0, sizeof *d);

	d->display = wl_display_connect(NULL);
	if (d->display == NULL) {
		fprintf(stderr, "failed to create display: %m\n");
		return NULL;
	}

	d->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
	d->display_fd = wl_display_get_fd(d->display, event_mask_update, d);
	d->display_task.run = handle_display_data;
	display_watch_fd(d, d->display_fd, EPOLLIN, &d->display_task);

	wl_list_init(&d->deferred_list);
	wl_list_init(&d->input_list);

	/* Set up listener so we'll catch all events. */
	wl_display_add_global_listener(d->display,
				       display_handle_global, d);

	/* Process connection events. */
	wl_display_iterate(d->display, WL_DISPLAY_READABLE);
	if (init_egl(d) < 0)
		return NULL;

	d->image_target_texture_2d =
		(void *) eglGetProcAddress("glEGLImageTargetTexture2DOES");
	d->create_image = (void *) eglGetProcAddress("eglCreateImageKHR");
	d->destroy_image = (void *) eglGetProcAddress("eglDestroyImageKHR");

	create_pointer_surfaces(d);

	display_render_frame(d);

	wl_list_init(&d->window_list);

	init_xkb(d);

	return d;
}

struct wl_display *
display_get_display(struct display *display)
{
	return display->display;
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

EGLConfig
display_get_rgb_egl_config(struct display *d)
{
	return d->rgb_config;
}

EGLConfig
display_get_argb_egl_config(struct display *d)
{
	return d->premultiplied_argb_config;
}

struct wl_shell *
display_get_shell(struct display *display)
{
	return display->shell;
}

int
display_acquire_window_surface(struct display *display,
			       struct window *window,
			       EGLContext ctx)
{
#ifdef HAVE_CAIRO_EGL
	struct egl_window_surface_data *data;
	cairo_device_t *device;

	if (!window->cairo_surface)
		return -1;
	device = cairo_surface_get_device(window->cairo_surface);
	if (!device)
		return -1;

	if (!ctx) {
		if (device == display->rgb_device)
			ctx = display->rgb_ctx;
		else if (device == display->argb_device)
			ctx = display->argb_ctx;
		else
			assert(0);
	}

	data = cairo_surface_get_user_data(window->cairo_surface,
					   &surface_data_key);

	cairo_device_acquire(device);
	if (!eglMakeCurrent(display->dpy, data->surf, data->surf, ctx))
		fprintf(stderr, "failed to make surface current\n");

	return 0;
#else
	return -1;
#endif
}

void
display_release_window_surface(struct display *display,
			       struct window *window)
{
#ifdef HAVE_CAIRO_EGL
	cairo_device_t *device;
	
	device = cairo_surface_get_device(window->cairo_surface);
	if (!device)
		return;

	if (!eglMakeCurrent(display->dpy, NULL, NULL, display->rgb_ctx))
		fprintf(stderr, "failed to make context current\n");
	cairo_device_release(device);
#endif
}

void
display_defer(struct display *display, struct task *task)
{
	wl_list_insert(&display->deferred_list, &task->link);
}

void
display_watch_fd(struct display *display,
		 int fd, uint32_t events, struct task *task)
{
	struct epoll_event ep;

	ep.events = events;
	ep.data.ptr = task;
	epoll_ctl(display->epoll_fd, EPOLL_CTL_ADD, fd, &ep);
}

void
display_run(struct display *display)
{
	struct task *task;
	struct epoll_event ep[16];
	int i, count;

	while (1) {
		while (display->mask & WL_DISPLAY_WRITABLE)
			wl_display_iterate(display->display,
					   WL_DISPLAY_WRITABLE);

		count = epoll_wait(display->epoll_fd,
				   ep, ARRAY_LENGTH(ep), -1);
		for (i = 0; i < count; i++) {
			task = ep[i].data.ptr;
			task->run(task, ep[i].events);
		}

		while (!wl_list_empty(&display->deferred_list)) {
			task = container_of(display->deferred_list.next,
					    struct task, link);
			wl_list_remove(&task->link);
			task->run(task, 0);
		}
	}
}
