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
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <glib.h>

#include "wayland-client.h"
#include "wayland-glib.h"
#include "screenshooter-client-protocol.h"

/* The screenshooter is a good example of a custom object exposed by
 * the compositor and serves as a test bed for implementing client
 * side marshalling outside libwayland.so */

static struct wl_output *output;
static struct wl_shm *shm;
static struct wl_visual *visual;
static struct screenshooter *screenshooter;
static int output_width, output_height;

static void
display_handle_geometry(void *data,
			struct wl_output *output,
			int32_t x, int32_t y, int32_t width, int32_t height)
{
	output_width = width;
	output_height = height;
}

static const struct wl_output_listener output_listener = {
	display_handle_geometry,
};

static void
handle_global(struct wl_display *display, uint32_t id,
	      const char *interface, uint32_t version, void *data)
{
	static int visual_count;

	if (strcmp(interface, "wl_output") == 0) {
		output = wl_output_create(display, id, 1);
		wl_output_add_listener(output, &output_listener, NULL);
	} else if (strcmp(interface, "wl_shm") == 0) {
		shm = wl_shm_create(display, id, 1);
	} else if (strcmp(interface, "wl_visual") == 0) {
		if  (visual_count++ == 1)
			visual = wl_visual_create(display, id, 1);
	} else if (strcmp(interface, "screenshooter") == 0) {
		screenshooter = screenshooter_create(display, id, 1);
	}
}

static void
sync_callback(void *data)
{
   int *done = data;

   *done = 1;
}

static void
roundtrip(struct wl_display *display)
{
	int done;

	done = 0;
	wl_display_sync_callback(display, sync_callback, &done);
	wl_display_iterate(display, WL_DISPLAY_WRITABLE);
	while (!done)
		wl_display_iterate(display, WL_DISPLAY_READABLE);
}

static struct wl_buffer *
create_shm_buffer(int width, int height, void **data_out)
{
	char filename[] = "/tmp/wayland-shm-XXXXXX";
	struct wl_buffer *buffer;
	int fd, size, stride;
	void *data;

	fd = mkstemp(filename);
	if (fd < 0) {
		fprintf(stderr, "open %s failed: %m\n", filename);
		return NULL;
	}
	stride = width * 4;
	size = stride * height;
	if (ftruncate(fd, size) < 0) {
		fprintf(stderr, "ftruncate failed: %m\n");
		close(fd);
		return NULL;
	}

	data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	unlink(filename);

	if (data == MAP_FAILED) {
		fprintf(stderr, "mmap failed: %m\n");
		close(fd);
		return NULL;
	}

	buffer = wl_shm_create_buffer(shm, fd, width, height, stride, visual);

	close(fd);

	*data_out = data;

	return buffer;
}

int main(int argc, char *argv[])
{
	struct wl_display *display;
	struct wl_buffer *buffer;
	void *data;

	display = wl_display_connect(NULL);
	if (display == NULL) {
		fprintf(stderr, "failed to create display: %m\n");
		return -1;
	}

	wl_display_add_global_listener(display, handle_global, &screenshooter);
	wl_display_iterate(display, WL_DISPLAY_READABLE);
	roundtrip(display);
	if (screenshooter == NULL) {
		fprintf(stderr, "display doesn't support screenshooter\n");
		return -1;
	}

	buffer = create_shm_buffer(output_width, output_height, &data);
	screenshooter_shoot(screenshooter, output, buffer);
	roundtrip(display);

	/* FIXME: write png */

	return 0;
}
