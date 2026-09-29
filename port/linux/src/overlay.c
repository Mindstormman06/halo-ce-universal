/*
OVERLAY.C

The desktop ports' settings overlay. F10, or Back and Start together on a
controller, opens it over the game; the game gets no input while it is
open, and stops if it is a game on this machine alone
(port/linux/game/overlay_pause.c). The display, sound and mouse settings
change as the player moves through it, and each is written into
config.toml (port_config.c) as it is set.

It also draws the frames-per-second counter (display.show_fps) and, until
the settings are first opened, a line at start-up saying how to open them.

It draws itself with a program of its own, after the game's picture is
presented (D3DDevice_Present, d3d8_gl.c): boxes with rounded corners
measured in the fragment shader, and text from a signed distance field
font (overlay_font.h, tools/overlay_font.py). Everything is placed in the
window's pixels, in units that grow with the window.
*/

#ifndef HALO_ANDROID

#include "platform.h"
#include "sdl_platform.h"
#include "gl.h"
#include "port_config.h"
#include "input_bindings.h"
#include "overlay.h"
#include "overlay_font.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- the settings */

enum overlay_tab
{
	_tab_display,
	_tab_audio,
	_tab_controls,
	_tab_keybinds,
	NUMBER_OF_TABS
};

enum item_kind
{
	/* one of a few values, stepped through with left and right */
	_kind_choice,
	_kind_toggle,
	_kind_slider,
	/* a key or mouse button, chosen by pressing it */
	_kind_bind,
};

enum overlay_item
{
	_item_display_mode,
	_item_window_size,
	_item_widescreen,
	_item_render_scale,
	_item_vsync,
	_item_interpolation,
	_item_show_fps,
	_item_master_volume,
	_item_music_volume,
	_item_effects_volume,
	_item_dialog_volume,
	_item_mouse_sensitivity,
	_item_invert_mouse,
	/* one for each input_action */
	_item_bind_first,
	NUMBER_OF_ITEMS = _item_bind_first + NUMBER_OF_INPUT_ACTIONS
};

struct item_definition
{
	enum overlay_tab tab;
	enum item_kind kind;
	/* in config.toml */
	const char *setting;
	const char *label;
	const char *help;
	/* a slider's range, and how far a step moves it */
	float minimum, maximum, step;
};

static const struct item_definition items[NUMBER_OF_ITEMS] =
{
	{ _tab_display, _kind_choice, "display.fullscreen", "Display mode",
		"Fullscreen takes the display for the game alone. Borderless covers it with a window without a frame, "
		"so switching to other windows is instant. F11 switches between fullscreen and the window." },
	{ _tab_display, _kind_choice, "display.window_height", "Window size",
		"The size of the window, which the game draws at. The window can also be resized by its edges." },
	{ _tab_display, _kind_toggle, "display.widescreen", "Widescreen",
		"The picture takes the shape of the display or the window. Off keeps the Xbox's 4:3, with bars at "
		"the sides." },
	{ _tab_display, _kind_choice, "display.render_scale", "Resolution",
		"Native draws at the height of the display or window. Whole multiple draws at the largest multiple of "
		"the Xbox's 480 lines that fits, and scales the rest: for a 1080p display on which the menus show thin "
		"lines. Original draws 480 lines." },
	{ _tab_display, _kind_toggle, "display.vsync", "Vertical sync",
		"Waits for the display between frames, which stops tearing." },
	{ _tab_display, _kind_toggle, "display.interpolation", "Smooth motion",
		"Draws a frame for every refresh of the display, blending between the game's 30 ticks a second. Off "
		"keeps the original 30 frames a second. Takes effect when the game next starts." },
	{ _tab_display, _kind_toggle, "display.show_fps", "Frame counter",
		"Shows how many frames are drawn each second, in the corner." },
	{ _tab_audio, _kind_slider, "audio.volume", "Master volume",
		"The volume of everything.", 0.0f, 1.0f, 0.05f },
	{ _tab_audio, _kind_slider, "audio.music_volume", "Music",
		"The volume of the music.", 0.0f, 1.0f, 0.05f },
	{ _tab_audio, _kind_slider, "audio.effects_volume", "Effects",
		"The volume of weapons, vehicles, the world and everything else that is neither music nor speech.",
		0.0f, 1.0f, 0.05f },
	{ _tab_audio, _kind_slider, "audio.dialog_volume", "Dialogue",
		"The volume of speech: Cortana, the Marines and the Covenant.", 0.0f, 1.0f, 0.05f },
	{ _tab_controls, _kind_slider, "input.mouse_sensitivity", "Mouse sensitivity",
		"How far the view turns for the mouse's movement.", 0.1f, 5.0f, 0.1f },
	{ _tab_controls, _kind_toggle, "input.invert_mouse", "Invert mouse",
		"Moving the mouse forward looks down." },
#define BIND_ITEM { _tab_keybinds, _kind_bind }
	BIND_ITEM, BIND_ITEM, BIND_ITEM, BIND_ITEM, BIND_ITEM, BIND_ITEM, BIND_ITEM,
	BIND_ITEM, BIND_ITEM, BIND_ITEM, BIND_ITEM, BIND_ITEM, BIND_ITEM, BIND_ITEM,
#undef BIND_ITEM
};

static const char *const tab_names[NUMBER_OF_TABS] = { "DISPLAY", "AUDIO", "CONTROLS", "KEYBINDS" };
static const char *const render_scale_settings[] = { "native", "integer", "original" };
static const char *const render_scale_names[] = { "Native", "Whole multiple", "Original (480p)" };
/* in the order of enum platform_display_mode */
static const char *const display_mode_names[] = { "Fullscreen", "Borderless", "Windowed" };
#define NUMBER_OF_RENDER_SCALES ((int)(sizeof(render_scale_settings) / sizeof(render_scale_settings[0])))

/* ---------- layout */

#define MAXIMUM_ROWS 16
/* the keybinds fill two columns of this many rows */
#define KEYBIND_ROWS_PER_COLUMN 7

struct box
{
	float x, y, width, height;
};

struct overlay_layout
{
	/* a pixel of the design, in the window's pixels */
	float unit;
	struct box panel;
	struct box tabs[NUMBER_OF_TABS];
	int row_count;
	int row_items[MAXIMUM_ROWS];
	struct box rows[MAXIMUM_ROWS];
	struct box controls[MAXIMUM_ROWS];
	/* where a slider's track is, in its control */
	struct box tracks[MAXIMUM_ROWS];
	struct box help;
	struct box footer;
	struct box quit;
};

/* ---------- state */

static struct
{
	BOOL open;
	enum overlay_tab tab;
	/* the row with the focus: row_count is the quit button */
	int selected;
	/* the quit button asks to be pressed again */
	BOOL quit_armed;
	/* the item a slider is dragged of, or -1 */
	int dragging;
	/* the item waiting for a key or mouse button, or -1 */
	int listening;

	/* choices are indices, toggles 0 or 1 */
	float values[NUMBER_OF_ITEMS];
	/* the window heights offered (platform_video_window_heights) */
	int window_heights[16];
	int window_height_count;
	/* changed, and not yet written into config.toml */
	BOOL unsaved[NUMBER_OF_ITEMS];

	/* the window, as last drawn */
	int width, height;
	float mouse_x, mouse_y;

	/* controllers */
	BOOL back_held, start_held;
	int stick_x, stick_y;
	BOOL release_pending;

	/* the frame counter, and the hint at start-up */
	Uint64 frames_since, frame_count;
	int frames_per_second;
	Uint64 hint_until;
	BOOL hint_checked;
} overlay = { FALSE, _tab_display, 0, FALSE, -1, -1 };

BOOL overlay_is_open(void)
{
	return overlay.open;
}

/* ---------- values */

static int choice_count(enum overlay_item item)
{
	switch (item)
	{
	case _item_display_mode:
		return 3;
	case _item_window_size:
		return overlay.window_height_count;
	case _item_render_scale:
		return NUMBER_OF_RENDER_SCALES;
	default:
		return 1;
	}
}

static void choice_text(enum overlay_item item, int index, char *text, size_t size)
{
	switch (item)
	{
	case _item_display_mode:
		snprintf(text, size, "%s", display_mode_names[index]);
		break;
	case _item_window_size:
	{
		int height = overlay.window_heights[index];

		snprintf(text, size, "%d x %d", platform_video_window_width(height), height);
		break;
	}
	case _item_render_scale:
		snprintf(text, size, "%s", render_scale_names[index]);
		break;
	default:
		text[0] = 0;
		break;
	}
}

static void slider_text(enum overlay_item item, char *text, size_t size)
{
	if (item == _item_mouse_sensitivity)
		snprintf(text, size, "%.1fx", overlay.values[item]);
	else
		snprintf(text, size, "%d%%", (int)floorf(overlay.values[item] * 100.0f + 0.5f));
}

static float clampf(float value, float minimum, float maximum)
{
	return value < minimum ? minimum : value > maximum ? maximum : value;
}

static const char *item_label(enum overlay_item item)
{
	return items[item].kind == _kind_bind ? input_action_label((enum input_action)(item - _item_bind_first)) :
		items[item].label;
}

static const char *item_help(enum overlay_item item)
{
	return items[item].kind == _kind_bind ? input_action_help((enum input_action)(item - _item_bind_first)) :
		items[item].help;
}

/* the window heights offered, and which is the window's: the largest no
taller than it */
static void window_heights_load(void)
{
	long height = config_integer("display.window_height");
	int index;

	if (height < 240)
		height = 480 * (config_integer("display.window_scale") < 1 ? 1 : config_integer("display.window_scale"));
	overlay.window_height_count = platform_video_window_heights(overlay.window_heights,
		(int)(sizeof(overlay.window_heights) / sizeof(overlay.window_heights[0])));
	overlay.values[_item_window_size] = 0.0f;
	for (index = 0; index < overlay.window_height_count; index++)
	{
		if (overlay.window_heights[index] <= height)
			overlay.values[_item_window_size] = (float)index;
	}
}

/* the values as they are now */
static void overlay_load(void)
{
	const char *render_scale = config_string("display.render_scale");
	int index;

	overlay.values[_item_display_mode] = (float)platform_video_display_mode();
	window_heights_load();
	overlay.values[_item_render_scale] = 0.0f;
	for (index = 0; index < NUMBER_OF_RENDER_SCALES; index++)
	{
		if (!strcmp(render_scale, render_scale_settings[index]))
			overlay.values[_item_render_scale] = (float)index;
	}
	for (index = 0; index < NUMBER_OF_ITEMS; index++)
	{
		if (items[index].kind == _kind_toggle)
			overlay.values[index] = config_boolean(items[index].setting) ? 1.0f : 0.0f;
		else if (items[index].kind == _kind_slider)
			overlay.values[index] = clampf((float)config_real(items[index].setting), items[index].minimum,
				items[index].maximum);
	}
}

/* makes an item's value take effect, and remembers to write it */
static void item_apply(enum overlay_item item)
{
	float value = overlay.values[item];

	switch (item)
	{
	case _item_display_mode:
		platform_video_set_display_mode((enum platform_display_mode)(int)value);
		config_set_boolean("display.fullscreen", value != (float)_platform_display_windowed);
		/* (a window keeps the fullscreen last chosen, for F11) */
		if (value != (float)_platform_display_windowed)
			config_set_boolean("display.exclusive", value == (float)_platform_display_exclusive);
		break;
	case _item_window_size:
		config_set_integer(items[item].setting, overlay.window_heights[(int)value]);
		platform_video_set_window_height(overlay.window_heights[(int)value]);
		break;
	case _item_widescreen:
		/* (d3d8_gl.c takes it up between frames; the window takes the shape) */
		config_set_boolean(items[item].setting, value != 0.0f);
		window_heights_load();
		platform_video_set_window_height(overlay.window_heights[(int)overlay.values[_item_window_size]]);
		break;
	case _item_render_scale:
		/* (d3d8_gl.c takes it up between frames) */
		config_set_string(items[item].setting, render_scale_settings[(int)value]);
		break;
	case _item_vsync:
		platform_video_set_vsync(value != 0.0f);
		config_set_boolean(items[item].setting, value != 0.0f);
		break;
	case _item_master_volume:
	case _item_music_volume:
	case _item_effects_volume:
	case _item_dialog_volume:
		config_set_real(items[item].setting, value);
		dsound_volumes_set(overlay.values[_item_master_volume], overlay.values[_item_music_volume],
			overlay.values[_item_effects_volume], overlay.values[_item_dialog_volume]);
		break;
	default:
		if (items[item].kind == _kind_toggle)
			config_set_boolean(items[item].setting, value != 0.0f);
		else
			config_set_real(items[item].setting, value);
		break;
	}
	overlay.unsaved[item] = TRUE;
}

/* writes the changed settings into config.toml */
static void overlay_save(void)
{
	int item;

	for (item = 0; item < NUMBER_OF_ITEMS; item++)
	{
		float value = overlay.values[item];
		int written;

		if (!overlay.unsaved[item])
			continue;
		overlay.unsaved[item] = FALSE;
		switch (item)
		{
		case _item_display_mode:
			written = config_write_boolean("display.fullscreen", value != (float)_platform_display_windowed);
			if (value != (float)_platform_display_windowed)
				written &= config_write_boolean("display.exclusive", value == (float)_platform_display_exclusive);
			break;
		case _item_window_size:
			written = config_write_integer(items[item].setting, overlay.window_heights[(int)value]);
			break;
		case _item_render_scale:
			written = config_write_string(items[item].setting, render_scale_settings[(int)value]);
			break;
		default:
			if (items[item].kind == _kind_bind)
			{
				const char *setting = input_action_setting((enum input_action)(item - _item_bind_first));

				written = config_write_string(setting, config_string(setting));
			}
			else if (items[item].kind == _kind_toggle)
				written = config_write_boolean(items[item].setting, value != 0.0f);
			else
				written = config_write_real(items[item].setting, value);
			break;
		}
		if (!written)
			platform_log("settings: cannot write %s", items[item].kind == _kind_bind ?
				input_action_setting((enum input_action)(item - _item_bind_first)) : items[item].setting);
	}
}

static void item_set(enum overlay_item item, float value)
{
	const struct item_definition *definition = &items[item];

	if (definition->kind == _kind_slider)
	{
		value = definition->minimum +
			floorf((value - definition->minimum) / definition->step + 0.5f) * definition->step;
		value = clampf(value, definition->minimum, definition->maximum);
	}
	else if (definition->kind == _kind_choice)
	{
		value = clampf(value, 0.0f, (float)(choice_count(item) - 1));
	}
	if (value == overlay.values[item])
		return;
	overlay.values[item] = value;
	item_apply(item);
}

/* left (-1) or right (1) */
static void item_step(enum overlay_item item, int direction)
{
	switch (items[item].kind)
	{
	case _kind_choice:
		item_set(item, overlay.values[item] + (float)direction);
		break;
	case _kind_toggle:
		item_set(item, direction > 0 ? 1.0f : 0.0f);
		break;
	case _kind_slider:
		item_set(item, overlay.values[item] + (float)direction * items[item].step);
		break;
	case _kind_bind:
		break;
	}
}

/* gives the item's action the binding */
static void item_bind(enum overlay_item item, int binding)
{
	int swapped = input_binding_set((enum input_action)(item - _item_bind_first), binding);

	overlay.unsaved[item] = TRUE;
	if (swapped >= 0)
		overlay.unsaved[_item_bind_first + swapped] = TRUE;
}

static void item_bind_default(enum overlay_item item)
{
	item_bind(item, input_binding_default((enum input_action)(item - _item_bind_first)));
}

/* enter, or a click on its label */
static void item_activate(enum overlay_item item)
{
	switch (items[item].kind)
	{
	case _kind_choice:
	{
		int count = choice_count(item);

		item_set(item, (float)(((int)overlay.values[item] + 1) % count));
		break;
	}
	case _kind_toggle:
		item_set(item, overlay.values[item] != 0.0f ? 0.0f : 1.0f);
		break;
	case _kind_slider:
		break;
	case _kind_bind:
		overlay.listening = (int)item;
		break;
	}
}

/* ---------- layout */

static float text_width(float size, const char *text)
{
	float width = 0.0f;

	for (; *text; text++)
	{
		int code = (unsigned char)*text;

		if (code >= OVERLAY_FONT_FIRST && code <= OVERLAY_FONT_LAST)
			width += overlay_font_glyphs[code - OVERLAY_FONT_FIRST].advance;
	}
	return width * size / (float)OVERLAY_FONT_SIZE;
}

static BOOL box_contains(const struct box *box, float x, float y)
{
	return x >= box->x && x < box->x + box->width && y >= box->y && y < box->y + box->height;
}

static struct box make_box(float x, float y, float width, float height)
{
	struct box box;

	box.x = x;
	box.y = y;
	box.width = width;
	box.height = height;
	return box;
}

static void overlay_layout(struct overlay_layout *layout)
{
	float unit = fminf((float)overlay.width / 1280.0f, (float)overlay.height / 800.0f);
	float x, right;
	int tab, item;

	memset(layout, 0, sizeof(*layout));
	unit = clampf(unit, 0.6f, 2.0f);
	layout->unit = unit;
	layout->panel = make_box(floorf(((float)overlay.width - 760.0f * unit) / 2.0f),
		floorf(((float)overlay.height - 600.0f * unit) / 2.0f), floorf(760.0f * unit), floorf(600.0f * unit));

	x = layout->panel.x + 24.0f * unit;
	right = layout->panel.x + layout->panel.width - 24.0f * unit;
	for (tab = 0; tab < NUMBER_OF_TABS; tab++)
	{
		float width = text_width(18.0f * unit, tab_names[tab]) + 32.0f * unit;

		layout->tabs[tab] = make_box(x, layout->panel.y + 68.0f * unit, width, 40.0f * unit);
		x += width + 4.0f * unit;
	}

	for (item = 0; item < NUMBER_OF_ITEMS; item++)
	{
		int row = layout->row_count;
		struct box *control;

		if (items[item].tab != overlay.tab || row >= MAXIMUM_ROWS)
			continue;
		layout->row_items[row] = item;
		if (items[item].kind == _kind_bind)
		{
			/* two columns */
			float width = (right - layout->panel.x - 24.0f * unit - 16.0f * unit) / 2.0f;
			int column = row / KEYBIND_ROWS_PER_COLUMN;

			layout->rows[row] = make_box(layout->panel.x + 24.0f * unit + column * (width + 16.0f * unit),
				layout->panel.y + (122.0f + 46.0f * (row % KEYBIND_ROWS_PER_COLUMN)) * unit, width, 42.0f * unit);
			control = &layout->controls[row];
			*control = make_box(layout->rows[row].x + width - 12.0f * unit - 140.0f * unit,
				layout->rows[row].y + (layout->rows[row].height - 30.0f * unit) / 2.0f, 140.0f * unit, 30.0f * unit);
		}
		else
		{
			layout->rows[row] = make_box(layout->panel.x + 24.0f * unit,
				layout->panel.y + (122.0f + 46.0f * row) * unit, right - layout->panel.x - 24.0f * unit, 42.0f * unit);
			control = &layout->controls[row];
			*control = make_box(right - 20.0f * unit - 300.0f * unit,
				layout->rows[row].y + (layout->rows[row].height - 30.0f * unit) / 2.0f, 300.0f * unit, 30.0f * unit);
		}
		layout->tracks[row] = make_box(control->x + 9.0f * unit, control->y + control->height / 2.0f - 3.0f * unit,
			control->width - 80.0f * unit, 6.0f * unit);
		layout->row_count++;
	}

	layout->help = make_box(layout->panel.x + 48.0f * unit, layout->panel.y + 460.0f * unit,
		layout->panel.width - 96.0f * unit, 70.0f * unit);
	layout->footer = make_box(layout->panel.x, layout->panel.y + layout->panel.height - 58.0f * unit,
		layout->panel.width, 58.0f * unit);
	layout->quit = make_box(right - 200.0f * unit, layout->footer.y + 11.0f * unit, 200.0f * unit, 36.0f * unit);
}

/* ---------- drawing */

struct overlay_vertex
{
	float x, y;
	/* from the box's center in pixels, or the atlas's coordinates */
	float u, v;
	float half_width, half_height;
	float radius, stroke;
	float kind;
	unsigned char color[4];
};

enum
{
	_shape_box,
	_shape_text,
	_shape_frame,
};

#define MAXIMUM_VERTICES 24576

static struct overlay_vertex vertices[MAXIMUM_VERTICES];
static int vertex_count;

static struct
{
	BOOL tried, ready;
	GLuint program, vertex_array, buffer, texture;
	GLint screen, atlas;
} overlay_gl;

#define COLOR(rgba) (unsigned long)(rgba)
#define COLOR_TEXT 0xE4ECF4FFUL
#define COLOR_DIM 0x8C9CB0FFUL
#define COLOR_ACCENT 0x52AEFFFFUL
#define COLOR_PANEL 0x0A1019F0UL
#define COLOR_DANGER 0xFF6060FFUL

static unsigned long color_alpha(unsigned long color, float alpha)
{
	unsigned long a = (unsigned long)((float)(color & 0xff) * clampf(alpha, 0.0f, 1.0f) + 0.5f);

	return (color & 0xffffff00UL) | a;
}

static void push_vertex(float x, float y, float u, float v, float half_width, float half_height, float radius,
	float stroke, int kind, unsigned long color)
{
	struct overlay_vertex *vertex = &vertices[vertex_count++];

	vertex->x = x;
	vertex->y = y;
	vertex->u = u;
	vertex->v = v;
	vertex->half_width = half_width;
	vertex->half_height = half_height;
	vertex->radius = radius;
	vertex->stroke = stroke;
	vertex->kind = (float)kind;
	vertex->color[0] = (unsigned char)(color >> 24);
	vertex->color[1] = (unsigned char)(color >> 16);
	vertex->color[2] = (unsigned char)(color >> 8);
	vertex->color[3] = (unsigned char)color;
}

static void push_quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1,
	float half_width, float half_height, float radius, float stroke, int kind, unsigned long color)
{
	if (vertex_count + 6 > MAXIMUM_VERTICES)
		return;
	push_vertex(x0, y0, u0, v0, half_width, half_height, radius, stroke, kind, color);
	push_vertex(x1, y0, u1, v0, half_width, half_height, radius, stroke, kind, color);
	push_vertex(x0, y1, u0, v1, half_width, half_height, radius, stroke, kind, color);
	push_vertex(x1, y0, u1, v0, half_width, half_height, radius, stroke, kind, color);
	push_vertex(x1, y1, u1, v1, half_width, half_height, radius, stroke, kind, color);
	push_vertex(x0, y1, u0, v1, half_width, half_height, radius, stroke, kind, color);
}

/* a box with rounded corners, or its outline stroke pixels wide; a pixel
of margin all round leaves room for the edge's smoothing */
static void draw_shape(struct box box, float radius, float stroke, unsigned long color)
{
	float half_width = box.width / 2.0f;
	float half_height = box.height / 2.0f;

	radius = fminf(radius, fminf(half_width, half_height));
	push_quad(box.x - 1.0f, box.y - 1.0f, box.x + box.width + 1.0f, box.y + box.height + 1.0f,
		-half_width - 1.0f, -half_height - 1.0f, half_width + 1.0f, half_height + 1.0f,
		half_width, half_height, radius, stroke, stroke > 0.0f ? _shape_frame : _shape_box, color);
}

static void draw_box(struct box box, float radius, unsigned long color)
{
	draw_shape(box, radius, 0.0f, color);
}

static void draw_frame(struct box box, float radius, float stroke, unsigned long color)
{
	draw_shape(box, radius, stroke, color);
}

static void draw_circle(float x, float y, float radius, unsigned long color)
{
	draw_box(make_box(x - radius, y - radius, radius * 2.0f, radius * 2.0f), radius, color);
}

/* from the pen on the baseline; returns the width */
static float draw_text(float x, float baseline, float size, unsigned long color, const char *text)
{
	float scale = size / (float)OVERLAY_FONT_SIZE;
	float pen = x;

	for (; *text; text++)
	{
		int code = (unsigned char)*text;
		const struct overlay_glyph *glyph;

		if (code < OVERLAY_FONT_FIRST || code > OVERLAY_FONT_LAST)
			continue;
		glyph = &overlay_font_glyphs[code - OVERLAY_FONT_FIRST];
		if (glyph->width)
		{
			float x0 = pen + glyph->left * scale;
			float y0 = baseline + glyph->top * scale;

			push_quad(x0, y0, x0 + glyph->width * scale, y0 + glyph->height * scale,
				(float)glyph->x / OVERLAY_FONT_ATLAS_WIDTH, (float)glyph->y / OVERLAY_FONT_ATLAS_HEIGHT,
				(float)(glyph->x + glyph->width) / OVERLAY_FONT_ATLAS_WIDTH,
				(float)(glyph->y + glyph->height) / OVERLAY_FONT_ATLAS_HEIGHT,
				0.0f, 0.0f, 0.0f, 0.0f, _shape_text, color);
		}
		pen += glyph->advance * scale;
	}
	return pen - x;
}

/* the baseline that centers capitals on y */
static float centered_baseline(float y, float size)
{
	return y + size * 0.34f;
}

/* words wrapped to width, at most lines of them */
static void draw_wrapped(struct box box, float size, float line_height, int lines, unsigned long color,
	const char *text)
{
	float baseline = box.y + size;

	while (*text && lines-- > 0)
	{
		char line[256];
		size_t length = 0, fits = 0;

		while (*text == ' ')
			text++;
		/* the most whole words that fit */
		while (text[length] && length + 1 < sizeof(line))
		{
			size_t end = length;

			while (text[end] == ' ')
				end++;
			while (text[end] && text[end] != ' ')
				end++;
			if (end >= sizeof(line))
				break;
			memcpy(line, text, end);
			line[end] = 0;
			if (fits && text_width(size, line) > box.width)
				break;
			fits = length = end;
		}
		if (!fits)
			break;
		memcpy(line, text, fits);
		line[fits] = 0;
		draw_text(box.x, baseline, size, color, line);
		text += fits;
		baseline += line_height;
	}
}

static void draw_control(const struct overlay_layout *layout, int row, BOOL selected)
{
	enum overlay_item item = (enum overlay_item)layout->row_items[row];
	struct box control = layout->controls[row];
	float unit = layout->unit;
	float middle = control.y + control.height / 2.0f;
	char text[64];

	switch (items[item].kind)
	{
	case _kind_choice:
	{
		int index = (int)overlay.values[item];
		int count = choice_count(item);
		float size = 18.0f * unit;

		draw_box(control, 6.0f * unit, selected ? 0xFFFFFF1AUL : 0xFFFFFF0DUL);
		draw_text(control.x + 12.0f * unit, centered_baseline(middle, size), size,
			index > 0 ? COLOR_ACCENT : color_alpha(COLOR_DIM, 0.4f), "<");
		draw_text(control.x + control.width - 12.0f * unit - text_width(size, ">"), centered_baseline(middle, size),
			size, index < count - 1 ? COLOR_ACCENT : color_alpha(COLOR_DIM, 0.4f), ">");
		choice_text(item, index, text, sizeof(text));
		draw_text(control.x + (control.width - text_width(size, text)) / 2.0f, centered_baseline(middle, size), size,
			COLOR_TEXT, text);
		break;
	}
	case _kind_toggle:
	{
		BOOL on = overlay.values[item] != 0.0f;
		struct box pill = make_box(control.x + control.width - 52.0f * unit, middle - 13.0f * unit, 52.0f * unit,
			26.0f * unit);
		float size = 17.0f * unit;
		const char *state = on ? "On" : "Off";

		draw_text(pill.x - 12.0f * unit - text_width(size, state), centered_baseline(middle, size), size,
			on ? COLOR_TEXT : COLOR_DIM, state);
		draw_box(pill, pill.height / 2.0f, on ? COLOR_ACCENT : 0xFFFFFF26UL);
		draw_circle(on ? pill.x + pill.width - 13.0f * unit : pill.x + 13.0f * unit, middle, 10.0f * unit,
			0xFFFFFFFFUL);
		break;
	}
	case _kind_slider:
	{
		struct box track = layout->tracks[row];
		float fraction = (overlay.values[item] - items[item].minimum) / (items[item].maximum - items[item].minimum);
		float size = 17.0f * unit;

		draw_box(track, track.height / 2.0f, 0xFFFFFF26UL);
		draw_box(make_box(track.x, track.y, track.width * fraction, track.height), track.height / 2.0f, COLOR_ACCENT);
		draw_circle(track.x + track.width * fraction, middle, (selected ? 10.0f : 9.0f) * unit, 0xFFFFFFFFUL);
		slider_text(item, text, sizeof(text));
		draw_text(control.x + control.width - text_width(size, text), centered_baseline(middle, size), size,
			COLOR_TEXT, text);
		break;
	}
	case _kind_bind:
	{
		BOOL listening = overlay.listening == (int)item;
		float size = 17.0f * unit;

		draw_box(control, 6.0f * unit, listening ? color_alpha(COLOR_ACCENT, 0.25f) :
			selected ? 0xFFFFFF1AUL : 0xFFFFFF0DUL);
		if (listening)
		{
			strcpy(text, "Press a key");
			draw_frame(control, 6.0f * unit, fmaxf(1.0f, unit), COLOR_ACCENT);
		}
		else
		{
			input_binding_name(input_binding((enum input_action)(item - _item_bind_first)), text, sizeof(text));
		}
		draw_text(control.x + (control.width - text_width(size, text)) / 2.0f, centered_baseline(middle, size), size,
			listening ? COLOR_ACCENT : COLOR_TEXT, text);
		break;
	}
	}
}

static void draw_panel(const struct overlay_layout *layout)
{
	float unit = layout->unit;
	struct box panel = layout->panel;
	float left = panel.x + 24.0f * unit;
	float right = panel.x + panel.width - 24.0f * unit;
	const char *help = NULL;
	int tab, row;

	draw_box(make_box(0.0f, 0.0f, (float)overlay.width, (float)overlay.height), 0.0f, 0x000000A0UL);
	draw_box(panel, 12.0f * unit, COLOR_PANEL);
	draw_frame(panel, 12.0f * unit, fmaxf(1.0f, 1.5f * unit), color_alpha(COLOR_ACCENT, 0.35f));

	draw_text(left + 4.0f * unit, panel.y + 52.0f * unit, 30.0f * unit, COLOR_TEXT, "SETTINGS");
	draw_box(make_box(left + 4.0f * unit, panel.y + 60.0f * unit, 40.0f * unit, 3.0f * unit), 1.5f * unit,
		COLOR_ACCENT);

	for (tab = 0; tab < NUMBER_OF_TABS; tab++)
	{
		struct box box = layout->tabs[tab];
		float size = 18.0f * unit;
		BOOL active = (int)overlay.tab == tab;
		BOOL hovered = box_contains(&box, overlay.mouse_x, overlay.mouse_y);

		draw_text(box.x + 16.0f * unit, centered_baseline(box.y + box.height / 2.0f, size), size,
			active ? COLOR_TEXT : hovered ? 0xC0CCDAFFUL : COLOR_DIM, tab_names[tab]);
		if (active)
		{
			draw_box(make_box(box.x + 16.0f * unit, box.y + box.height - 4.0f * unit, box.width - 32.0f * unit,
				3.0f * unit), 1.5f * unit, COLOR_ACCENT);
		}
	}
	draw_box(make_box(left, panel.y + 110.0f * unit, right - left, fmaxf(1.0f, unit)), 0.0f, 0xFFFFFF18UL);

	for (row = 0; row < layout->row_count; row++)
	{
		enum overlay_item item = (enum overlay_item)layout->row_items[row];
		struct box box = layout->rows[row];
		BOOL selected = overlay.selected == row;
		float size = 20.0f * unit;

		if (selected)
		{
			draw_box(box, 6.0f * unit, color_alpha(COLOR_ACCENT, 0.12f));
			draw_box(make_box(box.x, box.y + 9.0f * unit, 3.0f * unit, box.height - 18.0f * unit), 1.5f * unit,
				COLOR_ACCENT);
			help = item_help(item);
		}
		draw_text(box.x + 20.0f * unit, centered_baseline(box.y + box.height / 2.0f, size), size,
			selected ? COLOR_TEXT : 0xC8D2DEFFUL, item_label(item));
		draw_control(layout, row, selected);
	}

	if (help)
		draw_wrapped(layout->help, 16.0f * unit, 22.0f * unit, 3, COLOR_DIM, help);

	draw_box(make_box(left, layout->footer.y, right - left, fmaxf(1.0f, unit)), 0.0f, 0xFFFFFF18UL);
	{
		float size = 15.0f * unit;
		float baseline = centered_baseline(layout->footer.y + layout->footer.height / 2.0f, size);
		float x = left + 4.0f * unit;
		static const char *const keys[][2] =
		{
			{ "F10", "Close" },
			{ "Q  E", "Tabs" },
			{ "Arrows", "Change" },
			{ "Del", "Default key" },
		};
		int count = (int)(sizeof(keys) / sizeof(keys[0])) - (overlay.tab == _tab_keybinds ? 0 : 1);
		int index;

		if (overlay.listening >= 0)
		{
			x += draw_text(x, baseline, size, COLOR_TEXT, "Esc") + 7.0f * unit;
			draw_text(x, baseline, size, COLOR_DIM, "Cancel");
			count = 0;
		}
		for (index = 0; index < count; index++)
		{
			x += draw_text(x, baseline, size, COLOR_TEXT, keys[index][0]) + 7.0f * unit;
			x += draw_text(x, baseline, size, COLOR_DIM, keys[index][1]) + 22.0f * unit;
		}
	}
	{
		struct box quit = layout->quit;
		BOOL selected = overlay.selected == layout->row_count;
		BOOL hovered = box_contains(&quit, overlay.mouse_x, overlay.mouse_y);
		const char *label = overlay.quit_armed ? "Press again to quit" : "Quit game";
		float size = 17.0f * unit;

		draw_box(quit, 6.0f * unit, overlay.quit_armed ? color_alpha(COLOR_DANGER, 0.25f) :
			selected || hovered ? 0xFFFFFF1AUL : 0xFFFFFF0AUL);
		draw_frame(quit, 6.0f * unit, fmaxf(1.0f, unit),
			selected || overlay.quit_armed ? COLOR_DANGER : 0xFFFFFF30UL);
		draw_text(quit.x + (quit.width - text_width(size, label)) / 2.0f,
			centered_baseline(quit.y + quit.height / 2.0f, size), size,
			overlay.quit_armed || selected ? COLOR_DANGER : COLOR_TEXT, label);
	}
}

static void draw_frames_per_second(float unit)
{
	char text[32];
	float size = 16.0f * unit;
	float width;

	snprintf(text, sizeof(text), "%d FPS", overlay.frames_per_second);
	width = text_width(size, text);
	draw_box(make_box(10.0f * unit, 10.0f * unit, width + 16.0f * unit, 26.0f * unit), 5.0f * unit, 0x00000099UL);
	draw_text(18.0f * unit, centered_baseline(23.0f * unit, size), size, COLOR_TEXT, text);
}

static void draw_hint(float unit, float alpha)
{
	const char *key = "F10";
	const char *text = "  Settings";
	float size = 19.0f * unit;
	float width = text_width(size, key) + text_width(size, text);
	struct box box = make_box(((float)overlay.width - width) / 2.0f - 18.0f * unit,
		(float)overlay.height - 76.0f * unit, width + 36.0f * unit, 40.0f * unit);
	float baseline = centered_baseline(box.y + box.height / 2.0f, size);
	float x = box.x + 18.0f * unit;

	draw_box(box, 8.0f * unit, color_alpha(COLOR_PANEL, 0.85f * alpha));
	draw_frame(box, 8.0f * unit, fmaxf(1.0f, unit), color_alpha(COLOR_ACCENT, 0.5f * alpha));
	x += draw_text(x, baseline, size, color_alpha(COLOR_ACCENT, alpha), key);
	draw_text(x, baseline, size, color_alpha(COLOR_TEXT, alpha), text);
}

/* ---------- OpenGL */

static const char overlay_vertex_shader[] =
	"#version 330 core\n"
	"in vec2 position;\n"
	"in vec2 coordinate_in;\n"
	"in vec2 half_size_in;\n"
	"in vec2 shape_in;\n"
	"in float kind_in;\n"
	"in vec4 color_in;\n"
	"uniform vec2 screen;\n"
	"out vec2 coordinate;\n"
	"flat out vec2 half_size;\n"
	"flat out vec2 shape;\n"
	"flat out float kind;\n"
	"out vec4 color;\n"
	"void main()\n"
	"{\n"
	"	coordinate = coordinate_in;\n"
	"	half_size = half_size_in;\n"
	"	shape = shape_in;\n"
	"	kind = kind_in;\n"
	"	color = color_in;\n"
	/* window pixels down from the top; the renderer's clip control
	(GL_UPPER_LEFT, d3d8_gl.c) puts -1 at the top */
	"	gl_Position = vec4(position.x / screen.x * 2.0 - 1.0, position.y / screen.y * 2.0 - 1.0, 0.0, 1.0);\n"
	"}\n";

static const char overlay_fragment_shader[] =
	"#version 330 core\n"
	"in vec2 coordinate;\n"
	"flat in vec2 half_size;\n"
	"flat in vec2 shape;\n"
	"flat in float kind;\n"
	"in vec4 color;\n"
	"uniform sampler2D atlas;\n"
	"out vec4 fragment;\n"
	"void main()\n"
	"{\n"
	"	float alpha;\n"
	"	if (kind > 0.5 && kind < 1.5)\n"
	"	{\n"
	"		float distance = texture(atlas, coordinate).r;\n"
	"		float width = max(fwidth(distance), 0.0001);\n"
	"		alpha = clamp((distance - 0.5) / width + 0.5, 0.0, 1.0);\n"
	"	}\n"
	"	else\n"
	"	{\n"
	"		vec2 q = abs(coordinate) - half_size + shape.x;\n"
	"		float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - shape.x;\n"
	"		alpha = clamp(0.5 - d, 0.0, 1.0);\n"
	"		if (kind > 1.5)\n"
	"			alpha *= clamp(0.5 + d + shape.y, 0.0, 1.0);\n"
	"	}\n"
	"	fragment = vec4(color.rgb, color.a * alpha);\n"
	"}\n";

static GLuint overlay_compile(GLenum type, const char *source)
{
	GLuint shader = glCreateShader(type);
	GLint status = 0;

	glShaderSource(shader, 1, &source, NULL);
	glCompileShader(shader);
	glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
	if (!status)
	{
		char log[2048];

		glGetShaderInfoLog(shader, sizeof(log), NULL, log);
		platform_log("overlay: cannot compile a shader: %s", log);
		glDeleteShader(shader);
		return 0;
	}
	return shader;
}

static BOOL overlay_gl_initialize(void)
{
	static const char *const attributes[] =
		{ "position", "coordinate_in", "half_size_in", "shape_in", "kind_in", "color_in" };
	GLuint vertex_shader, fragment_shader;
	GLint status = 0;
	GLuint index;

	if (overlay_gl.tried)
		return overlay_gl.ready;
	overlay_gl.tried = TRUE;
	vertex_shader = overlay_compile(GL_VERTEX_SHADER, overlay_vertex_shader);
	fragment_shader = overlay_compile(GL_FRAGMENT_SHADER, overlay_fragment_shader);
	if (!vertex_shader || !fragment_shader)
		return FALSE;
	overlay_gl.program = glCreateProgram();
	glAttachShader(overlay_gl.program, vertex_shader);
	glAttachShader(overlay_gl.program, fragment_shader);
	for (index = 0; index < sizeof(attributes) / sizeof(attributes[0]); index++)
		glBindAttribLocation(overlay_gl.program, index, attributes[index]);
	glLinkProgram(overlay_gl.program);
	glDeleteShader(vertex_shader);
	glDeleteShader(fragment_shader);
	glGetProgramiv(overlay_gl.program, GL_LINK_STATUS, &status);
	if (!status)
	{
		char log[2048];

		glGetProgramInfoLog(overlay_gl.program, sizeof(log), NULL, log);
		platform_log("overlay: cannot link its program: %s", log);
		return FALSE;
	}
	overlay_gl.screen = glGetUniformLocation(overlay_gl.program, "screen");
	overlay_gl.atlas = glGetUniformLocation(overlay_gl.program, "atlas");

	glGenVertexArrays(1, &overlay_gl.vertex_array);
	glBindVertexArray(overlay_gl.vertex_array);
	glGenBuffers(1, &overlay_gl.buffer);
	glBindBuffer(GL_ARRAY_BUFFER, overlay_gl.buffer);
#define ATTRIBUTE(index, count, type, normalized, field) \
	glEnableVertexAttribArray(index); \
	glVertexAttribPointer(index, count, type, normalized, sizeof(struct overlay_vertex), \
		(const void *)offsetof(struct overlay_vertex, field))
	ATTRIBUTE(0, 2, GL_FLOAT, GL_FALSE, x);
	ATTRIBUTE(1, 2, GL_FLOAT, GL_FALSE, u);
	ATTRIBUTE(2, 2, GL_FLOAT, GL_FALSE, half_width);
	ATTRIBUTE(3, 2, GL_FLOAT, GL_FALSE, radius);
	ATTRIBUTE(4, 1, GL_FLOAT, GL_FALSE, kind);
	ATTRIBUTE(5, 4, GL_UNSIGNED_BYTE, GL_TRUE, color);
#undef ATTRIBUTE

	glActiveTexture(GL_TEXTURE0);
	glGenTextures(1, &overlay_gl.texture);
	glBindTexture(GL_TEXTURE_2D, overlay_gl.texture);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, OVERLAY_FONT_ATLAS_WIDTH, OVERLAY_FONT_ATLAS_HEIGHT, 0, GL_RED,
		GL_UNSIGNED_BYTE, overlay_font_atlas);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	overlay_gl.ready = TRUE;
	return TRUE;
}

static void overlay_submit(int width, int height)
{
	if (!vertex_count || !overlay_gl_initialize())
		return;
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glViewport(0, 0, width, height);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_STENCIL_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_SCISSOR_TEST);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
	glEnable(GL_BLEND);
	glBlendEquation(GL_FUNC_ADD);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glUseProgram(overlay_gl.program);
	glUniform2f(overlay_gl.screen, (float)width, (float)height);
	glUniform1i(overlay_gl.atlas, 0);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, overlay_gl.texture);
	glBindSampler(0, 0);
	glBindVertexArray(overlay_gl.vertex_array);
	glBindBuffer(GL_ARRAY_BUFFER, overlay_gl.buffer);
	glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(vertex_count * sizeof(struct overlay_vertex)), vertices,
		GL_STREAM_DRAW);
	glDrawArrays(GL_TRIANGLES, 0, vertex_count);
}

void overlay_draw(int width, int height)
{
	Uint64 now = SDL_GetTicks();
	float unit;

	overlay.width = width;
	overlay.height = height;
	vertex_count = 0;
	unit = clampf(fminf((float)width / 1280.0f, (float)height / 800.0f), 0.6f, 2.0f);

	/* frames drawn in the last half second */
	overlay.frame_count++;
	if (!overlay.frames_since)
		overlay.frames_since = now;
	if (now - overlay.frames_since >= 500)
	{
		overlay.frames_per_second = (int)(overlay.frame_count * 1000 / (now - overlay.frames_since));
		overlay.frame_count = 0;
		overlay.frames_since = now;
	}
	if (config_boolean("display.show_fps"))
		draw_frames_per_second(unit);

	if (!overlay.hint_checked)
	{
		overlay.hint_checked = TRUE;
		if (config_boolean("display.settings_hint"))
			overlay.hint_until = now + 10000;
	}
	if (!overlay.open && now < overlay.hint_until)
	{
		Uint64 left = overlay.hint_until - now;

		draw_hint(unit, left < 1000 ? (float)left / 1000.0f : 1.0f);
	}

	if (overlay.open)
	{
		struct overlay_layout layout;

		/* (F11 switches outside the overlay too) */
		overlay.values[_item_display_mode] = (float)platform_video_display_mode();
		overlay_layout(&layout);
		draw_panel(&layout);
	}
	overlay_submit(width, height);
}

/* ---------- input */

static enum overlay_event_result overlay_open(void)
{
	overlay.open = TRUE;
	overlay.selected = 0;
	overlay.quit_armed = FALSE;
	overlay.dragging = -1;
	overlay.listening = -1;
	overlay.mouse_x = overlay.mouse_y = -1.0f;
	overlay.hint_until = 0;
	overlay_load();
	if (config_boolean("display.settings_hint"))
		config_write_boolean("display.settings_hint", FALSE);
	return _overlay_event_opened;
}

static enum overlay_event_result overlay_close(void)
{
	overlay.open = FALSE;
	overlay.dragging = -1;
	overlay.listening = -1;
	overlay.quit_armed = FALSE;
	overlay_save();
	return _overlay_event_closed;
}

static void overlay_quit(void)
{
	if (!overlay.quit_armed)
	{
		overlay.quit_armed = TRUE;
		return;
	}
	overlay_save();
	platform_log("quit from the settings overlay");
	exit(EXIT_SUCCESS);
}

static void select_row(int row, int row_count)
{
	row = (row + row_count + 1) % (row_count + 1);
	if (row != overlay.selected)
		overlay.quit_armed = FALSE;
	overlay.selected = row;
}

static void select_tab(int tab)
{
	overlay.tab = (enum overlay_tab)((tab + NUMBER_OF_TABS) % NUMBER_OF_TABS);
	overlay.selected = 0;
	overlay.dragging = -1;
	overlay.listening = -1;
	overlay.quit_armed = FALSE;
}

/* the window's coordinates, as SDL reports them, in its pixels */
static void pointer_pixels(float x, float y, float *pixel_x, float *pixel_y)
{
	int window_width = 0, window_height = 0, pixel_width = 0, pixel_height = 0;

	platform_video_window_size(&window_width, &window_height);
	platform_video_drawable_size(&pixel_width, &pixel_height);
	*pixel_x = window_width > 0 ? x * (float)pixel_width / (float)window_width : x;
	*pixel_y = window_height > 0 ? y * (float)pixel_height / (float)window_height : y;
}

static void slider_drag(const struct overlay_layout *layout, int row)
{
	enum overlay_item item = (enum overlay_item)layout->row_items[row];
	const struct box *track = &layout->tracks[row];
	float fraction = clampf((overlay.mouse_x - track->x) / track->width, 0.0f, 1.0f);

	item_set(item, items[item].minimum + fraction * (items[item].maximum - items[item].minimum));
}

/* what the pointer is over: a row, the quit button (row_count), or -1 */
static int row_at(const struct overlay_layout *layout, float x, float y)
{
	int row;

	for (row = 0; row < layout->row_count; row++)
	{
		if (box_contains(&layout->rows[row], x, y))
			return row;
	}
	return box_contains(&layout->quit, x, y) ? layout->row_count : -1;
}

static enum overlay_event_result overlay_mouse(const SDL_Event *event)
{
	struct overlay_layout layout;
	int row, tab;

	overlay_layout(&layout);
	switch (event->type)
	{
	case SDL_EVENT_MOUSE_MOTION:
		pointer_pixels(event->motion.x, event->motion.y, &overlay.mouse_x, &overlay.mouse_y);
		if (overlay.dragging >= 0)
		{
			for (row = 0; row < layout.row_count; row++)
			{
				if (layout.row_items[row] == overlay.dragging)
					slider_drag(&layout, row);
			}
		}
		else if ((row = row_at(&layout, overlay.mouse_x, overlay.mouse_y)) >= 0)
		{
			select_row(row, layout.row_count);
		}
		break;
	case SDL_EVENT_MOUSE_BUTTON_DOWN:
		pointer_pixels(event->button.x, event->button.y, &overlay.mouse_x, &overlay.mouse_y);
		if (overlay.listening >= 0)
		{
			/* any button is the answer */
			if (input_binding_allowed(INPUT_BINDING_MOUSE + event->button.button))
				item_bind((enum overlay_item)overlay.listening, INPUT_BINDING_MOUSE + event->button.button);
			overlay.listening = -1;
			break;
		}
		if (event->button.button == SDL_BUTTON_RIGHT)
			return overlay_close();
		if (event->button.button != SDL_BUTTON_LEFT)
			break;
		if (!box_contains(&layout.panel, overlay.mouse_x, overlay.mouse_y))
			return overlay_close();
		for (tab = 0; tab < NUMBER_OF_TABS; tab++)
		{
			if (box_contains(&layout.tabs[tab], overlay.mouse_x, overlay.mouse_y))
				select_tab(tab);
		}
		row = row_at(&layout, overlay.mouse_x, overlay.mouse_y);
		if (row < 0)
			break;
		select_row(row, layout.row_count);
		if (row == layout.row_count)
		{
			overlay_quit();
		}
		else
		{
			enum overlay_item item = (enum overlay_item)layout.row_items[row];
			const struct box *control = &layout.controls[row];

			if (items[item].kind == _kind_slider)
			{
				if (overlay.mouse_x >= control->x)
				{
					overlay.dragging = item;
					slider_drag(&layout, row);
				}
			}
			else if (items[item].kind == _kind_choice && box_contains(control, overlay.mouse_x, overlay.mouse_y))
			{
				/* as the game's menus: the left half goes back, the right on */
				item_step(item, overlay.mouse_x < control->x + control->width / 2.0f ? -1 : 1);
			}
			else
			{
				item_activate(item);
			}
		}
		break;
	case SDL_EVENT_MOUSE_BUTTON_UP:
		if (event->button.button == SDL_BUTTON_LEFT)
			overlay.dragging = -1;
		break;
	case SDL_EVENT_MOUSE_WHEEL:
		row = row_at(&layout, overlay.mouse_x, overlay.mouse_y);
		if (row >= 0 && row < layout.row_count && event->wheel.y != 0.0f &&
			items[layout.row_items[row]].kind != _kind_toggle)
		{
			item_step((enum overlay_item)layout.row_items[row], event->wheel.y > 0.0f ? 1 : -1);
		}
		break;
	default:
		break;
	}
	if (overlay.dragging < 0)
		overlay_save();
	return _overlay_event_consumed;
}

enum overlay_navigation
{
	_navigate_up,
	_navigate_down,
	_navigate_left,
	_navigate_right,
	_navigate_activate,
	_navigate_previous_tab,
	_navigate_next_tab,
};

static void overlay_navigate(enum overlay_navigation navigation)
{
	struct overlay_layout layout;
	BOOL on_row;

	if (overlay.listening >= 0)
		return;
	overlay_layout(&layout);
	on_row = overlay.selected < layout.row_count;
	switch (navigation)
	{
	case _navigate_up:
		select_row(overlay.selected - 1, layout.row_count);
		break;
	case _navigate_down:
		select_row(overlay.selected + 1, layout.row_count);
		break;
	case _navigate_left:
	case _navigate_right:
		if (on_row && items[layout.row_items[overlay.selected]].kind == _kind_bind)
		{
			/* to the other column */
			int row = overlay.selected + (navigation == _navigate_left ? -1 : 1) * KEYBIND_ROWS_PER_COLUMN;

			if (row >= 0 && row < layout.row_count)
				select_row(row, layout.row_count);
		}
		else if (on_row)
		{
			item_step((enum overlay_item)layout.row_items[overlay.selected], navigation == _navigate_left ? -1 : 1);
		}
		break;
	case _navigate_activate:
		if (on_row)
			item_activate((enum overlay_item)layout.row_items[overlay.selected]);
		else
			overlay_quit();
		break;
	case _navigate_previous_tab:
		select_tab((int)overlay.tab - 1);
		break;
	case _navigate_next_tab:
		select_tab((int)overlay.tab + 1);
		break;
	}
	overlay_save();
}

static enum overlay_event_result overlay_key(const SDL_KeyboardEvent *key)
{
	if (!key->down)
		return _overlay_event_consumed;
	if (overlay.listening >= 0)
	{
		/* the next key is the answer; escape and F10 give up */
		if (key->repeat)
			return _overlay_event_consumed;
		if (input_binding_allowed((int)key->scancode))
			item_bind((enum overlay_item)overlay.listening, (int)key->scancode);
		if (key->scancode == SDL_SCANCODE_ESCAPE || key->scancode == SDL_SCANCODE_F10 ||
			input_binding_allowed((int)key->scancode))
		{
			overlay.listening = -1;
			overlay_save();
		}
		return _overlay_event_consumed;
	}
	switch (key->scancode)
	{
	case SDL_SCANCODE_DELETE:
		if (overlay.tab == _tab_keybinds)
		{
			struct overlay_layout layout;

			overlay_layout(&layout);
			if (overlay.selected < layout.row_count)
			{
				item_bind_default((enum overlay_item)layout.row_items[overlay.selected]);
				overlay_save();
			}
		}
		break;
	case SDL_SCANCODE_ESCAPE:
	case SDL_SCANCODE_F10:
		if (!key->repeat)
			return overlay_close();
		break;
	case SDL_SCANCODE_UP:
	case SDL_SCANCODE_W:
		overlay_navigate(_navigate_up);
		break;
	case SDL_SCANCODE_DOWN:
	case SDL_SCANCODE_S:
		overlay_navigate(_navigate_down);
		break;
	case SDL_SCANCODE_LEFT:
	case SDL_SCANCODE_A:
		overlay_navigate(_navigate_left);
		break;
	case SDL_SCANCODE_RIGHT:
	case SDL_SCANCODE_D:
		overlay_navigate(_navigate_right);
		break;
	case SDL_SCANCODE_RETURN:
	case SDL_SCANCODE_KP_ENTER:
	case SDL_SCANCODE_SPACE:
		if (!key->repeat)
			overlay_navigate(_navigate_activate);
		break;
	case SDL_SCANCODE_Q:
	case SDL_SCANCODE_PAGEUP:
		overlay_navigate(_navigate_previous_tab);
		break;
	case SDL_SCANCODE_E:
	case SDL_SCANCODE_PAGEDOWN:
		overlay_navigate(_navigate_next_tab);
		break;
	case SDL_SCANCODE_TAB:
		overlay_navigate(key->mod & SDL_KMOD_SHIFT ? _navigate_previous_tab : _navigate_next_tab);
		break;
	default:
		break;
	}
	return _overlay_event_consumed;
}

static enum overlay_event_result overlay_gamepad(const SDL_Event *event)
{
	if (event->type == SDL_EVENT_GAMEPAD_AXIS_MOTION)
	{
		/* the left stick moves the focus once each time it is pushed */
		int *state = event->gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX ? &overlay.stick_x :
			event->gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY ? &overlay.stick_y : NULL;
		int direction = event->gaxis.value > 20000 ? 1 : event->gaxis.value < -20000 ? -1 : 0;

		if (!state)
			return _overlay_event_consumed;
		if (direction && direction != *state)
		{
			if (state == &overlay.stick_x)
				overlay_navigate(direction < 0 ? _navigate_left : _navigate_right);
			else
				overlay_navigate(direction < 0 ? _navigate_up : _navigate_down);
		}
		if (direction || abs(event->gaxis.value) < 10000)
			*state = direction;
		return _overlay_event_consumed;
	}
	if (!event->gbutton.down)
	{
		/* (on letting go, so the game does not see B when it resumes) */
		if (event->gbutton.button == SDL_GAMEPAD_BUTTON_EAST)
		{
			if (overlay.listening >= 0)
			{
				overlay.listening = -1;
				return _overlay_event_consumed;
			}
			return overlay_close();
		}
		return _overlay_event_consumed;
	}
	switch (event->gbutton.button)
	{
	case SDL_GAMEPAD_BUTTON_DPAD_UP:
		overlay_navigate(_navigate_up);
		break;
	case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
		overlay_navigate(_navigate_down);
		break;
	case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
		overlay_navigate(_navigate_left);
		break;
	case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
		overlay_navigate(_navigate_right);
		break;
	case SDL_GAMEPAD_BUTTON_SOUTH:
		overlay_navigate(_navigate_activate);
		break;
	case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:
		overlay_navigate(_navigate_previous_tab);
		break;
	case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:
		overlay_navigate(_navigate_next_tab);
		break;
	default:
		break;
	}
	return _overlay_event_consumed;
}

enum overlay_event_result overlay_handle_event(const SDL_Event *event)
{
	switch (event->type)
	{
	case SDL_EVENT_KEY_DOWN:
	case SDL_EVENT_KEY_UP:
		if (!overlay.open)
		{
			if (event->key.down && !event->key.repeat && event->key.scancode == SDL_SCANCODE_F10)
				return overlay_open();
			return _overlay_event_ignored;
		}
		/* (F11 still switches to and from fullscreen) */
		if (event->key.scancode == SDL_SCANCODE_F11)
			return _overlay_event_ignored;
		return overlay_key(&event->key);
	case SDL_EVENT_MOUSE_MOTION:
	case SDL_EVENT_MOUSE_BUTTON_DOWN:
	case SDL_EVENT_MOUSE_BUTTON_UP:
	case SDL_EVENT_MOUSE_WHEEL:
		return overlay.open ? overlay_mouse(event) : _overlay_event_ignored;
	case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
	case SDL_EVENT_GAMEPAD_BUTTON_UP:
		if (event->gbutton.button == SDL_GAMEPAD_BUTTON_BACK)
			overlay.back_held = event->gbutton.down;
		if (event->gbutton.button == SDL_GAMEPAD_BUTTON_START)
			overlay.start_held = event->gbutton.down;
		/* Back and Start together open and close it */
		if (event->gbutton.down && overlay.back_held && overlay.start_held &&
			(event->gbutton.button == SDL_GAMEPAD_BUTTON_BACK || event->gbutton.button == SDL_GAMEPAD_BUTTON_START))
		{
			return overlay.open ? overlay_close() : overlay_open();
		}
		return overlay.open ? overlay_gamepad(event) : _overlay_event_ignored;
	case SDL_EVENT_GAMEPAD_AXIS_MOTION:
		return overlay.open ? overlay_gamepad(event) : _overlay_event_ignored;
	case SDL_EVENT_WINDOW_FOCUS_LOST:
		overlay.dragging = -1;
		overlay_save();
		return _overlay_event_ignored;
	default:
		return _overlay_event_ignored;
	}
}

BOOL overlay_holds_gamepad(const XINPUT_GAMEPAD *pad)
{
	BOOL pressed = pad->wButtons != 0;
	int index;

	for (index = 0; index < 8; index++)
	{
		if (pad->bAnalogButtons[index] > 30)
			pressed = TRUE;
	}
	if (overlay.open)
	{
		overlay.release_pending = TRUE;
		return TRUE;
	}
	if (overlay.release_pending)
	{
		if (pressed)
			return TRUE;
		overlay.release_pending = FALSE;
	}
	return FALSE;
}

#endif
