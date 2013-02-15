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

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <linux/input.h>
#include <cairo.h>

#include "window.h"
#include "text-client-protocol.h"

static const char *font_name = "sans-serif";
static int font_size = 14;

struct text_layout {
	cairo_glyph_t *glyphs;
	int num_glyphs;
	cairo_text_cluster_t *clusters;
	int num_clusters;
	cairo_text_cluster_flags_t cluster_flags;
	cairo_scaled_font_t *font;
};

struct text_entry {
	struct widget *widget;
	struct window *window;
	char *text;
	int active;
	uint32_t cursor;
	uint32_t anchor;
	struct {
		char *text;
		int32_t cursor;
		char *commit;
	} preedit;
	struct {
		int32_t cursor;
	} preedit_info;
	struct text_model *model;
	struct text_layout *layout;
	struct {
		xkb_mod_mask_t shift_mask;
	} keysym;
	uint32_t serial;
};

struct editor {
	struct text_model_factory *text_model_factory;
	struct display *display;
	struct window *window;
	struct widget *widget;
	struct text_entry *entry;
	struct text_entry *editor;
	struct text_entry *active_entry;
};

static const char *
utf8_start_char(const char *text, const char *p)
{
	for (; p >= text; --p) {
		if ((*p & 0xc0) != 0x80)
			return p;
	}
	return NULL;
}

static const char *
utf8_prev_char(const char *text, const char *p)
{
	if (p > text)
		return utf8_start_char(text, --p);
	return NULL;
}

static const char *
utf8_end_char(const char *p)
{
	while ((*p & 0xc0) == 0x80)
		p++;
	return p;
}

static const char *
utf8_next_char(const char *p)
{
	if (*p != 0)
		return utf8_end_char(++p);
	return NULL;
}

static struct text_layout *
text_layout_create(void)
{
	struct text_layout *layout;
	cairo_surface_t *surface;
	cairo_t *cr;

	layout = malloc(sizeof *layout);
	if (!layout)
		return NULL;

	layout->glyphs = NULL;
	layout->num_glyphs = 0;

	layout->clusters = NULL;
	layout->num_clusters = 0;

	surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 0, 0);
	cr = cairo_create(surface);
	cairo_set_font_size(cr, font_size);
	cairo_select_font_face(cr, font_name, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
	layout->font = cairo_get_scaled_font(cr);
	cairo_scaled_font_reference(layout->font);

	cairo_destroy(cr);
	cairo_surface_destroy(surface);

	return layout;
}

static void
text_layout_destroy(struct text_layout *layout)
{
	if (layout->glyphs)
		cairo_glyph_free(layout->glyphs);

	if (layout->clusters)
		cairo_text_cluster_free(layout->clusters);

	cairo_scaled_font_destroy(layout->font);

	free(layout);
}

static void
text_layout_set_text(struct text_layout *layout,
		     const char *text)
{
	if (layout->glyphs)
		cairo_glyph_free(layout->glyphs);

	if (layout->clusters)
		cairo_text_cluster_free(layout->clusters);

	layout->glyphs = NULL;
	layout->num_glyphs = 0;
	layout->clusters = NULL;
	layout->num_clusters = 0;

	cairo_scaled_font_text_to_glyphs(layout->font, 0, 0, text, -1,
					 &layout->glyphs, &layout->num_glyphs,
					 &layout->clusters, &layout->num_clusters,
					 &layout->cluster_flags);
}

static void
text_layout_draw(struct text_layout *layout, cairo_t *cr)
{
	cairo_save(cr);
	cairo_set_scaled_font(cr, layout->font);
	cairo_show_glyphs(cr, layout->glyphs, layout->num_glyphs);
	cairo_restore(cr);
}

static void
text_layout_extents(struct text_layout *layout, cairo_text_extents_t *extents)
{
	cairo_scaled_font_glyph_extents(layout->font,
					layout->glyphs, layout->num_glyphs,
					extents);
}

static uint32_t
bytes_from_glyphs(struct text_layout *layout, uint32_t index)
{
	int i;
	uint32_t glyphs = 0, bytes = 0; 

	for (i = 0; i < layout->num_clusters && glyphs < index; i++) {
		bytes += layout->clusters[i].num_bytes;
		glyphs += layout->clusters[i].num_glyphs;
	}

	return bytes;
}

static uint32_t
glyphs_from_bytes(struct text_layout *layout, uint32_t index)
{
	int i;
	uint32_t glyphs = 0, bytes = 0; 

	for (i = 0; i < layout->num_clusters && bytes < index; i++) {
		bytes += layout->clusters[i].num_bytes;
		glyphs += layout->clusters[i].num_glyphs;
	}

	return glyphs;
}

static int
text_layout_xy_to_index(struct text_layout *layout, double x, double y)
{
	cairo_text_extents_t extents;
	int i;
	double d;

	if (layout->num_glyphs == 0)
		return 0;

	cairo_scaled_font_glyph_extents(layout->font,
					layout->glyphs, layout->num_glyphs,
					&extents);

	if (x < 0)
		return 0;

	for (i = 0; i < layout->num_glyphs - 1; ++i) {
		d = layout->glyphs[i + 1].x - layout->glyphs[i].x;
		if (x < layout->glyphs[i].x + d/2)
			return bytes_from_glyphs(layout, i);
	}

	d = extents.width - layout->glyphs[layout->num_glyphs - 1].x;
	if (x < layout->glyphs[layout->num_glyphs - 1].x + d/2)
		return bytes_from_glyphs(layout, layout->num_glyphs - 1);

	return bytes_from_glyphs(layout, layout->num_glyphs);
}

static void
text_layout_index_to_pos(struct text_layout *layout, uint32_t index, cairo_rectangle_t *pos)
{
	cairo_text_extents_t extents;
	int glyph_index = glyphs_from_bytes(layout, index);

	if (!pos)
		return;

	cairo_scaled_font_glyph_extents(layout->font,
					layout->glyphs, layout->num_glyphs,
					&extents);

	if (glyph_index >= layout->num_glyphs) {
		pos->x = extents.x_advance;
		pos->y = layout->num_glyphs ? layout->glyphs[layout->num_glyphs - 1].y : 0;
		pos->width = 1;
		pos->height = extents.height;
		return;
	}

	pos->x = layout->glyphs[glyph_index].x;
	pos->y = layout->glyphs[glyph_index].y;
	pos->width = glyph_index < layout->num_glyphs - 1 ? layout->glyphs[glyph_index + 1].x : extents.x_advance - pos->x;
	pos->height = extents.height;
}

static void
text_layout_get_cursor_pos(struct text_layout *layout, int index, cairo_rectangle_t *pos)
{
	text_layout_index_to_pos(layout, index, pos);
	pos->width = 1;
}

static void text_entry_redraw_handler(struct widget *widget, void *data);
static void text_entry_button_handler(struct widget *widget,
				      struct input *input, uint32_t time,
				      uint32_t button,
				      enum wl_pointer_button_state state, void *data);
static void text_entry_insert_at_cursor(struct text_entry *entry, const char *text);
static void text_entry_set_preedit(struct text_entry *entry,
				   const char *preedit_text,
				   int preedit_cursor);
static void text_entry_delete_text(struct text_entry *entry,
				   uint32_t index, uint32_t length);
static void text_entry_delete_selected_text(struct text_entry *entry);
static void text_entry_reset_preedit(struct text_entry *entry);
static void text_entry_commit_and_reset(struct text_entry *entry);

static void
text_model_commit_string(void *data,
			 struct text_model *text_model,
			 uint32_t serial,
			 const char *text,
			 uint32_t index)
{
	struct text_entry *entry = data;

	if (index > strlen(text))
		fprintf(stderr, "Invalid cursor index %d\n", index);

	text_entry_reset_preedit(entry);

	text_entry_delete_selected_text(entry);
	text_entry_insert_at_cursor(entry, text);

	widget_schedule_redraw(entry->widget);
}

static void
text_model_preedit_string(void *data,
			  struct text_model *text_model,
			  uint32_t serial,
			  const char *text,
			  const char *commit)
{
	struct text_entry *entry = data;

	text_entry_delete_selected_text(entry);
	text_entry_set_preedit(entry, text, entry->preedit_info.cursor);
	entry->preedit.commit = strdup(commit);

	entry->preedit_info.cursor = 0;

	widget_schedule_redraw(entry->widget);
}

static void
text_model_delete_surrounding_text(void *data,
				   struct text_model *text_model,
				   uint32_t serial,
				   int32_t index,
				   uint32_t length)
{
	struct text_entry *entry = data;
	uint32_t cursor_index = index + entry->cursor;
	const char *start, *end;

	if (cursor_index > strlen(entry->text)) {
		fprintf(stderr, "Invalid cursor index %d\n", index);
		return;
	}

	if (cursor_index + length > strlen(entry->text)) {
		fprintf(stderr, "Invalid length %d\n", length);
		return;
	}

	if (length == 0)
		return;

	start = utf8_start_char(entry->text, entry->text + cursor_index);
	end = utf8_end_char(entry->text + cursor_index + length);

	text_entry_delete_text(entry,
			       start - entry->text,
			       end - start);
}

static void
text_model_preedit_styling(void *data,
			   struct text_model *text_model,
			   uint32_t serial,
			   uint32_t index,
			   uint32_t length,
			   uint32_t style)
{
}

static void
text_model_preedit_cursor(void *data,
			  struct text_model *text_model,
			  uint32_t serial,
			  int32_t index)
{
	struct text_entry *entry = data;

	entry->preedit_info.cursor = index;
}

static void
text_model_modifiers_map(void *data,
			 struct text_model *text_model,
			 struct wl_array *map)
{
	struct text_entry *entry = data;

	entry->keysym.shift_mask = keysym_modifiers_get_mask(map, "Shift");
}

static void
text_model_keysym(void *data,
		  struct text_model *text_model,
		  uint32_t serial,
		  uint32_t time,
		  uint32_t key,
		  uint32_t state,
		  uint32_t modifiers)
{
	struct text_entry *entry = data;
	const char *state_label = "release";
	const char *key_label = "Unknown";
	const char *new_char;

	if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		state_label = "pressed";
	}

	if (key == XKB_KEY_Left ||
	    key == XKB_KEY_Right) {
		if (state != WL_KEYBOARD_KEY_STATE_RELEASED)
			return;

		if (key == XKB_KEY_Left)
			new_char = utf8_prev_char(entry->text, entry->text + entry->cursor);
		else
			new_char = utf8_next_char(entry->text + entry->cursor);

		if (new_char != NULL) {
			entry->cursor = new_char - entry->text;
			if (!(modifiers & entry->keysym.shift_mask))
				entry->anchor = entry->cursor;
			widget_schedule_redraw(entry->widget);
		}

		return;
	}

	switch (key) {
		case XKB_KEY_Tab:
			key_label = "Tab";
			break;
		case XKB_KEY_KP_Enter:
		case XKB_KEY_Return:
			key_label = "Enter";
			break;
	}

	fprintf(stderr, "%s key was %s.\n", key_label, state_label);
}

static void
text_model_selection_replacement(void *data,
				 struct text_model *text_model)
{
}

static void
text_model_direction(void *data,
		     struct text_model *text_model)
{
}

static void
text_model_locale(void *data,
		  struct text_model *text_model)
{
}

static void
text_model_enter(void *data,
		 struct text_model *text_model,
		 struct wl_surface *surface)
{
	struct text_entry *entry = data;

	if (surface != window_get_wl_surface(entry->window))
		return;

	entry->active = 1;

	widget_schedule_redraw(entry->widget);
}

static void
text_model_leave(void *data,
		 struct text_model *text_model)
{
	struct text_entry *entry = data;

	text_entry_commit_and_reset(entry);

	entry->active = 0;

	widget_schedule_redraw(entry->widget);
}

static const struct text_model_listener text_model_listener = {
	text_model_commit_string,
	text_model_preedit_string,
	text_model_delete_surrounding_text,
	text_model_preedit_styling,
	text_model_preedit_cursor,
	text_model_modifiers_map,
	text_model_keysym,
	text_model_selection_replacement,
	text_model_direction,
	text_model_locale,
	text_model_enter,
	text_model_leave
};

static struct text_entry*
text_entry_create(struct editor *editor, const char *text)
{
	struct text_entry *entry;

	entry = calloc(1, sizeof *entry);

	entry->widget = widget_add_widget(editor->widget, entry);
	entry->window = editor->window;
	entry->text = strdup(text);
	entry->active = 0;
	entry->cursor = strlen(text);
	entry->anchor = entry->cursor;
	entry->model = text_model_factory_create_text_model(editor->text_model_factory);
	text_model_add_listener(entry->model, &text_model_listener, entry);

	entry->layout = text_layout_create();
	text_layout_set_text(entry->layout, entry->text);

	widget_set_redraw_handler(entry->widget, text_entry_redraw_handler);
	widget_set_button_handler(entry->widget, text_entry_button_handler);

	return entry;
}

static void
text_entry_destroy(struct text_entry *entry)
{
	widget_destroy(entry->widget);
	text_model_destroy(entry->model);
	text_layout_destroy(entry->layout);
	free(entry->text);
	free(entry);
}

static void
redraw_handler(struct widget *widget, void *data)
{
	struct editor *editor = data;
	cairo_surface_t *surface;
	struct rectangle allocation;
	cairo_t *cr;

	surface = window_get_surface(editor->window);
	widget_get_allocation(editor->widget, &allocation);

	cr = cairo_create(surface);
	cairo_rectangle(cr, allocation.x, allocation.y, allocation.width, allocation.height);
	cairo_clip(cr);

	cairo_translate(cr, allocation.x, allocation.y);

	/* Draw background */
	cairo_push_group(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_rgba(cr, 1, 1, 1, 1);
	cairo_rectangle(cr, 0, 0, allocation.width, allocation.height);
	cairo_fill(cr);

	cairo_pop_group_to_source(cr);
	cairo_paint(cr);

	cairo_destroy(cr);
	cairo_surface_destroy(surface);
}

static void
text_entry_allocate(struct text_entry *entry, int32_t x, int32_t y,
		    int32_t width, int32_t height)
{
	widget_set_allocation(entry->widget, x, y, width, height);
}

static void
resize_handler(struct widget *widget,
	       int32_t width, int32_t height, void *data)
{
	struct editor *editor = data;
	struct rectangle allocation;

	widget_get_allocation(editor->widget, &allocation);

	text_entry_allocate(editor->entry,
			    allocation.x + 20, allocation.y + 20,
			    width - 40, height / 2 - 40);
	text_entry_allocate(editor->editor,
			    allocation.x + 20, allocation.y + height / 2 + 20,
			    width - 40, height / 2 - 40);
}

static void
text_entry_activate(struct text_entry *entry,
		    struct wl_seat *seat)
{
	struct wl_surface *surface = window_get_wl_surface(entry->window);

	entry->serial++;

	text_model_activate(entry->model,
			    entry->serial,
			    seat,
			    surface);
}

static void
text_entry_deactivate(struct text_entry *entry,
		      struct wl_seat *seat)
{
	text_model_deactivate(entry->model,
			      seat);
}

static void
text_entry_update_layout(struct text_entry *entry)
{
	char *text;

	assert(((unsigned int)entry->cursor) <= strlen(entry->text) +
	       (entry->preedit.text ? strlen(entry->preedit.text) : 0));

	if (!entry->preedit.text) {
		text_layout_set_text(entry->layout, entry->text);
		return;
	}

	text = malloc(strlen(entry->text) + strlen(entry->preedit.text) + 1);
	strncpy(text, entry->text, entry->cursor);
	strcpy(text + entry->cursor, entry->preedit.text);
	strcpy(text + entry->cursor + strlen(entry->preedit.text),
	       entry->text + entry->cursor);

	text_layout_set_text(entry->layout, text);
	free(text);

	widget_schedule_redraw(entry->widget);

	text_model_set_surrounding_text(entry->model,
					entry->text,
					entry->cursor,
					entry->anchor);
}

static void
text_entry_insert_at_cursor(struct text_entry *entry, const char *text)
{
	char *new_text = malloc(strlen(entry->text) + strlen(text) + 1);

	strncpy(new_text, entry->text, entry->cursor);
	strcpy(new_text + entry->cursor, text);
	strcpy(new_text + entry->cursor + strlen(text),
	       entry->text + entry->cursor);

	free(entry->text);
	entry->text = new_text;
	entry->cursor += strlen(text);
	entry->anchor += strlen(text);

	text_entry_update_layout(entry);
}

static void
text_entry_reset_preedit(struct text_entry *entry)
{
	entry->preedit.cursor = 0;

	free(entry->preedit.text);
	entry->preedit.text = NULL;

	free(entry->preedit.commit);
	entry->preedit.commit = NULL;
}

static void
text_entry_commit_and_reset(struct text_entry *entry)
{
	char *commit = NULL;

	if (entry->preedit.commit)
		commit = strdup(entry->preedit.commit);

	text_entry_reset_preedit(entry);
	if (commit) {
		text_entry_insert_at_cursor(entry, commit);
		free(commit);
	}
}

static void
text_entry_set_preedit(struct text_entry *entry,
		       const char *preedit_text,
		       int preedit_cursor)
{
	text_entry_reset_preedit(entry);

	if (!preedit_text)
		return;

	entry->preedit.text = strdup(preedit_text);
	entry->preedit.cursor = preedit_cursor;

	text_entry_update_layout(entry);
}

static void
text_entry_set_cursor_position(struct text_entry *entry,
			       int32_t x, int32_t y)
{
	text_entry_commit_and_reset(entry);

	entry->cursor = text_layout_xy_to_index(entry->layout, x, y);

	entry->serial++;

	text_model_reset(entry->model, entry->serial);

	if (entry->preedit.cursor > 0 &&
	    entry->cursor >= (uint32_t)entry->preedit.cursor) {
		entry->cursor -= entry->preedit.cursor;
	}

	text_entry_update_layout(entry);

	widget_schedule_redraw(entry->widget);
}

static void
text_entry_set_anchor_position(struct text_entry *entry,
			       int32_t x, int32_t y)
{
	entry->anchor = text_layout_xy_to_index(entry->layout, x, y);

	widget_schedule_redraw(entry->widget);
}

static void
text_entry_delete_text(struct text_entry *entry,
		       uint32_t index, uint32_t length)
{
	if (entry->cursor > index)
		entry->cursor -= length;

	entry->anchor = entry->cursor;

	entry->text[index] = '\0';
	strcat(entry->text, entry->text + index + length);

	text_entry_update_layout(entry);

	widget_schedule_redraw(entry->widget);
}

static void
text_entry_delete_selected_text(struct text_entry *entry)
{
	uint32_t start_index = entry->anchor < entry->cursor ? entry->anchor : entry->cursor;
	uint32_t end_index = entry->anchor < entry->cursor ? entry->cursor : entry->anchor;

	if (entry->anchor == entry->cursor)
		return;

	text_entry_delete_text(entry, start_index, end_index - start_index);

	entry->anchor = entry->cursor;
}

static void
text_entry_draw_selection(struct text_entry *entry, cairo_t *cr)
{
	cairo_text_extents_t extents;
	uint32_t start_index = entry->anchor < entry->cursor ? entry->anchor : entry->cursor;
	uint32_t end_index = entry->anchor < entry->cursor ? entry->cursor : entry->anchor;
	cairo_rectangle_t start;
	cairo_rectangle_t end;

	if (entry->anchor == entry->cursor)
		return;

	text_layout_extents(entry->layout, &extents);

	text_layout_index_to_pos(entry->layout, start_index, &start);
	text_layout_index_to_pos(entry->layout, end_index, &end);

	cairo_save (cr);

	cairo_set_source_rgba(cr, 0.3, 0.3, 1.0, 0.5);
	cairo_rectangle(cr,
			start.x, extents.y_bearing + extents.height + 2,
			end.x - start.x, -extents.height - 4);
	cairo_fill(cr);

	cairo_rectangle(cr,
			start.x, extents.y_bearing + extents.height,
			end.x - start.x, -extents.height);
	cairo_clip(cr);
	cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
	text_layout_draw(entry->layout, cr);

	cairo_restore (cr);
}

static void
text_entry_draw_cursor(struct text_entry *entry, cairo_t *cr)
{
	cairo_text_extents_t extents;
	cairo_rectangle_t cursor_pos;

	if (entry->preedit.text && entry->preedit.cursor < 0)
		return;

	text_layout_extents(entry->layout, &extents);
	text_layout_get_cursor_pos(entry->layout,
				   entry->cursor + entry->preedit.cursor,
				   &cursor_pos);

	cairo_set_line_width(cr, 1.0);
	cairo_move_to(cr, cursor_pos.x, extents.y_bearing + extents.height + 2);
	cairo_line_to(cr, cursor_pos.x, extents.y_bearing - 2);
	cairo_stroke(cr);
}

static void
text_entry_draw_preedit(struct text_entry *entry, cairo_t *cr)
{
	cairo_text_extents_t extents;
	cairo_rectangle_t start;
	cairo_rectangle_t end;

	if (!entry->preedit.text)
		return;

	text_layout_extents(entry->layout, &extents);

	text_layout_index_to_pos(entry->layout, entry->cursor, &start);
	text_layout_index_to_pos(entry->layout,
				 entry->cursor + strlen(entry->preedit.text),
				 &end);

	cairo_save (cr);

	cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 1.0);
	cairo_rectangle(cr,
			start.x, 0,
			end.x - start.x, 1);
	cairo_fill(cr);

	cairo_restore (cr);
}

static const int text_offset_left = 10;

static void
text_entry_redraw_handler(struct widget *widget, void *data)
{
	struct text_entry *entry = data;
	cairo_surface_t *surface;
	struct rectangle allocation;
	cairo_t *cr;

	surface = window_get_surface(entry->window);
	widget_get_allocation(entry->widget, &allocation);

	cr = cairo_create(surface);
	cairo_rectangle(cr, allocation.x, allocation.y, allocation.width, allocation.height);
	cairo_clip(cr);

	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);

	cairo_push_group(cr);
	cairo_translate(cr, allocation.x, allocation.y);

	cairo_set_source_rgba(cr, 1, 1, 1, 1);
	cairo_rectangle(cr, 0, 0, allocation.width, allocation.height);
	cairo_fill(cr);

	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

	if (entry->active) {
		cairo_rectangle(cr, 0, 0, allocation.width, allocation.height);
		cairo_set_line_width (cr, 3);
		cairo_set_source_rgba(cr, 0, 0, 1, 1.0);
		cairo_stroke(cr);
	}

	cairo_set_source_rgba(cr, 0, 0, 0, 1);

	cairo_translate(cr, text_offset_left, allocation.height / 2);
	text_layout_draw(entry->layout, cr);

	text_entry_draw_selection(entry, cr);

	text_entry_draw_cursor(entry, cr);

	text_entry_draw_preedit(entry, cr);

	cairo_pop_group_to_source(cr);
	cairo_paint(cr);

	cairo_destroy(cr);
	cairo_surface_destroy(surface);
}

static int
text_entry_motion_handler(struct widget *widget,
			  struct input *input, uint32_t time,
			  float x, float y, void *data)
{
	struct text_entry *entry = data;
	struct rectangle allocation;

	widget_get_allocation(entry->widget, &allocation);

	text_entry_set_cursor_position(entry,
				       x - allocation.x - text_offset_left,
				       y - allocation.y - text_offset_left);

	return CURSOR_IBEAM;
}

static void
text_entry_button_handler(struct widget *widget,
			  struct input *input, uint32_t time,
			  uint32_t button,
			  enum wl_pointer_button_state state, void *data)
{
	struct text_entry *entry = data;
	struct rectangle allocation;
	struct editor *editor;
	int32_t x, y;

	widget_get_allocation(entry->widget, &allocation);
	input_get_position(input, &x, &y);

	editor = window_get_user_data(entry->window);

	if (button != BTN_LEFT) {
		return;
	}

	text_entry_set_cursor_position(entry,
				       x - allocation.x - text_offset_left,
				       y - allocation.y - text_offset_left);

	if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
		struct wl_seat *seat = input_get_seat(input);

		text_entry_activate(entry, seat);
		editor->active_entry = entry;

		text_entry_set_anchor_position(entry,
					       x - allocation.x - text_offset_left,
					       y - allocation.y - text_offset_left);

		widget_set_motion_handler(entry->widget, text_entry_motion_handler);
	} else {
		widget_set_motion_handler(entry->widget, NULL);
	}
}

static void
editor_button_handler(struct widget *widget,
		      struct input *input, uint32_t time,
		      uint32_t button,
		      enum wl_pointer_button_state state, void *data)
{
	struct editor *editor = data;

	if (button != BTN_LEFT) {
		return;
	}

	if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
		struct wl_seat *seat = input_get_seat(input);

		text_entry_deactivate(editor->entry, seat);
		text_entry_deactivate(editor->editor, seat);
		editor->active_entry = NULL;
	}
}

static void
key_handler(struct window *window,
	    struct input *input, uint32_t time,
	    uint32_t key, uint32_t sym, enum wl_keyboard_key_state state,
	    void *data)
{
	struct editor *editor = data;
	struct text_entry *entry;
	const char *start, *end, *new_char;
	char text[16];

	if (!editor->active_entry)
		return;

	entry = editor->active_entry;

	if (state != WL_KEYBOARD_KEY_STATE_PRESSED)
		return;

	switch (sym) {
		case XKB_KEY_BackSpace:
			text_entry_commit_and_reset(entry);

			start = utf8_prev_char(entry->text, entry->text + entry->cursor);

			if (start == NULL)
				break;

			end = utf8_end_char(entry->text + entry->cursor);
			text_entry_delete_text(entry,
					       start - entry->text,
					       end - start);
			break;
		case XKB_KEY_Delete:
			text_entry_commit_and_reset(entry);

			start = utf8_start_char(entry->text, entry->text + entry->cursor);

			if (start == NULL)
				break;

			end = utf8_next_char(start);

			if (end == NULL)
				break;

			text_entry_delete_text(entry,
					       start - entry->text,
					       end - start);
			break;
		case XKB_KEY_Left:
			text_entry_commit_and_reset(entry);

			new_char = utf8_prev_char(entry->text, entry->text + entry->cursor);
			if (new_char != NULL) {
				entry->cursor = new_char - entry->text;
				entry->anchor = entry->cursor;
				widget_schedule_redraw(entry->widget);
			}
			break;
		case XKB_KEY_Right:
			text_entry_commit_and_reset(entry);

			new_char = utf8_next_char(entry->text + entry->cursor);
			if (new_char != NULL) {
				entry->cursor = new_char - entry->text;
				entry->anchor = entry->cursor;
				widget_schedule_redraw(entry->widget);
			}
			break;
		default:
			if (xkb_keysym_to_utf8(sym, text, sizeof(text)) <= 0)
				break;

			text_entry_commit_and_reset(entry);

			text_entry_insert_at_cursor(entry, text);
			break;
	}

	widget_schedule_redraw(entry->widget);
}

static void
global_handler(struct display *display, uint32_t name,
	       const char *interface, uint32_t version, void *data)
{
	struct editor *editor = data;

	if (!strcmp(interface, "text_model_factory")) {
		editor->text_model_factory =
			display_bind(display, name,
				     &text_model_factory_interface, 1);
	}
}

int
main(int argc, char *argv[])
{
	struct editor editor;

	memset(&editor, 0, sizeof editor);

	editor.display = display_create(argc, argv);
	if (editor.display == NULL) {
		fprintf(stderr, "failed to create display: %m\n");
		return -1;
	}

	display_set_user_data(editor.display, &editor);
	display_set_global_handler(editor.display, global_handler);

	editor.window = window_create(editor.display);
	editor.widget = frame_create(editor.window, &editor);

	editor.entry = text_entry_create(&editor, "Entry");
	editor.editor = text_entry_create(&editor, "Editor");

	window_set_title(editor.window, "Text Editor");
	window_set_key_handler(editor.window, key_handler);
	window_set_user_data(editor.window, &editor);

	widget_set_redraw_handler(editor.widget, redraw_handler);
	widget_set_resize_handler(editor.widget, resize_handler);
	widget_set_button_handler(editor.widget, editor_button_handler);

	window_schedule_resize(editor.window, 500, 400);

	display_run(editor.display);

	text_entry_destroy(editor.entry);
	text_entry_destroy(editor.editor);

	return 0;
}
