/*
 * OpenCE Forge ("HaloCE Forge.exe")
 * Mode 2: 3D In-Engine Mode (Single-Player & Online Multiplayer / Co-op)
 * - 6DOF 343 Guilty Spark Monitor flight with native Mouse Look
 * - 3rd-Person Monitor View toggle (F4 or console `third_person`)
 * - Physics Beam / Raycast Object Grab, Move, Rotate (15/45/90 deg snap) & Ground Snap
 * - Halo 3-Style Left-Aligned Compact Forge Menu & Object Properties using small terminal font
 * - Interactive AI Unit Placement (Friendly vs. Enemy)
 * - "Inject Custom Tags" UI Modal & "Save Copy As..." Standalone Map Export Modal
 * Strictly modular inside FORGE CODE/ with zero modifications to source/ or port/.
 */

#include "cseries.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "game/game.h"
#include "game/players.h"
#include "camera/director.h"
#include "camera/observer.h"
#include "objects/objects.h"
#include "units/units.h"
#include "physics/collisions.h"
#include "interface/interface.h"
#include "interface/ui_widget.h"
#include "scenario/scenario.h"
#include "scenario/scenario_definitions.h"
#include "structures/structure_bsp_definitions.h"
#include "bitmaps/bitmap_group.h"
#include "bitmaps/bitmaps.h"
#include "rasterizer/rasterizer.h"
#include "render/render.h"
#include "render/render_cameras.h"
#include "text/draw_string.h"

#include "objects/object_lights.h"
#include "objects/widgets/widgets.h"
#include "devices/devices.h"
#include "devices/device_light_fixtures.h"
#include "forge_engine.h"

extern unsigned long system_milliseconds(void);
extern void halo_linux_mouse_look(short controller_index, float *yaw_delta, float *pitch_delta);
extern int platform_scoreboard_scroll(int active, int *wheel_notches, float *touch_dy);
extern boolean render_camera_world_to_screen(
	const struct render_camera *camera,
	const struct render_frustum *frustum,
	const real_point3d *world_point,
	real_point2d *screen_point);
extern boolean rasterizer_bitmap_new(struct bitmap_data *bitmap);
extern void halo_menus_art_register(void const *texture, char const *png);

static void forge_hide_spartan_linked_attachments_and_children(long unit_idx, int hide_linked)
{
	struct unit_datum *u;
	struct object_definition *def;
	long child_idx;
	short a;

	u = (struct unit_datum *)object_try_and_get_and_verify_type(unit_idx, _object_mask_unit);
	if (!u)
		return;

	/* Setting active_camouflage = 1.0f causes lights_preprocess_scene() (object_lights.c:1126),
	 * shield_glow_flare() (shield_glow.c:192), and unit shadows (render_objects.c:795) to scale
	 * attached lens flares (the 2 green helmet lights), point lights, and shield glow to 0.0f
	 * without deleting any light/widget datums (preventing data.c:412 assert crash on F1),
	 * while still invoking render_model() which our hook intercepts to draw 343 Guilty Spark! */
	u->unit.active_camouflage = hide_linked ? 1.0f : 0.0f;

	if (hide_linked && u->definition_index != NONE)
	{
		def = object_definition_get(u->definition_index);
		if (def)
		{
			for (a = 0; a < def->object.attachments.count; a++)
			{
				struct object_attachment_definition *att = TAG_BLOCK_GET_ELEMENT(
					&def->object.attachments, a, struct object_attachment_definition);
				if (att && att->type.group_tag == 'ligh' && att->type.index != NONE)
				{
					/* Clear _light_definition_dont_fade_active_camouflage_bit (bit 5) so helmet lights fade out completely */
					uint32_t *lflags = (uint32_t *)tag_get('ligh', att->type.index);
					if (lflags)
						*lflags &= ~(1u << 5);
				}
			}
		}
	}

	for (child_idx = u->object.first_child_object_index; child_idx != NONE; )
	{
		struct object_datum *c_obj = (struct object_datum *)object_try_and_get_and_verify_type(child_idx, (unsigned long)-1);
		if (!c_obj)
			break;
		SET_FLAG(c_obj->object.flags, _object_invisible_bit, hide_linked ? TRUE : FALSE);
		child_idx = c_obj->object.next_object_index;
	}
}

#define FORGE_IN_MATCH_HUD_SCALE 0.75f
#define FORGE_COUNTER_HUD_SCALE (FORGE_IN_MATCH_HUD_SCALE * 0.5f)

static float s_hud_ui_scale = 1.0f;
static float s_hud_ui_origin_x = 0.0f;
static float s_hud_ui_origin_y = 0.0f;

static void set_forge_hud_ui_scale(float scale, float origin_x, float origin_y)
{
	s_hud_ui_scale = (scale > 0.05f) ? scale : 1.0f;
	s_hud_ui_origin_x = origin_x;
	s_hud_ui_origin_y = origin_y;
}

static void draw_quad(rectangle2d const *r, uint32_t argb)
{
	short x0, y0, x1, y1;
	if (!r || r->x1 <= r->x0 || r->y1 <= r->y0 || ((argb >> 24) & 0xFFu) == 0)
		return;
	if (s_hud_ui_scale != 1.0f)
	{
		x0 = (short)(s_hud_ui_origin_x + ((float)r->x0 - s_hud_ui_origin_x) * s_hud_ui_scale + 0.5f);
		y0 = (short)(s_hud_ui_origin_y + ((float)r->y0 - s_hud_ui_origin_y) * s_hud_ui_scale + 0.5f);
		x1 = (short)(s_hud_ui_origin_x + ((float)r->x1 - s_hud_ui_origin_x) * s_hud_ui_scale + 0.5f);
		y1 = (short)(s_hud_ui_origin_y + ((float)r->y1 - s_hud_ui_origin_y) * s_hud_ui_scale + 0.5f);
		if (x1 <= x0) x1 = (short)(x0 + 1);
		if (y1 <= y0) y1 = (short)(y0 + 1);
	}
	else
	{
		x0 = r->x0;
		y0 = r->y0;
		x1 = r->x1;
		y1 = r->y1;
	}
	forge_draw_screen_rect(x0, y0, x1, y1, argb);
}

static void draw_box_border(short x0, short y0, short x1, short y1, short thick, uint32_t argb)
{
	rectangle2d r;
	if (((argb >> 24) & 0xFFu) == 0 || thick <= 0)
		return;
	r.x0 = x0; r.y0 = y0; r.x1 = x1; r.y1 = (short)(y0 + thick); draw_quad(&r, argb);
	r.x0 = x0; r.y0 = (short)(y1 - thick); r.x1 = x1; r.y1 = y1; draw_quad(&r, argb);
	r.x0 = x0; r.y0 = y0; r.x1 = (short)(x0 + thick); r.y1 = y1; draw_quad(&r, argb);
	r.x0 = (short)(x1 - thick); r.y0 = y0; r.x1 = x1; r.y1 = y1; draw_quad(&r, argb);
}

static long get_small_forge_font(long fallback_font)
{
	long f = tag_loaded('font', "ui\\small_ui");
	if (f == NONE)
		f = tag_loaded('font', "ui\\large_ui");
	if (f == NONE)
		f = interface_get_tag_index(_interface_font_system);
	if (f == NONE)
		f = fallback_font;
	return f;
}

static long get_large_forge_font(long fallback_font)
{
	long f = tag_loaded('font', "ui\\large_ui");
	if (f == NONE)
		f = tag_loaded('font', "ui\\small_ui");
	if (f == NONE)
		f = interface_get_tag_index(_interface_font_system);
	if (f == NONE)
		f = fallback_font;
	return f;
}

/* ============================================================================
 * Editable Forge UI Theme & PNG Background System (OpenCE Forge/ui/forge_ui.xml)
 * ============================================================================ */

enum forge_ui_area_id
{
	_forge_ui_area_spawn_menu = 0,
	_forge_ui_area_preview_panel,
	_forge_ui_area_properties_menu,
	_forge_ui_area_top_bar,
	_forge_ui_area_bottom_bar,
	_forge_ui_area_save_modal,
	_forge_ui_area_maps_modal,
	_forge_ui_area_toast_bar,
	_forge_ui_area_markers,
	NUMBER_OF_FORGE_UI_AREAS
};

struct forge_ui_area_style
{
	char font_name[64];
	short font_style; /* -1=plain, 0=bold, 1=italic, 2=condense, 3=underline */
	float font_scale;
	uint32_t title_color;
	uint32_t text_color;
	uint32_t selected_color;
	uint32_t bg_color;
	uint32_t header_color;
	uint32_t highlight_color;
	uint32_t border_color;
	uint32_t image_tint;
	char bg_image[64];
	int use_image;
	struct bitmap_data bitmap;
	uint8_t dummy_pixels[16];
	int bitmap_initialized;
	int bitmap_valid;
};

static struct
{
	int initialized;
	int reload_rev;
	char registered_map[128];
	struct forge_ui_area_style areas[NUMBER_OF_FORGE_UI_AREAS];
} s_ui_theme;

static uint32_t parse_hex_color_attr(char const *str, uint32_t fallback)
{
	unsigned long val = 0;
	char const *p = str;
	size_t len;

	if (!p || !p[0])
		return fallback;
	if (*p == '#')
		p++;
	else if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
		p += 2;
	len = strlen(p);
	if (len != 6 && len != 8)
		return fallback;
	val = strtoul(p, NULL, 16);
	if (len == 6)
		val |= 0xFF000000ul;
	return (uint32_t)val;
}

static short parse_font_style_attr(char const *str)
{
	if (!str || !str[0])
		return NONE;
	if (_stricmp(str, "bold") == 0)
		return 0; /* _text_style_bold */
	if (_stricmp(str, "italic") == 0)
		return 1; /* _text_style_italic */
	if (_stricmp(str, "condense") == 0 || _stricmp(str, "condensed") == 0)
		return 2; /* _text_style_condense */
	if (_stricmp(str, "underline") == 0)
		return 3; /* _text_style_underline */
	return NONE; /* plain */
}

static int extract_xml_attr(char const *tag_start, char const *tag_end, char const *attr_name, char *out_val, size_t out_sz)
{
	size_t nlen = strlen(attr_name);
	char const *p = tag_start;

	while (p && p + nlen + 2 < tag_end)
	{
		if (_strnicmp(p, attr_name, nlen) == 0 &&
			(p == tag_start || p[-1] == ' ' || p[-1] == '\t' || p[-1] == '\r' || p[-1] == '\n'))
		{
			char const *eq = p + nlen;
			while (eq < tag_end && (*eq == ' ' || *eq == '\t'))
				eq++;
			if (eq < tag_end && *eq == '=')
			{
				char quote;
				char const *val_start;
				char const *val_end;
				size_t copy_len;
				eq++;
				while (eq < tag_end && (*eq == ' ' || *eq == '\t'))
					eq++;
				if (eq < tag_end && (*eq == '"' || *eq == '\''))
				{
					quote = *eq++;
					val_start = eq;
					val_end = memchr(val_start, quote, (size_t)(tag_end - val_start));
					if (val_end)
					{
						copy_len = (size_t)(val_end - val_start);
						if (copy_len >= out_sz)
							copy_len = out_sz - 1;
						memcpy(out_val, val_start, copy_len);
						out_val[copy_len] = 0;
						return 1;
					}
				}
			}
		}
		p++;
	}
	return 0;
}

static void init_default_area_style(struct forge_ui_area_style *st, char const *default_png)
{
	snprintf(st->font_name, sizeof(st->font_name), "large");
	st->font_style = NONE;
	st->font_scale = 1.0f;
	st->title_color = 0xFFFFFFFFu;
	st->text_color = 0xFF2896FFu;
	st->selected_color = 0xFFFFFFFFu;
	st->bg_color = 0xC00A2649u;
	st->header_color = 0x00000000u;
	st->highlight_color = 0xD8225C9Cu;
	st->border_color = 0xFF2896FFu;
	st->image_tint = 0xFFFFFFFFu;
	snprintf(st->bg_image, sizeof(st->bg_image), "%s", default_png ? default_png : "none");
	st->use_image = (default_png && default_png[0] && _stricmp(default_png, "none") != 0) ? 1 : 0;
}

static void sync_and_register_area_png(struct forge_ui_area_style *st, int area_idx)
{
	char src_path[256];
	char dst_dir[256];
	char dst_path[256];
	char rel_png[128];
	FILE *in_f, *out_f;
	unsigned char *buf;
	long sz;

	st->bitmap_valid = 0;
	if (!st->use_image || !st->bg_image[0] || _stricmp(st->bg_image, "none") == 0)
		return;

	snprintf(src_path, sizeof(src_path), "OpenCE Forge/ui/%s", st->bg_image);
	in_f = fopen(src_path, "rb");
	if (!in_f)
		return;
	if (fseek(in_f, 0, SEEK_END) != 0 || (sz = ftell(in_f)) <= 0 || fseek(in_f, 0, SEEK_SET) != 0)
	{
		fclose(in_f);
		return;
	}
	buf = (unsigned char *)malloc((size_t)sz);
	if (!buf || fread(buf, 1, (size_t)sz, in_f) != (size_t)sz)
	{
		free(buf);
		fclose(in_f);
		return;
	}
	fclose(in_f);

	{
		extern int __cdecl _mkdir(char const *path);
		_mkdir("OpenCE Forge");
		_mkdir("OpenCE Forge/menus");
		_mkdir("OpenCE Forge/menus/forge_ui");
	}

	snprintf(rel_png, sizeof(rel_png), "forge_ui/r%d_a%d_%s", s_ui_theme.reload_rev, area_idx, st->bg_image);
	snprintf(dst_path, sizeof(dst_path), "OpenCE Forge/menus/%s", rel_png);
	out_f = fopen(dst_path, "wb");
	if (out_f)
	{
		fwrite(buf, 1, (size_t)sz, out_f);
		fclose(out_f);
	}
	free(buf);

	if (!st->bitmap_initialized)
	{
		memset(&st->bitmap, 0, sizeof(st->bitmap));
		memset(st->dummy_pixels, 0xFF, sizeof(st->dummy_pixels));
		st->bitmap.signature = 'bitm';
		st->bitmap.width = 2;
		st->bitmap.height = 2;
		st->bitmap.depth = 1;
		st->bitmap.type = 0;   /* _bitmap_type_2d_texture */
		st->bitmap.format = 11; /* _bitmap_format_a8r8g8b8 */
		st->bitmap.flags = 1;  /* _bitmap_has_power_of_two_dimensions_bit */
		st->bitmap.base_address = st->dummy_pixels;
		if (rasterizer_bitmap_new(&st->bitmap) && st->bitmap.hardware_format)
			st->bitmap_initialized = 1;
	}

	if (st->bitmap_initialized && st->bitmap.hardware_format)
	{
		halo_menus_art_register(st->bitmap.hardware_format, rel_png);
		st->bitmap_valid = 1;
	}
}

static void parse_area_xml_node(char const *xml_text, char const *node_name, struct forge_ui_area_style *st)
{
	char open_pat[80];
	char const *p;
	char const *end;
	char val[128];

	snprintf(open_pat, sizeof(open_pat), "<%s", node_name);
	p = strstr(xml_text, open_pat);
	if (!p)
		return;
	end = strchr(p, '>');
	if (!end)
		return;

	if (extract_xml_attr(p, end, "font", val, sizeof(val)))
		snprintf(st->font_name, sizeof(st->font_name), "%s", val);
	if (extract_xml_attr(p, end, "style", val, sizeof(val)))
		st->font_style = parse_font_style_attr(val);
	if (extract_xml_attr(p, end, "size", val, sizeof(val)) || extract_xml_attr(p, end, "scale", val, sizeof(val)))
	{
		float sc = (float)atof(val);
		if (sc >= 0.25f && sc <= 4.0f)
			st->font_scale = sc;
	}
	if (extract_xml_attr(p, end, "title_color", val, sizeof(val)))
		st->title_color = parse_hex_color_attr(val, st->title_color);
	if (extract_xml_attr(p, end, "text_color", val, sizeof(val)) || extract_xml_attr(p, end, "color", val, sizeof(val)))
		st->text_color = parse_hex_color_attr(val, st->text_color);
	if (extract_xml_attr(p, end, "selected_color", val, sizeof(val)))
		st->selected_color = parse_hex_color_attr(val, st->selected_color);
	if (extract_xml_attr(p, end, "bg_color", val, sizeof(val)))
		st->bg_color = parse_hex_color_attr(val, st->bg_color);
	if (extract_xml_attr(p, end, "header_color", val, sizeof(val)))
		st->header_color = parse_hex_color_attr(val, st->header_color);
	if (extract_xml_attr(p, end, "highlight_color", val, sizeof(val)))
		st->highlight_color = parse_hex_color_attr(val, st->highlight_color);
	if (extract_xml_attr(p, end, "border_color", val, sizeof(val)))
		st->border_color = parse_hex_color_attr(val, st->border_color);
	if (extract_xml_attr(p, end, "image_tint", val, sizeof(val)))
		st->image_tint = parse_hex_color_attr(val, st->image_tint);
	if (extract_xml_attr(p, end, "background", val, sizeof(val)))
		snprintf(st->bg_image, sizeof(st->bg_image), "%s", val);
	if (extract_xml_attr(p, end, "use_image", val, sizeof(val)))
		st->use_image = (_stricmp(val, "true") == 0 || strcmp(val, "1") == 0 || _stricmp(val, "yes") == 0) ? 1 : 0;
}

static void forge_ui_theme_load(int is_hot_reload)
{
	static char const *const area_nodes[NUMBER_OF_FORGE_UI_AREAS] = {
		"spawn_menu",
		"preview_panel",
		"properties_menu",
		"top_bar",
		"bottom_bar",
		"save_modal",
		"maps_modal",
		"toast_bar",
		"markers"
	};
	static char const *const area_default_pngs[NUMBER_OF_FORGE_UI_AREAS] = {
		"panel_spawn_menu.png",
		"panel_preview.png",
		"panel_modal.png",
		"panel_top_bar.png",
		"panel_bottom_bar.png",
		"escape_menu_background.png",
		"panel_modal.png",
		"panel_bottom_bar.png",
		"none"
	};
	FILE *f;
	int i;

	if (is_hot_reload)
		s_ui_theme.reload_rev++;

	for (i = 0; i < NUMBER_OF_FORGE_UI_AREAS; i++)
		init_default_area_style(&s_ui_theme.areas[i], area_default_pngs[i]);

	/* Specific default font/color tuning per area matching base game menu UI styling */
	snprintf(s_ui_theme.areas[_forge_ui_area_top_bar].font_name, sizeof(s_ui_theme.areas[_forge_ui_area_top_bar].font_name), "small");
	snprintf(s_ui_theme.areas[_forge_ui_area_bottom_bar].font_name, sizeof(s_ui_theme.areas[_forge_ui_area_bottom_bar].font_name), "small");
	snprintf(s_ui_theme.areas[_forge_ui_area_toast_bar].font_name, sizeof(s_ui_theme.areas[_forge_ui_area_toast_bar].font_name), "small");
	snprintf(s_ui_theme.areas[_forge_ui_area_markers].font_name, sizeof(s_ui_theme.areas[_forge_ui_area_markers].font_name), "small");
	s_ui_theme.areas[_forge_ui_area_toast_bar].text_color = 0xFFFFFFFFu;
	s_ui_theme.areas[_forge_ui_area_markers].title_color = 0xFFFFFFFFu;
	s_ui_theme.areas[_forge_ui_area_markers].text_color = 0xFF2896FFu;
	s_ui_theme.areas[_forge_ui_area_markers].selected_color = 0xFFFFFFFFu;

	f = fopen("OpenCE Forge/ui/forge_ui.xml", "rb");
	if (f)
	{
		long sz;
		if (fseek(f, 0, SEEK_END) == 0 && (sz = ftell(f)) > 0 && sz < 262144 && fseek(f, 0, SEEK_SET) == 0)
		{
			char *xml_buf = (char *)malloc((size_t)sz + 1);
			if (xml_buf)
			{
				size_t rd = fread(xml_buf, 1, (size_t)sz, f);
				xml_buf[rd] = 0;
				for (i = 0; i < NUMBER_OF_FORGE_UI_AREAS; i++)
					parse_area_xml_node(xml_buf, area_nodes[i], &s_ui_theme.areas[i]);
				free(xml_buf);
			}
		}
		fclose(f);
	}

	for (i = 0; i < NUMBER_OF_FORGE_UI_AREAS; i++)
		sync_and_register_area_png(&s_ui_theme.areas[i], i);

	snprintf(s_ui_theme.registered_map, sizeof(s_ui_theme.registered_map), "%s",
		main_get_map_name() ? main_get_map_name() : "");
	s_ui_theme.initialized = 1;
}

static void forge_ui_theme_ensure_loaded(void)
{
	char const *cur_map = main_get_map_name();
	if (!s_ui_theme.initialized)
	{
		forge_ui_theme_load(0);
	}
	else if (cur_map && strcmp(s_ui_theme.registered_map, cur_map) != 0)
	{
		/* Re-register PNG textures after map transitions because menu_tags_unloaded() clears menu art */
		int i;
		for (i = 0; i < NUMBER_OF_FORGE_UI_AREAS; i++)
			sync_and_register_area_png(&s_ui_theme.areas[i], i);
		snprintf(s_ui_theme.registered_map, sizeof(s_ui_theme.registered_map), "%s", cur_map);
	}
}

static void draw_ui_png_quad_uv(
	struct bitmap_data *bitmap,
	short x0, short y0,
	short x1, short y1,
	float u0, float v0,
	float u1, float v1,
	uint32_t tint_argb)
{
	struct dynamic_screen_vertex vertices[4];
	struct rasterizer_dynamic_screen_geometry_parameters parameters;
	int v;

	if (!bitmap || !bitmap->hardware_format || x1 <= x0 || y1 <= y0)
		return;

	for (v = 0; v < 4; v++)
	{
		real vx = (v % 3) ? (real)x1 : (real)x0;
		real vy = (v > 1) ? (real)y1 : (real)y0;
		if (s_hud_ui_scale != 1.0f)
		{
			vx = s_hud_ui_origin_x + (vx - s_hud_ui_origin_x) * s_hud_ui_scale;
			vy = s_hud_ui_origin_y + (vy - s_hud_ui_origin_y) * s_hud_ui_scale;
		}
		vertices[v].position.x = vx;
		vertices[v].position.y = vy;
		vertices[v].texture_coordinates.x = (v % 3) ? u1 : u0;
		vertices[v].texture_coordinates.y = (v > 1) ? v1 : v0;
		vertices[v].color = tint_argb;
	}

	memset(&parameters, 0, sizeof(parameters));
	parameters.meter_parameters = NULL;
	parameters.map_offset[0] = NULL;
	parameters.map_wrapped[0] = FALSE;
	parameters.map_wrapped[1] = FALSE;
	parameters.map_wrapped[2] = FALSE;
	parameters.map_scale[0].i = 1.0f;
	parameters.map_scale[0].j = 1.0f;
	parameters.map_texture_scale[0].i = 1.0f;
	parameters.map_texture_scale[0].j = 1.0f;
	parameters.point_sampled = FALSE;
	parameters.framebuffer_blend_function = 0;
	parameters.map[0] = bitmap;
	parameters.map[1] = NULL;
	parameters.map[2] = NULL;

	rasterizer_psuedo_dynamic_screen_quad_draw(&parameters, vertices);
}

static void draw_ui_png_quad(struct bitmap_data *bitmap, short x0, short y0, short x1, short y1, uint32_t tint_argb)
{
	draw_ui_png_quad_uv(bitmap, x0, y0, x1, y1, 0.0f, 0.0f, 1.0f, 1.0f, tint_argb);
}

/* ============================================================================
 * Base Game Menu Bitmap & Font Rendering Helpers
 * Draws authentic Halo CE / OpenCE menu tags (pausebox, objectives, option_bkds,
 * list_item_bkd, sel_list_item_bkd, text_button_background, arrow_sm_*, blue)
 * and ui\large_ui / ui\small_ui fonts directly in-game.
 * ============================================================================ */

static int draw_ce_menu_bitmap_uv(
	char const *tag_name,
	short frame_idx,
	short x0, short y0,
	short x1, short y1,
	float u0, float v0,
	float u1, float v1,
	uint32_t tint_argb)
{
	long tag_idx;
	struct bitmap_data *bmp;

	if (!tag_name || !tag_name[0] || x1 <= x0 || y1 <= y0)
		return 0;

	tag_idx = tag_loaded('bitm', tag_name);
	if (tag_idx == NONE)
		return 0;

	bmp = bitmap_group_get_bitmap_from_sequence(tag_idx, 0, frame_idx);
	if (!bmp && frame_idx != 0)
		bmp = bitmap_group_get_bitmap_from_sequence(tag_idx, 0, 0);
	if (!bmp || !bmp->hardware_format || bmp->width <= 0 || bmp->height <= 0)
		return 0;

	draw_ui_png_quad_uv(bmp, x0, y0, x1, y1, u0, v0, u1, v1, tint_argb);
	return 1;
}

static int draw_ce_menu_bitmap_cropped(
	char const *tag_name,
	short frame_idx,
	short x0, short y0,
	short x1, short y1,
	short crop_w, short crop_h,
	uint32_t tint_argb)
{
	long tag_idx;
	struct bitmap_data *bmp;
	float u1 = 1.0f, v1 = 1.0f;

	if (!tag_name || !tag_name[0] || x1 <= x0 || y1 <= y0)
		return 0;

	tag_idx = tag_loaded('bitm', tag_name);
	if (tag_idx == NONE)
		return 0;

	bmp = bitmap_group_get_bitmap_from_sequence(tag_idx, 0, frame_idx);
	if (!bmp && frame_idx != 0)
		bmp = bitmap_group_get_bitmap_from_sequence(tag_idx, 0, 0);
	if (!bmp || !bmp->hardware_format || bmp->width <= 0 || bmp->height <= 0)
		return 0;

	if (crop_w > 0 && crop_w < bmp->width)
		u1 = (float)crop_w / (float)bmp->width;
	if (crop_h > 0 && crop_h < bmp->height)
		v1 = (float)crop_h / (float)bmp->height;

	draw_ui_png_quad_uv(bmp, x0, y0, x1, y1, 0.0f, 0.0f, u1, v1, tint_argb);
	return 1;
}

/* Draws a 3-slice horizontally rounded Halo CE option/row highlight bar
 * using pc\bitmaps\option_bkds_sm so both left and right ends have crisp rounded caps. */
static int draw_ce_rounded_row_bar(
	short frame_idx,
	short x0, short y0,
	short x1, short y1)
{
	short w = (short)(x1 - x0);
	short cap = 16;
	float v1 = 28.0f / 32.0f;
	float u_cap = 16.0f / 256.0f;

	if (w <= 0 || y1 <= y0)
		return 0;
	if (w <= cap * 2)
		return draw_ce_menu_bitmap_cropped("pc\\bitmaps\\option_bkds_sm", frame_idx, x0, y0, x1, y1, 256, 28, 0xFFFFFFFFu);

	if (draw_ce_menu_bitmap_uv("pc\\bitmaps\\option_bkds_sm", frame_idx, x0, y0, (short)(x0 + cap), y1, 0.0f, 0.0f, u_cap, v1, 0xFFFFFFFFu) &&
		draw_ce_menu_bitmap_uv("pc\\bitmaps\\option_bkds_sm", frame_idx, (short)(x0 + cap), y0, (short)(x1 - cap), y1, u_cap, 0.0f, 1.0f - u_cap, v1, 0xFFFFFFFFu) &&
		draw_ce_menu_bitmap_uv("pc\\bitmaps\\option_bkds_sm", frame_idx, (short)(x1 - cap), y0, x1, y1, 1.0f - u_cap, 0.0f, 1.0f, v1, 0xFFFFFFFFu))
	{
		return 1;
	}
	return 0;
}

/* Draws the authentic 9-slice Halo CE pause/objectives container box with 1:1 rounded corners */
static void draw_ce_pause_box(short x0, short y0, short width, short height, int with_header)
{
	short x1 = (short)(x0 + width);
	short y1 = (short)(y0 + height);
	char const *top_left   = with_header ? "pc\\pause\\objectives_left"   : "pc\\pause\\pausebox_left";
	char const *top_center = with_header ? "pc\\pause\\objectives_center" : "pc\\pause\\pausebox_center";
	char const *top_right  = with_header ? "pc\\pause\\objectives_right"  : "pc\\pause\\pausebox_right";
	char const *obj_left   = "pc\\pause\\objectives_left";
	char const *obj_center = "pc\\pause\\objectives_center";
	char const *obj_right  = "pc\\pause\\objectives_right";
	short frame_idx = 1;
	short top_h = with_header ? 30 : 14;
	short bot_h = 14;
	float v_top_end = (float)top_h / 256.0f;
	float v_body_start = 32.0f / 256.0f;
	float v_body_end = 211.0f / 256.0f;
	float v_bot_end = 225.0f / 256.0f;

	if (width <= 32 || height <= 28)
		return;

	/* Top slice (rounded top corners; includes 29px dark header bar when with_header != 0,
	 * or clean blue rounded top cap from pausebox when with_header == 0) */
	if (draw_ce_menu_bitmap_uv(top_left, frame_idx, x0, y0, (short)(x0 + 16), (short)(y0 + top_h), 0.0f, 0.0f, 1.0f, v_top_end, 0xFFFFFFFFu) &&
		draw_ce_menu_bitmap_uv(top_center, frame_idx, (short)(x0 + 16), y0, (short)(x1 - 16), (short)(y0 + top_h), 0.0f, 0.0f, 1.0f, v_top_end, 0xFFFFFFFFu) &&
		draw_ce_menu_bitmap_uv(top_right, frame_idx, (short)(x1 - 16), y0, x1, (short)(y0 + top_h), 0.0f, 0.0f, 1.0f, v_top_end, 0xFFFFFFFFu) &&
		/* Middle body slice (clean translucent blue gradient from objectives_left/center/right) */
		draw_ce_menu_bitmap_uv(obj_left, frame_idx, x0, (short)(y0 + top_h), (short)(x0 + 16), (short)(y1 - bot_h), 0.0f, v_body_start, 1.0f, v_body_end, 0xFFFFFFFFu) &&
		draw_ce_menu_bitmap_uv(obj_center, frame_idx, (short)(x0 + 16), (short)(y0 + top_h), (short)(x1 - 16), (short)(y1 - bot_h), 0.0f, v_body_start, 1.0f, v_body_end, 0xFFFFFFFFu) &&
		draw_ce_menu_bitmap_uv(obj_right, frame_idx, (short)(x1 - 16), (short)(y0 + top_h), x1, (short)(y1 - bot_h), 0.0f, v_body_start, 1.0f, v_body_end, 0xFFFFFFFFu) &&
		/* Bottom slice (1:1 unstretched rounded bottom corners from objectives_left/center/right, no footer line) */
		draw_ce_menu_bitmap_uv(obj_left, frame_idx, x0, (short)(y1 - bot_h), (short)(x0 + 16), y1, 0.0f, v_body_end, 1.0f, v_bot_end, 0xFFFFFFFFu) &&
		draw_ce_menu_bitmap_uv(obj_center, frame_idx, (short)(x0 + 16), (short)(y1 - bot_h), (short)(x1 - 16), y1, 0.0f, v_body_end, 1.0f, v_bot_end, 0xFFFFFFFFu) &&
		draw_ce_menu_bitmap_uv(obj_right, frame_idx, (short)(x1 - 16), (short)(y1 - bot_h), x1, y1, 0.0f, v_body_end, 1.0f, v_bot_end, 0xFFFFFFFFu))
	{
		return;
	}

	/* Fallback if menu tags are not present */
	{
		rectangle2d body, hdr;
		body.x0 = x0; body.y0 = y0; body.x1 = x1; body.y1 = y1;
		draw_quad(&body, 0xC00A2649u);
		if (with_header)
		{
			hdr.x0 = (short)(x0 + 2); hdr.y0 = (short)(y0 + 2); hdr.x1 = (short)(x1 - 2); hdr.y1 = (short)(y0 + 28);
			draw_quad(&hdr, 0xE0000000u);
			hdr.y0 = (short)(y0 + 28); hdr.y1 = (short)(y0 + 29);
			draw_quad(&hdr, 0xFF2896FFu);
		}
		draw_box_border(x0, y0, x1, y1, 2, 0xFF2896FFu);
	}
}

static void draw_ce_blue_line(short x0, short y0, short x1, short height)
{
	if (height <= 0)
		height = 1;
	if (!draw_ce_menu_bitmap_cropped("pc\\bitmaps\\blue", 0, x0, y0, x1, (short)(y0 + height), 4, 4, 0xFFFFFFFFu))
	{
		rectangle2d r;
		r.x0 = x0; r.y0 = y0; r.x1 = x1; r.y1 = (short)(y0 + height);
		draw_quad(&r, 0xFF2896FFu);
	}
}

static void draw_ce_menu_str_box(
	int use_large_font,
	long fallback_font,
	short x0, short y0,
	short x1, short y1,
	short justification,
	char const *ascii,
	uint32_t argb_col)
{
	wchar_t wbuf[256];
	rectangle2d bounds;
	real_argb_color col;
	long font_idx;
	int i;

	if (!ascii || !ascii[0] || x1 <= x0 || y1 <= y0)
		return;
	for (i = 0; ascii[i] && i < 254; i++)
	{
		unsigned char ch = (unsigned char)ascii[i];
		if (ch == '|')
			ch = '/';
		wbuf[i] = (wchar_t)ch;
	}
	wbuf[i] = 0;

	col.alpha = (real)((argb_col >> 24) & 0xFFu) / 255.0f;
	col.red   = (real)((argb_col >> 16) & 0xFFu) / 255.0f;
	col.green = (real)((argb_col >> 8)  & 0xFFu) / 255.0f;
	col.blue  = (real)(argb_col         & 0xFFu) / 255.0f;

	font_idx = use_large_font ? get_large_forge_font(fallback_font) : get_small_forge_font(fallback_font);

	bounds.x0 = x0;
	bounds.y0 = y0;
	bounds.x1 = x1;
	bounds.y1 = y1;

	if (s_hud_ui_scale != 1.0f)
		rasterizer_text_set_scale(s_hud_ui_scale, s_hud_ui_origin_x, s_hud_ui_origin_y);
	draw_string_set_draw_mode(font_idx, NONE, justification, 0, &col);
	rasterizer_draw_unicode_string(&bounds, NULL, NULL, 0, wbuf);
	if (s_hud_ui_scale != 1.0f)
		rasterizer_text_set_scale(1.0f, 0.0f, 0.0f);
}

static void draw_ce_menu_str(
	int use_large_font,
	long fallback_font,
	short x,
	short y,
	char const *ascii,
	uint32_t argb_col)
{
	draw_ce_menu_str_box(use_large_font, fallback_font, x, y, (short)(x + 2048), (short)(y + 128), 0, ascii, argb_col);
}

static void draw_ce_text_button(
	long fallback_font,
	short x0, short y0,
	short width, short height,
	int focused,
	char const *label)
{
	short x1 = (short)(x0 + width);
	short y1 = (short)(y0 + height);
	short text_y = (short)(y0 + (height - 20) / 2);
	uint32_t text_col = focused ? 0xFFFFFFFFu : 0xFF2896FFu;

	if (!draw_ce_menu_bitmap_cropped("pc\\bitmaps\\text_button_background", (short)(focused ? 1 : 0), x0, y0, x1, y1, 128, 21, 0xFFFFFFFFu))
	{
		rectangle2d r;
		r.x0 = x0; r.y0 = y0; r.x1 = x1; r.y1 = y1;
		draw_quad(&r, focused ? 0xD8143C70u : 0xB007162Cu);
		draw_box_border(x0, y0, x1, y1, 1, 0xFF2896FFu);
	}
	draw_ce_menu_str_box(0, fallback_font, (short)(x0 + 4), text_y, (short)(x1 - 4), (short)(y1 + 4), 2 /* center */, label, text_col);
}

static void draw_themed_panel(enum forge_ui_area_id area_id, rectangle2d const *r, short header_height)
{
	struct forge_ui_area_style *st = &s_ui_theme.areas[area_id];
	if (!r || r->x1 <= r->x0 || r->y1 <= r->y0)
		return;

	if (st->use_image && st->bitmap_valid && st->bitmap.hardware_format)
	{
		draw_ui_png_quad(&st->bitmap, r->x0, r->y0, r->x1, r->y1, st->image_tint);
	}
	else
	{
		draw_quad(r, st->bg_color);
	}

	if (header_height > 0 && ((st->header_color >> 24) & 0xFFu) != 0)
	{
		rectangle2d hdr;
		hdr.x0 = r->x0;
		hdr.y0 = r->y0;
		hdr.x1 = r->x1;
		hdr.y1 = (short)(r->y0 + header_height);
		draw_quad(&hdr, st->header_color);
	}

	if (((st->border_color >> 24) & 0xFFu) != 0)
	{
		draw_box_border(r->x0, r->y0, r->x1, r->y1, 1, st->border_color);
	}
}

static long resolve_theme_font(char const *font_name, long fallback_font)
{
	long f = NONE;
	if (font_name && font_name[0])
	{
		if (_stricmp(font_name, "terminal") == 0)
			f = interface_get_tag_index(_interface_font_terminal);
		else if (_stricmp(font_name, "system") == 0)
			f = interface_get_tag_index(_interface_font_system);
		else if (_stricmp(font_name, "small") == 0 || _stricmp(font_name, "small_ui") == 0)
			f = tag_loaded('font', "ui\\small_ui");
		else if (_stricmp(font_name, "large") == 0 || _stricmp(font_name, "large_ui") == 0)
			f = tag_loaded('font', "ui\\large_ui");
		else if (_stricmp(font_name, "ticker") == 0)
			f = tag_loaded('font', "ui\\ticker");
		else
			f = tag_loaded('font', font_name);
	}
	if (f == NONE)
		f = get_large_forge_font(fallback_font);
	return f;
}

static void draw_themed_str(
	enum forge_ui_area_id area_id,
	long fallback_font,
	short x,
	short y,
	char const *ascii,
	uint32_t argb_col)
{
	struct forge_ui_area_style const *st = &s_ui_theme.areas[area_id];
	wchar_t wbuf[256];
	rectangle2d bounds;
	real_argb_color col;
	long font_idx;
	short fstyle = NONE;
	float scale = 1.0f;
	int i;

	if (!ascii || !ascii[0])
		return;
	for (i = 0; ascii[i] && i < 254; i++)
	{
		unsigned char ch = (unsigned char)ascii[i];
		if (ch == '|')
			ch = '/';
		wbuf[i] = (wchar_t)ch;
	}
	wbuf[i] = 0;

	col.alpha = (real)((argb_col >> 24) & 0xFFu) / 255.0f;
	col.red   = (real)((argb_col >> 16) & 0xFFu) / 255.0f;
	col.green = (real)((argb_col >> 8)  & 0xFFu) / 255.0f;
	col.blue  = (real)(argb_col         & 0xFFu) / 255.0f;

	font_idx = resolve_theme_font(st->font_name, fallback_font);
	fstyle = st->font_style;
	scale = st->font_scale;
	if (scale < 0.25f) scale = 0.25f;
	if (scale > 4.0f) scale = 4.0f;

	bounds.x0 = x;
	bounds.y0 = y;
	bounds.x1 = (short)(x + 2048);
	bounds.y1 = (short)(y + 128);

	if (scale != 1.0f)
		rasterizer_text_set_scale(scale, (real)x, (real)y);
	draw_string_set_draw_mode(font_idx, fstyle, 0, 0, &col);
	rasterizer_draw_unicode_string(&bounds, NULL, NULL, 0, wbuf);
	if (scale != 1.0f)
		rasterizer_text_set_scale(1.0f, 0.0f, 0.0f);
}

static float snap_angle_rad(float angle_rad, enum forge_snap_angle_mode mode)
{
	float step_deg = 0.0f;
	float step_rad;
	if (mode == _forge_snap_15_deg) step_deg = 15.0f;
	else if (mode == _forge_snap_45_deg) step_deg = 45.0f;
	else if (mode == _forge_snap_90_deg) step_deg = 90.0f;
	else return angle_rad;

	step_rad = step_deg * (3.1415926535f / 180.0f);
	return (float)floor((double)(angle_rad / step_rad) + 0.5) * step_rad;
}

static struct object_datum *forge_valid_object_get(long object_index)
{
	if (object_index == NONE)
		return NULL;
	return (struct object_datum *)object_try_and_get_and_verify_type(object_index, (unsigned long)-1);
}

static void euler_to_forward_up(float yaw, float pitch, float roll, real_vector3d *forward, real_vector3d *up)
{
	float cy = (float)cos((double)yaw);
	float sy = (float)sin((double)yaw);
	float cp = (float)cos((double)pitch);
	float sp = (float)sin((double)pitch);
	float cr = (float)cos((double)roll);
	float sr = (float)sin((double)roll);

	forward->i = cy * cp;
	forward->j = sy * cp;
	forward->k = sp;

	up->i = -cy * sp * cr + sy * sr;
	up->j = -sy * sp * cr - cy * sr;
	up->k = cp * cr;
}

static void forward_up_to_euler(
	real_vector3d const *forward,
	real_vector3d const *up,
	float *out_yaw,
	float *out_pitch,
	float *out_roll)
{
	float fk = forward->k;
	float yaw, pitch, roll, cy, sy, cp, sp;

	if (fk > 1.0f) fk = 1.0f;
	if (fk < -1.0f) fk = -1.0f;
	pitch = (float)asin((double)fk);
	yaw = (float)atan2((double)forward->j, (double)forward->i);
	cy = (float)cos((double)yaw);
	sy = (float)sin((double)yaw);
	cp = (float)cos((double)pitch);
	sp = (float)sin((double)pitch);
	roll = (float)atan2(
		(double)(sy * up->i - cy * up->j),
		(double)(-cy * sp * up->i - sy * sp * up->j + cp * up->k));

	*out_yaw = yaw;
	*out_pitch = pitch;
	*out_roll = roll;
}

static int32_t adopt_default_scenario_object(long object_index)
{
	struct object_datum *obj;
	struct unit_datum *unit;
	long def_idx;
	uint32_t grp;
	char const *tname;
	int32_t i, live_idx;
	struct forge_live_instance *inst;

	obj = forge_valid_object_get(object_index);
	if (!obj)
		return -1;

	/* Never adopt a player's controlled biped */
	unit = (struct unit_datum *)object_try_and_get_and_verify_type(object_index, _object_mask_biped);
	if (unit && unit->unit.player_index != NONE)
		return -1;

	/* Check if already in live_instances */
	for (i = 0; i < g_forge.live_instance_count; i++)
	{
		if (g_forge.live_instances[i].active &&
			g_forge.live_instances[i].object_index == object_index)
		{
			return i;
		}
	}

	if (g_forge.live_instance_count >= FORGE_MAX_LIVE_INSTANCES)
		return -1;

	def_idx = obj->definition_index;
	grp = (def_idx != NONE) ? (uint32_t)tag_get_group_tag(def_idx) : 'scen';
	tname = (def_idx != NONE) ? tag_get_name(def_idx) : "scenario_object";
	if (!tname || !tname[0])
		tname = "scenario_object";

	if (obj->object.parent_object_index != NONE)
		object_detach(object_index);
	SET_FLAG(obj->object.flags, _object_cannot_be_garbage_bit, TRUE);
	SET_FLAG(obj->object.flags, _object_deleted_when_deactivated_bit, FALSE);
	object_set_automatic_deactivation(object_index, FALSE);

	live_idx = g_forge.live_instance_count++;
	inst = &g_forge.live_instances[live_idx];
	memset(inst, 0, sizeof(*inst));
	inst->active = 1;
	inst->object_index = object_index;
	inst->actor_index = NONE;
	inst->edit_entry_index = -1;
	inst->group_tag = grp;
	inst->source_cache_id = 0;
	inst->is_default_scenario_object = 1;
	inst->orig_x = obj->object.position.x;
	inst->orig_y = obj->object.position.y;
	inst->orig_z = obj->object.position.z;
	inst->x = obj->object.position.x;
	inst->y = obj->object.position.y;
	inst->z = obj->object.position.z;
	forward_up_to_euler(&obj->object.forward, &obj->object.up, &inst->yaw, &inst->pitch, &inst->roll);
	snprintf(inst->tag_path, sizeof(inst->tag_path), "%s", tname);

	/* Check if an existing default_edit already tracks this default scenario object (e.g. from a loaded saved map) */
	for (i = 0; i < (int32_t)g_forge.edit_state.default_edit_count; i++)
	{
		struct forge_default_object_edit *de = &g_forge.edit_state.default_edits[i];
		float dx, dy, dz;
		if (!de->active || de->deleted || de->group_tag != grp)
			continue;
		dx = de->new_x - inst->x;
		dy = de->new_y - inst->y;
		dz = de->new_z - inst->z;
		if (dx * dx + dy * dy < 9.0f && (float)fabs((double)dz) < 5.0f &&
			_stricmp(de->tag_path, tname) == 0)
		{
			inst->edit_entry_index = i;
			inst->orig_x = de->orig_x;
			inst->orig_y = de->orig_y;
			inst->orig_z = de->orig_z;
			return live_idx;
		}
	}

	if (g_forge.edit_state.default_edit_count < FORGE_MAX_DEFAULT_OBJECT_EDITS)
	{
		struct forge_default_object_edit *de =
			&g_forge.edit_state.default_edits[g_forge.edit_state.default_edit_count];
		inst->edit_entry_index = g_forge.edit_state.default_edit_count++;
		memset(de, 0, sizeof(*de));
		de->active = 1;
		de->deleted = 0;
		de->group_tag = grp;
		snprintf(de->tag_path, sizeof(de->tag_path), "%s", tname);
		de->orig_x = inst->orig_x;
		de->orig_y = inst->orig_y;
		de->orig_z = inst->orig_z;
		de->new_x = inst->x;
		de->new_y = inst->y;
		de->new_z = inst->z;
		de->new_yaw = inst->yaw;
		de->new_pitch = inst->pitch;
		de->new_roll = inst->roll;
		de->gametype_flags = inst->gametype_flags;
	}

	return live_idx;
}

/* Syncs all active 'spwn' live_instances into the in-memory scenario->players
 * AND into g_forge.edit_state.player_spawns so Red/Blue/FFA player spawns work live and save cleanly. */
static void sync_live_player_spawns_to_scenario(void)
{
	struct scenario *scen = global_scenario_get();
	int i, out_count = 0;

	g_forge.edit_state.replace_player_spawns = 1;
	for (i = 0; i < g_forge.live_instance_count && out_count < FORGE_MAX_PLAYER_SPAWNS; i++)
	{
		struct forge_live_instance *inst = &g_forge.live_instances[i];
		struct forge_player_spawn_entry *sp;
		int16_t mode_type = 12; /* _game_engine_all */

		if (!inst->active || inst->group_tag != 'spwn')
			continue;

		if (inst->gametype_flags == 0x0001u) mode_type = 1;      /* CTF Only */
		else if (inst->gametype_flags == 0x0002u) mode_type = 2; /* Slayer Only */
		else if (inst->gametype_flags == 0x0004u) mode_type = 3; /* Oddball Only */
		else if (inst->gametype_flags == 0x0008u) mode_type = 4; /* KOTH Only */
		else if (inst->gametype_flags == 0x0010u) mode_type = 5; /* Race Only */

		sp = &g_forge.edit_state.player_spawns[out_count];
		memset(sp, 0, sizeof(*sp));
		sp->x = inst->x;
		sp->y = inst->y;
		sp->z = inst->z;
		sp->yaw = inst->yaw;
		sp->bsp_index = (int16_t)inst->pad_inst;

		if (inst->team_designator == 2 || inst->team_designator < 0 || inst->team_designator > 1)
		{
			/* FFA Spawn (Yellow) */
			inst->team_designator = 2;
			sp->team_index = 0;
			sp->type0 = 13; /* _game_engine_all_non_team (FFA Slayer/Oddball/KOTH/Race) */
			sp->type1 = mode_type;
			snprintf(inst->tag_path, sizeof(inst->tag_path), "spawn\\player_ffa");
		}
		else if (inst->team_designator == 1)
		{
			/* Blue Team Spawn (Blue) */
			sp->team_index = 1;
			sp->type0 = (mode_type != 12) ? mode_type :
				((inst->netgame_flag_type > 0 && inst->netgame_flag_type != 13) ? inst->netgame_flag_type : 12);
			sp->type1 = 12;
			snprintf(inst->tag_path, sizeof(inst->tag_path), "spawn\\player_blue");
		}
		else
		{
			/* Red Team Spawn (Red) */
			inst->team_designator = 0;
			sp->team_index = 0;
			sp->type0 = (mode_type != 12) ? mode_type :
				((inst->netgame_flag_type > 0 && inst->netgame_flag_type != 13) ? inst->netgame_flag_type : 12);
			sp->type1 = 12;
			snprintf(inst->tag_path, sizeof(inst->tag_path), "spawn\\player_red");
		}

		inst->edit_entry_index = out_count;
		out_count++;
	}
	g_forge.edit_state.player_spawn_count = out_count;

	if (scen && out_count <= FORGE_MAX_PLAYER_SPAWNS)
	{
		static uint8_t *s_live_sp_buf = NULL;
		if (!s_live_sp_buf)
		{
			s_live_sp_buf = (uint8_t *)calloc(FORGE_MAX_PLAYER_SPAWNS, 52u);
		}
		if (s_live_sp_buf)
		{
			memset(s_live_sp_buf, 0, (size_t)out_count * 52u);
			for (i = 0; i < out_count; i++)
			{
				struct forge_player_spawn_entry const *sp = &g_forge.edit_state.player_spawns[i];
				uint8_t *dst = s_live_sp_buf + (size_t)i * 52u;
				*(float *)(dst + 0x00) = sp->x;
				*(float *)(dst + 0x04) = sp->y;
				*(float *)(dst + 0x08) = sp->z;
				*(float *)(dst + 0x0C) = sp->yaw;
				*(int16_t *)(dst + 0x10) = sp->team_index;
				*(int16_t *)(dst + 0x12) = sp->bsp_index;
				*(int16_t *)(dst + 0x14) = sp->type0;
				*(int16_t *)(dst + 0x16) = sp->type1;
				*(int16_t *)(dst + 0x18) = sp->type2;
				*(int16_t *)(dst + 0x1A) = sp->type3;
			}
			scen->players.address = s_live_sp_buf;
			scen->players.count = out_count;
		}
	}
}

/* Syncs all active 'tele' and 'flag' live_instances into the in-memory scenario->netgame_flags
 * AND into g_forge.edit_state.netgame_flags so teleporters and KOTH/Oddball/Race/CTF markers work live and save cleanly. */
static void sync_live_netgame_flags_to_scenario(void)
{
	struct scenario *scen = global_scenario_get();
	static uint8_t *s_live_nf_buf = NULL;
	static uint8_t *s_orig_nf_addr = NULL;
	static int s_orig_nf_count = 0;
	int i, out_count = 0;

	g_forge.edit_state.replace_netgame_flags = 1;
	for (i = 0; i < g_forge.live_instance_count && out_count < FORGE_MAX_NETGAME_FLAGS; i++)
	{
		struct forge_live_instance *inst = &g_forge.live_instances[i];
		struct forge_netgame_flag_entry *nf;
		if (!inst->active || (inst->group_tag != 'tele' && inst->group_tag != 'flag'))
			continue;

		if (inst->group_tag == 'flag' && inst->netgame_flag_type == _netgame_flag_ctf_flag)
		{
			inst->team_designator = (inst->team_designator == 1) ? 1 : 0;
			snprintf(inst->tag_path, sizeof(inst->tag_path), "%s",
				inst->team_designator == 1 ? "objective\\ctf_flag_blue" : "objective\\ctf_flag_red");
		}

		nf = &g_forge.edit_state.netgame_flags[out_count];
		memset(nf, 0, sizeof(*nf));
		nf->x = inst->x;
		nf->y = inst->y;
		nf->z = inst->z;
		nf->yaw = inst->yaw;
		nf->type = inst->netgame_flag_type;
		nf->usage_id = inst->team_designator;
		inst->edit_entry_index = out_count;
		out_count++;
	}
	g_forge.edit_state.netgame_flag_count = out_count;

	if (scen && out_count <= FORGE_MAX_NETGAME_FLAGS)
	{
		/* Reallocate scenario->netgame_flags in memory if needed so newly added teleporters/hills work live */
		if (!s_live_nf_buf)
		{
			s_live_nf_buf = (uint8_t *)calloc(FORGE_MAX_NETGAME_FLAGS, 148u);
		}
		if (scen->netgame_flags.address && scen->netgame_flags.address != s_live_nf_buf)
		{
			s_orig_nf_addr = (uint8_t *)scen->netgame_flags.address;
			s_orig_nf_count = scen->netgame_flags.count;
		}
		if (s_live_nf_buf)
		{
			memset(s_live_nf_buf, 0, (size_t)out_count * 148u);
			for (i = 0; i < out_count; i++)
			{
				uint8_t *dst = s_live_nf_buf + (size_t)i * 148u;
				*(float *)(dst + 0x00) = g_forge.edit_state.netgame_flags[i].x;
				*(float *)(dst + 0x04) = g_forge.edit_state.netgame_flags[i].y;
				*(float *)(dst + 0x08) = g_forge.edit_state.netgame_flags[i].z;
				*(float *)(dst + 0x0C) = g_forge.edit_state.netgame_flags[i].yaw;
				*(int16_t *)(dst + 0x10) = g_forge.edit_state.netgame_flags[i].type;
				*(int16_t *)(dst + 0x12) = g_forge.edit_state.netgame_flags[i].usage_id;
				*(int32_t *)(dst + 0x1C) = NONE; /* weapon_group.index = NONE */
			}
			/* Also update CTF flag base positions in s_orig_nf_addr if ctf_globals cached a pointer to it */
			if (s_orig_nf_addr && s_orig_nf_count > 0)
			{
				int j;
				for (j = 0; j < s_orig_nf_count; j++)
				{
					uint8_t *orig_f = s_orig_nf_addr + (size_t)j * 148u;
					int16_t otype = *(int16_t const *)(orig_f + 0x10);
					int16_t oteam = *(int16_t const *)(orig_f + 0x12);
					if (otype == _netgame_flag_ctf_flag)
					{
						for (i = 0; i < out_count; i++)
						{
							if (g_forge.edit_state.netgame_flags[i].type == _netgame_flag_ctf_flag &&
								g_forge.edit_state.netgame_flags[i].usage_id == oteam)
							{
								*(float *)(orig_f + 0x00) = g_forge.edit_state.netgame_flags[i].x;
								*(float *)(orig_f + 0x04) = g_forge.edit_state.netgame_flags[i].y;
								*(float *)(orig_f + 0x08) = g_forge.edit_state.netgame_flags[i].z;
								*(float *)(orig_f + 0x0C) = g_forge.edit_state.netgame_flags[i].yaw;
								break;
							}
						}
					}
				}
			}
			scen->netgame_flags.address = s_live_nf_buf;
			scen->netgame_flags.count = out_count;
		}
		if (player_data)
		{
			long p_idx;
			for (p_idx = data_next_index(player_data, NONE); p_idx != NONE; p_idx = data_next_index(player_data, p_idx))
			{
				struct player_datum *pl = player_get(p_idx);
				if (pl && (pl->teleporter_index < 0 || pl->teleporter_index >= out_count))
					pl->teleporter_index = NONE;
			}
		}
	}
}

static void draw_ui_str(
	long font_index,
	short x,
	short y,
	short w,
	short h,
	char const *ascii,
	real a, real r, real g, real b)
{
	wchar_t wbuf[256];
	rectangle2d bounds;
	real_argb_color col;
	int i;

	(void)w;
	(void)h;
	if (!ascii || !ascii[0])
		return;
	for (i = 0; ascii[i] && i < 254; i++)
	{
		unsigned char ch = (unsigned char)ascii[i];
		/* Replace '|' because Halo's parse_unicode_string treats '|' as a format escape sequence! */
		if (ch == '|')
			ch = '/';
		wbuf[i] = (wchar_t)ch;
	}
	wbuf[i] = 0;

	col.alpha = a;
	col.red = r;
	col.green = g;
	col.blue = b;

	/* Give generous bottom and right bounds so Custom Edition fonts with ascending_height >= 13
	 * never fail draw_unicode_string's (cursor.y < line_bounds.y1) check and text never clips! */
	bounds.x0 = x;
	bounds.y0 = y;
	bounds.x1 = (short)(x + 2048);
	bounds.y1 = (short)(y + 64);

	draw_string_set_draw_mode(get_small_forge_font(font_index), NONE, 0, 0, &col);
	rasterizer_draw_unicode_string(&bounds, NULL, NULL, 0, wbuf);
}

int32_t forge_3d_spawn_object_at(
	uint32_t group_tag,
	char const *tag_path,
	uint8_t source_cache_id,
	float x, float y, float z,
	float yaw, float pitch, float roll,
	int16_t team_designator,
	int broadcast_net)
{
	int32_t live_idx = -1;
	struct forge_live_instance *inst;

	if (!tag_path || !tag_path[0])
		return -1;

	if (g_forge.live_instance_count >= FORGE_MAX_LIVE_INSTANCES)
	{
		forge_set_status("Forge Object Budget Reached (%d/%d)!", g_forge.live_instance_count, FORGE_MAX_LIVE_INSTANCES);
		return -1;
	}

	live_idx = g_forge.live_instance_count++;
	inst = &g_forge.live_instances[live_idx];
	memset(inst, 0, sizeof(*inst));
	inst->active = 1;
	inst->object_index = NONE;
	inst->actor_index = NONE;
	inst->edit_entry_index = -1;
	inst->group_tag = group_tag;
	inst->source_cache_id = source_cache_id;
	inst->gametype_flags = 0; /* 0 = All Modes */
	inst->netgame_flag_type = -1;
	inst->team_designator = team_designator;
	inst->x = x;
	inst->y = y;
	inst->z = z;
	inst->yaw = yaw;
	inst->pitch = pitch;
	inst->roll = roll;
	snprintf(inst->tag_path, sizeof(inst->tag_path), "%s", tag_path);

	if (group_tag == 'spwn')
	{
		inst->team_designator = strstr(tag_path, "blue") ? 1 : strstr(tag_path, "ffa") ? 2 : 0;
		inst->netgame_flag_type = (inst->team_designator == 2) ? 13 : 12;
		sync_live_player_spawns_to_scenario();
		forge_set_status("Placed %s Player Spawn at (%.1f, %.1f, %.1f) [R Rotate Arrow / X Change Team]",
			inst->team_designator == 1 ? "Blue Team" : (inst->team_designator == 2 ? "FFA (Yellow)" : "Red Team"),
			x, y, z);
	}
	else if (group_tag == 'tele')
	{
		/* _netgame_flag_teleport_from = 6, _netgame_flag_teleport_to = 7 */
		inst->netgame_flag_type = strstr(tag_path, "destination") ? 7 : 6;
		inst->team_designator = (team_designator >= 0 && team_designator < 16) ? team_designator : 0;
		sync_live_netgame_flags_to_scenario();
		forge_set_status("Placed Teleporter %s [Ref #%d] (Press X while holding to change Reference ID)",
			inst->netgame_flag_type == 6 ? "IN (Origin)" : "OUT (Destination)",
			(int)inst->team_designator);
	}
	else if (group_tag == 'flag')
	{
		/* _netgame_flag_ctf_flag = 0, _netgame_flag_oddball_ball_spawn = 2, _netgame_flag_race_track = 3, _netgame_flag_hill_flag = 8 */
		inst->netgame_flag_type =
			strstr(tag_path, "oddball") ? 2 :
			strstr(tag_path, "koth") ? 8 :
			strstr(tag_path, "race") ? 3 : 0;
		if (inst->netgame_flag_type == 0)
		{
			inst->team_designator = strstr(tag_path, "blue") ? 1 : 0;
		}
		else if (inst->netgame_flag_type == 2 && team_designator == 0)
		{
			/* Auto-assign next unused Oddball spawn ID (0..15) so multiple ball spawns each have a valid ID */
			int cand, j, used;
			int16_t chosen_id = 0;
			for (cand = 0; cand < 16; cand++)
			{
				used = 0;
				for (j = 0; j < live_idx; j++)
				{
					if (g_forge.live_instances[j].active &&
						g_forge.live_instances[j].group_tag == 'flag' &&
						g_forge.live_instances[j].netgame_flag_type == 2 &&
						g_forge.live_instances[j].team_designator == cand)
					{
						used = 1;
						break;
					}
				}
				if (!used)
				{
					chosen_id = (int16_t)cand;
					break;
				}
			}
			inst->team_designator = chosen_id;
		}
		else
		{
			inst->team_designator = (team_designator >= 0) ? team_designator : 0;
		}
		sync_live_netgame_flags_to_scenario();
		forge_set_status("Placed Objective '%s' [Ref/Team #%d] at (%.1f, %.1f, %.1f)",
			tag_path, (int)inst->team_designator, x, y, z);
	}
	else
	{
		uint32_t actual_grp = group_tag;
		long def_idx = forge_resolve_engine_tag(group_tag, tag_path, &actual_grp);
		if (def_idx != NONE)
		{
			struct object_placement_data placement;
			object_placement_data_new(&placement, def_idx, NONE);
			SET_FLAG(placement.flags, _new_object_never_automatically_delete_bit, TRUE);
			placement.position.x = x;
			placement.position.y = y;
			placement.position.z = z;
			euler_to_forward_up(yaw, pitch, roll, &placement.forward, &placement.up);
			inst->object_index = object_new(&placement);
			{
				struct object_datum *obj = forge_valid_object_get(inst->object_index);
				if (obj)
				{
					SET_FLAG(obj->object.flags, _object_cannot_be_garbage_bit, TRUE);
					SET_FLAG(obj->object.flags, _object_deleted_when_deactivated_bit, FALSE);
					SET_FLAG(obj->object.flags, _object_outside_of_map_bit, FALSE);
					if (group_tag == 'vehi')
					{
						unit_set_enterable_by_player(inst->object_index, TRUE);
						unit_set_controllable(inst->object_index, TRUE);
					}
					else if (obj->object.type == _object_type_machine ||
							 obj->object.type == _object_type_light_fixture ||
							 obj->object.type == _object_type_control)
					{
						struct scenario_device_datum scen_dev;
						memset(&scen_dev, 0, sizeof(scen_dev));
						scen_dev.power_group_index = NONE;
						scen_dev.position_group_index = NONE;
						scen_dev.flags = 0;
						device_add_scenario_information(inst->object_index, &scen_dev);
						if (obj->object.type == _object_type_light_fixture)
						{
							struct light_fixture_datum *lf = (struct light_fixture_datum *)obj;
							lf->light_fixture.color.red = 1.0f;
							lf->light_fixture.color.green = 1.0f;
							lf->light_fixture.color.blue = 1.0f;
							lf->light_fixture.intensity = 1.0f;
							lf->light_fixture.falloff_angle = 1.0f;
							lf->light_fixture.cutoff_angle = 1.2f;
						}
					}
					object_set_automatic_deactivation(inst->object_index, FALSE);
					object_activate(inst->object_index);
				}
				else
				{
					inst->object_index = NONE;
				}
			}
		}

		if (g_forge.edit_state.placed_object_count < FORGE_MAX_PLACED_OBJECTS)
		{
			struct forge_placed_object_entry *po = &g_forge.edit_state.placed_objects[g_forge.edit_state.placed_object_count];
			inst->edit_entry_index = g_forge.edit_state.placed_object_count++;
			memset(po, 0, sizeof(*po));
			po->active = 1;
			po->source_cache_id = source_cache_id;
			po->group_tag = group_tag;
			po->gametype_flags = inst->gametype_flags;
			po->category = (group_tag == 'vehi') ? _forge_object_vehicle :
				(group_tag == 'weap') ? _forge_object_weapon :
				(group_tag == 'bipd') ? _forge_object_biped :
				(group_tag == 'eqip') ? _forge_object_equipment : _forge_object_scenery;
			snprintf(po->tag_path, sizeof(po->tag_path), "%s", tag_path);
			po->x = x;
			po->y = y;
			po->z = z;
			po->yaw = yaw;
			po->pitch = pitch;
			po->roll = roll;
		}
		forge_set_status("Placed '%s' at (%.1f, %.1f, %.1f)", tag_path, x, y, z);
	}

	if (broadcast_net && forge_is_multiplayer_session())
	{
		forge_network_send_spawn(group_tag, tag_path, source_cache_id, x, y, z, yaw, pitch, roll, team_designator);
	}

	return live_idx;
}

void forge_3d_move_instance(
	int32_t live_idx,
	float x, float y, float z,
	float yaw, float pitch, float roll,
	int broadcast_net)
{
	struct forge_live_instance *inst;

	if (live_idx < 0 || live_idx >= g_forge.live_instance_count)
		return;
	inst = &g_forge.live_instances[live_idx];
	if (!inst->active)
		return;

	inst->x = x;
	inst->y = y;
	inst->z = z;
	inst->yaw = yaw;
	inst->pitch = pitch;
	inst->roll = roll;

	if (inst->object_index != NONE)
	{
		struct object_datum *obj = forge_valid_object_get(inst->object_index);
		if (obj)
		{
			real_point3d pos;
			real_vector3d fwd, up;
			pos.x = x;
			pos.y = y;
			pos.z = z;
			euler_to_forward_up(yaw, pitch, roll, &fwd, &up);
			SET_FLAG(obj->object.flags, _object_outside_of_map_bit, FALSE);
			SET_FLAG(obj->object.flags, _object_no_collisions_bit, FALSE);
			SET_FLAG(obj->object.flags, _object_at_rest_bit, FALSE);
			object_set_position(inst->object_index, &pos, &fwd, &up);
			object_set_automatic_deactivation(inst->object_index, FALSE);
			object_activate(inst->object_index);
			obj->object.translational_velocity.i = 0.0f;
			obj->object.translational_velocity.j = 0.0f;
			obj->object.translational_velocity.k = 0.0f;
			obj->object.angular_velocity.i = 0.0f;
			obj->object.angular_velocity.j = 0.0f;
			obj->object.angular_velocity.k = 0.0f;
		}
		else
		{
			inst->object_index = NONE;
		}
	}

	if (inst->group_tag == 'tele' || inst->group_tag == 'flag')
	{
		sync_live_netgame_flags_to_scenario();
	}
	else if (inst->group_tag == 'spwn')
	{
		sync_live_player_spawns_to_scenario();
	}
	else if (inst->is_default_scenario_object)
	{
		if (inst->edit_entry_index >= 0 && inst->edit_entry_index < g_forge.edit_state.default_edit_count)
		{
			struct forge_default_object_edit *de = &g_forge.edit_state.default_edits[inst->edit_entry_index];
			de->new_x = x;
			de->new_y = y;
			de->new_z = z;
			de->new_yaw = yaw;
			de->new_pitch = pitch;
			de->new_roll = roll;
			de->gametype_flags = inst->gametype_flags;
		}
	}
	else if (!inst->is_ai_unit && inst->edit_entry_index >= 0 &&
		inst->edit_entry_index < g_forge.edit_state.placed_object_count)
	{
		struct forge_placed_object_entry *po = &g_forge.edit_state.placed_objects[inst->edit_entry_index];
		po->x = x;
		po->y = y;
		po->z = z;
		po->yaw = yaw;
		po->pitch = pitch;
		po->roll = roll;
		po->gametype_flags = inst->gametype_flags;
	}

	if (broadcast_net && forge_is_multiplayer_session())
	{
		forge_network_send_transform(live_idx, x, y, z, yaw, pitch, roll);
	}
}

void forge_3d_delete_instance(int32_t live_idx, int broadcast_net)
{
	struct forge_live_instance *inst;

	if (live_idx < 0 || live_idx >= g_forge.live_instance_count)
		return;
	inst = &g_forge.live_instances[live_idx];
	if (!inst->active)
		return;

	if (inst->object_index != NONE)
	{
		if (forge_valid_object_get(inst->object_index))
			object_delete(inst->object_index);
		inst->object_index = NONE;
	}
	inst->active = 0;

	if (inst->group_tag == 'tele' || inst->group_tag == 'flag')
	{
		sync_live_netgame_flags_to_scenario();
	}
	else if (inst->group_tag == 'spwn')
	{
		sync_live_player_spawns_to_scenario();
	}
	else if (inst->is_default_scenario_object)
	{
		if (inst->edit_entry_index >= 0 && inst->edit_entry_index < g_forge.edit_state.default_edit_count)
		{
			g_forge.edit_state.default_edits[inst->edit_entry_index].deleted = 1;
		}
	}
	else if (!inst->is_ai_unit && inst->edit_entry_index >= 0 &&
		inst->edit_entry_index < g_forge.edit_state.placed_object_count)
	{
		g_forge.edit_state.placed_objects[inst->edit_entry_index].active = 0;
		g_forge.edit_state.placed_objects[inst->edit_entry_index].x = -99999.0f;
	}

	forge_set_status("Deleted '%s'", inst->tag_path);

	if (broadcast_net && forge_is_multiplayer_session())
	{
		forge_network_send_delete(live_idx);
	}
}

static int get_filtered_catalog_indices_for_cat(enum forge_palette_category cat, int *out_indices, int max_out)
{
	int i, count = 0;
	for (i = 0; i < g_forge.catalog_count && count < max_out; i++)
	{
		if (g_forge.catalog[i].category == (uint8_t)cat &&
			forge_catalog_entry_allowed(&g_forge.catalog[i]))
		{
			if (out_indices)
				out_indices[count] = i;
			count++;
		}
	}
	return count;
}

static int get_filtered_catalog_indices(int *out_indices, int max_out)
{
	return get_filtered_catalog_indices_for_cat(g_forge.current_category, out_indices, max_out);
}

static int s_save_modal_just_opened = 0;

static void update_save_modal_input(void)
{
	static int prev_key[256];
	int vk;

	director_inhibit_input(0);
	director_inhibit_facing(0);

	if (s_save_modal_just_opened)
	{
		for (vk = 0; vk < 256; vk++)
			prev_key[vk] = (GetAsyncKeyState(vk) & 0x8000) != 0;
		s_save_modal_just_opened = 0;
		return;
	}

	for (vk = 'A'; vk <= 'Z'; vk++)
	{
		int down = (GetAsyncKeyState(vk) & 0x8000) != 0;
		if (down && !prev_key[vk] && g_forge.save_map_name_len < 48)
		{
			g_forge.save_map_name[g_forge.save_map_name_len++] = (char)(vk - 'A' + 'a');
			g_forge.save_map_name[g_forge.save_map_name_len] = 0;
		}
		prev_key[vk] = down;
	}
	for (vk = '0'; vk <= '9'; vk++)
	{
		int down = (GetAsyncKeyState(vk) & 0x8000) != 0;
		if (down && !prev_key[vk] && g_forge.save_map_name_len < 48)
		{
			g_forge.save_map_name[g_forge.save_map_name_len++] = (char)vk;
			g_forge.save_map_name[g_forge.save_map_name_len] = 0;
		}
		prev_key[vk] = down;
	}
	{
		int minus_down = (GetAsyncKeyState(VK_OEM_MINUS) & 0x8000) != 0;
		if (minus_down && !prev_key[VK_OEM_MINUS] && g_forge.save_map_name_len < 48)
		{
			g_forge.save_map_name[g_forge.save_map_name_len++] = '_';
			g_forge.save_map_name[g_forge.save_map_name_len] = 0;
		}
		prev_key[VK_OEM_MINUS] = minus_down;
	}
	{
		int bk_down = (GetAsyncKeyState(VK_BACK) & 0x8000) != 0;
		if (bk_down && !prev_key[VK_BACK] && g_forge.save_map_name_len > 0)
		{
			g_forge.save_map_name[--g_forge.save_map_name_len] = 0;
		}
		prev_key[VK_BACK] = bk_down;
	}
	{
		int esc_down = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
		if (esc_down && !prev_key[VK_ESCAPE])
		{
			g_forge.active_modal = _forge_modal_none;
			prev_key[VK_ESCAPE] = esc_down;
			return;
		}
		prev_key[VK_ESCAPE] = esc_down;
	}
	{
		int ent_down = (GetAsyncKeyState(VK_RETURN) & 0x8000) != 0;
		if (ent_down && !prev_key[VK_RETURN])
		{
			forge_ui_save_copy_as(g_forge.save_map_name);
			g_forge.active_modal = _forge_modal_none;
		}
		prev_key[VK_RETURN] = ent_down;
	}
}

static int find_valid_bsp_spawn_point(real_point3d *out_pos, float *out_yaw)
{
	struct scenario *scen = global_scenario_get();
	long i;

	if (!scen)
		return 0;

	/* 1. Standard scenario player starting locations inside the active BSP */
	for (i = 0; i < scen->players.count; i++)
	{
		struct encounter_player_starting_location *loc = TAG_BLOCK_GET_ELEMENT(
			&scen->players, i, struct encounter_player_starting_location);
		real_point3d test_pt = loc->position;
		test_pt.z += 0.3f;
		if (scenario_leaf_index_from_point(&test_pt) != NONE)
		{
			*out_pos = loc->position;
			*out_yaw = loc->facing;
			return 1;
		}
	}

	/* 2. Scenario cutscene flags inside the active BSP (useful on SP maps whose start BSP has no player spawn) */
	for (i = 0; i < scen->cutscene_flags.count; i++)
	{
		struct scenario_cutscene_flag *flg = TAG_BLOCK_GET_ELEMENT(
			&scen->cutscene_flags, i, struct scenario_cutscene_flag);
		real_point3d test_pt = flg->position;
		test_pt.z += 0.3f;
		if (scenario_leaf_index_from_point(&test_pt) != NONE)
		{
			*out_pos = flg->position;
			*out_yaw = flg->facing.yaw;
			return 1;
		}
	}

	/* 3. Any placed object inside the active BSP */
	{
		struct object_iterator iter;
		struct object_datum *obj;
		object_iterator_new(&iter, (unsigned long)-1, 0);
		while ((obj = object_iterator_next(&iter)) != NULL)
		{
			real_point3d test_pt = obj->object.position;
			test_pt.z += 0.5f;
			if (scenario_leaf_index_from_point(&test_pt) != NONE)
			{
				*out_pos = test_pt;
				*out_yaw = 0.0f;
				return 1;
			}
		}
	}

	/* 4. Fallback to first player starting location even if leaf test failed */
	if (scen->players.count > 0)
	{
		struct encounter_player_starting_location *loc = TAG_BLOCK_GET_ELEMENT(
			&scen->players, 0, struct encounter_player_starting_location);
		*out_pos = loc->position;
		*out_yaw = loc->facing;
		return 1;
	}
	return 0;
}

static void forge_init_map_scenario_entities(void)
{
	struct scenario *scen = global_scenario_get();
	long i;

	if (!scen)
		return;

	/* 1. Load existing scenario player starting locations (scen->players) into g_forge.live_instances
	 *    so Red Team, Blue Team, and FFA player spawns appear as coloured ground circles + facing arrows
	 *    and can be grabbed, moved, rotated, deleted, or team-edited on any map! */
	for (i = 0; i < scen->players.count && g_forge.live_instance_count < FORGE_MAX_LIVE_INSTANCES; i++)
	{
		struct player_starting_location *loc = TAG_BLOCK_GET_ELEMENT(
			&scen->players, i, struct player_starting_location);
		struct forge_live_instance *inst;
		int16_t t0 = loc->game_types[0];
		int16_t t1 = loc->game_types[1];
		int16_t team_desig;
		char const *label_path;

		if (t0 == 13 /* _game_engine_all_non_team (FFA) */ ||
			(t0 == 0 && t1 == 0) ||
			(loc->team_index < 0 || loc->team_index >= 2))
		{
			team_desig = 2; /* FFA Spawn - Yellow */
			label_path = "spawn\\player_ffa";
		}
		else if (loc->team_index == 1)
		{
			team_desig = 1; /* Blue Team Spawn - Blue */
			label_path = "spawn\\player_blue";
		}
		else
		{
			team_desig = 0; /* Red Team Spawn - Red */
			label_path = "spawn\\player_red";
		}

		inst = &g_forge.live_instances[g_forge.live_instance_count++];
		memset(inst, 0, sizeof(*inst));
		inst->active = 1;
		inst->group_tag = 'spwn';
		inst->source_cache_id = 0;
		inst->is_default_scenario_object = 1;
		inst->pad_inst = (uint8_t)(loc->structure_bsp_reference_index >= 0 ? loc->structure_bsp_reference_index : 0);
		inst->edit_entry_index = -1;
		inst->netgame_flag_type = t0;
		inst->team_designator = team_desig;
		inst->gametype_flags = 0x0FFFu;
		snprintf(inst->tag_path, sizeof(inst->tag_path), "%s", label_path);
		inst->orig_x = loc->position.x;
		inst->orig_y = loc->position.y;
		inst->orig_z = loc->position.z;
		inst->x = loc->position.x;
		inst->y = loc->position.y;
		inst->z = loc->position.z;
		inst->yaw = loc->facing;
		inst->object_index = NONE;
	}
	sync_live_player_spawns_to_scenario();

	/* 2. Load existing scenario netgame_flags (Teleporters In/Out, KOTH Hills, Oddballs, CTF Flags, Race Nodes)
	 *    into g_forge.live_instances so they can be grabbed, moved, rotated, deleted, and linked via Reference ID! */
	for (i = 0; i < scen->netgame_flags.count && g_forge.live_instance_count < FORGE_MAX_LIVE_INSTANCES; i++)
	{
		struct scenario_netgame_flag *flg = TAG_BLOCK_GET_ELEMENT(
			&scen->netgame_flags, i, struct scenario_netgame_flag);
		struct forge_live_instance *inst;
		char const *label_path = NULL;
		uint32_t grp = 'flag';

		if (flg->type == _netgame_flag_teleporter_source)
		{
			grp = 'tele';
			label_path = "teleporter\\origin";
		}
		else if (flg->type == _netgame_flag_teleporter_target)
		{
			grp = 'tele';
			label_path = "teleporter\\destination";
		}
		else if (flg->type == _netgame_flag_hill)
		{
			grp = 'flag';
			label_path = "objective\\koth_hill";
		}
		else if (flg->type == _netgame_flag_oddball_ball_spawn)
		{
			grp = 'flag';
			label_path = "objective\\oddball_spawn";
		}
		else if (flg->type == _netgame_flag_ctf_flag)
		{
			grp = 'flag';
			label_path = (flg->team_index == 1) ? "objective\\ctf_flag_blue" : "objective\\ctf_flag_red";
		}
		else if (flg->type == _netgame_flag_ctf_vehicle)
		{
			grp = 'flag';
			label_path = (flg->team_index == 1) ? "objective\\ctf_vehicle_blue" : "objective\\ctf_vehicle_red";
		}
		else if (flg->type == _netgame_flag_race_track)
		{
			grp = 'flag';
			label_path = "objective\\race_track";
		}
		else if (flg->type == _netgame_flag_race_vehicle)
		{
			grp = 'flag';
			label_path = "objective\\race_vehicle";
		}
		else
		{
			grp = 'flag';
			label_path = "objective\\netgame_flag";
		}

		inst = &g_forge.live_instances[g_forge.live_instance_count++];
		memset(inst, 0, sizeof(*inst));
		inst->active = 1;
		inst->group_tag = grp;
		inst->source_cache_id = 0;
		inst->is_default_scenario_object = 1;
		inst->edit_entry_index = -1;
		inst->netgame_flag_type = flg->type;
		inst->team_designator = flg->team_index;
		inst->gametype_flags = 0x0FFFu;
		snprintf(inst->tag_path, sizeof(inst->tag_path), "%s", label_path);
		inst->orig_x = flg->position.x;
		inst->orig_y = flg->position.y;
		inst->orig_z = flg->position.z;
		inst->x = flg->position.x;
		inst->y = flg->position.y;
		inst->z = flg->position.z;
		inst->yaw = flg->facing;
		inst->object_index = NONE;
	}
	sync_live_netgame_flags_to_scenario();

	/* 2. Points 7 & 13: In Single Player mode (!game_engine_running()), the engine skips spawning
	 *    netgame_equipment (weapons/grenades/powerups) and multiplayer-flagged vehicles.
	 *    Force every weapon, item, and vehicle present on the map to spawn! */
	if (!game_engine_running())
	{
		for (i = 0; i < scen->netgame_equipment.count; i++)
		{
			struct scenario_netgame_equipment *eq = TAG_BLOCK_GET_ELEMENT(
				&scen->netgame_equipment, i, struct scenario_netgame_equipment);
			long item_def_idx = NONE;

			if (eq->item_collection.index != NONE)
			{
				uint8_t const *itmc = (uint8_t const *)tag_get('itmc', eq->item_collection.index);
				if (itmc)
				{
					int32_t perm_count = *(int32_t const *)(itmc + 0x00);
					uint8_t const *perms = *(uint8_t const *const *)(itmc + 0x04);
					if (perm_count > 0 && perms)
					{
						/* item_permutation is 84 bytes; item tag_reference is at +0x24 (index at +0x30) */
						item_def_idx = *(int32_t const *)(perms + 0x30);
					}
				}
			}

			if (item_def_idx != NONE)
			{
				struct object_placement_data pdata;
				long oidx;
				object_placement_data_new(&pdata, item_def_idx, NONE);
				pdata.position = eq->position;
				pdata.position.z += 0.08f;
				euler_to_forward_up(eq->facing, 0.0f, 0.0f, &pdata.forward, &pdata.up);
				oidx = object_new(&pdata);
				if (oidx != NONE)
				{
					object_set_automatic_deactivation(oidx, FALSE);
					object_activate(oidx);
				}
			}
		}

		/* Also ensure all scenario vehicles are spawned if the single-player game skipped any MP-flagged ones */
		for (i = 0; i < scen->vehicles.count; i++)
		{
			struct scenario_placeholder_datum *sv = TAG_BLOCK_GET_ELEMENT(
				&scen->vehicles, i, struct scenario_placeholder_datum);
			if (sv->object.palette_entry_index >= 0 && sv->object.palette_entry_index < scen->vehicle_palette.count)
			{
				struct scenario_object_palette_entry *pal = TAG_BLOCK_GET_ELEMENT(
					&scen->vehicle_palette, sv->object.palette_entry_index, struct scenario_object_palette_entry);
				if (pal->reference.index != NONE)
				{
					/* Check if a vehicle already exists within 1.5 world units of this scenario spawn */
					struct object_iterator viter;
					struct object_datum *vobj;
					int already_spawned = 0;
					object_iterator_new(&viter, _object_mask_vehicle, 0);
					while ((vobj = object_iterator_next(&viter)) != NULL)
					{
						float dx = vobj->object.position.x - sv->object.position.x;
						float dy = vobj->object.position.y - sv->object.position.y;
						float dz = vobj->object.position.z - sv->object.position.z;
						if (dx * dx + dy * dy + dz * dz < 2.25f)
						{
							already_spawned = 1;
							break;
						}
					}
					if (!already_spawned)
					{
						struct object_placement_data pdata;
						long v_idx;
						object_placement_data_new(&pdata, pal->reference.index, NONE);
						pdata.position = sv->object.position;
						pdata.position.z += 0.12f;
						euler_to_forward_up(sv->object.rotation.yaw, sv->object.rotation.pitch, sv->object.rotation.roll, &pdata.forward, &pdata.up);
						v_idx = object_new(&pdata);
						if (v_idx != NONE)
						{
							object_set_automatic_deactivation(v_idx, FALSE);
							object_activate(v_idx);
						}
					}
				}
			}
		}
	}
}

static int count_active_instances_for_catalog_entry(struct forge_catalog_entry const *ce)
{
	int i, count = 0;
	if (!ce || !ce->tag_path[0])
		return 0;
	for (i = 0; i < g_forge.live_instance_count; i++)
	{
		if (g_forge.live_instances[i].active &&
			g_forge.live_instances[i].group_tag == ce->group_tag &&
			_stricmp(g_forge.live_instances[i].tag_path, ce->tag_path) == 0)
		{
			count++;
		}
	}
	return count;
}

static void format_clean_uppercase_item_name(char const *display_name, char *out_buf, size_t out_sz)
{
	char const *p = display_name ? display_name : "";
	size_t i = 0;
	if (!out_buf || out_sz == 0)
		return;
	/* Preserve "[c] " custom .ocetags badge while stripping generic "[WEAP] ", "[VEHI] ", "[SPAWN] ", "[KOTH] " prefixes */
	if (_strnicmp(p, "[c]", 3) == 0)
	{
		p += 3;
		while (*p == ' ')
			p++;
		if (out_sz >= 5)
		{
			out_buf[0] = '[';
			out_buf[1] = 'c';
			out_buf[2] = ']';
			out_buf[3] = ' ';
			i = 4;
		}
	}
	else if (p[0] == '[')
	{
		char const *rb = strchr(p, ']');
		if (rb)
		{
			p = rb + 1;
			while (*p == ' ')
				p++;
		}
	}
	while (*p && i + 1 < out_sz)
	{
		char ch = *p++;
		if (ch >= 'a' && ch <= 'z')
			ch = (char)(ch - 'a' + 'A');
		out_buf[i++] = ch;
	}
	out_buf[i] = '\0';
}

static char const *gametype_flags_label(uint16_t flags)
{
	if (flags == 0x0001u) return "CTF Only";
	if (flags == 0x0002u) return "Slayer Only";
	if (flags == 0x0004u) return "Oddball Only";
	if (flags == 0x0008u) return "King of the Hill";
	if (flags == 0x0010u) return "Race Only";
	return "All Modes (Default)";
}

static uint16_t cycle_gametype_flags(uint16_t flags, int dir)
{
	static uint16_t const modes[6] = { 0x0FFFu, 0x0001u, 0x0002u, 0x0004u, 0x0008u, 0x0010u };
	int i, cur = 0;
	for (i = 0; i < 6; i++)
	{
		if (modes[i] == flags)
		{
			cur = i;
			break;
		}
	}
	cur = (cur + 6 + dir) % 6;
	return modes[cur];
}

static void forge_cancel_or_release_held_item(void)
{
	if (g_forge.grabbed_live_index >= 0 && g_forge.grabbed_live_index < g_forge.live_instance_count)
	{
		if (g_forge.grabbed_is_new_spawn)
		{
			if (g_forge.koth_group_placing)
			{
				int k;
				for (k = 0; k < 10; k++)
				{
					int32_t nidx = g_forge.koth_group_live_indices[k];
					if (nidx >= 0 && nidx < g_forge.live_instance_count)
						forge_3d_delete_instance(nidx, 1);
				}
				g_forge.koth_group_placing = 0;
			}
			else
			{
				forge_3d_delete_instance(g_forge.grabbed_live_index, 1);
			}
		}
		else
		{
			forge_3d_move_instance(
				g_forge.grabbed_live_index,
				g_forge.grab_orig_x,
				g_forge.grab_orig_y,
				g_forge.grab_orig_z,
				g_forge.grab_orig_yaw,
				g_forge.grab_orig_pitch,
				g_forge.grab_orig_roll,
				1);
		}
	}
	g_forge.grabbed_live_index = -1;
	g_forge.grabbed_object_index = NONE;
	g_forge.grabbed_is_new_spawn = 0;
	g_forge.koth_group_placing = 0;
}

void forge_3d_update(float dt)
{
	static int prev_f1, prev_f2, prev_f4, prev_f5, prev_f8, prev_esc, prev_x, prev_bk, prev_lmb;
	static int prev_up, prev_down, prev_left, prev_right, prev_enter, prev_g, prev_z, prev_f, prev_del;
	static int s_init_wait_ticks = 0;
	static struct scenario *s_last_scen = NULL;
	static int s_prev_modal = 0;
	struct scenario *scen;
	int cur_f1, cur_f2, cur_f4, cur_f5, cur_f8, cur_esc, cur_x, cur_bk, cur_lmb;
	int cur_up, cur_down, cur_left, cur_right, cur_enter, cur_g, cur_z, cur_f, cur_del;

	forge_runtime_init();

	if (g_forge.active_modal == _forge_modal_save_copy_as && s_prev_modal != _forge_modal_save_copy_as)
		s_save_modal_just_opened = 1;
	s_prev_modal = g_forge.active_modal;

	if (main_get_map_name() && strcmp(main_get_map_name(), "levels\\ui\\ui") == 0)
		return;
	scen = global_scenario_get();
	if (!scen || scen->type == 2 /* _scenario_type_main_menu */)
		return;
	if (scen != s_last_scen)
	{
		s_last_scen = scen;
		s_init_wait_ticks = 0;
		g_forge.monitor_cam_initialized = 0;
		g_forge.live_instance_count = 0;
		if (!g_forge_session_enabled)
		{
			/* When playing a saved Forge map in normal Multiplayer / Splitscreen outside Forge mode,
			 * still load any non-baked or AI objects from [ForgeState] once on map load! */
			forge_load_saved_map_state_if_present(main_get_map_name());
		}
	}

	/* Only run Forge editor controls when inside a Forge session */
	if (!g_forge_session_enabled)
		return;

	/* Point 4: When starting a game, spawn as the Spartan at a default player spawn point */
	if (!g_forge.monitor_cam_initialized)
	{
		long unit_idx = player_control_get_unit_index(0);
		real_point3d spawn_pos;
		float spawn_yaw = 0.0f;
		int ready = 0;

		s_init_wait_ticks++;
		if (unit_idx != NONE)
		{
			struct unit_datum *u = (struct unit_datum *)object_try_and_get_and_verify_type(unit_idx, _object_mask_unit);
			if (u)
			{
				real_point3d test_pt = u->object.position;
				test_pt.z += 0.3f;
				if (scenario_leaf_index_from_point(&test_pt) != NONE)
				{
					spawn_pos = u->object.position;
					spawn_yaw = (float)atan2((double)u->object.forward.j, (double)u->object.forward.i);
					ready = 1;
				}
				else if (find_valid_bsp_spawn_point(&spawn_pos, &spawn_yaw))
				{
					real_vector3d fwd, up;
					euler_to_forward_up(spawn_yaw, 0.0f, 0.0f, &fwd, &up);
					SET_FLAG(u->object.flags, _object_no_collisions_bit, FALSE);
					object_set_position(unit_idx, &spawn_pos, &fwd, &up);
					ready = 1;
				}
			}
		}
		else if (s_init_wait_ticks >= 4 && find_valid_bsp_spawn_point(&spawn_pos, &spawn_yaw))
		{
			ready = 1;
		}

		if (ready)
		{
			s_init_wait_ticks = 0;
			g_forge.cam_x = spawn_pos.x;
			g_forge.cam_y = spawn_pos.y;
			g_forge.cam_z = spawn_pos.z + 0.65f;
			g_forge.cam_yaw = spawn_yaw;
			g_forge.cam_pitch = 0.0f;
			g_forge.monitor_cam_initialized = 1;
			g_forge.mode = _forge_mode_play; /* Spawn as Spartan at default player spawn */
			forge_init_map_scenario_entities();
			forge_load_saved_map_state_if_present(main_get_map_name());
			forge_set_status("Forge Ready / Spawned as Spartan [F1 Forge Mode / F5 Save / F8 Reload UI]");
		}
		else
		{
			return;
		}
	}
	else if (g_forge.live_instance_count == 0 && (scen->players.count > 0 || scen->netgame_flags.count > 0))
	{
		forge_init_map_scenario_entities();
		forge_load_saved_map_state_if_present(main_get_map_name());
	}

	/* Ensure any vehicle driven by a player has powered seat_power[0] = 1.0f so flyers/hovercraft thrust works */
	{
		long unit_idx = player_control_get_unit_index(0);
		if (unit_idx != NONE)
		{
			struct unit_datum *u = (struct unit_datum *)object_try_and_get_and_verify_type(unit_idx, _object_mask_unit);
			if (u && u->object.parent_object_index != NONE)
			{
				struct unit_datum *veh = (struct unit_datum *)object_try_and_get_and_verify_type(
					u->object.parent_object_index, _object_mask_vehicle);
				if (veh)
				{
					veh->unit.seat_power[0] = 1.0f;
					veh->unit.seat_power[1] = 1.0f;
				}
			}
		}
	}

	/* Point 2 & Point 8: Always keep Monitor floating at cam_x/y/z, invulnerable, zero-velocity, and hide Spartan-linked
	 * green lights/weapons EVEN when F2 Forge Menu, Save Modal, or Escape Pause Menu is open! */
	if (g_forge.mode == _forge_mode_3d_monitor)
	{
		long unit_idx = player_control_get_unit_index(0);
		if (unit_idx != NONE)
		{
			struct unit_datum *u = (struct unit_datum *)object_try_and_get_and_verify_type(unit_idx, _object_mask_unit);
			if (u)
			{
				real_point3d u_pos;
				real_vector3d m_fwd, m_up;
				u_pos.x = g_forge.cam_x;
				u_pos.y = g_forge.cam_y;
				u_pos.z = g_forge.cam_z - 0.65f;
				euler_to_forward_up(g_forge.cam_yaw, g_forge.cam_pitch, g_forge.cam_roll, &m_fwd, &m_up);
				SET_FLAG(u->object.flags, _object_no_collisions_bit, TRUE);
				SET_FLAG(u->object.damage_flags, _object_cannot_take_damage_bit, TRUE);
				SET_FLAG(u->object.damage_flags, _object_shield_depleted_bit, FALSE);
				u->object.body_vitality = 1.0f;
				u->object.shield_vitality = 1.0f;
				forge_hide_spartan_linked_attachments_and_children(unit_idx, 1);
				object_set_automatic_deactivation(unit_idx, FALSE);
				if (scenario_leaf_index_from_point(&u_pos) != NONE)
				{
					object_set_position(unit_idx, &u_pos, &m_fwd, &m_up);
				}
				else
				{
					u->object.position = u_pos;
					u->object.forward = m_fwd;
					u->object.up = m_up;
				}
				u->unit.desired_facing_vector = m_fwd;
				u->unit.desired_aiming_vector = m_fwd;
				u->unit.aiming_vector = m_fwd;
				u->unit.desired_looking_vector = m_fwd;
				u->unit.looking_vector = m_fwd;
				u->object.translational_velocity.i = 0.0f;
				u->object.translational_velocity.j = 0.0f;
				u->object.translational_velocity.k = 0.0f;
				u->object.angular_velocity.i = 0.0f;
				u->object.angular_velocity.j = 0.0f;
				u->object.angular_velocity.k = 0.0f;
			}
		}
		if (player_data)
		{
			long p_idx;
			for (p_idx = data_next_index(player_data, NONE); p_idx != NONE; p_idx = data_next_index(player_data, p_idx))
			{
				struct player_datum *pl = player_get(p_idx);
				if (pl && pl->unit_index != NONE)
					forge_hide_spartan_linked_attachments_and_children(pl->unit_index, 1);
			}
		}
	}

	/* When the native in-game Escape Pause Menu is open, pause Forge flight/editor input */
	if (ui_widgets_active())
		return;

	/* Validate grabbed_object_index each tick so a despawned/recreated object never leaves a stale handle */
	if (g_forge.grabbed_object_index != NONE && !forge_valid_object_get(g_forge.grabbed_object_index))
	{
		if (g_forge.grabbed_live_index >= 0 && g_forge.grabbed_live_index < g_forge.live_instance_count)
			g_forge.live_instances[g_forge.grabbed_live_index].object_index = NONE;
		g_forge.grabbed_object_index = NONE;
	}

	cur_f1 = (GetAsyncKeyState(VK_F1) & 0x8000) != 0;
	cur_f2 = (GetAsyncKeyState(VK_F2) & 0x8000) != 0;
	cur_f4 = (GetAsyncKeyState(VK_F4) & 0x8000) != 0;
	cur_f5 = (GetAsyncKeyState(VK_F5) & 0x8000) != 0;
	cur_f8 = (GetAsyncKeyState(0x77) & 0x8000) != 0; /* VK_F8 */
	cur_esc = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
	cur_x = (GetAsyncKeyState('X') & 0x8000) != 0;
	cur_bk = (GetAsyncKeyState(VK_BACK) & 0x8000) != 0;
	cur_lmb = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;

	/* Hot-reload Forge UI XML & PNG backgrounds live in-game with F8 */
	if (cur_f8 && !prev_f8)
	{
		forge_ui_theme_load(1);
		forge_set_status("Reloaded Forge UI Theme (OpenCE Forge/ui/forge_ui.xml + PNG backgrounds)");
	}

	/* Mode & Modal Hotkeys (Point 2: Seamless location continuity between Spartan and Forge Mode; Point 11: Overhead viewer removed) */
	if (cur_f1 && !prev_f1)
	{
		long unit_idx = player_control_get_unit_index(0);
		if (g_forge.grabbed_live_index >= 0)
			forge_cancel_or_release_held_item();
		g_forge.active_modal = _forge_modal_none;
		if (g_forge.mode == _forge_mode_3d_monitor)
		{
			g_forge.mode = _forge_mode_play;
			if (unit_idx != NONE)
			{
				struct unit_datum *u = (struct unit_datum *)object_try_and_get_and_verify_type(unit_idx, _object_mask_unit);
				if (u)
				{
					real_point3d sp_pos;
					real_vector3d fwd, up;
					sp_pos.x = g_forge.cam_x;
					sp_pos.y = g_forge.cam_y;
					sp_pos.z = g_forge.cam_z - 0.65f;
					euler_to_forward_up(g_forge.cam_yaw, 0.0f, 0.0f, &fwd, &up);
					SET_FLAG(u->object.flags, _object_no_collisions_bit, FALSE);
					SET_FLAG(u->object.flags, _object_at_rest_bit, FALSE);
					SET_FLAG(u->object.damage_flags, _object_cannot_take_damage_bit, FALSE);
					forge_hide_spartan_linked_attachments_and_children(unit_idx, 0);
					object_set_position(unit_idx, &sp_pos, &fwd, &up);
					u->unit.desired_facing_vector = fwd;
					u->unit.desired_aiming_vector = fwd;
					u->unit.aiming_vector = fwd;
					u->unit.desired_looking_vector = fwd;
					u->unit.looking_vector = fwd;
					object_activate(unit_idx);
				}
			}
			forge_set_status("Spartan Play Mode [Press F1 for Forge Mode]");
		}
		else
		{
			if (unit_idx != NONE)
			{
				struct unit_datum *u = (struct unit_datum *)object_try_and_get_and_verify_type(unit_idx, _object_mask_unit);
				if (u)
				{
					g_forge.cam_x = u->object.position.x;
					g_forge.cam_y = u->object.position.y;
					g_forge.cam_z = u->object.position.z + 0.65f;
					g_forge.cam_yaw = (float)atan2((double)u->unit.aiming_vector.j, (double)u->unit.aiming_vector.i);
					g_forge.cam_pitch = (float)asin((double)u->unit.aiming_vector.k);
					forge_hide_spartan_linked_attachments_and_children(unit_idx, 1);
				}
			}
			g_forge.mode = _forge_mode_3d_monitor;
			forge_set_status("Forge Mode (Invulnerable) [F2 Forge Menu / F4 3rd-Person / F5 Save]");
		}
	}
	/* Point 1: Only allow F2 to open Forge Menu when the player is in Forge Mode (_forge_mode_3d_monitor)! */
	if (cur_f2 && !prev_f2)
	{
		if (g_forge.mode == _forge_mode_3d_monitor)
		{
			if (g_forge.active_modal == _forge_modal_spawn_palette)
			{
				g_forge.active_modal = _forge_modal_none;
			}
			else
			{
				if (g_forge.grabbed_live_index >= 0)
					forge_cancel_or_release_held_item();
				g_forge.active_modal = _forge_modal_spawn_palette;
				g_forge.h3_menu_stage = 1; /* Unified single-stage tabbed menu: cycle categories with [<-] / [->] */
				if (g_forge.current_category == _forge_cat_bipeds ||
					g_forge.current_category == _forge_cat_ai_squads)
				{
					g_forge.current_category = _forge_cat_weapons;
				}
			}
		}
	}
	if (cur_f4 && !prev_f4 && g_forge.mode == _forge_mode_3d_monitor)
	{
		g_forge.third_person_view = !g_forge.third_person_view;
		forge_set_status("3rd-Person Forge View: %s", g_forge.third_person_view ? "ON" : "OFF");
	}
	/* F5 in-game is strictly for Quick Save only (available after initial save via Esc -> SAVE MAP..) */
	if (cur_f5 && !prev_f5)
	{
		if (g_forge.has_saved_map_once && g_forge.save_map_name[0])
		{
			forge_ui_save_copy_as(g_forge.save_map_name);
		}
		else
		{
			forge_set_status("Quick Save unavailable: Use [Esc] -> SAVE MAP.. for initial save first");
		}
	}
	if (cur_x && !prev_x && g_forge.mode == _forge_mode_3d_monitor && g_forge.grabbed_live_index >= 0)
	{
		g_forge.active_modal = (g_forge.active_modal == _forge_modal_object_properties)
			? _forge_modal_none : _forge_modal_object_properties;
		g_forge.props_cursor = 0;
	}
	if (((cur_esc && !prev_esc) || (cur_bk && !prev_bk)) && g_forge.active_modal != _forge_modal_none && g_forge.active_modal != _forge_modal_save_copy_as)
	{
		g_forge.active_modal = _forge_modal_none;
	}

	prev_f1 = cur_f1;
	prev_f2 = cur_f2;
	prev_f4 = cur_f4;
	prev_f5 = cur_f5;
	prev_f8 = cur_f8;
	prev_esc = cur_esc;
	prev_x = cur_x;

	if (g_forge.active_modal == _forge_modal_save_copy_as)
	{
		update_save_modal_input();
		prev_bk = cur_bk;
		prev_lmb = cur_lmb;
		return;
	}

	cur_up = (GetAsyncKeyState(VK_UP) & 0x8000) != 0;
	cur_down = (GetAsyncKeyState(VK_DOWN) & 0x8000) != 0;
	cur_left = (GetAsyncKeyState(VK_LEFT) & 0x8000) != 0;
	cur_right = (GetAsyncKeyState(VK_RIGHT) & 0x8000) != 0;
	cur_enter = (GetAsyncKeyState(VK_RETURN) & 0x8000) != 0;
	cur_g = (GetAsyncKeyState('G') & 0x8000) != 0;
	cur_z = (GetAsyncKeyState('Z') & 0x8000) != 0;
	cur_f = (GetAsyncKeyState('F') & 0x8000) != 0;
	cur_del = ((GetAsyncKeyState(VK_DELETE) & 0x8000) || (GetAsyncKeyState(VK_BACK) & 0x8000)) != 0;

	/* Unified Tabbed Spawn Palette Menu: [<-] / [->] switches category, [Up] / [Down] / Scroll selects item */
	if (g_forge.active_modal == _forge_modal_spawn_palette)
	{
		int filtered[512];
		int fcount;
		int const visible_rows = 10;

		g_forge.h3_menu_stage = 1;
		if (g_forge.current_category == _forge_cat_bipeds ||
			g_forge.current_category == _forge_cat_ai_squads)
		{
			g_forge.current_category = _forge_cat_weapons;
		}

		if (cur_left && !prev_left)
		{
			do {
				g_forge.current_category = (enum forge_palette_category)(
					(g_forge.current_category + NUMBER_OF_FORGE_PALETTE_CATEGORIES - 1) % NUMBER_OF_FORGE_PALETTE_CATEGORIES);
			} while (g_forge.current_category == _forge_cat_bipeds ||
					 g_forge.current_category == _forge_cat_ai_squads);
			g_forge.palette_cursor = 0;
			g_forge.palette_scroll = 0;
		}
		if (cur_right && !prev_right)
		{
			do {
				g_forge.current_category = (enum forge_palette_category)(
					(g_forge.current_category + 1) % NUMBER_OF_FORGE_PALETTE_CATEGORIES);
			} while (g_forge.current_category == _forge_cat_bipeds ||
					 g_forge.current_category == _forge_cat_ai_squads);
			g_forge.palette_cursor = 0;
			g_forge.palette_scroll = 0;
		}

		fcount = get_filtered_catalog_indices(filtered, 512);
		if (g_forge.palette_cursor >= fcount)
			g_forge.palette_cursor = (fcount > 0) ? (fcount - 1) : 0;
		if (g_forge.palette_scroll > g_forge.palette_cursor)
			g_forge.palette_scroll = g_forge.palette_cursor;

		if (cur_up && !prev_up && g_forge.palette_cursor > 0)
		{
			g_forge.palette_cursor--;
			if (g_forge.palette_cursor < g_forge.palette_scroll)
				g_forge.palette_scroll = g_forge.palette_cursor;
		}
		if (cur_down && !prev_down && g_forge.palette_cursor + 1 < fcount)
		{
			g_forge.palette_cursor++;
			if (g_forge.palette_cursor >= g_forge.palette_scroll + visible_rows)
				g_forge.palette_scroll = g_forge.palette_cursor - (visible_rows - 1);
		}
		if (cur_enter && !prev_enter && fcount > 0 && g_forge.palette_cursor < fcount)
		{
			struct forge_catalog_entry const *ce = &g_forge.catalog[filtered[g_forge.palette_cursor]];
			real_vector3d cam_fwd, cam_up;
			real_point3d ray_orig;
			real_vector3d ray_vec;
			struct collision_result coll;
			unsigned long cflags = FLAG(_collision_test_structure_bit) |
								   FLAG(_collision_test_objects_bit) |
								   FLAG(_collision_test_front_facing_surfaces_bit);
			short p_idx = local_player_get_player_index(0);
			long local_unit = (p_idx != NONE && player_get(p_idx)) ? player_get(p_idx)->unit_index : NONE;
			float spawn_dist = 4.5f;
			float sx, sy, sz;
			int32_t spawned_idx = -1;

			if (g_forge.grabbed_live_index >= 0)
				forge_cancel_or_release_held_item();

			/* Perform a fresh raycast along the current camera view ray so newly spawned items
			 * stick directly to the placement cursor and camera angle! */
			euler_to_forward_up(g_forge.cam_yaw, g_forge.cam_pitch, g_forge.cam_roll, &cam_fwd, &cam_up);
			ray_orig.x = g_forge.cam_x;
			ray_orig.y = g_forge.cam_y;
			ray_orig.z = g_forge.cam_z;
			ray_vec.i = cam_fwd.i * 20.0f;
			ray_vec.j = cam_fwd.j * 20.0f;
			ray_vec.k = cam_fwd.k * 20.0f;
			memset(&coll, 0, sizeof(coll));
			if (collision_test_vector(cflags, &ray_orig, &ray_vec, local_unit, &coll))
			{
				float dx = coll.point.x - g_forge.cam_x;
				float dy = coll.point.y - g_forge.cam_y;
				float dz = coll.point.z - g_forge.cam_z;
				spawn_dist = (float)sqrt((double)(dx * dx + dy * dy + dz * dz));
				if (spawn_dist < 1.5f) spawn_dist = 1.5f;
				if (spawn_dist > 20.0f) spawn_dist = 20.0f;
				g_forge.ray_hit_valid = 1;
				g_forge.ray_hit_x = coll.point.x;
				g_forge.ray_hit_y = coll.point.y;
				g_forge.ray_hit_z = coll.point.z;
				sx = coll.point.x;
				sy = coll.point.y;
				sz = coll.point.z + 0.08f;
			}
			else
			{
				spawn_dist = 4.5f;
				sx = g_forge.cam_x + cam_fwd.i * spawn_dist;
				sy = g_forge.cam_y + cam_fwd.j * spawn_dist;
				sz = g_forge.cam_z + cam_fwd.k * spawn_dist;
			}

			g_forge.koth_group_placing = 0;

			/* Item 4: Spawning [KOTH] Hill Marker drops a 4m diameter circle made of 10 linked points! */
			if (ce->group_tag == 'flag' && strstr(ce->tag_path, "koth") != NULL)
			{
				int cand_id, j, k, used;
				int16_t hill_id = 0;
				float r_init = 2.0f * (1.0f / 3.048f); /* 2.0m radius = 4.0m diameter in Halo world units */

				for (cand_id = 0; cand_id < 16; cand_id++)
				{
					used = 0;
					for (j = 0; j < g_forge.live_instance_count; j++)
					{
						if (g_forge.live_instances[j].active &&
							g_forge.live_instances[j].group_tag == 'flag' &&
							g_forge.live_instances[j].netgame_flag_type == 8 &&
							g_forge.live_instances[j].team_designator == cand_id)
						{
							used = 1;
							break;
						}
					}
					if (!used)
					{
						hill_id = (int16_t)cand_id;
						break;
					}
				}

				g_forge.koth_group_hill_id = hill_id;
				g_forge.koth_group_radius = r_init;
				for (k = 0; k < 10; k++)
				{
					float ang = (float)k * (6.283185307f / 10.0f);
					float nx = sx + r_init * (float)cos((double)ang);
					float ny = sy + r_init * (float)sin((double)ang);
					int32_t nidx = forge_3d_spawn_object_at(
						ce->group_tag,
						ce->tag_path,
						ce->source_cache_id,
						nx, ny, sz,
						ang, 0.0f, 0.0f,
						hill_id,
						1);
					g_forge.koth_group_live_indices[k] = nidx;
					if (k == 0)
						spawned_idx = nidx;
				}
				if (spawned_idx >= 0)
					g_forge.koth_group_placing = 1;
			}
			else
			{
				spawned_idx = forge_3d_spawn_object_at(
					ce->group_tag,
					ce->tag_path,
					ce->source_cache_id,
					sx, sy, sz,
					g_forge.cam_yaw, 0.0f, 0.0f,
					0,
					1);
			}

			/* Hitting Enter confirms the previewed item and attaches it to the placement beam */
			if (!(GetAsyncKeyState(VK_SHIFT) & 0x8000))
			{
				g_forge.active_modal = _forge_modal_none;
				if (spawned_idx >= 0 && g_forge.mode == _forge_mode_3d_monitor)
				{
					g_forge.grabbed_live_index = spawned_idx;
					g_forge.grabbed_object_index = g_forge.live_instances[spawned_idx].object_index;
					g_forge.grabbed_is_new_spawn = 1;
					g_forge.grab_distance = spawn_dist;
					g_forge.grab_yaw = g_forge.cam_yaw;
					g_forge.grab_pitch = 0.0f;
					g_forge.grab_roll = 0.0f;
					if (g_forge.koth_group_placing)
					{
						forge_set_status("KOTH Hill #%d  [C+Scroll] Radius  [Z] Floor  [Click] Place",
							g_forge.koth_group_hill_id);
					}
					else
					{
						forge_set_status("'%s'  [Scroll] Dist  [Z] Floor  [R] Rot  [Click] Place",
							ce->display_name);
					}
				}
			}
			/* Prevent this same Enter press from immediately dropping the newly spawned item */
			prev_enter = 1;
			goto update_prev_keys;
		}
		goto update_prev_keys;
	}

	/* Halo 3 Object Properties Modal (Points 5, 6, 8, 13: Team for Spawns/CTF, Reference/Channel ID for Teleporters & Objectives, Spawn Gametype Mode for objects) */
	if (g_forge.active_modal == _forge_modal_object_properties && g_forge.grabbed_live_index >= 0)
	{
		struct forge_live_instance *inst = &g_forge.live_instances[g_forge.grabbed_live_index];
		int dir = (cur_left && !prev_left) ? -1 : ((cur_right && !prev_right) || (cur_enter && !prev_enter) ? 1 : 0);
		if (cur_up && !prev_up && g_forge.props_cursor > 0)
			g_forge.props_cursor--;
		if (cur_down && !prev_down && g_forge.props_cursor < 4)
			g_forge.props_cursor++;
		if (dir != 0)
		{
			if (g_forge.props_cursor == 0)
			{
				if (inst->group_tag == 'spwn')
				{
					/* Cycle Player Spawn Team: 0 = Red Team (Red), 1 = Blue Team (Blue), 2 = FFA (Yellow) */
					int cur_t = (inst->team_designator >= 0 && inst->team_designator <= 2) ? (int)inst->team_designator : 2;
					inst->team_designator = (int16_t)((cur_t + 3 + dir) % 3);
					sync_live_player_spawns_to_scenario();
				}
				else if (inst->group_tag == 'flag' &&
					(inst->netgame_flag_type == _netgame_flag_ctf_flag || inst->netgame_flag_type == _netgame_flag_ctf_vehicle))
				{
					/* Cycle CTF Flag Team: 0 = Red Team, 1 = Blue Team */
					int cur_t = (inst->team_designator == 1) ? 1 : 0;
					inst->team_designator = (int16_t)((cur_t + 2 + dir) % 2);
					sync_live_netgame_flags_to_scenario();
				}
				else if (inst->group_tag == 'flag' &&
					(inst->netgame_flag_type == _netgame_flag_race_track || inst->netgame_flag_type == _netgame_flag_race_vehicle))
				{
					/* Race Checkpoint / Vehicle Order ID (0..31) */
					inst->team_designator = (int16_t)((inst->team_designator + 32 + dir) % 32);
					sync_live_netgame_flags_to_scenario();
				}
				else
				{
					/* Reference / Teleporter Channel / KOTH Hill / Oddball ID (0..15) */
					inst->team_designator = (int16_t)((inst->team_designator + 16 + dir) % 16);
					if (!inst->is_default_scenario_object && inst->edit_entry_index >= 0 &&
						inst->edit_entry_index < (int)g_forge.edit_state.placed_object_count)
					{
						g_forge.edit_state.placed_objects[inst->edit_entry_index].team_index = inst->team_designator;
					}
					if (inst->group_tag == 'tele' || inst->group_tag == 'flag')
						sync_live_netgame_flags_to_scenario();
				}
			}
			else if (g_forge.props_cursor == 1)
			{
				/* Point 13: Toggle which gametype modes this item will default spawn on */
				inst->gametype_flags = cycle_gametype_flags(inst->gametype_flags, dir);
				if (inst->group_tag == 'spwn')
				{
					sync_live_player_spawns_to_scenario();
				}
				if (!inst->is_default_scenario_object && inst->edit_entry_index >= 0 &&
					inst->edit_entry_index < (int)g_forge.edit_state.placed_object_count)
				{
					g_forge.edit_state.placed_objects[inst->edit_entry_index].gametype_flags = inst->gametype_flags;
				}
				if (inst->is_default_scenario_object && inst->edit_entry_index >= 0 &&
					inst->edit_entry_index < (int)g_forge.edit_state.default_edit_count)
				{
					g_forge.edit_state.default_edits[inst->edit_entry_index].gametype_flags = inst->gametype_flags;
				}
			}
			else if (g_forge.props_cursor == 2)
			{
				g_forge.snap_mode = (enum forge_snap_angle_mode)((g_forge.snap_mode + NUMBER_OF_FORGE_SNAP_MODES + dir) % NUMBER_OF_FORGE_SNAP_MODES);
			}
			else if (g_forge.props_cursor == 3)
			{
				/* Place on ground */
				cur_z = 1;
			}
			else if (g_forge.props_cursor == 4 && cur_enter && !prev_enter)
			{
				forge_3d_delete_instance(g_forge.grabbed_live_index, 1);
				g_forge.grabbed_live_index = -1;
				g_forge.grabbed_object_index = NONE;
				g_forge.active_modal = _forge_modal_none;
				goto update_prev_keys;
			}
		}
	}

	if (g_forge.mode == _forge_mode_3d_monitor)
	{
		float boost = (GetAsyncKeyState(VK_SHIFT) & 0x8000) ? 2.5f : 1.0f;
		float spd = g_forge.cam_speed * boost * dt;
		float dyaw = 0.0f, dpitch = 0.0f;
		real_vector3d fwd, up, right_vec;

		/* Native Mouse Look (same as regular player look) */
		halo_linux_mouse_look(0, &dyaw, &dpitch);
		g_forge.cam_yaw += dyaw;
		g_forge.cam_pitch += dpitch;
		if (g_forge.grabbed_live_index >= 0)
			g_forge.grab_yaw += dyaw;

		/* Optional keyboard fine rotation when modal is closed */
		if (g_forge.active_modal == _forge_modal_none)
		{
			if (GetAsyncKeyState(VK_LEFT) & 0x8000)
			{
				g_forge.cam_yaw += 1.6f * dt;
				if (g_forge.grabbed_live_index >= 0)
					g_forge.grab_yaw += 1.6f * dt;
			}
			if (GetAsyncKeyState(VK_RIGHT) & 0x8000)
			{
				g_forge.cam_yaw -= 1.6f * dt;
				if (g_forge.grabbed_live_index >= 0)
					g_forge.grab_yaw -= 1.6f * dt;
			}
			if (GetAsyncKeyState(VK_UP) & 0x8000) g_forge.cam_pitch += 1.2f * dt;
			if (GetAsyncKeyState(VK_DOWN) & 0x8000) g_forge.cam_pitch -= 1.2f * dt;
		}
		if (g_forge.cam_pitch > 1.48f) g_forge.cam_pitch = 1.48f;
		if (g_forge.cam_pitch < -1.48f) g_forge.cam_pitch = -1.48f;

		euler_to_forward_up(g_forge.cam_yaw, g_forge.cam_pitch, g_forge.cam_roll, &fwd, &up);
		right_vec.i = (float)sin((double)g_forge.cam_yaw);
		right_vec.j = -(float)cos((double)g_forge.cam_yaw);
		right_vec.k = 0.0f;

		/* 6DOF Monitor Flight (WASD + Space/E up + Ctrl/Q down) with BSP collision & map bounds clamping */
		{
			float move_dx = 0.0f, move_dy = 0.0f, move_dz = 0.0f;
			if (GetAsyncKeyState('W') & 0x8000)
			{
				move_dx += fwd.i * spd;
				move_dy += fwd.j * spd;
				move_dz += fwd.k * spd;
			}
			if (GetAsyncKeyState('S') & 0x8000)
			{
				move_dx -= fwd.i * spd;
				move_dy -= fwd.j * spd;
				move_dz -= fwd.k * spd;
			}
			if (GetAsyncKeyState('D') & 0x8000)
			{
				move_dx += right_vec.i * spd;
				move_dy += right_vec.j * spd;
			}
			if (GetAsyncKeyState('A') & 0x8000)
			{
				move_dx -= right_vec.i * spd;
				move_dy -= right_vec.j * spd;
			}
			if ((GetAsyncKeyState(VK_SPACE) & 0x8000) || (GetAsyncKeyState('E') & 0x8000))
				move_dz += spd;
			if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) || (GetAsyncKeyState('Q') & 0x8000))
				move_dz -= spd;

			if (move_dx != 0.0f || move_dy != 0.0f || move_dz != 0.0f)
			{
				float move_len = (float)sqrt((double)(move_dx * move_dx + move_dy * move_dy + move_dz * move_dz));
				if (move_len > 0.0001f)
				{
					real_point3d start_pt, target_pt;
					real_vector3d move_vec, ray_vec;
					struct collision_result pill_coll, ray_coll;
					int hit_pill, hit_ray;
					float pill_rad = 0.38f;
					float pad = 0.42f;
					unsigned long pill_flags = FLAG(_collision_test_structure_bit);
					unsigned long ray_flags = FLAG(_collision_test_structure_bit) |
											  FLAG(_collision_test_front_facing_surfaces_bit) |
											  FLAG(_collision_test_back_facing_surfaces_bit);
					struct structure_bsp const *bsp = global_structure_bsp_get();

					start_pt.x = g_forge.cam_x;
					start_pt.y = g_forge.cam_y;
					start_pt.z = g_forge.cam_z;

					move_vec.i = move_dx;
					move_vec.j = move_dy;
					move_vec.k = move_dz;

					ray_vec.i = move_dx * ((move_len + pad) / move_len);
					ray_vec.j = move_dy * ((move_len + pad) / move_len);
					ray_vec.k = move_dz * ((move_len + pad) / move_len);

					memset(&pill_coll, 0, sizeof(pill_coll));
					memset(&ray_coll, 0, sizeof(ray_coll));
					hit_pill = collision_test_pill(pill_flags, &start_pt, &move_vec, pill_rad, NONE, &pill_coll) ? 1 : 0;
					hit_ray = collision_test_vector(ray_flags, &start_pt, &ray_vec, NONE, &ray_coll) ? 1 : 0;

					if (hit_pill || hit_ray)
					{
						/* Use whichever collision hit closer; pill catches two-sided player_clip & skybox rims */
						struct collision_result coll = hit_pill ? pill_coll : ray_coll;
						float allow;
						float vn;

						if (hit_pill && hit_ray)
						{
							float pill_allow = pill_coll.t * move_len - 0.02f;
							float ray_allow = ray_coll.t * (move_len + pad) - pad;
							if (ray_allow < pill_allow)
							{
								coll = ray_coll;
								allow = ray_allow;
							}
							else
							{
								coll = pill_coll;
								allow = pill_allow;
							}
						}
						else if (hit_pill)
						{
							allow = pill_coll.t * move_len - 0.02f;
						}
						else
						{
							allow = ray_coll.t * (move_len + pad) - pad;
						}

						if (allow < 0.0f)
							allow = 0.0f;
						target_pt.x = start_pt.x + (move_dx / move_len) * allow;
						target_pt.y = start_pt.y + (move_dy / move_len) * allow;
						target_pt.z = start_pt.z + (move_dz / move_len) * allow;

						/* Attempt wall-slide with remaining movement projected onto hit plane */
						vn = move_dx * coll.plane.n.i + move_dy * coll.plane.n.j + move_dz * coll.plane.n.k;
						if (vn < 0.0f)
						{
							float slide_dx = move_dx - vn * coll.plane.n.i;
							float slide_dy = move_dy - vn * coll.plane.n.j;
							float slide_dz = move_dz - vn * coll.plane.n.k;
							float slide_len = (float)sqrt((double)(slide_dx * slide_dx + slide_dy * slide_dy + slide_dz * slide_dz));
							if (slide_len > 0.0001f)
							{
								real_vector3d slide_vec, slide_ray;
								struct collision_result slide_pill_coll, slide_ray_coll;
								slide_vec.i = slide_dx;
								slide_vec.j = slide_dy;
								slide_vec.k = slide_dz;
								slide_ray.i = slide_dx * ((slide_len + pad) / slide_len);
								slide_ray.j = slide_dy * ((slide_len + pad) / slide_len);
								slide_ray.k = slide_dz * ((slide_len + pad) / slide_len);
								memset(&slide_pill_coll, 0, sizeof(slide_pill_coll));
								memset(&slide_ray_coll, 0, sizeof(slide_ray_coll));
								if (!collision_test_pill(pill_flags, &target_pt, &slide_vec, pill_rad, NONE, &slide_pill_coll) &&
									!collision_test_vector(ray_flags, &target_pt, &slide_ray, NONE, &slide_ray_coll))
								{
									target_pt.x += slide_dx;
									target_pt.y += slide_dy;
									target_pt.z += slide_dz;
								}
							}
						}
					}
					else
					{
						target_pt.x = start_pt.x + move_dx;
						target_pt.y = start_pt.y + move_dy;
						target_pt.z = start_pt.z + move_dz;
					}

					/* Clamp strictly inside visible structure BSP world_bounds so Monitor cannot fly above skybox/player_clip ceiling */
					if (bsp)
					{
						float margin_xy = 0.60f;
						float margin_z_low = 0.60f;
						float margin_z_high = 0.85f;
						if (bsp->world_bounds.x1 - bsp->world_bounds.x0 > 2.0f)
						{
							if (target_pt.x < bsp->world_bounds.x0 + margin_xy)
								target_pt.x = bsp->world_bounds.x0 + margin_xy;
							if (target_pt.x > bsp->world_bounds.x1 - margin_xy)
								target_pt.x = bsp->world_bounds.x1 - margin_xy;
						}
						if (bsp->world_bounds.y1 - bsp->world_bounds.y0 > 2.0f)
						{
							if (target_pt.y < bsp->world_bounds.y0 + margin_xy)
								target_pt.y = bsp->world_bounds.y0 + margin_xy;
							if (target_pt.y > bsp->world_bounds.y1 - margin_xy)
								target_pt.y = bsp->world_bounds.y1 - margin_xy;
						}
						if (bsp->world_bounds.z1 - bsp->world_bounds.z0 > 2.0f)
						{
							if (target_pt.z < bsp->world_bounds.z0 + margin_z_low)
								target_pt.z = bsp->world_bounds.z0 + margin_z_low;
							if (target_pt.z > bsp->world_bounds.z1 - margin_z_high)
								target_pt.z = bsp->world_bounds.z1 - margin_z_high;
						}
					}

					/* Only commit movement if destination remains inside valid BSP leaf bounds and has unobstructed line-of-sight from start_pt */
					if (scenario_leaf_index_from_point(&target_pt) != NONE)
					{
						real_vector3d verify_vec;
						struct collision_result verify_coll;
						verify_vec.i = target_pt.x - start_pt.x;
						verify_vec.j = target_pt.y - start_pt.y;
						verify_vec.k = target_pt.z - start_pt.z;
						memset(&verify_coll, 0, sizeof(verify_coll));
						if (!collision_test_vector(ray_flags, &start_pt, &verify_vec, NONE, &verify_coll))
						{
							g_forge.cam_x = target_pt.x;
							g_forge.cam_y = target_pt.y;
							g_forge.cam_z = target_pt.z;
						}
					}
				}
			}
		}

		/* Points 1, 2 & 8: Sync local player's unit position & facing to Monitor position AND keep Invulnerable! */
		{
			long unit_idx = player_control_get_unit_index(0);
			if (unit_idx != NONE)
			{
				struct unit_datum *u = (struct unit_datum *)object_try_and_get_and_verify_type(unit_idx, _object_mask_unit);
				if (u)
				{
					real_point3d u_pos;
					u_pos.x = g_forge.cam_x;
					u_pos.y = g_forge.cam_y;
					u_pos.z = g_forge.cam_z - 0.65f;
					SET_FLAG(u->object.flags, _object_no_collisions_bit, TRUE);
					SET_FLAG(u->object.damage_flags, _object_cannot_take_damage_bit, TRUE);
					SET_FLAG(u->object.damage_flags, _object_shield_depleted_bit, FALSE);
					u->object.body_vitality = 1.0f;
					u->object.shield_vitality = 1.0f;
					forge_hide_spartan_linked_attachments_and_children(unit_idx, 1);
					object_set_automatic_deactivation(unit_idx, FALSE);
					if (scenario_leaf_index_from_point(&u_pos) != NONE)
					{
						object_set_position(unit_idx, &u_pos, &fwd, &up);
					}
					else
					{
						u->object.position = u_pos;
						u->object.forward = fwd;
						u->object.up = up;
					}
					u->unit.desired_facing_vector = fwd;
					u->unit.desired_aiming_vector = fwd;
					u->unit.aiming_vector = fwd;
					u->unit.desired_looking_vector = fwd;
					u->unit.looking_vector = fwd;
					u->object.translational_velocity.i = 0.0f;
					u->object.translational_velocity.j = 0.0f;
					u->object.translational_velocity.k = 0.0f;
					u->object.angular_velocity.i = 0.0f;
					u->object.angular_velocity.j = 0.0f;
					u->object.angular_velocity.k = 0.0f;
				}
			}
		}

		/* Point 4: Perform Physics Beam Raycast + Ray-Cone / Proximity World Object & Marker Scanner (strictly within 20m!) */
		{
			real_point3d ray_origin;
			real_vector3d ray_vec;
			struct collision_result coll;
			unsigned long cflags = FLAG(_collision_test_structure_bit) |
				FLAG(_collision_test_objects_bit) |
				_collision_test_objects_all_types_flags |
				FLAG(_collision_test_front_facing_surfaces_bit);
			long local_unit = player_control_get_unit_index(0);
			long ignore_obj = g_forge.grabbed_object_index;
			float best_obj_score = 4.5f;
			float best_marker_score = 1.6f;
			int best_marker_idx = -1;
			int direct_obj_hit = 0;

			if (!forge_valid_object_get(ignore_obj))
				ignore_obj = local_unit;
			if (!forge_valid_object_get(ignore_obj))
				ignore_obj = NONE;

			ray_origin.x = g_forge.cam_x;
			ray_origin.y = g_forge.cam_y;
			ray_origin.z = g_forge.cam_z;
			/* Point 4: Limit interaction raycast strictly to 20m */
			ray_vec.i = fwd.i * 20.0f;
			ray_vec.j = fwd.j * 20.0f;
			ray_vec.k = fwd.k * 20.0f;

			g_forge.hovered_live_index = -1;
			memset(&coll, 0, sizeof(coll));
			if (collision_test_vector(cflags, &ray_origin, &ray_vec, ignore_obj, &coll))
			{
				g_forge.ray_hit_valid = 1;
				g_forge.ray_hit_x = coll.point.x;
				g_forge.ray_hit_y = coll.point.y;
				g_forge.ray_hit_z = coll.point.z;
				g_forge.hovered_object_index = (coll.type == _collision_result_object) ? coll.object_index : NONE;
				if (g_forge.hovered_object_index == local_unit)
					g_forge.hovered_object_index = NONE;
				if (g_forge.hovered_object_index != NONE)
				{
					direct_obj_hit = 1;
					best_obj_score = 0.0f;
				}
			}
			else
			{
				g_forge.ray_hit_valid = 1;
				g_forge.ray_hit_x = g_forge.cam_x + fwd.i * 8.0f;
				g_forge.ray_hit_y = g_forge.cam_y + fwd.j * 8.0f;
				g_forge.ray_hit_z = g_forge.cam_z + fwd.k * 8.0f;
				g_forge.hovered_object_index = NONE;
			}

			/* Secondary ray-cone & ray-endpoint scan over all map objects within 20m so weapons, equipment & scenery are selectable! */
			if (g_forge.hovered_object_index == NONE)
			{
				struct object_iterator iter;
				struct object_datum *obj;
				unsigned long edit_mask = _object_mask_scenery | _object_mask_vehicle |
					_object_mask_weapon | _object_mask_equipment | _object_mask_biped | _object_mask_device;
				long best_obj = NONE;

				object_iterator_new(&iter, edit_mask, 0);
				while ((obj = object_iterator_next(&iter)) != NULL)
				{
					long oidx = iter.index;
					struct unit_datum *u;
					float vx, vy, vz, cam_d2, dist_along, px, py, pz, perp_d2, hit_dx, hit_dy, hit_dz, hit_d2;

					if (oidx == local_unit || oidx == g_forge.grabbed_object_index)
						continue;
					if (obj->object.parent_object_index != NONE)
						continue;
					u = (struct unit_datum *)object_try_and_get_and_verify_type(oidx, _object_mask_biped);
					if (u && u->unit.player_index != NONE)
						continue;

					vx = obj->object.position.x - g_forge.cam_x;
					vy = obj->object.position.y - g_forge.cam_y;
					vz = obj->object.position.z - g_forge.cam_z;
					cam_d2 = vx * vx + vy * vy + vz * vz;
					/* Point 4: Only objects within 20m (20^2 = 400.0f) can be interacted with */
					if (cam_d2 > 400.0f)
						continue;

					dist_along = vx * fwd.i + vy * fwd.j + vz * fwd.k;
					if (dist_along > 0.6f && dist_along <= 20.0f)
					{
						px = vx - fwd.i * dist_along;
						py = vy - fwd.j * dist_along;
						pz = vz - fwd.k * dist_along;
						perp_d2 = px * px + py * py + pz * pz;
						if (perp_d2 < 1.8f && perp_d2 < best_obj_score)
						{
							best_obj_score = perp_d2;
							best_obj = oidx;
						}
					}

					hit_dx = obj->object.position.x - g_forge.ray_hit_x;
					hit_dy = obj->object.position.y - g_forge.ray_hit_y;
					hit_dz = obj->object.position.z - g_forge.ray_hit_z;
					hit_d2 = hit_dx * hit_dx + hit_dy * hit_dy + hit_dz * hit_dz;
					if (hit_d2 < 3.2f && hit_d2 < best_obj_score)
					{
						best_obj_score = hit_d2;
						best_obj = oidx;
					}
				}
				g_forge.hovered_object_index = best_obj;
			}

			/* Scan virtual markers (Player Spawns, CTF Flags, Oddballs, KOTH Hills, Race Nodes, Teleporters) within 20m!
			 * Compare marker distance against indirect object scan so nearby rocks/weapons never block selecting spawns/flags! */
			{
				int i;
				for (i = 0; i < g_forge.live_instance_count; i++)
				{
					struct forge_live_instance const *inst = &g_forge.live_instances[i];
					float vx, vy, vz, cam_d2, dist_along, px, py, pz, perp_d2, hit_dx, hit_dy, hit_dz, hit_d2;
					if (!inst->active || inst->object_index != NONE || i == g_forge.grabbed_live_index)
						continue;
					vx = inst->x - g_forge.cam_x;
					vy = inst->y - g_forge.cam_y;
					vz = (inst->z + 0.15f) - g_forge.cam_z;
					cam_d2 = vx * vx + vy * vy + vz * vz;
					if (cam_d2 > 400.0f)
						continue;

					dist_along = vx * fwd.i + vy * fwd.j + vz * fwd.k;
					if (dist_along > 0.5f && dist_along <= 20.0f)
					{
						px = vx - fwd.i * dist_along;
						py = vy - fwd.j * dist_along;
						pz = vz - fwd.k * dist_along;
						perp_d2 = px * px + py * py + pz * pz;
						if (perp_d2 < 1.2f && perp_d2 < best_marker_score)
						{
							best_marker_score = perp_d2;
							best_marker_idx = i;
						}
					}

					hit_dx = inst->x - g_forge.ray_hit_x;
					hit_dy = inst->y - g_forge.ray_hit_y;
					hit_dz = inst->z - g_forge.ray_hit_z;
					hit_d2 = hit_dx * hit_dx + hit_dy * hit_dy + hit_dz * hit_dz;
					if (hit_d2 < 1.2f && hit_d2 < best_marker_score)
					{
						best_marker_score = hit_d2;
						best_marker_idx = i;
					}
				}

				if (best_marker_idx >= 0 && (!direct_obj_hit || best_marker_score < 0.35f) &&
					best_marker_score <= best_obj_score + 0.25f)
				{
					g_forge.hovered_live_index = best_marker_idx;
					g_forge.hovered_object_index = NONE;
				}
			}

			/* Update hovered_object_label for HUD display (including Player Spawns, Teleporters & KOTH/Oddball/CTF/Race markers) */
			g_forge.hovered_object_label[0] = 0;
			if (g_forge.hovered_live_index >= 0 && g_forge.hovered_live_index < g_forge.live_instance_count)
			{
				struct forge_live_instance const *inst = &g_forge.live_instances[g_forge.hovered_live_index];
				if (inst->group_tag == 'spwn')
				{
					char const *tname = (inst->team_designator == 1) ? "Blue Team Spawn" :
						(inst->team_designator == 2 ? "FFA Spawn (Yellow)" : "Red Team Spawn");
					snprintf(g_forge.hovered_object_label, sizeof(g_forge.hovered_object_label), "%s", tname);
				}
				else if (inst->group_tag == 'tele')
				{
					int is_sender = (inst->netgame_flag_type == _netgame_flag_teleporter_source ||
						strstr(inst->tag_path, "sender") != NULL ||
						strstr(inst->tag_path, "origin") != NULL);
					snprintf(g_forge.hovered_object_label, sizeof(g_forge.hovered_object_label),
						"Teleporter %s (Ref #%d)", is_sender ? "In [^]" : "Out [v]", (int)inst->team_designator);
				}
				else if (inst->group_tag == 'flag')
				{
					if (inst->netgame_flag_type == _netgame_flag_ctf_flag)
						snprintf(g_forge.hovered_object_label, sizeof(g_forge.hovered_object_label),
							"%s CTF Flag Point", inst->team_designator == 1 ? "Blue" : "Red");
					else if (inst->netgame_flag_type == _netgame_flag_oddball_ball_spawn)
						snprintf(g_forge.hovered_object_label, sizeof(g_forge.hovered_object_label),
							"Oddball Spawn #%d", (int)inst->team_designator);
					else if (inst->netgame_flag_type == _netgame_flag_hill)
						snprintf(g_forge.hovered_object_label, sizeof(g_forge.hovered_object_label),
							"KOTH Hill Marker #%d", (int)inst->team_designator);
					else if (inst->netgame_flag_type == _netgame_flag_race_track)
						snprintf(g_forge.hovered_object_label, sizeof(g_forge.hovered_object_label),
							"Race Node #%d", (int)inst->team_designator);
					else
					{
						char const *slash = strrchr(inst->tag_path, '\\');
						snprintf(g_forge.hovered_object_label, sizeof(g_forge.hovered_object_label),
							"%s (#%d)", slash ? (slash + 1) : inst->tag_path, (int)inst->team_designator);
					}
				}
			}
			else if (g_forge.hovered_object_index != NONE)
			{
				struct object_datum *hobj = forge_valid_object_get(g_forge.hovered_object_index);
				if (hobj && hobj->definition_index != NONE)
				{
					float vx = hobj->object.position.x - g_forge.cam_x;
					float vy = hobj->object.position.y - g_forge.cam_y;
					float vz = hobj->object.position.z - g_forge.cam_z;
					if (vx * vx + vy * vy + vz * vz > 400.0f)
					{
						g_forge.hovered_object_index = NONE;
					}
					else
					{
						char const *tname = tag_get_name(hobj->definition_index);
						char const *slash = tname ? strrchr(tname, '\\') : NULL;
						snprintf(g_forge.hovered_object_label, sizeof(g_forge.hovered_object_label),
							"%s", slash ? (slash + 1) : (tname ? tname : "map_object"));
					}
				}
				else
				{
					g_forge.hovered_object_index = NONE;
				}
			}
		}

		/* Cycle Rotation Snap Mode (G: Off / 15 deg / 45 deg / 90 deg) */
		if (cur_g && !prev_g)
		{
			static char const *const snap_names[4] = { "Off (Free)", "15 Degrees", "45 Degrees", "90 Degrees" };
			g_forge.snap_mode = (enum forge_snap_angle_mode)((g_forge.snap_mode + 1) % NUMBER_OF_FORGE_SNAP_MODES);
			forge_set_status("Rotation Snapping: %s", snap_names[g_forge.snap_mode]);
		}

		/* Grab / Release / Confirm Placement of Object on Physics Beam (Left Click or Enter when holding) */
		if ((g_forge.active_modal == _forge_modal_none && cur_lmb && !prev_lmb) ||
			(g_forge.active_modal == _forge_modal_none && g_forge.grabbed_live_index >= 0 && cur_enter && !prev_enter))
		{
			if (g_forge.grabbed_live_index >= 0)
			{
				struct forge_live_instance *released = &g_forge.live_instances[g_forge.grabbed_live_index];
				struct object_datum *robj = forge_valid_object_get(released->object_index);
				/* Point 12: Apply game physics to vehicles & dynamic objects immediately upon releasing in Forge Mode */
				if (robj)
				{
					SET_FLAG(robj->object.flags, _object_no_collisions_bit, FALSE);
					SET_FLAG(robj->object.flags, _object_at_rest_bit, FALSE);
					SET_FLAG(robj->object.flags, _object_outside_of_map_bit, FALSE);
					object_set_automatic_deactivation(released->object_index, FALSE);
					object_activate(released->object_index);
					if (robj->object.type == _object_type_vehicle || robj->object.type == _object_type_weapon || robj->object.type == _object_type_equipment)
					{
						/* Give a tiny downward velocity nudge so Halo's havok/vehicle physics solver wakes up and settles onto terrain */
						robj->object.translational_velocity.i = 0.0f;
						robj->object.translational_velocity.j = 0.0f;
						robj->object.translational_velocity.k = -0.02f;
					}
				}
				if (g_forge.koth_group_placing)
				{
					forge_set_status("Placed 10-Node KOTH Hill #%d! Grab individual nodes to fine-tune hill shape.",
						g_forge.koth_group_hill_id);
					g_forge.koth_group_placing = 0;
				}
				else
				{
					forge_set_status("Confirmed Placement: '%s'", released->tag_path);
				}
				g_forge.grabbed_live_index = -1;
				g_forge.grabbed_object_index = NONE;
				g_forge.grabbed_is_new_spawn = 0;
			}
			else
			{
				int i, best = -1;
				float best_d2 = 25.0f;

				/* 1. If aiming directly at a virtual marker (Player Spawn, CTF Flag, Oddball, KOTH Hill, Race Node, Teleporter), grab it! */
				if (g_forge.hovered_live_index >= 0 &&
					g_forge.hovered_live_index < g_forge.live_instance_count &&
					g_forge.live_instances[g_forge.hovered_live_index].active)
				{
					best = g_forge.hovered_live_index;
				}

				/* 2. If aiming at any world object within 20m (Forge-placed OR Default Map Scenario object), adopt/select it! */
				if (best < 0 && g_forge.hovered_object_index != NONE)
				{
					best = adopt_default_scenario_object(g_forge.hovered_object_index);
				}

				/* 3. Check virtual markers (Player Spawns, Teleporters In/Out, KOTH Hills, Oddballs, CTF Flags, Race Nodes) within 20m along view ray */
				if (best < 0)
				{
					float best_ray_perp = 1.6f;
					for (i = 0; i < g_forge.live_instance_count; i++)
					{
						struct forge_live_instance const *inst = &g_forge.live_instances[i];
						float vx, vy, vz, dist_along, px, py, pz, perp_d2;
						if (!inst->active)
							continue;
						vx = inst->x - g_forge.cam_x;
						vy = inst->y - g_forge.cam_y;
						vz = (inst->z + 0.15f) - g_forge.cam_z;
						if (vx * vx + vy * vy + vz * vz > 400.0f)
							continue;
						dist_along = vx * fwd.i + vy * fwd.j + vz * fwd.k;
						if (dist_along > 0.5f && dist_along <= 20.0f)
						{
							px = vx - fwd.i * dist_along;
							py = vy - fwd.j * dist_along;
							pz = vz - fwd.k * dist_along;
							perp_d2 = px * px + py * py + pz * pz;
							if (perp_d2 < best_ray_perp)
							{
								best_ray_perp = perp_d2;
								best = i;
							}
						}
					}
				}

				/* 4. Fallback: check existing live_instances near ray_hit within 20m of camera */
				if (best < 0)
				{
					for (i = 0; i < g_forge.live_instance_count; i++)
					{
						float cx_d, cy_d, cz_d, dx, dy, dz, d2;
						if (!g_forge.live_instances[i].active)
							continue;
						cx_d = g_forge.live_instances[i].x - g_forge.cam_x;
						cy_d = g_forge.live_instances[i].y - g_forge.cam_y;
						cz_d = g_forge.live_instances[i].z - g_forge.cam_z;
						if (cx_d * cx_d + cy_d * cy_d + cz_d * cz_d > 400.0f)
							continue;
						dx = g_forge.live_instances[i].x - g_forge.ray_hit_x;
						dy = g_forge.live_instances[i].y - g_forge.ray_hit_y;
						dz = g_forge.live_instances[i].z - g_forge.ray_hit_z;
						d2 = dx * dx + dy * dy + dz * dz;
						if (d2 < best_d2)
						{
							best_d2 = d2;
							best = i;
						}
					}
				}

				if (best >= 0)
				{
					struct forge_live_instance *inst = &g_forge.live_instances[best];
					struct object_datum *obj = forge_valid_object_get(inst->object_index);
					float dx, dy, dz;
					if (obj)
					{
						inst->x = obj->object.position.x;
						inst->y = obj->object.position.y;
						inst->z = obj->object.position.z;
						forward_up_to_euler(&obj->object.forward, &obj->object.up, &inst->yaw, &inst->pitch, &inst->roll);
					}
					dx = inst->x - g_forge.cam_x;
					dy = inst->y - g_forge.cam_y;
					dz = inst->z - g_forge.cam_z;
					g_forge.koth_group_placing = 0;
					g_forge.grabbed_live_index = best;
					g_forge.grabbed_object_index = inst->object_index;
					g_forge.grabbed_is_new_spawn = 0;
					g_forge.grab_orig_x = inst->x;
					g_forge.grab_orig_y = inst->y;
					g_forge.grab_orig_z = inst->z;
					g_forge.grab_orig_yaw = inst->yaw;
					g_forge.grab_orig_pitch = inst->pitch;
					g_forge.grab_orig_roll = inst->roll;
					g_forge.grab_distance = (float)sqrt((double)(dx * dx + dy * dy + dz * dz));
					if (g_forge.grab_distance < 1.0f) g_forge.grab_distance = 1.0f;
					if (g_forge.grab_distance > 20.0f) g_forge.grab_distance = 20.0f;
					g_forge.grab_yaw = inst->yaw;
					g_forge.grab_pitch = inst->pitch;
					g_forge.grab_roll = inst->roll;
					forge_set_status("Holding '%s' [Scroll 1-30m / Hold Z+Click Floor / R Rotate / X Opts / Click Place]",
						inst->tag_path);
				}
			}
		}

		/* Update Grabbed Object Transform */
		if (g_forge.grabbed_live_index >= 0 && g_forge.grabbed_live_index < g_forge.live_instance_count)
		{
			float tx, ty, tz, syaw, spitch, sroll;
			int wheel_notches = 0;
			int radius_mod_held = ((GetAsyncKeyState('C') & 0x8000) || (GetAsyncKeyState(VK_SHIFT) & 0x8000)) != 0;

			/* Point 3 & Item 4: Scroll wheel moves held item away/towards camera, OR scales KOTH Hill radius when C or Shift is held! */
			platform_scoreboard_scroll(1, &wheel_notches, NULL);
			if (g_forge.koth_group_placing && radius_mod_held && wheel_notches != 0)
			{
				g_forge.koth_group_radius += (float)wheel_notches * (0.25f / 3.048f);
				if (g_forge.koth_group_radius < (0.4f / 3.048f)) g_forge.koth_group_radius = (0.4f / 3.048f);
				if (g_forge.koth_group_radius > (15.0f / 3.048f)) g_forge.koth_group_radius = (15.0f / 3.048f);
				forge_set_status("KOTH Hill #%d Diameter: %.1fm (Radius: %.2fm) [Hold C/Shift+Scroll / Click Place]",
					g_forge.koth_group_hill_id,
					g_forge.koth_group_radius * 2.0f * 3.048f,
					g_forge.koth_group_radius * 3.048f);
			}
			else if (wheel_notches != 0)
			{
				g_forge.grab_distance += (float)wheel_notches * 1.0f;
			}

			if (g_forge.koth_group_placing)
			{
				if ((GetAsyncKeyState(VK_OEM_PLUS) & 0x8000) || (GetAsyncKeyState(VK_OEM_6) & 0x8000))
				{
					g_forge.koth_group_radius += (1.5f / 3.048f) * dt;
					if (g_forge.koth_group_radius > (15.0f / 3.048f)) g_forge.koth_group_radius = (15.0f / 3.048f);
				}
				if ((GetAsyncKeyState(VK_OEM_MINUS) & 0x8000) || (GetAsyncKeyState(VK_OEM_4) & 0x8000))
				{
					g_forge.koth_group_radius -= (1.5f / 3.048f) * dt;
					if (g_forge.koth_group_radius < (0.4f / 3.048f)) g_forge.koth_group_radius = (0.4f / 3.048f);
				}
			}

			if (GetAsyncKeyState(VK_PRIOR) & 0x8000) g_forge.grab_distance += 8.0f * dt;
			if (GetAsyncKeyState(VK_NEXT) & 0x8000) g_forge.grab_distance -= 8.0f * dt;
			if (g_forge.grab_distance < 1.0f) g_forge.grab_distance = 1.0f;
			if (g_forge.grab_distance > 30.0f) g_forge.grab_distance = 30.0f;

			if (GetAsyncKeyState('R') & 0x8000)
				g_forge.grab_yaw += 1.6f * dt;

			/* Keep the placement cursor and held object locked to the camera crosshair ray
			 * (and clamped against any closer BSP/surface hit along the beam) */
			{
				real_point3d beam_orig;
				real_vector3d beam_vec;
				struct collision_result bcoll;
				long ignore_grab = forge_valid_object_get(g_forge.grabbed_object_index) ? g_forge.grabbed_object_index : NONE;
				beam_orig.x = g_forge.cam_x;
				beam_orig.y = g_forge.cam_y;
				beam_orig.z = g_forge.cam_z;
				beam_vec.i = fwd.i * g_forge.grab_distance;
				beam_vec.j = fwd.j * g_forge.grab_distance;
				beam_vec.k = fwd.k * g_forge.grab_distance;
				memset(&bcoll, 0, sizeof(bcoll));
				if (collision_test_vector(
					FLAG(_collision_test_structure_bit) | FLAG(_collision_test_front_facing_surfaces_bit),
					&beam_orig, &beam_vec, ignore_grab, &bcoll))
				{
					tx = bcoll.point.x + bcoll.plane.n.i * 0.05f;
					ty = bcoll.point.y + bcoll.plane.n.j * 0.05f;
					tz = bcoll.point.z + bcoll.plane.n.k * 0.05f;
				}
				else
				{
					tx = g_forge.cam_x + fwd.i * g_forge.grab_distance;
					ty = g_forge.cam_y + fwd.j * g_forge.grab_distance;
					tz = g_forge.cam_z + fwd.k * g_forge.grab_distance;
				}
				g_forge.ray_hit_valid = 1;
				g_forge.ray_hit_x = tx;
				g_forge.ray_hit_y = ty;
				g_forge.ray_hit_z = tz;
			}

			/* Hold Z to continuously snap held object to floor, then Click (or Enter) to place on ground! */
			if (cur_z)
			{
				real_point3d down_orig;
				real_vector3d down_vec;
				struct collision_result gcoll;
				long ignore_grab = forge_valid_object_get(g_forge.grabbed_object_index) ? g_forge.grabbed_object_index : NONE;
				down_orig.x = tx;
				down_orig.y = ty;
				down_orig.z = tz + 2.0f;
				down_vec.i = 0.0f;
				down_vec.j = 0.0f;
				down_vec.k = -200.0f;
				memset(&gcoll, 0, sizeof(gcoll));
				if (collision_test_vector(
					FLAG(_collision_test_structure_bit) | FLAG(_collision_test_front_facing_surfaces_bit),
					&down_orig, &down_vec, ignore_grab, &gcoll))
				{
					tz = gcoll.point.z + 0.08f;
					if (!prev_z)
						forge_set_status("Snapping to Floor (Z=%.2f) [Hold Z + Click to Place on Ground]", tz);
				}
			}

			/* Hold/Press F to Snap to Surface (snaps position and aligns object Up vector to the surface normal) */
			if (cur_f)
			{
				real_point3d surf_orig;
				real_vector3d surf_vec;
				struct collision_result scoll;
				long ignore_grab = forge_valid_object_get(g_forge.grabbed_object_index) ? g_forge.grabbed_object_index : NONE;
				surf_orig.x = g_forge.cam_x;
				surf_orig.y = g_forge.cam_y;
				surf_orig.z = g_forge.cam_z;
				surf_vec.i = fwd.i * 30.0f;
				surf_vec.j = fwd.j * 30.0f;
				surf_vec.k = fwd.k * 30.0f;
				memset(&scoll, 0, sizeof(scoll));
				if (collision_test_vector(
					FLAG(_collision_test_structure_bit) | FLAG(_collision_test_objects_bit) | FLAG(_collision_test_front_facing_surfaces_bit),
					&surf_orig, &surf_vec, ignore_grab, &scoll))
				{
					real_vector3d surf_up = scoll.plane.n;
					real_vector3d ref_fwd, surf_fwd;
					float dot_fu, flen;
					tx = scoll.point.x + surf_up.i * 0.05f;
					ty = scoll.point.y + surf_up.j * 0.05f;
					tz = scoll.point.z + surf_up.k * 0.05f;
					g_forge.ray_hit_x = tx;
					g_forge.ray_hit_y = ty;
					g_forge.ray_hit_z = tz;
					ref_fwd.i = (float)cos((double)g_forge.grab_yaw);
					ref_fwd.j = (float)sin((double)g_forge.grab_yaw);
					ref_fwd.k = 0.0f;
					dot_fu = ref_fwd.i * surf_up.i + ref_fwd.j * surf_up.j + ref_fwd.k * surf_up.k;
					surf_fwd.i = ref_fwd.i - surf_up.i * dot_fu;
					surf_fwd.j = ref_fwd.j - surf_up.j * dot_fu;
					surf_fwd.k = ref_fwd.k - surf_up.k * dot_fu;
					flen = (float)sqrt((double)(surf_fwd.i * surf_fwd.i + surf_fwd.j * surf_fwd.j + surf_fwd.k * surf_fwd.k));
					if (flen < 0.001f)
					{
						dot_fu = fwd.i * surf_up.i + fwd.j * surf_up.j + fwd.k * surf_up.k;
						surf_fwd.i = fwd.i - surf_up.i * dot_fu;
						surf_fwd.j = fwd.j - surf_up.j * dot_fu;
						surf_fwd.k = fwd.k - surf_up.k * dot_fu;
						flen = (float)sqrt((double)(surf_fwd.i * surf_fwd.i + surf_fwd.j * surf_fwd.j + surf_fwd.k * surf_fwd.k));
					}
					if (flen < 0.001f)
					{
						surf_fwd.i = 0.0f;
						surf_fwd.j = 0.0f;
						surf_fwd.k = (surf_up.i > 0.0f) ? -1.0f : 1.0f;
						dot_fu = surf_fwd.i * surf_up.i + surf_fwd.j * surf_up.j + surf_fwd.k * surf_up.k;
						surf_fwd.i -= surf_up.i * dot_fu;
						surf_fwd.j -= surf_up.j * dot_fu;
						surf_fwd.k -= surf_up.k * dot_fu;
						flen = (float)sqrt((double)(surf_fwd.i * surf_fwd.i + surf_fwd.j * surf_fwd.j + surf_fwd.k * surf_fwd.k));
					}
					if (flen > 0.0001f)
					{
						surf_fwd.i /= flen;
						surf_fwd.j /= flen;
						surf_fwd.k /= flen;
						forward_up_to_euler(&surf_fwd, &surf_up, &g_forge.grab_yaw, &g_forge.grab_pitch, &g_forge.grab_roll);
					}
					if (!prev_f)
						forge_set_status("Snapped to Surface (Normal=%.2f,%.2f,%.2f) [Hold F + Click to Place]",
							surf_up.i, surf_up.j, surf_up.k);
				}
			}

			syaw = snap_angle_rad(g_forge.grab_yaw, g_forge.snap_mode);
			spitch = snap_angle_rad(g_forge.grab_pitch, g_forge.snap_mode);
			sroll = snap_angle_rad(g_forge.grab_roll, g_forge.snap_mode);

			if (g_forge.koth_group_placing)
			{
				int k;
				for (k = 0; k < 10; k++)
				{
					int32_t nidx = g_forge.koth_group_live_indices[k];
					float ang = syaw + (float)k * (6.283185307f / 10.0f);
					float nx = tx + g_forge.koth_group_radius * (float)cos((double)ang);
					float ny = ty + g_forge.koth_group_radius * (float)sin((double)ang);
					float nz = tz;
					if (cur_z)
					{
						real_point3d n_orig;
						real_vector3d n_vec;
						struct collision_result ncoll;
						n_orig.x = nx;
						n_orig.y = ny;
						n_orig.z = tz + 2.0f;
						n_vec.i = 0.0f;
						n_vec.j = 0.0f;
						n_vec.k = -200.0f;
						memset(&ncoll, 0, sizeof(ncoll));
						if (collision_test_vector(
							FLAG(_collision_test_structure_bit) | FLAG(_collision_test_front_facing_surfaces_bit),
							&n_orig, &n_vec, NONE, &ncoll))
						{
							nz = ncoll.point.z + 0.04f;
						}
					}
					if (nidx >= 0 && nidx < g_forge.live_instance_count)
						forge_3d_move_instance(nidx, nx, ny, nz, ang, 0.0f, 0.0f, 1);
				}
			}
			else
			{
				forge_3d_move_instance(g_forge.grabbed_live_index, tx, ty, tz, syaw, spitch, sroll, 1);
			}
		}
		else
		{
			platform_scoreboard_scroll(0, NULL, NULL);
		}

		/* Delete Grabbed Object */
		if (cur_del && !prev_del && g_forge.grabbed_live_index >= 0)
		{
			if (g_forge.koth_group_placing)
			{
				int k;
				for (k = 0; k < 10; k++)
				{
					int32_t nidx = g_forge.koth_group_live_indices[k];
					if (nidx >= 0 && nidx < g_forge.live_instance_count)
						forge_3d_delete_instance(nidx, 1);
				}
				g_forge.koth_group_placing = 0;
			}
			else
			{
				forge_3d_delete_instance(g_forge.grabbed_live_index, 1);
			}
			g_forge.grabbed_live_index = -1;
			g_forge.grabbed_object_index = NONE;
			g_forge.grabbed_is_new_spawn = 0;
		}
	}
	else
	{
		platform_scoreboard_scroll(0, NULL, NULL);
	}

update_prev_keys:
	prev_up = cur_up;
	prev_down = cur_down;
	prev_left = cur_left;
	prev_right = cur_right;
	prev_enter = cur_enter;
	prev_g = cur_g;
	prev_z = cur_z;
	prev_f = cur_f;
	prev_del = cur_del;
	prev_bk = cur_bk;
	prev_lmb = cur_lmb;
}

void forge_3d_apply_monitor_camera(short local_player_index)
{
	struct observer_command cmd;

	if (!g_forge_session_enabled || local_player_index != 0)
		return;
	if (main_get_map_name() && strcmp(main_get_map_name(), "levels\\ui\\ui") == 0)
		return;
	if (g_forge.mode == _forge_mode_play && g_forge.active_modal == _forge_modal_none)
		return;

	director_inhibit_input(0);
	director_inhibit_facing(0);

	if (g_forge.mode == _forge_mode_3d_monitor)
	{
		memset(&cmd, 0, sizeof(cmd));
		cmd.flags = FLAG(_observer_command_valid_bit) | FLAG(_observer_command_ignore_obstructions_bit);
		euler_to_forward_up(g_forge.cam_yaw, g_forge.cam_pitch, g_forge.cam_roll, &cmd.forward, &cmd.up);
		if (g_forge.third_person_view)
		{
			/* Pull camera back 2.4 world units behind Monitor, clamped against BSP/skybox walls */
			real_point3d mon_pt, cam_pt;
			real_vector3d pullback_vec;
			struct collision_result cam_coll;
			unsigned long cflags = FLAG(_collision_test_structure_bit) |
								   FLAG(_collision_test_front_facing_surfaces_bit) |
								   FLAG(_collision_test_back_facing_surfaces_bit);
			struct structure_bsp const *bsp = global_structure_bsp_get();

			mon_pt.x = g_forge.cam_x;
			mon_pt.y = g_forge.cam_y;
			mon_pt.z = g_forge.cam_z;

			pullback_vec.i = -cmd.forward.i * 2.4f;
			pullback_vec.j = -cmd.forward.j * 2.4f;
			pullback_vec.k = -cmd.forward.k * 2.4f + 0.25f;

			memset(&cam_coll, 0, sizeof(cam_coll));
			if (collision_test_pill(FLAG(_collision_test_structure_bit), &mon_pt, &pullback_vec, 0.22f, NONE, &cam_coll) ||
				collision_test_vector(cflags, &mon_pt, &pullback_vec, NONE, &cam_coll))
			{
				float frac = cam_coll.t - 0.12f;
				if (frac < 0.0f)
					frac = 0.0f;
				cam_pt.x = mon_pt.x + pullback_vec.i * frac;
				cam_pt.y = mon_pt.y + pullback_vec.j * frac;
				cam_pt.z = mon_pt.z + pullback_vec.k * frac;
			}
			else
			{
				cam_pt.x = mon_pt.x + pullback_vec.i;
				cam_pt.y = mon_pt.y + pullback_vec.j;
				cam_pt.z = mon_pt.z + pullback_vec.k;
			}

			if (bsp)
			{
				if (bsp->world_bounds.x1 - bsp->world_bounds.x0 > 2.0f)
				{
					if (cam_pt.x < bsp->world_bounds.x0 + 0.35f) cam_pt.x = bsp->world_bounds.x0 + 0.35f;
					if (cam_pt.x > bsp->world_bounds.x1 - 0.35f) cam_pt.x = bsp->world_bounds.x1 - 0.35f;
				}
				if (bsp->world_bounds.y1 - bsp->world_bounds.y0 > 2.0f)
				{
					if (cam_pt.y < bsp->world_bounds.y0 + 0.35f) cam_pt.y = bsp->world_bounds.y0 + 0.35f;
					if (cam_pt.y > bsp->world_bounds.y1 - 0.35f) cam_pt.y = bsp->world_bounds.y1 - 0.35f;
				}
				if (bsp->world_bounds.z1 - bsp->world_bounds.z0 > 2.0f)
				{
					if (cam_pt.z < bsp->world_bounds.z0 + 0.35f) cam_pt.z = bsp->world_bounds.z0 + 0.35f;
					if (cam_pt.z > bsp->world_bounds.z1 - 0.45f) cam_pt.z = bsp->world_bounds.z1 - 0.45f;
				}
			}

			if (scenario_leaf_index_from_point(&cam_pt) == NONE)
				cam_pt = mon_pt;

			cmd.focus_position = cam_pt;
		}
		else
		{
			cmd.focus_position.x = g_forge.cam_x;
			cmd.focus_position.y = g_forge.cam_y;
			cmd.focus_position.z = g_forge.cam_z;
		}
		cmd.focus_distance = 0.0f;
		cmd.field_of_view = 1.3962634f; /* 80 degrees */
		observer_set_camera(0, &cmd);
	}
}

static void draw_screen_line_thick(
	short x0, short y0,
	short x1, short y1,
	short half_thick,
	uint32_t argb_col,
	short win_x0, short win_y0,
	short win_x1, short win_y1)
{
	int dx = (int)x1 - (int)x0;
	int dy = (int)y1 - (int)y0;
	int adx = dx < 0 ? -dx : dx;
	int ady = dy < 0 ? -dy : dy;
	int max_d = adx > ady ? adx : ady;
	int steps, s;

	if ((x0 < win_x0 && x1 < win_x0) || (x0 > win_x1 && x1 > win_x1) ||
		(y0 < win_y0 && y1 < win_y0) || (y0 > win_y1 && y1 > win_y1))
	{
		return;
	}

	if (max_d <= 2)
	{
		rectangle2d r;
		r.x0 = (short)((x0 < x1 ? x0 : x1) - half_thick);
		r.y0 = (short)((y0 < y1 ? y0 : y1) - half_thick);
		r.x1 = (short)((x0 > x1 ? x0 : x1) + half_thick + 1);
		r.y1 = (short)((y0 > y1 ? y0 : y1) + half_thick + 1);
		if (r.x0 < win_x0) r.x0 = win_x0;
		if (r.y0 < win_y0) r.y0 = win_y0;
		if (r.x1 > win_x1) r.x1 = win_x1;
		if (r.y1 > win_y1) r.y1 = win_y1;
		if (r.x0 < r.x1 && r.y0 < r.y1)
			draw_quad(&r, argb_col);
		return;
	}

	steps = max_d / 2;
	if (steps < 2) steps = 2;
	if (steps > 36) steps = 36;

	for (s = 0; s <= steps; s++)
	{
		short px = (short)(x0 + (dx * s) / steps);
		short py = (short)(y0 + (dy * s) / steps);
		if (px >= win_x0 + half_thick && px <= win_x1 - half_thick &&
			py >= win_y0 + half_thick && py <= win_y1 - half_thick)
		{
			rectangle2d r;
			r.x0 = (short)(px - half_thick);
			r.y0 = (short)(py - half_thick);
			r.x1 = (short)(px + half_thick + 1);
			r.y1 = (short)(py + half_thick + 1);
			draw_quad(&r, argb_col);
		}
	}
}

static int project_world_pt_to_window(
	float wx, float wy, float wz,
	short window_x0, short window_y0,
	short *out_sx, short *out_sy)
{
	union real_point3d wpt;
	union real_point2d spt;
	wpt.x = wx;
	wpt.y = wy;
	wpt.z = wz;
	if (!render_camera_world_to_screen(&render.camera, &render.frustum, &wpt, &spt))
		return 0;
	if (spt.x < -1500.0f || spt.x > 3500.0f || spt.y < -1500.0f || spt.y > 3500.0f)
		return 0;
	*out_sx = (short)(window_x0 + (short)(spt.x + 0.5f));
	*out_sy = (short)(window_y0 + (short)(spt.y + 0.5f));
	return 1;
}

static void draw_ground_circle_and_facing_arrow(
	float cx_w, float cy_w, float cz_w,
	float yaw_rad,
	float radius_w,
	int draw_arrow,
	int is_highlighted,
	uint32_t primary_col,
	uint32_t dark_col,
	short window_x0, short window_y0,
	short window_x1, short window_y1)
{
	enum { NUM_SEGS = 16 };
	short ring_sx[NUM_SEGS], ring_sy[NUM_SEGS];
	uint8_t ring_ok[NUM_SEGS];
	float z_g = cz_w + 0.04f;
	short half_t = is_highlighted ? 2 : 1;
	int k;

	for (k = 0; k < NUM_SEGS; k++)
	{
		float ang = (float)k * (6.283185307f / (float)NUM_SEGS);
		float wx = cx_w + radius_w * (float)cos((double)ang);
		float wy = cy_w + radius_w * (float)sin((double)ang);
		ring_ok[k] = (uint8_t)project_world_pt_to_window(wx, wy, z_g, window_x0, window_y0, &ring_sx[k], &ring_sy[k]);
	}

	/* Draw ground circle perimeter segments */
	for (k = 0; k < NUM_SEGS; k++)
	{
		int nk = (k + 1) % NUM_SEGS;
		if (ring_ok[k] && ring_ok[nk])
		{
			draw_screen_line_thick(
				ring_sx[k], ring_sy[k], ring_sx[nk], ring_sy[nk],
				(short)(half_t + 1), dark_col, window_x0, window_y0, window_x1, window_y1);
			draw_screen_line_thick(
				ring_sx[k], ring_sy[k], ring_sx[nk], ring_sy[nk],
				half_t, primary_col, window_x0, window_y0, window_x1, window_y1);
		}
	}

	/* Draw directional arrow on the ground pointing in yaw_rad */
	if (draw_arrow)
	{
		float fx = (float)cos((double)yaw_rad);
		float fy = (float)sin((double)yaw_rad);
		float rx = -fy;
		float ry = fx;
		short tail_x, tail_y, tip_x, tip_y, wl_x, wl_y, wr_x, wr_y, il_x, il_y, ir_x, ir_y;
		int ok_tail = project_world_pt_to_window(
			cx_w - fx * (radius_w * 0.58f),
			cy_w - fy * (radius_w * 0.58f),
			z_g, window_x0, window_y0, &tail_x, &tail_y);
		int ok_tip = project_world_pt_to_window(
			cx_w + fx * (radius_w * 1.38f),
			cy_w + fy * (radius_w * 1.38f),
			z_g, window_x0, window_y0, &tip_x, &tip_y);
		int ok_wl = project_world_pt_to_window(
			cx_w + fx * (radius_w * 0.68f) - rx * (radius_w * 0.44f),
			cy_w + fy * (radius_w * 0.68f) - ry * (radius_w * 0.44f),
			z_g, window_x0, window_y0, &wl_x, &wl_y);
		int ok_wr = project_world_pt_to_window(
			cx_w + fx * (radius_w * 0.68f) + rx * (radius_w * 0.44f),
			cy_w + fy * (radius_w * 0.68f) + ry * (radius_w * 0.44f),
			z_g, window_x0, window_y0, &wr_x, &wr_y);
		int ok_il = project_world_pt_to_window(
			cx_w + fx * (radius_w * 0.85f) - rx * (radius_w * 0.22f),
			cy_w + fy * (radius_w * 0.85f) - ry * (radius_w * 0.22f),
			z_g, window_x0, window_y0, &il_x, &il_y);
		int ok_ir = project_world_pt_to_window(
			cx_w + fx * (radius_w * 0.85f) + rx * (radius_w * 0.22f),
			cy_w + fy * (radius_w * 0.85f) + ry * (radius_w * 0.22f),
			z_g, window_x0, window_y0, &ir_x, &ir_y);

		if (ok_tail && ok_tip)
		{
			draw_screen_line_thick(tail_x, tail_y, tip_x, tip_y, (short)(half_t + 1), dark_col, window_x0, window_y0, window_x1, window_y1);
			draw_screen_line_thick(tail_x, tail_y, tip_x, tip_y, half_t, primary_col, window_x0, window_y0, window_x1, window_y1);
		}
		if (ok_tip && ok_wl)
		{
			draw_screen_line_thick(tip_x, tip_y, wl_x, wl_y, (short)(half_t + 1), dark_col, window_x0, window_y0, window_x1, window_y1);
			draw_screen_line_thick(tip_x, tip_y, wl_x, wl_y, half_t, primary_col, window_x0, window_y0, window_x1, window_y1);
		}
		if (ok_tip && ok_wr)
		{
			draw_screen_line_thick(tip_x, tip_y, wr_x, wr_y, (short)(half_t + 1), dark_col, window_x0, window_y0, window_x1, window_y1);
			draw_screen_line_thick(tip_x, tip_y, wr_x, wr_y, half_t, primary_col, window_x0, window_y0, window_x1, window_y1);
		}
		if (ok_wl && ok_wr)
		{
			draw_screen_line_thick(wl_x, wl_y, wr_x, wr_y, half_t, primary_col, window_x0, window_y0, window_x1, window_y1);
		}
		if (ok_tip && ok_il && ok_ir)
		{
			draw_screen_line_thick(tip_x, tip_y, il_x, il_y, half_t, primary_col, window_x0, window_y0, window_x1, window_y1);
			draw_screen_line_thick(tip_x, tip_y, ir_x, ir_y, half_t, primary_col, window_x0, window_y0, window_x1, window_y1);
		}
	}
}

static void draw_teleporter_and_objective_markers(
	short window_x0,
	short window_y0,
	short window_x1,
	short window_y1,
	long font_index)
{
	int i;

	/* Player Spawns, Teleporter Origin/Destination, Race Markers, Oddball Markers,
	 * and CTF Flags now render as true 3D translucent world models in
	 * render_world_objective_and_spawn_markers() (forge_runtime_tags.c).
	 * In 2D HUD space we only draw floating text labels while in Forge Mode.
	 * In Spartan Play mode, hide Forge-only Spawn Point and Objective markers and labels completely! */
	if (g_forge.mode != _forge_mode_3d_monitor)
		return;

	/* Only show floating marker names when the pickup reticle is over the item (or holding it) */
	for (i = 0; i < g_forge.live_instance_count; i++)
	{
		struct forge_live_instance const *inst = &g_forge.live_instances[i];
		union real_point3d wpt;
		union real_point2d spt;
		int is_hi;
		short sx, sy;

		if (!inst->active)
			continue;
		if (inst->group_tag != 'spwn' && inst->group_tag != 'tele' && inst->group_tag != 'flag')
			continue;

		is_hi = (i == g_forge.grabbed_live_index || i == g_forge.hovered_live_index);
		if (!is_hi)
			continue;

		if (inst->group_tag == 'spwn')
		{
			uint32_t ring_col;
			char const *team_lbl;

			if (inst->team_designator == 1)
			{
				ring_col = 0xFF2896FFu;
				team_lbl = "BLUE SPAWN";
			}
			else if (inst->team_designator == 0)
			{
				ring_col = 0xFFFF6868u;
				team_lbl = "RED SPAWN";
			}
			else
			{
				ring_col = 0xFFFFFFFFu;
				team_lbl = "FFA SPAWN";
			}

			wpt.x = inst->x;
			wpt.y = inst->y;
			wpt.z = inst->z + 0.14f;
			if (render_camera_world_to_screen(&render.camera, &render.frustum, &wpt, &spt))
			{
				sx = (short)(window_x0 + (short)(spt.x + 0.5f));
				sy = (short)(window_y0 + (short)(spt.y + 0.5f));
				if (sx >= window_x0 + 20 && sx <= window_x1 - 20 && sy >= window_y0 + 10 && sy <= window_y1 - 10)
				{
					set_forge_hud_ui_scale(FORGE_IN_MATCH_HUD_SCALE, (float)sx, (float)sy);
					draw_ce_menu_str_box(0, font_index, (short)(sx - 80), (short)(sy + 6), (short)(sx + 80), (short)(sy + 32), 2, team_lbl, ring_col);
				}
			}
			continue;
		}

		wpt.x = inst->x;
		wpt.y = inst->y;
		wpt.z = inst->z + 0.28f;
		if (!render_camera_world_to_screen(&render.camera, &render.frustum, &wpt, &spt))
			continue;

		sx = (short)(window_x0 + (short)(spt.x + 0.5f));
		sy = (short)(window_y0 + (short)(spt.y + 0.5f));
		if (sx < window_x0 + 10 || sx > window_x1 - 10 || sy < window_y0 + 10 || sy > window_y1 - 10)
			continue;

		set_forge_hud_ui_scale(FORGE_IN_MATCH_HUD_SCALE, (float)sx, (float)sy);
		if (inst->group_tag == 'tele')
		{
			int is_sender = (inst->netgame_flag_type == _netgame_flag_teleporter_source ||
				strstr(inst->tag_path, "sender") != NULL ||
				strstr(inst->tag_path, "origin") != NULL);
			char lbl[64];

			snprintf(lbl, sizeof(lbl), "%s #%d", is_sender ? "TELE ORIGIN" : "TELE DEST", (int)inst->team_designator);
			draw_ce_menu_str_box(0, font_index, (short)(sx - 90), (short)(sy + 8), (short)(sx + 90), (short)(sy + 34), 2, lbl, 0xFFFFFFFFu);
		}
		else if (inst->group_tag == 'flag')
		{
			char lbl[64];
			char const *kind = "OBJECTIVE";
			uint32_t border_col = 0xFFFFFFFFu;

			if (inst->netgame_flag_type == _netgame_flag_hill || strstr(inst->tag_path, "koth"))
			{
				kind = "KOTH HILL";
				border_col = 0xFFFFFFFFu;
			}
			else if (inst->netgame_flag_type == _netgame_flag_oddball_ball_spawn || strstr(inst->tag_path, "oddball"))
			{
				kind = "ODDBALL";
				border_col = 0xFFFFFFFFu;
			}
			else if (inst->netgame_flag_type == _netgame_flag_ctf_flag ||
					 inst->netgame_flag_type == _netgame_flag_ctf_vehicle ||
					 strstr(inst->tag_path, "ctf"))
			{
				kind = (inst->team_designator == 1) ? "BLUE FLAG" : "RED FLAG";
				border_col = (inst->team_designator == 1) ? 0xFF2896FFu : 0xFFFF6868u;
			}
			else if (inst->netgame_flag_type == _netgame_flag_race_track ||
					 inst->netgame_flag_type == _netgame_flag_race_vehicle ||
					 strstr(inst->tag_path, "race"))
			{
				kind = (inst->netgame_flag_type == _netgame_flag_race_vehicle) ? "RACE VEH" : "RACE MARKER";
				border_col = 0xFFFFFFFFu;
			}

			snprintf(lbl, sizeof(lbl), "%s #%d", kind, (int)inst->team_designator);
			draw_ce_menu_str_box(0, font_index, (short)(sx - 90), (short)(sy + 8), (short)(sx + 90), (short)(sy + 34), 2, lbl, border_col);
		}
	}
	set_forge_hud_ui_scale(1.0f, 0.0f, 0.0f);
}

void forge_3d_draw_hud(
	short local_player_index,
	short window_x0,
	short window_y0,
	short window_x1,
	short window_y1,
	long font_index,
	short line_height)
{
	static char const *const cat_names[NUMBER_OF_FORGE_PALETTE_CATEGORIES] = {
		"WEAPONS",
		"EQUIPMENT",
		"VEHICLES",
		"BIPEDS",
		"SCENERY",
		"DOORS",
		"SPAWNS",
		"OBJECTIVES",
		"AI SQUADS"
	};
	static char const *const snap_names[4] = { "OFF", "15 DEG", "45 DEG", "90 DEG" };
	char buf[256];
	short w = (short)(window_x1 - window_x0);
	short h = (short)(window_y1 - window_y0);

	(void)line_height;

	/* Never draw any Forge overlay on the main menu (levels\ui\ui) or when not in a Forge session */
	if (!g_forge_session_enabled || local_player_index != 0 || w <= 0 || h <= 0)
		return;
	if (main_get_map_name() && strcmp(main_get_map_name(), "levels\\ui\\ui") == 0)
		return;
	if (ui_widgets_active())
		return;

	/* Ensure customizable UI theme & PNG backgrounds from OpenCE Forge/ui/ are loaded */
	forge_ui_theme_ensure_loaded();

	/* Draw 3D-projected Player Spawns & Teleporter / Objective floating labels */
	draw_teleporter_and_objective_markers(window_x0, window_y0, window_x1, window_y1, font_index);

	/* Spartan Play Mode: Bottom bar styled with base game menu row bitmap & ui\small_ui font */
	if (g_forge.mode == _forge_mode_play && g_forge.active_modal == _forge_modal_none)
	{
		short bbar_w = g_forge.has_saved_map_once ? 236 : 132;
		short bx0 = (short)(window_x0 + 118);
		short by0 = (short)(window_y1 - 26);
		short bx1 = (short)(bx0 + bbar_w);
		short by1 = (short)(window_y1 - 2);

		set_forge_hud_ui_scale(FORGE_IN_MATCH_HUD_SCALE, (float)bx0, (float)by1);

		if (!draw_ce_menu_bitmap_cropped("pc\\bitmaps\\option_bkds_sm", 0, bx0, by0, bx1, by1, 256, 28, 0xFFFFFFFFu))
		{
			rectangle2d bbar = { by0, bx0, by1, bx1 };
			draw_themed_panel(_forge_ui_area_bottom_bar, &bbar, 0);
		}
		if (g_forge.has_saved_map_once)
			snprintf(buf, sizeof(buf), "[F1] FORGE    [F5] QUICK SAVE");
		else
			snprintf(buf, sizeof(buf), "[F1] FORGE");
		draw_ce_menu_str_box(0, font_index, bx0, (short)(by0 + 3), bx1, by1, 2, buf, 0xFF2896FFu);
	}
	else if (g_forge.mode == _forge_mode_3d_monitor)
	{
		/* Clean Unarmed Monitor Reticle at screen center */
		short cx = (short)((window_x0 + window_x1) / 2);
		short cy = (short)((window_y0 + window_y1) / 2);
		uint32_t ret_col = (g_forge.grabbed_live_index >= 0 || g_forge.hovered_live_index >= 0 || g_forge.hovered_object_index != NONE || g_forge.hovered_object_label[0])
			? 0xFFFFFFFFu : 0xFF2896FFu;
		rectangle2d rt;
		short px0, py0, px1, py1;

		set_forge_hud_ui_scale(FORGE_IN_MATCH_HUD_SCALE, (float)cx, (float)cy);

		/* Center dot */
		rt.x0 = (short)(cx - 1); rt.y0 = (short)(cy - 1); rt.x1 = (short)(cx + 2); rt.y1 = (short)(cy + 2);
		draw_quad(&rt, ret_col);
		/* Four unarmed bracket ticks around center */
		rt.x0 = (short)(cx - 11); rt.y0 = (short)(cy - 1); rt.x1 = (short)(cx - 5); rt.y1 = (short)(cy + 2);
		draw_quad(&rt, ret_col);
		rt.x0 = (short)(cx + 6); rt.y0 = (short)(cy - 1); rt.x1 = (short)(cx + 12); rt.y1 = (short)(cy + 2);
		draw_quad(&rt, ret_col);
		rt.x0 = (short)(cx - 1); rt.y0 = (short)(cy - 11); rt.x1 = (short)(cx + 2); rt.y1 = (short)(cy - 5);
		draw_quad(&rt, ret_col);
		rt.x0 = (short)(cx - 1); rt.y0 = (short)(cy + 6); rt.x1 = (short)(cx + 2); rt.y1 = (short)(cy + 12);
		draw_quad(&rt, ret_col);

		/* Show hovered object/marker label right below reticle in base-game UI font */
		if (g_forge.grabbed_live_index < 0 && g_forge.hovered_object_label[0])
		{
			char upper_lbl[96];
			format_clean_uppercase_item_name(g_forge.hovered_object_label, upper_lbl, sizeof(upper_lbl));
			snprintf(buf, sizeof(buf), "[CLICK] %s", upper_lbl);
			draw_ce_menu_str_box(0, font_index, (short)(cx - 160), (short)(cy + 16), (short)(cx + 160), (short)(cy + 42), 2, buf, 0xFFFFFFFFu);
		}

		/* Top-Right Forge Mode Status & Budget Box (positioned right below the health/shield HUD,
		 * scaled down an additional 50% and styled with 9-slice rounded Halo CE box & UI fonts) */
		px0 = (short)(window_x1 - 236);
		py0 = (short)(window_y0 + 52);
		px1 = (short)(window_x1 - 12);
		py1 = (short)(py0 + 54);
		set_forge_hud_ui_scale(FORGE_COUNTER_HUD_SCALE, (float)px1, (float)py0);
		draw_ce_pause_box(px0, py0, (short)(px1 - px0), (short)(py1 - py0), 0);

		draw_ce_menu_str(1, font_index, (short)(px0 + 14), (short)(py0 + 6), "FORGE", 0xFFFFFFFFu);
		snprintf(buf, sizeof(buf), "%d / %d",
			g_forge.live_instance_count, FORGE_MAX_LIVE_INSTANCES);
		draw_ce_menu_str_box(1, font_index, (short)(px0 + 84), (short)(py0 + 6), (short)(px1 - 14), (short)(py0 + 30), 1 /* right */,
			buf, 0xFFFFFFFFu);

		snprintf(buf, sizeof(buf), "[G] SNAP: %s",
			snap_names[g_forge.snap_mode]);
		draw_ce_menu_str_box(0, font_index, (short)(px0 + 14), (short)(py0 + 28), (short)(px1 - 14), (short)(py1 - 6), 0,
			buf, 0xFF2896FFu);

		/* Bottom Control Strip styled with base game option_bkds & ui\small_ui font */
		if (g_forge.active_modal == _forge_modal_none)
		{
			short bx0 = (short)(window_x0 + 118);
			short by0 = (short)(window_y1 - 26);
			short bx1 = (short)(window_x1 - 12);
			short by1 = (short)(window_y1 - 2);

			set_forge_hud_ui_scale(FORGE_IN_MATCH_HUD_SCALE, (float)(bx0 + bx1) * 0.5f, (float)by1);

			if (!draw_ce_menu_bitmap_cropped("pc\\bitmaps\\option_bkds", 0, bx0, by0, bx1, by1, 512, 28, 0xFFFFFFFFu))
			{
				rectangle2d bbar = { by0, bx0, by1, bx1 };
				draw_themed_panel(_forge_ui_area_bottom_bar, &bbar, 0);
			}

			if (g_forge.grabbed_live_index >= 0)
			{
				snprintf(buf, sizeof(buf),
					"[CLICK] PLACE   [SCROLL] %.0fM   [Z] FLOOR   [F] SURFACE   [R] ROTATE   [X] OPTIONS   [DEL] DELETE",
					g_forge.grab_distance);
			}
			else
			{
				if (g_forge.has_saved_map_once)
				{
					snprintf(buf, sizeof(buf),
						"[F2] OBJECTS   [CLICK] GRAB   [F4] VIEW   [F5] QUICK SAVE   [F1] PLAY");
				}
				else
				{
					snprintf(buf, sizeof(buf),
						"[F2] OBJECTS   [CLICK] GRAB   [F4] VIEW   [ESC] SAVE MAP..   [F1] PLAY");
				}
			}
			draw_ce_menu_str_box(0, font_index, bx0, (short)(by0 + 2), bx1, by1, 2, buf, 0xFF2896FFu);
		}
	}

	/* ========================================================================
	 * UNIFIED TABBED FORGE OBJECT PALETTE MENU (F2) - BASE GAME MENU STYLING
	 * Uses authentic Halo CE 9-slice objectives container, option_bkds_sm spinner
	 * with arrow_sm_left/right, rounded row highlight pill, text_button_background,
	 * and ui\large_ui + ui\small_ui fonts (#FF2896FF & #FFFFFFFF).
	 * ======================================================================== */
	if (g_forge.active_modal == _forge_modal_spawn_palette)
	{
		int filtered[512];
		int fcount = get_filtered_catalog_indices(filtered, 512);
		int r, max_rows = 10;
		short box_w = 296;
		short box_h = 336;
		short px0 = (short)(window_x0 + 16);
		short py0 = (short)(window_y0 + 34);
		short px1 = (short)(px0 + box_w);
		short py1 = (short)(py0 + box_h);
		short spin_x0 = (short)(px0 + 14);
		short spin_y0 = (short)(py0 + 32);
		short spin_x1 = (short)(px1 - 14);
		short spin_y1 = (short)(spin_y0 + 26);
		short y_cur;
		char clean_name[64];

		set_forge_hud_ui_scale(FORGE_IN_MATCH_HUD_SCALE, (float)px0, (float)py0);

		/* Authentic Halo CE 9-slice Objectives/Pause Box container */
		draw_ce_pause_box(px0, py0, box_w, box_h, 1);

		/* Top Header Bar Text in ui\large_ui */
		draw_ce_menu_str(1, font_index, (short)(px0 + 14), (short)(py0 + 4),
			"FORGE PALETTE", 0xFFFFFFFFu);
		snprintf(buf, sizeof(buf), "%d / %d", g_forge.live_instance_count, FORGE_MAX_LIVE_INSTANCES);
		draw_ce_menu_str_box(0, font_index, (short)(px1 - 110), (short)(py0 + 6), (short)(px1 - 14), (short)(py0 + 28), 1 /* right */,
			buf, 0xFF2896FFu);

		/* Category Spinner Row (3-slice rounded option_bkds_sm + arrow_sm_left + arrow_sm_right + ui\large_ui) */
		draw_ce_rounded_row_bar(1, spin_x0, spin_y0, spin_x1, spin_y1);
		draw_ce_menu_bitmap_cropped("pc\\bitmaps\\arrow_sm_left", 0, (short)(spin_x0 + 6), (short)(spin_y0 + 7), (short)(spin_x0 + 12), (short)(spin_y0 + 19), 6, 12, 0xFFFFFFFFu);
		draw_ce_menu_bitmap_cropped("pc\\bitmaps\\arrow_sm_right", 0, (short)(spin_x1 - 12), (short)(spin_y0 + 7), (short)(spin_x1 - 6), (short)(spin_y0 + 19), 6, 12, 0xFFFFFFFFu);

		snprintf(buf, sizeof(buf), "%s (%d/%d)",
			cat_names[g_forge.current_category],
			fcount > 0 ? (g_forge.palette_cursor + 1) : 0,
			fcount);
		draw_ce_menu_str_box(1, font_index, (short)(spin_x0 + 16), (short)(spin_y0 + 2), (short)(spin_x1 - 16), spin_y1, 2 /* center */,
			buf, 0xFFFFFFFFu);

		/* Horizontal Blue Divider Line (bitmaps/blue) */
		draw_ce_blue_line((short)(px0 + 14), (short)(spin_y1 + 3), (short)(px1 - 14), 2);

		/* 10 Item Rows with rounded highlight on BOTH left and right ends when selected */
		y_cur = (short)(spin_y1 + 8);
		for (r = 0; r < max_rows && (g_forge.palette_scroll + r) < fcount; r++)
		{
			int idx = g_forge.palette_scroll + r;
			struct forge_catalog_entry const *ce = &g_forge.catalog[filtered[idx]];
			int sel = (idx == g_forge.palette_cursor);
			int placed_cnt = count_active_instances_for_catalog_entry(ce);
			uint32_t row_col = sel ? 0xFFFFFFFFu : 0xFF2896FFu;
			short rx0 = (short)(px0 + 14);
			short ry0 = y_cur;
			short rx1 = (short)(px1 - 14);
			short ry1 = (short)(ry0 + 22);

			if (sel)
				draw_ce_rounded_row_bar(1, rx0, ry0, rx1, ry1);

			format_clean_uppercase_item_name(ce->display_name, clean_name, sizeof(clean_name));
			draw_ce_menu_str_box(0, font_index, (short)(rx0 + 10), (short)(ry0 + 1), (short)(rx1 - 38), ry1, 0,
				clean_name, row_col);

			snprintf(buf, sizeof(buf), "%d", placed_cnt);
			draw_ce_menu_str_box(0, font_index, (short)(rx1 - 40), (short)(ry0 + 1), (short)(rx1 - 10), ry1, 1 /* right */,
				buf, row_col);

			y_cur = (short)(y_cur + 23);
		}

		/* Scroll indicator arrows using authentic arrow_sm_up / arrow_sm_down bitmaps */
		if (g_forge.palette_scroll > 0)
		{
			draw_ce_menu_bitmap_cropped("pc\\bitmaps\\arrow_sm_up", 1,
				(short)(px1 - 24), (short)(spin_y1 + 9), (short)(px1 - 12), (short)(spin_y1 + 21), 16, 16, 0xFFFFFFFFu);
		}
		if (g_forge.palette_scroll + max_rows < fcount)
		{
			draw_ce_menu_bitmap_cropped("pc\\bitmaps\\arrow_sm_down", 1,
				(short)(px1 - 24), (short)(py1 - 50), (short)(px1 - 12), (short)(py1 - 38), 16, 16, 0xFFFFFFFFu);
		}

		/* Footer Action Buttons using authentic bitmaps/text_button_background (128x24) */
		draw_ce_text_button(font_index, (short)(px0 + 14), (short)(py1 - 32), 128, 24, 1, "[ENTER] PLACE");
		draw_ce_text_button(font_index, (short)(px1 - 142), (short)(py1 - 32), 128, 24, 0, "[ESC] DONE");
	}

	/* ========================================================================
	 * OBJECT PROPERTIES PANEL (X when holding an object) - BASE GAME MENU STYLING
	 * ======================================================================== */
	if (g_forge.active_modal == _forge_modal_object_properties && g_forge.grabbed_live_index >= 0)
	{
		struct forge_live_instance const *inst = &g_forge.live_instances[g_forge.grabbed_live_index];
		short box_w = 312;
		short box_h = 224;
		short px0 = (short)(window_x0 + 16);
		short py0 = (short)(window_y0 + 34);
		short px1 = (short)(px0 + box_w);
		short py1 = (short)(py0 + box_h);
		char labels[5][32];
		char values[5][48];
		int r;

		set_forge_hud_ui_scale(FORGE_IN_MATCH_HUD_SCALE, (float)px0, (float)py0);

		draw_ce_pause_box(px0, py0, box_w, box_h, 1);

		draw_ce_menu_str(1, font_index, (short)(px0 + 14), (short)(py0 + 4),
			"OBJECT PROPERTIES", 0xFFFFFFFFu);

		if (inst->group_tag == 'spwn')
		{
			snprintf(labels[0], sizeof(labels[0]), "TEAM:");
			snprintf(values[0], sizeof(values[0]), "%s",
				(inst->team_designator == 1) ? "BLUE TEAM" : (inst->team_designator == 2 ? "FFA" : "RED TEAM"));
		}
		else if (inst->group_tag == 'flag' &&
			(inst->netgame_flag_type == _netgame_flag_ctf_flag || inst->netgame_flag_type == _netgame_flag_ctf_vehicle))
		{
			snprintf(labels[0], sizeof(labels[0]), "TEAM:");
			snprintf(values[0], sizeof(values[0]), "%s",
				inst->team_designator == 1 ? "BLUE TEAM" : "RED TEAM");
		}
		else if (inst->group_tag == 'flag' &&
			(inst->netgame_flag_type == _netgame_flag_race_track || inst->netgame_flag_type == _netgame_flag_race_vehicle))
		{
			snprintf(labels[0], sizeof(labels[0]), "ORDER:");
			snprintf(values[0], sizeof(values[0]), "#%d", (int)inst->team_designator);
		}
		else
		{
			snprintf(labels[0], sizeof(labels[0]), "CHANNEL:");
			snprintf(values[0], sizeof(values[0]), "#%d", (int)inst->team_designator);
		}
		snprintf(labels[1], sizeof(labels[1]), "GAME MODE:");
		snprintf(values[1], sizeof(values[1]), "%s", gametype_flags_label(inst->gametype_flags));
		snprintf(labels[2], sizeof(labels[2]), "ROT SNAP:");
		snprintf(values[2], sizeof(values[2]), "%s", snap_names[g_forge.snap_mode]);
		snprintf(labels[3], sizeof(labels[3]), "[Z] DROP TO FLOOR");
		values[3][0] = 0;
		snprintf(labels[4], sizeof(labels[4]), "[DEL] DELETE OBJECT");
		values[4][0] = 0;

		for (r = 0; r < 5; r++)
		{
			short rx0 = (short)(px0 + 14);
			short ry0 = (short)(py0 + 34 + r * 28);
			short rx1 = (short)(px1 - 14);
			short ry1 = (short)(ry0 + 25);
			int sel = (g_forge.props_cursor == r);
			uint32_t col = sel ? 0xFFFFFFFFu : 0xFF2896FFu;

			draw_ce_rounded_row_bar((short)(sel ? 1 : 0), rx0, ry0, rx1, ry1);

			draw_ce_menu_str(1, font_index, (short)(rx0 + 10), (short)(ry0 + 2), labels[r], col);
			if (values[r][0])
			{
				short vx0 = (short)(rx0 + 118);
				short vx1 = (short)(rx1 - 8);
				draw_ce_menu_bitmap_cropped("pc\\bitmaps\\arrow_sm_left", 0, vx0, (short)(ry0 + 6), (short)(vx0 + 6), (short)(ry0 + 18), 6, 12, 0xFFFFFFFFu);
				draw_ce_menu_bitmap_cropped("pc\\bitmaps\\arrow_sm_right", 0, (short)(vx1 - 6), (short)(ry0 + 6), vx1, (short)(ry0 + 18), 6, 12, 0xFFFFFFFFu);
				draw_ce_menu_str_box(0, font_index, (short)(vx0 + 10), (short)(ry0 + 3), (short)(vx1 - 10), ry1, 2 /* center */, values[r], col);
			}
		}

		draw_ce_text_button(font_index, (short)(px0 + 14), (short)(py1 - 32), 136, 24, 1, "[ENTER] SELECT");
		draw_ce_text_button(font_index, (short)(px1 - 150), (short)(py1 - 32), 136, 24, 0, "[ESC] DONE");
	}

	/* ========================================================================
	 * INITIAL SAVE MAP DIALOG BOX (Opened from Esc -> SAVE MAP..)
	 * Styled with the base game's pause/objectives box, rounded option_bkds bar,
	 * text_button_background buttons, and ui\large_ui / ui\small_ui fonts!
	 * ======================================================================== */
	if (g_forge.active_modal == _forge_modal_save_copy_as)
	{
		short cx = (short)((window_x0 + window_x1) / 2);
		short cy = (short)((window_y0 + window_y1) / 2);
		short box_w = 416;
		short box_h = 225;
		short px0 = (short)(cx - box_w / 2);
		short py0 = (short)(cy - box_h / 2 - 8);
		short px1 = (short)(px0 + box_w);
		short py1 = (short)(py0 + box_h);
		short in_x0 = (short)(px0 + 28);
		short in_y0 = (short)(py0 + 74);
		short in_x1 = (short)(px1 - 28);
		short in_y1 = (short)(in_y0 + 28);
		size_t name_len = strlen(g_forge.save_map_name);
		char const *visible_input = g_forge.save_map_name;
		char short_name[32];

		set_forge_hud_ui_scale(FORGE_IN_MATCH_HUD_SCALE, (float)cx, (float)cy);

		/* If the typed name is very long, scroll the input view to show the last 22 characters */
		if (name_len > 22)
			visible_input = g_forge.save_map_name + (name_len - 22);

		/* Truncate map name for the output file preview so it never overflows the dialog box */
		if (name_len > 18)
			snprintf(short_name, sizeof(short_name), "%.15s...", g_forge.save_map_name);
		else
			snprintf(short_name, sizeof(short_name), "%s", g_forge.save_map_name);

		/* Draw the authentic 9-slice Halo CE Pause / Objectives Box */
		draw_ce_pause_box(px0, py0, box_w, box_h, 1);

		/* Header Title in ui\large_ui */
		draw_ce_menu_str(1, font_index, (short)(px0 + 18), (short)(py0 + 4),
			"SAVE MAP..", 0xFFFFFFFFu);

		/* Field Label in ui\large_ui (#FF2896FF) */
		draw_ce_menu_str(1, font_index, in_x0, (short)(py0 + 44),
			"ENTER FORGE MAP NAME:", 0xFF2896FFu);

		/* Base game 3-slice rounded option_bkds text entry field */
		if (!draw_ce_rounded_row_bar(1, in_x0, in_y0, in_x1, in_y1))
		{
			rectangle2d ib = { in_y0, in_x0, in_y1, in_x1 };
			draw_quad(&ib, 0xE8040C1Cu);
			draw_box_border(in_x0, in_y0, in_x1, in_y1, 1, 0xFF2896FFu);
		}

		snprintf(buf, sizeof(buf), "%s_", visible_input);
		draw_ce_menu_str_box(1, font_index, (short)(in_x0 + 14), (short)(in_y0 + 3), (short)(in_x1 - 14), in_y1, 0,
			buf, 0xFFFFFFFFu);

		draw_ce_blue_line(in_x0, (short)(py0 + 114), in_x1, 2);

		snprintf(buf, sizeof(buf), "Saves as: custom_maps\\%s.map", short_name);
		draw_ce_menu_str_box(0, font_index, in_x0, (short)(py0 + 124), in_x1, (short)(py0 + 144), 0,
			buf, 0xFF2896FFu);

		draw_ce_menu_str_box(0, font_index, in_x0, (short)(py0 + 146), in_x1, (short)(py0 + 168), 0,
			"After initial save, press [F5] in-game for Quick Save.", 0xFF2896FFu);

		/* Base game text_button_background buttons at bottom of dialog */
		draw_ce_text_button(font_index, (short)(cx - 138), (short)(py1 - 34), 128, 24, 1, "[ENTER] SAVE");
		draw_ce_text_button(font_index, (short)(cx + 10), (short)(py1 - 34), 128, 24, 0, "[ESC] CANCEL");
	}

	/* Status Toast Banner styled with base game option_bkds (frame 1) & ui\small_ui font */
	if (g_forge.status_message[0] && (uint32_t)system_milliseconds() < g_forge.status_message_until_ms)
	{
		short sx0 = (short)(window_x0 + 118);
		short sy0 = (short)(window_y1 - 54);
		short sx1 = (short)(window_x1 - 12);
		short sy1 = (short)(window_y1 - 30);

		set_forge_hud_ui_scale(FORGE_IN_MATCH_HUD_SCALE, (float)(sx0 + sx1) * 0.5f, (float)sy1);

		if (!draw_ce_menu_bitmap_cropped("pc\\bitmaps\\option_bkds", 1, sx0, sy0, sx1, sy1, 512, 28, 0xFFFFFFFFu))
		{
			rectangle2d sbar = { sy0, sx0, sy1, sx1 };
			draw_themed_panel(_forge_ui_area_toast_bar, &sbar, 0);
		}
		draw_ce_menu_str_box(0, font_index, (short)(sx0 + 12), (short)(sy0 + 3), (short)(sx1 - 12), sy1, 2 /* center */,
			g_forge.status_message, 0xFFFFFFFFu);
	}

	set_forge_hud_ui_scale(1.0f, 0.0f, 0.0f);
}



