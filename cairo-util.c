/*
 * Copyright © 2008 Kristian Høgsberg
 * Copyright © 2012 Intel Corporation
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
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <cairo.h>
#include "cairo-util.h"

#include "../shared/config-parser.h"

#define ARRAY_LENGTH(a) (sizeof (a) / sizeof (a)[0])

void
surface_flush_device(cairo_surface_t *surface)
{
	cairo_device_t *device;

	device = cairo_surface_get_device(surface);
	if (device)
		cairo_device_flush(device);
}

void
blur_surface(cairo_surface_t *surface, int margin)
{
	int32_t width, height, stride, x, y, z, w;
	uint8_t *src, *dst;
	uint32_t *s, *d, a, p;
	int i, j, k, size, half;
	uint32_t kernel[71];
	double f;

	size = ARRAY_LENGTH(kernel);
	width = cairo_image_surface_get_width(surface);
	height = cairo_image_surface_get_height(surface);
	stride = cairo_image_surface_get_stride(surface);
	src = cairo_image_surface_get_data(surface);

	dst = malloc(height * stride);

	half = size / 2;
	a = 0;
	for (i = 0; i < size; i++) {
		f = (i - half);
		kernel[i] = exp(- f * f / ARRAY_LENGTH(kernel)) * 10000;
		a += kernel[i];
	}

	for (i = 0; i < height; i++) {
		s = (uint32_t *) (src + i * stride);
		d = (uint32_t *) (dst + i * stride);
		for (j = 0; j < width; j++) {
			if (margin < j && j < width - margin) {
				d[j] = s[j];
				continue;
			}

			x = 0;
			y = 0;
			z = 0;
			w = 0;
			for (k = 0; k < size; k++) {
				if (j - half + k < 0 || j - half + k >= width)
					continue;
				p = s[j - half + k];

				x += (p >> 24) * kernel[k];
				y += ((p >> 16) & 0xff) * kernel[k];
				z += ((p >> 8) & 0xff) * kernel[k];
				w += (p & 0xff) * kernel[k];
			}
			d[j] = (x / a << 24) | (y / a << 16) | (z / a << 8) | w / a;
		}
	}

	for (i = 0; i < height; i++) {
		s = (uint32_t *) (dst + i * stride);
		d = (uint32_t *) (src + i * stride);
		for (j = 0; j < width; j++) {
			if (margin <= i && i < height - margin) {
				d[j] = s[j];
				continue;
			}

			x = 0;
			y = 0;
			z = 0;
			w = 0;
			for (k = 0; k < size; k++) {
				if (i - half + k < 0 || i - half + k >= height)
					continue;
				s = (uint32_t *) (dst + (i - half + k) * stride);
				p = s[j];

				x += (p >> 24) * kernel[k];
				y += ((p >> 16) & 0xff) * kernel[k];
				z += ((p >> 8) & 0xff) * kernel[k];
				w += (p & 0xff) * kernel[k];
			}
			d[j] = (x / a << 24) | (y / a << 16) | (z / a << 8) | w / a;
		}
	}

	free(dst);
	cairo_surface_mark_dirty(surface);
}

void
tile_mask(cairo_t *cr, cairo_surface_t *surface,
	  int x, int y, int width, int height, int margin, int top_margin)
{
	cairo_pattern_t *pattern;
	cairo_matrix_t matrix;
	int i, fx, fy, vmargin;

	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
	pattern = cairo_pattern_create_for_surface (surface);
	cairo_pattern_set_filter(pattern, CAIRO_FILTER_NEAREST);

	for (i = 0; i < 4; i++) {
		fx = i & 1;
		fy = i >> 1;

		cairo_matrix_init_translate(&matrix,
					    -x + fx * (128 - width),
					    -y + fy * (128 - height));
		cairo_pattern_set_matrix(pattern, &matrix);

		if (fy)
			vmargin = margin;
		else
			vmargin = top_margin;

		cairo_reset_clip(cr);
		cairo_rectangle(cr,
				x + fx * (width - margin),
				y + fy * (height - vmargin),
				margin, vmargin);
		cairo_clip (cr);
		cairo_mask(cr, pattern);
	}

	/* Top stretch */
	cairo_matrix_init_translate(&matrix, 60, 0);
	cairo_matrix_scale(&matrix, 8.0 / width, 1);
	cairo_matrix_translate(&matrix, -x - width / 2, -y);
	cairo_pattern_set_matrix(pattern, &matrix);
	cairo_rectangle(cr, x + margin, y, width - 2 * margin, margin);

	cairo_reset_clip(cr);
	cairo_rectangle(cr,
			x + margin,
			y,
			width - 2 * margin, margin);
	cairo_clip (cr);
	cairo_mask(cr, pattern);

	/* Bottom stretch */
	cairo_matrix_translate(&matrix, 0, -height + 128);
	cairo_pattern_set_matrix(pattern, &matrix);

	cairo_reset_clip(cr);
	cairo_rectangle(cr, x + margin, y + height - margin,
			width - 2 * margin, margin);
	cairo_clip (cr);
	cairo_mask(cr, pattern);

	/* Left stretch */
	cairo_matrix_init_translate(&matrix, 0, 60);
	cairo_matrix_scale(&matrix, 1, 8.0 / height);
	cairo_matrix_translate(&matrix, -x, -y - height / 2);
	cairo_pattern_set_matrix(pattern, &matrix);
	cairo_reset_clip(cr);
	cairo_rectangle(cr, x, y + margin, margin, height - 2 * margin);
	cairo_clip (cr);
	cairo_mask(cr, pattern);

	/* Right stretch */
	cairo_matrix_translate(&matrix, -width + 128, 0);
	cairo_pattern_set_matrix(pattern, &matrix);
	cairo_rectangle(cr, x + width - margin, y + margin,
			margin, height - 2 * margin);
	cairo_reset_clip(cr);
	cairo_clip (cr);
	cairo_mask(cr, pattern);

	cairo_pattern_destroy(pattern);
	cairo_reset_clip(cr);
}

void
tile_source(cairo_t *cr, cairo_surface_t *surface,
	    int x, int y, int width, int height, int margin, int top_margin)
{
	cairo_pattern_t *pattern;
	cairo_matrix_t matrix;
	int i, fx, fy, vmargin;

	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
	pattern = cairo_pattern_create_for_surface (surface);
	cairo_pattern_set_filter(pattern, CAIRO_FILTER_NEAREST);
	cairo_set_source(cr, pattern);
	cairo_pattern_destroy(pattern);

	for (i = 0; i < 4; i++) {
		fx = i & 1;
		fy = i >> 1;

		cairo_matrix_init_translate(&matrix,
					    -x + fx * (128 - width),
					    -y + fy * (128 - height));
		cairo_pattern_set_matrix(pattern, &matrix);

		if (fy)
			vmargin = margin;
		else
			vmargin = top_margin;

		cairo_rectangle(cr,
				x + fx * (width - margin),
				y + fy * (height - vmargin),
				margin, vmargin);
		cairo_fill(cr);
	}

	/* Top stretch */
	cairo_matrix_init_translate(&matrix, 60, 0);
	cairo_matrix_scale(&matrix, 8.0 / (width - 2 * margin), 1);
	cairo_matrix_translate(&matrix, -x - width / 2, -y);
	cairo_pattern_set_matrix(pattern, &matrix);
	cairo_rectangle(cr, x + margin, y, width - 2 * margin, top_margin);
	cairo_fill(cr);

	/* Bottom stretch */
	cairo_matrix_translate(&matrix, 0, -height + 128);
	cairo_pattern_set_matrix(pattern, &matrix);
	cairo_rectangle(cr, x + margin, y + height - margin,
			width - 2 * margin, margin);
	cairo_fill(cr);

	/* Left stretch */
	cairo_matrix_init_translate(&matrix, 0, 60);
	cairo_matrix_scale(&matrix, 1, 8.0 / (height - margin - top_margin));
	cairo_matrix_translate(&matrix, -x, -y - height / 2);
	cairo_pattern_set_matrix(pattern, &matrix);
	cairo_rectangle(cr, x, y + top_margin,
			margin, height - margin - top_margin);
	cairo_fill(cr);

	/* Right stretch */
	cairo_matrix_translate(&matrix, -width + 128, 0);
	cairo_pattern_set_matrix(pattern, &matrix);
	cairo_rectangle(cr, x + width - margin, y + top_margin,
			margin, height - margin - top_margin);
	cairo_fill(cr);
}

void
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

cairo_surface_t *
load_cairo_surface(const char *filename)
{
	pixman_image_t *image;
	int width, height, stride;
	void *data;

	image = load_image(filename);
	if (image == NULL) {
		return NULL;
	}

	data = pixman_image_get_data(image);
	width = pixman_image_get_width(image);
	height = pixman_image_get_height(image);
	stride = pixman_image_get_stride(image);

	return cairo_image_surface_create_for_data(data, CAIRO_FORMAT_ARGB32,
						   width, height, stride);
}
