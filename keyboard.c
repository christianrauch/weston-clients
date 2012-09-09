/*
 * Copyright © 2012 Openismus GmbH
 * Copyright © 2012 Intel Corporation
 *
 * Permission to use, copy, modify, distribute, and sell this software and
 * its documentation for any purpose is hereby granted without fee, provided
 * that the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of the copyright holders not be used in
 * advertising or publicity pertaining to distribution of the software
 * without specific, written prior permission.  The copyright holders make
 * no representations about the suitability of this software for any
 * purpose.  It is provided "as is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS
 * SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS, IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER
 * RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
 * CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <linux/input.h>
#include <cairo.h>

#include "window.h"
#include "input-method-client-protocol.h"
#include "desktop-shell-client-protocol.h"

struct virtual_keyboard {
	struct input_panel *input_panel;
	struct input_method *input_method;
	struct input_method_context *context;
	struct display *display;
	char *preedit_string;
};

enum key_type {
	keytype_default,
	keytype_backspace,
	keytype_enter,
	keytype_space,
	keytype_switch,
	keytype_symbols,
	keytype_tab
};

struct key {
	enum key_type key_type;

	char *label;
	char *alt;

	unsigned int width;
};

static const struct key keys[] = {
	{ keytype_default, "q", "Q", 1},
	{ keytype_default, "w", "W", 1},
	{ keytype_default, "e", "E", 1},
	{ keytype_default, "r", "R", 1},
	{ keytype_default, "t", "T", 1},
	{ keytype_default, "y", "Y", 1},
	{ keytype_default, "u", "U", 1},
	{ keytype_default, "i", "I", 1},
	{ keytype_default, "o", "O", 1},
	{ keytype_default, "p", "P", 1},
	{ keytype_backspace, "<--", "<--", 2},

	{ keytype_tab, "->|", "->|", 1},
	{ keytype_default, "a", "A", 1},
	{ keytype_default, "s", "S", 1},
	{ keytype_default, "d", "D", 1},
	{ keytype_default, "f", "F", 1},
	{ keytype_default, "g", "G", 1},
	{ keytype_default, "h", "H", 1},
	{ keytype_default, "j", "J", 1},
	{ keytype_default, "k", "K", 1},
	{ keytype_default, "l", "L", 1},
	{ keytype_enter, "Enter", "Enter", 2},

	{ keytype_switch, "ABC", "abc", 2},
	{ keytype_default, "z", "Z", 1},
	{ keytype_default, "x", "X", 1},
	{ keytype_default, "c", "C", 1},
	{ keytype_default, "v", "V", 1},
	{ keytype_default, "b", "B", 1},
	{ keytype_default, "n", "N", 1},
	{ keytype_default, "m", "M", 1},
	{ keytype_default, ",", ",", 1},
	{ keytype_default, ".", ".", 1},
	{ keytype_switch, "ABC", "abc", 1},

	{ keytype_symbols, "?123", "?123", 2},
	{ keytype_space, "", "", 8},
	{ keytype_symbols, "?123", "?123", 2}
};

static const unsigned int columns = 12;
static const unsigned int rows = 4;

static const double key_width = 60;
static const double key_height = 50;

enum keyboard_state {
	keyboardstate_default,
	keyboardstate_uppercase
};

struct keyboard {
	struct virtual_keyboard *keyboard;
	struct window *window;
	struct widget *widget;

	enum keyboard_state state;
};

static void
draw_key(const struct key *key,
	 cairo_t *cr,
	 enum keyboard_state state,
	 unsigned int row,
	 unsigned int col)
{
	const char *label;
	cairo_text_extents_t extents;

	cairo_save(cr);
	cairo_rectangle(cr,
			col * key_width, row * key_height,
			key->width * key_width, key_height);
	cairo_clip(cr);

	/* Paint frame */
	cairo_rectangle(cr,
			col * key_width, row * key_height,
			key->width * key_width, key_height);
	cairo_set_line_width(cr, 3);
	cairo_stroke(cr);

	/* Paint text */
	label = state == keyboardstate_default ? key->label : key->alt;
	cairo_text_extents(cr, label, &extents);

	cairo_translate(cr,
			col * key_width,
			row * key_height);
	cairo_translate(cr,
			(key->width * key_width - extents.width) / 2,
			(key_height - extents.y_bearing) / 2);
	cairo_show_text(cr, label);

	cairo_restore(cr);
}

static void
redraw_handler(struct widget *widget, void *data)
{
	struct keyboard *keyboard = data;
	cairo_surface_t *surface;
	struct rectangle allocation;
	cairo_t *cr;
	unsigned int i;
	unsigned int row = 0, col = 0;

	surface = window_get_surface(keyboard->window);
	widget_get_allocation(keyboard->widget, &allocation);

	cr = cairo_create(surface);
	cairo_rectangle(cr, allocation.x, allocation.y, allocation.width, allocation.height);
	cairo_clip(cr);

	cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
	cairo_set_font_size(cr, 16);

	cairo_translate(cr, allocation.x, allocation.y);

	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_rgba(cr, 1, 1, 1, 0.75);
	cairo_rectangle(cr, 0, 0, columns * key_width, rows * key_height);
	cairo_paint(cr);

	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

	for (i = 0; i < sizeof(keys) / sizeof(*keys); ++i) {
		cairo_set_source_rgb(cr, 0, 0, 0);
		draw_key(&keys[i], cr, keyboard->state, row, col);
		col += keys[i].width;
		if (col >= columns) {
			row += 1;
			col = 0;
		}
	}

	cairo_destroy(cr);
	cairo_surface_destroy(surface);
}

static void
resize_handler(struct widget *widget,
	       int32_t width, int32_t height, void *data)
{
	/* struct keyboard *keyboard = data; */
}

static void
keyboard_handle_key(struct keyboard *keyboard, const struct key *key)
{
	const char *label = keyboard->state == keyboardstate_default ? key->label : key->alt;

	switch (key->key_type) {
		case keytype_default:
			keyboard->keyboard->preedit_string = strcat(keyboard->keyboard->preedit_string,
								    label);
			input_method_context_preedit_string(keyboard->keyboard->context,
							    keyboard->keyboard->preedit_string,
							    strlen(keyboard->keyboard->preedit_string));
			break;
		case keytype_backspace:
			break;
		case keytype_enter:
			break;
		case keytype_space:
			keyboard->keyboard->preedit_string = strcat(keyboard->keyboard->preedit_string,
								    " ");
			input_method_context_preedit_string(keyboard->keyboard->context,
							    "",
							    0);
			input_method_context_commit_string(keyboard->keyboard->context,
							   keyboard->keyboard->preedit_string,
							   strlen(keyboard->keyboard->preedit_string));
			free(keyboard->keyboard->preedit_string);
			keyboard->keyboard->preedit_string = strdup("");
			break;
		case keytype_switch:
			if (keyboard->state == keyboardstate_default)
				keyboard->state = keyboardstate_uppercase;
			else
				keyboard->state = keyboardstate_default;
			break;
		case keytype_symbols:
			break;
		case keytype_tab:
			break;
	}
}

static void
button_handler(struct widget *widget,
	       struct input *input, uint32_t time,
	       uint32_t button,
	       enum wl_pointer_button_state state, void *data)
{
	struct keyboard *keyboard = data;
	struct rectangle allocation;
	int32_t x, y;
	int row, col;
	unsigned int i;

	if (state != WL_POINTER_BUTTON_STATE_PRESSED || button != BTN_LEFT) {
		return;
	}

	input_get_position(input, &x, &y);

	widget_get_allocation(keyboard->widget, &allocation);
	x -= allocation.x;
	y -= allocation.y;

	row = y / key_height;
	col = x / key_width + row * columns;
	for (i = 0; i < sizeof(keys) / sizeof(*keys); ++i) {
		col -= keys[i].width;
		if (col < 0)
			break;
	}

	keyboard_handle_key(keyboard, &keys[i]);

	widget_schedule_redraw(widget);
}

static void
input_method_context_surrounding_text(void *data,
				      struct input_method_context *context,
				      const char *text,
				      uint32_t cursor,
				      uint32_t anchor)
{
	fprintf(stderr, "Surrounding text updated: %s\n", text);
}

static const struct input_method_context_listener input_method_context_listener = {
	input_method_context_surrounding_text,
};

static void
input_method_activate(void *data,
		      struct input_method *input_method,
		      struct input_method_context *context)
{
	struct virtual_keyboard *keyboard = data;

	if (keyboard->context)
		input_method_context_destroy(keyboard->context);

	if (keyboard->preedit_string)
		free(keyboard->preedit_string);

	keyboard->preedit_string = strdup("");

	keyboard->context = context;
	input_method_context_add_listener(context,
					  &input_method_context_listener,
					  keyboard);
}

static void
input_method_deactivate(void *data,
			struct input_method *input_method,
			struct input_method_context *context)
{
	struct virtual_keyboard *keyboard = data;

	if (!keyboard->context)
		return;

	input_method_context_destroy(keyboard->context);
	keyboard->context = NULL;
}

static const struct input_method_listener input_method_listener = {
	input_method_activate,
	input_method_deactivate
};

static void
global_handler(struct wl_display *display, uint32_t id,
	       const char *interface, uint32_t version, void *data)
{
	struct virtual_keyboard *keyboard = data;

	if (!strcmp(interface, "input_panel")) {
		keyboard->input_panel = wl_display_bind(display, id, &input_panel_interface);
	} else if (!strcmp(interface, "input_method")) {
		keyboard->input_method = wl_display_bind(display, id, &input_method_interface);
		input_method_add_listener(keyboard->input_method, &input_method_listener, keyboard);
	}
}

static void
keyboard_create(struct output *output, struct virtual_keyboard *virtual_keyboard)
{
	struct keyboard *keyboard;

	keyboard = malloc(sizeof *keyboard);
	memset(keyboard, 0, sizeof *keyboard);

	keyboard->keyboard = virtual_keyboard;
	keyboard->window = window_create_custom(virtual_keyboard->display);
	keyboard->widget = window_add_widget(keyboard->window, keyboard);

	window_set_title(keyboard->window, "Virtual keyboard");
	window_set_user_data(keyboard->window, keyboard);

	widget_set_redraw_handler(keyboard->widget, redraw_handler);
	widget_set_resize_handler(keyboard->widget, resize_handler);
	widget_set_button_handler(keyboard->widget, button_handler);

	window_schedule_resize(keyboard->window,
			       columns * key_width,
			       rows * key_height);

	input_panel_set_surface(virtual_keyboard->input_panel,
				window_get_wl_surface(keyboard->window),
				output_get_wl_output(output));
}

static void
handle_output_configure(struct output *output, void *data)
{
	struct virtual_keyboard *virtual_keyboard = data;

	/* skip existing outputs */
	if (output_get_user_data(output))
		return;

	output_set_user_data(output, virtual_keyboard);

	keyboard_create(output, virtual_keyboard);
}

int
main(int argc, char *argv[])
{
	struct virtual_keyboard virtual_keyboard;

	virtual_keyboard.display = display_create(argc, argv);
	if (virtual_keyboard.display == NULL) {
		fprintf(stderr, "failed to create display: %m\n");
		return -1;
	}

	virtual_keyboard.context = NULL;
	virtual_keyboard.preedit_string = NULL;

	wl_display_add_global_listener(display_get_display(virtual_keyboard.display),
				       global_handler, &virtual_keyboard);

	display_set_user_data(virtual_keyboard.display, &virtual_keyboard);
	display_set_output_configure_handler(virtual_keyboard.display, handle_output_configure);

	display_run(virtual_keyboard.display);

	return 0;
}
