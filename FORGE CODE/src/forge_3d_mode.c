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
#include "bitmaps/bitmap_group.h"
#include "bitmaps/bitmaps.h"
#include "rasterizer/rasterizer.h"
#include "render/render.h"
#include "render/render_cameras.h"
#include "text/draw_string.h"

#include "objects/object_lights.h"
#include "objects/widgets/widgets.h"
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

static void draw_quad(rectangle2d const *r, uint32_t argb)
{
	if (!r || r->x1 <= r->x0 || r->y1 <= r->y0 || ((argb >> 24) & 0xFFu) == 0)
		return;
	forge_draw_screen_rect(r->x0, r->y0, r->x1, r->y1, argb);
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
	long f = interface_get_tag_index(_interface_font_terminal);
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
	snprintf(st->font_name, sizeof(st->font_name), "terminal");
	st->font_style = NONE;
	st->font_scale = 1.0f;
	st->title_color = 0xFFA6EBFFu;
	st->text_color = 0xFFC7DBFFu;
	st->selected_color = 0xFFFFF5A6u;
	st->bg_color = 0xE00A1526u;
	st->header_color = 0xB014325Au;
	st->highlight_color = 0xD8225C9Cu;
	st->border_color = 0xE84498F2u;
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
		st->bitmap.flags = 0;
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
		"panel_modal.png",
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

	/* Specific default color tuning per area */
	s_ui_theme.areas[_forge_ui_area_preview_panel].title_color = 0xFF80FFB8u;
	s_ui_theme.areas[_forge_ui_area_preview_panel].border_color = 0xC044FF98u;
	s_ui_theme.areas[_forge_ui_area_top_bar].bg_color = 0xCC0A1426u;
	s_ui_theme.areas[_forge_ui_area_top_bar].border_color = 0xD03C8CE6u;
	s_ui_theme.areas[_forge_ui_area_bottom_bar].bg_color = 0xB8081222u;
	s_ui_theme.areas[_forge_ui_area_bottom_bar].border_color = 0x903578C4u;
	s_ui_theme.areas[_forge_ui_area_toast_bar].bg_color = 0xD80A182Cu;
	s_ui_theme.areas[_forge_ui_area_toast_bar].text_color = 0xFFA6F2FFu;
	s_ui_theme.areas[_forge_ui_area_markers].title_color = 0xF259FF8Cu;
	s_ui_theme.areas[_forge_ui_area_markers].text_color = 0xF2FFE673u;
	s_ui_theme.areas[_forge_ui_area_markers].selected_color = 0xF273FFA6u;

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

static void draw_ui_png_quad(struct bitmap_data *bitmap, short x0, short y0, short x1, short y1, uint32_t tint_argb)
{
	struct dynamic_screen_vertex vertices[4];
	struct rasterizer_dynamic_screen_geometry_parameters parameters;
	int v;

	if (!bitmap || !bitmap->hardware_format || x1 <= x0 || y1 <= y0)
		return;

	for (v = 0; v < 4; v++)
	{
		vertices[v].position.x = (v % 3) ? (real)x1 : (real)x0;
		vertices[v].position.y = (v > 1) ? (real)y1 : (real)y0;
		vertices[v].texture_coordinates.x = (v % 3) ? 1.0f : 0.0f;
		vertices[v].texture_coordinates.y = (v > 1) ? 1.0f : 0.0f;
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
		f = get_small_forge_font(fallback_font);
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

	/* Enforce Multiplayer Asset Guard */
	if (forge_is_multiplayer_session() && source_cache_id != 0)
	{
		forge_set_status("Multiplayer Asset Guard: Custom tags from customforgeasset.map are disabled online!");
		return -1;
	}

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
		inst->team_designator = strstr(tag_path, "blue") ? 1 : 0;
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

static void update_save_modal_input(void)
{
	static int prev_key[256];
	int vk;

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

void forge_3d_update(float dt)
{
	static int prev_f1, prev_f2, prev_f4, prev_f5, prev_f6, prev_f8, prev_esc, prev_x, prev_bk, prev_lmb;
	static int prev_up, prev_down, prev_left, prev_right, prev_enter, prev_g, prev_z, prev_f, prev_del;
	static int s_init_wait_ticks = 0;
	int cur_f1, cur_f2, cur_f4, cur_f5, cur_f6, cur_f8, cur_esc, cur_x, cur_bk, cur_lmb;
	int cur_up, cur_down, cur_left, cur_right, cur_enter, cur_g, cur_z, cur_f, cur_del;

	forge_runtime_init();

	/* Only run Forge editor controls when inside a Forge session and not on the UI main menu map */
	if (!g_forge_session_enabled)
		return;
	if (main_get_map_name() && strcmp(main_get_map_name(), "levels\\ui\\ui") == 0)
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
	cur_f6 = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
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
				g_forge.active_modal = _forge_modal_spawn_palette;
				g_forge.h3_menu_stage = 0; /* Start on Halo 3 Category list */
				if (g_forge.current_category == _forge_cat_bipeds || g_forge.current_category == _forge_cat_ai_squads)
					g_forge.current_category = _forge_cat_weapons;
			}
		}
	}
	if (cur_f4 && !prev_f4 && g_forge.mode == _forge_mode_3d_monitor)
	{
		g_forge.third_person_view = !g_forge.third_person_view;
		forge_set_status("3rd-Person Forge View: %s", g_forge.third_person_view ? "ON" : "OFF");
	}
	if (cur_f5 && !prev_f5)
	{
		if (g_forge.active_modal == _forge_modal_save_copy_as)
			g_forge.active_modal = _forge_modal_none;
		else
			forge_trigger_save_map_action();
	}
	if (cur_f6 && !prev_f6)
	{
		if (forge_is_multiplayer_session())
		{
			forge_set_status("Multiplayer Asset Guard: 'Add Tags to Cache' is disabled during live online sessions!");
		}
		else
		{
			forge_runtime_reload_catalogs();
			g_forge.active_modal = (g_forge.active_modal == _forge_modal_inject_custom_tags)
				? _forge_modal_none : _forge_modal_inject_custom_tags;
		}
	}
	if (cur_x && !prev_x && g_forge.mode == _forge_mode_3d_monitor && g_forge.grabbed_live_index >= 0)
	{
		g_forge.active_modal = (g_forge.active_modal == _forge_modal_object_properties)
			? _forge_modal_none : _forge_modal_object_properties;
		g_forge.props_cursor = 0;
	}
	if (cur_esc && !prev_esc && g_forge.active_modal != _forge_modal_none)
	{
		if (g_forge.active_modal == _forge_modal_spawn_palette && g_forge.h3_menu_stage == 1)
			g_forge.h3_menu_stage = 0;
		else
			g_forge.active_modal = _forge_modal_none;
	}

	prev_f1 = cur_f1;
	prev_f2 = cur_f2;
	prev_f4 = cur_f4;
	prev_f5 = cur_f5;
	prev_f6 = cur_f6;
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

	if (g_forge.active_modal == _forge_modal_inject_custom_tags)
	{
		if (cur_up && !prev_up && g_forge.donor_map_cursor > 0)
			g_forge.donor_map_cursor--;
		if (cur_down && !prev_down && g_forge.donor_map_cursor + 1 < g_forge.donor_map_count)
			g_forge.donor_map_cursor++;
		if (cur_enter && !prev_enter && g_forge.donor_map_count > 0)
		{
			forge_ui_inject_custom_map(g_forge.donor_map_files[g_forge.donor_map_cursor]);
			g_forge.active_modal = _forge_modal_none;
		}
		goto update_prev_keys;
	}

	/* Halo 3-Style 2-Stage Spawn Palette Menu */
	if (g_forge.active_modal == _forge_modal_spawn_palette)
	{
		if (g_forge.h3_menu_stage == 0)
		{
			/* Stage 0: Category Selection (skipping Bipeds & AI Characters) */
			if (cur_up && !prev_up)
			{
				do {
					g_forge.current_category = (enum forge_palette_category)(
						(g_forge.current_category + NUMBER_OF_FORGE_PALETTE_CATEGORIES - 1) % NUMBER_OF_FORGE_PALETTE_CATEGORIES);
				} while (g_forge.current_category == _forge_cat_bipeds || g_forge.current_category == _forge_cat_ai_squads);
			}
			if (cur_down && !prev_down)
			{
				do {
					g_forge.current_category = (enum forge_palette_category)(
						(g_forge.current_category + 1) % NUMBER_OF_FORGE_PALETTE_CATEGORIES);
				} while (g_forge.current_category == _forge_cat_bipeds || g_forge.current_category == _forge_cat_ai_squads);
			}
			if ((cur_enter && !prev_enter) || (cur_right && !prev_right))
			{
				g_forge.h3_menu_stage = 1;
				g_forge.palette_cursor = 0;
				g_forge.palette_scroll = 0;
			}
		}
		else
		{
			/* Stage 1: Item Selection inside Category (9 visible rows) */
			int filtered[512];
			int fcount = get_filtered_catalog_indices(filtered, 512);
			int const visible_rows = 9;

			if ((cur_left && !prev_left) || (cur_bk && !prev_bk))
			{
				g_forge.h3_menu_stage = 0;
			}
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
				float sx = g_forge.ray_hit_valid ? g_forge.ray_hit_x : (g_forge.cam_x + (float)cos((double)g_forge.cam_yaw) * 4.0f);
				float sy = g_forge.ray_hit_valid ? g_forge.ray_hit_y : (g_forge.cam_y + (float)sin((double)g_forge.cam_yaw) * 4.0f);
				float sz = g_forge.ray_hit_valid ? (g_forge.ray_hit_z + 0.15f) : g_forge.cam_z;
				int32_t spawned_idx = forge_3d_spawn_object_at(
					ce->group_tag,
					ce->tag_path,
					ce->source_cache_id,
					sx, sy, sz,
					g_forge.cam_yaw, 0.0f, 0.0f,
					0,
					1);
				/* Hitting Enter confirms the previewed item and attaches it to the placement beam */
				if (!(GetAsyncKeyState(VK_SHIFT) & 0x8000))
				{
					g_forge.active_modal = _forge_modal_none;
					if (spawned_idx >= 0 && g_forge.mode == _forge_mode_3d_monitor)
					{
						g_forge.grabbed_live_index = spawned_idx;
						g_forge.grabbed_object_index = g_forge.live_instances[spawned_idx].object_index;
						g_forge.grab_distance = 4.5f;
						g_forge.grab_yaw = g_forge.cam_yaw;
						g_forge.grab_pitch = 0.0f;
						g_forge.grab_roll = 0.0f;
						forge_set_status("Placing '%s' [Scroll 1-30m / Hold Z+Click Floor / R Rotate / Click Place]",
							ce->display_name);
					}
				}
				/* Prevent this same Enter press from immediately dropping the newly spawned item */
				prev_enter = 1;
				goto update_prev_keys;
			}
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

		/* Optional keyboard fine rotation when modal is closed */
		if (g_forge.active_modal == _forge_modal_none)
		{
			if (GetAsyncKeyState(VK_LEFT) & 0x8000) g_forge.cam_yaw += 1.6f * dt;
			if (GetAsyncKeyState(VK_RIGHT) & 0x8000) g_forge.cam_yaw -= 1.6f * dt;
			if (GetAsyncKeyState(VK_UP) & 0x8000) g_forge.cam_pitch += 1.2f * dt;
			if (GetAsyncKeyState(VK_DOWN) & 0x8000) g_forge.cam_pitch -= 1.2f * dt;
		}
		if (g_forge.cam_pitch > 1.48f) g_forge.cam_pitch = 1.48f;
		if (g_forge.cam_pitch < -1.48f) g_forge.cam_pitch = -1.48f;

		euler_to_forward_up(g_forge.cam_yaw, g_forge.cam_pitch, g_forge.cam_roll, &fwd, &up);
		right_vec.i = (float)sin((double)g_forge.cam_yaw);
		right_vec.j = -(float)cos((double)g_forge.cam_yaw);
		right_vec.k = 0.0f;

		/* 6DOF Monitor Flight (WASD + Space/E up + Ctrl/Q down) */
		if (GetAsyncKeyState('W') & 0x8000)
		{
			g_forge.cam_x += fwd.i * spd;
			g_forge.cam_y += fwd.j * spd;
			g_forge.cam_z += fwd.k * spd;
		}
		if (GetAsyncKeyState('S') & 0x8000)
		{
			g_forge.cam_x -= fwd.i * spd;
			g_forge.cam_y -= fwd.j * spd;
			g_forge.cam_z -= fwd.k * spd;
		}
		if (GetAsyncKeyState('D') & 0x8000)
		{
			g_forge.cam_x += right_vec.i * spd;
			g_forge.cam_y += right_vec.j * spd;
		}
		if (GetAsyncKeyState('A') & 0x8000)
		{
			g_forge.cam_x -= right_vec.i * spd;
			g_forge.cam_y -= right_vec.j * spd;
		}
		if ((GetAsyncKeyState(VK_SPACE) & 0x8000) || (GetAsyncKeyState('E') & 0x8000))
			g_forge.cam_z += spd;
		if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) || (GetAsyncKeyState('Q') & 0x8000))
			g_forge.cam_z -= spd;

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

		/* Grab / Release / Confirm Placement of Object on Physics Beam (F, Left Click, or Enter when holding) */
		if ((cur_f && !prev_f) ||
			(g_forge.active_modal == _forge_modal_none && cur_lmb && !prev_lmb) ||
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
				forge_set_status("Confirmed Placement: '%s'", released->tag_path);
				g_forge.grabbed_live_index = -1;
				g_forge.grabbed_object_index = NONE;
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
					g_forge.grabbed_live_index = best;
					g_forge.grabbed_object_index = inst->object_index;
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

			/* Point 3: Scroll wheel moves held item away and towards camera (closest 1m, furthest 30m) */
			platform_scoreboard_scroll(1, &wheel_notches, NULL);
			if (wheel_notches != 0)
				g_forge.grab_distance += (float)wheel_notches * 1.0f;

			if (GetAsyncKeyState(VK_PRIOR) & 0x8000) g_forge.grab_distance += 8.0f * dt;
			if (GetAsyncKeyState(VK_NEXT) & 0x8000) g_forge.grab_distance -= 8.0f * dt;
			if (g_forge.grab_distance < 1.0f) g_forge.grab_distance = 1.0f;
			if (g_forge.grab_distance > 30.0f) g_forge.grab_distance = 30.0f;

			if (GetAsyncKeyState('R') & 0x8000)
				g_forge.grab_yaw += 1.6f * dt;

			tx = g_forge.cam_x + fwd.i * g_forge.grab_distance;
			ty = g_forge.cam_y + fwd.j * g_forge.grab_distance;
			tz = g_forge.cam_z + fwd.k * g_forge.grab_distance;

			/* Point 5: Hold Z to continuously snap held object to floor, then Click (or Enter/F) to place on ground! */
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

			syaw = snap_angle_rad(g_forge.grab_yaw, g_forge.snap_mode);
			spitch = snap_angle_rad(g_forge.grab_pitch, g_forge.snap_mode);
			sroll = snap_angle_rad(g_forge.grab_roll, g_forge.snap_mode);
			forge_3d_move_instance(g_forge.grabbed_live_index, tx, ty, tz, syaw, spitch, sroll, 1);
		}
		else
		{
			platform_scoreboard_scroll(0, NULL, NULL);
		}

		/* Delete Grabbed Object */
		if (cur_del && !prev_del && g_forge.grabbed_live_index >= 0)
		{
			forge_3d_delete_instance(g_forge.grabbed_live_index, 1);
			g_forge.grabbed_live_index = -1;
			g_forge.grabbed_object_index = NONE;
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
			/* Pull camera back 2.4 world units behind Monitor so player sees their Monitor body */
			cmd.focus_position.x = g_forge.cam_x - cmd.forward.i * 2.4f;
			cmd.focus_position.y = g_forge.cam_y - cmd.forward.j * 2.4f;
			cmd.focus_position.z = g_forge.cam_z - cmd.forward.k * 2.4f + 0.25f;
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
	struct forge_ui_area_style const *mst = &s_ui_theme.areas[_forge_ui_area_markers];
	int i;
	for (i = 0; i < g_forge.live_instance_count; i++)
	{
		struct forge_live_instance const *inst = &g_forge.live_instances[i];
		union real_point3d wpt;
		union real_point2d spt;
		float dx_cam, dy_cam, dz_cam, cam_d2;
		int is_hi;
		short sx, sy;

		if (!inst->active)
			continue;
		if (inst->group_tag != 'spwn' && inst->group_tag != 'tele' && inst->group_tag != 'flag')
			continue;

		dx_cam = inst->x - render.camera.position.x;
		dy_cam = inst->y - render.camera.position.y;
		dz_cam = inst->z - render.camera.position.z;
		cam_d2 = dx_cam * dx_cam + dy_cam * dy_cam + dz_cam * dz_cam;
		is_hi = (i == g_forge.grabbed_live_index || i == g_forge.hovered_live_index);

		/* Distance cull markers beyond 65 world units so distant horizon stays crisp */
		if (cam_d2 > 65.0f * 65.0f && !is_hi)
			continue;

		if (inst->group_tag == 'spwn')
		{
			/* Requirement 1: Player spawns appear as a circle on the ground with an arrow pointing
			 * in the direction the player will spawn facing, coloured to match the team colour:
			 *   FFA spawns - Yellow
			 *   Red team   - Red
			 *   Blue team  - Blue */
			uint32_t ring_col;
			uint32_t dark_col;
			char const *team_lbl;

			if (inst->team_designator == 1)
			{
				/* Blue Team - Blue */
				ring_col = is_hi ? 0xFF70B8FFu : 0xFF2888FFu;
				dark_col = 0xB8041C44u;
				team_lbl = "BLUE SPAWN";
			}
			else if (inst->team_designator == 0)
			{
				/* Red Team - Red */
				ring_col = is_hi ? 0xFFFF6868u : 0xFFFF2828u;
				dark_col = 0xB8440404u;
				team_lbl = "RED SPAWN";
			}
			else
			{
				/* FFA Spawn - Yellow */
				ring_col = is_hi ? 0xFFFFFF68u : 0xFFFFE018u;
				dark_col = 0xB8443400u;
				team_lbl = "FFA SPAWN";
			}

			draw_ground_circle_and_facing_arrow(
				inst->x, inst->y, inst->z,
				inst->yaw,
				0.46f,
				1,
				is_hi,
				ring_col,
				dark_col,
				window_x0, window_y0,
				window_x1, window_y1);

			if (is_hi || (g_forge.mode == _forge_mode_3d_monitor && cam_d2 <= 16.0f * 16.0f))
			{
				wpt.x = inst->x;
				wpt.y = inst->y;
				wpt.z = inst->z + 0.22f;
				if (render_camera_world_to_screen(&render.camera, &render.frustum, &wpt, &spt))
				{
					sx = (short)(window_x0 + (short)(spt.x + 0.5f));
					sy = (short)(window_y0 + (short)(spt.y + 0.5f));
					if (sx >= window_x0 + 20 && sx <= window_x1 - 20 && sy >= window_y0 + 10 && sy <= window_y1 - 10)
					{
						draw_themed_str(_forge_ui_area_markers, font_index, (short)(sx - 28), (short)(sy + 6), team_lbl, ring_col);
					}
				}
			}
			continue;
		}

		wpt.x = inst->x;
		wpt.y = inst->y;
		wpt.z = inst->z + 0.35f;
		/* Point 7: Fixed 4-argument signature for render_camera_world_to_screen so teleporters never crash! */
		if (!render_camera_world_to_screen(&render.camera, &render.frustum, &wpt, &spt))
			continue;

		sx = (short)(window_x0 + (short)(spt.x + 0.5f));
		sy = (short)(window_y0 + (short)(spt.y + 0.5f));
		if (sx < window_x0 + 10 || sx > window_x1 - 10 || sy < window_y0 + 10 || sy > window_y1 - 10)
			continue;

		if (inst->group_tag == 'tele')
		{
			/* Point 6: Green Square with Arrow UP (Origin / Sender) or Arrow DOWN (Destination / Receiver) + Reference ID */
			int is_sender = (inst->netgame_flag_type == _netgame_flag_teleporter_source ||
				strstr(inst->tag_path, "sender") != NULL ||
				strstr(inst->tag_path, "origin") != NULL);
			rectangle2d sq, r1, r2, r3;
			char lbl[64];

			draw_ground_circle_and_facing_arrow(
				inst->x, inst->y, inst->z,
				inst->yaw,
				0.42f,
				!is_sender,
				is_hi,
				0xFF28FF58u,
				0xB0043410u,
				window_x0, window_y0,
				window_x1, window_y1);

			sq.x0 = (short)(sx - 12);
			sq.y0 = (short)(sy - 12);
			sq.x1 = (short)(sx + 12);
			sq.y1 = (short)(sy + 12);
			draw_quad(&sq, 0xB8083814u);
			draw_box_border(sq.x0, sq.y0, sq.x1, sq.y1, 2, 0xFF28FF58u);

			/* Vertical arrow shaft */
			r1.x0 = (short)(sx - 2); r1.y0 = (short)(sy - 7); r1.x1 = (short)(sx + 2); r1.y1 = (short)(sy + 7);
			draw_quad(&r1, 0xFF40FF70u);

			if (is_sender)
			{
				/* UP Arrow Head */
				r2.x0 = (short)(sx - 5); r2.y0 = (short)(sy - 5); r2.x1 = (short)(sx + 5); r2.y1 = (short)(sy - 2);
				r3.x0 = (short)(sx - 3); r3.y0 = (short)(sy - 8); r3.x1 = (short)(sx + 3); r3.y1 = (short)(sy - 5);
			}
			else
			{
				/* DOWN Arrow Head */
				r2.x0 = (short)(sx - 5); r2.y0 = (short)(sy + 2); r2.x1 = (short)(sx + 5); r2.y1 = (short)(sy + 5);
				r3.x0 = (short)(sx - 3); r3.y0 = (short)(sy + 5); r3.x1 = (short)(sx + 3); r3.y1 = (short)(sy + 8);
			}
			draw_quad(&r2, 0xFF40FF70u);
			draw_quad(&r3, 0xFF40FF70u);

			snprintf(lbl, sizeof(lbl), "%s #%d", is_sender ? "TELE IN [^]" : "TELE OUT [v]", (int)inst->team_designator);
			draw_themed_str(_forge_ui_area_markers, font_index, (short)(sx - 34), (short)(sy + 14), lbl, mst->title_color);
		}
		else if (inst->group_tag == 'flag')
		{
			/* Point 5 & Req 2: Visual marker + ground ring for KOTH Hill, Oddball Spawn, CTF Flag, Race Checkpoint */
			rectangle2d sq;
			char lbl[64];
			char const *kind = "OBJECTIVE";
			uint32_t border_col = 0xFFFFC838u;
			uint32_t dark_col = 0xB0281C04u;
			float ring_rad = 0.40f;
			int show_arrow = 0;

			if (inst->netgame_flag_type == _netgame_flag_hill || strstr(inst->tag_path, "koth"))
			{
				kind = "KOTH HILL";
				border_col = 0xFFFFB828u;
				ring_rad = 0.28f;
			}
			else if (inst->netgame_flag_type == _netgame_flag_oddball_ball_spawn || strstr(inst->tag_path, "oddball"))
			{
				kind = "ODDBALL";
				border_col = 0xFF48E8FFu;
				dark_col = 0xB0042838u;
			}
			else if (inst->netgame_flag_type == _netgame_flag_ctf_flag ||
					 inst->netgame_flag_type == _netgame_flag_ctf_vehicle ||
					 strstr(inst->tag_path, "ctf"))
			{
				kind = (inst->team_designator == 1) ? "BLUE FLAG" : "RED FLAG";
				border_col = (inst->team_designator == 1) ? 0xFF3890FFu : 0xFFFF3838u;
				dark_col = (inst->team_designator == 1) ? 0xB0041C44u : 0xB0440404u;
				ring_rad = 0.52f;
				show_arrow = 1;
			}
			else if (inst->netgame_flag_type == _netgame_flag_race_track ||
					 inst->netgame_flag_type == _netgame_flag_race_vehicle ||
					 strstr(inst->tag_path, "race"))
			{
				kind = (inst->netgame_flag_type == _netgame_flag_race_vehicle) ? "RACE VEH" : "RACE NODE";
				border_col = 0xFFE868FFu;
				dark_col = 0xB02C0838u;
				show_arrow = 1;
			}

			draw_ground_circle_and_facing_arrow(
				inst->x, inst->y, inst->z,
				inst->yaw,
				ring_rad,
				show_arrow,
				is_hi,
				border_col,
				dark_col,
				window_x0, window_y0,
				window_x1, window_y1);

			sq.x0 = (short)(sx - 8);
			sq.y0 = (short)(sy - 8);
			sq.x1 = (short)(sx + 8);
			sq.y1 = (short)(sy + 8);
			draw_quad(&sq, 0xB0181408u);
			draw_box_border(sq.x0, sq.y0, sq.x1, sq.y1, 2, border_col);

			if (is_hi || inst->netgame_flag_type != _netgame_flag_hill || cam_d2 <= 22.0f * 22.0f)
			{
				snprintf(lbl, sizeof(lbl), "%s #%d", kind, (int)inst->team_designator);
				draw_themed_str(_forge_ui_area_markers, font_index, (short)(sx - 30), (short)(sy + 11), lbl, border_col);
			}
		}
	}
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
		"VEHICLES",
		"BIPEDS & UNITS",
		"SCENERY & PROPS",
		"BARRIERS & DEVICES",
		"SPAWNS & TELEPORTERS",
		"OBJECTIVES",
		"AI COMBAT SQUADS"
	};
	static char const *const snap_names[4] = { "Off", "15 deg", "45 deg", "90 deg" };
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

	/* Draw 3D-projected Player Spawns (Ground Circle + Facing Arrow) & Teleporter / Objective markers */
	draw_teleporter_and_objective_markers(window_x0, window_y0, window_x1, window_y1, font_index);

	/* Point 13: Lower the bottom bar of [F1] and such to below the radar height on the UI (and clear of bottom-left radar) */
	if (g_forge.mode == _forge_mode_play && g_forge.active_modal == _forge_modal_none)
	{
		struct forge_ui_area_style const *bst = &s_ui_theme.areas[_forge_ui_area_bottom_bar];
		rectangle2d bbar;
		bbar.x0 = (short)(window_x0 + 118);
		bbar.y0 = (short)(window_y1 - 17);
		bbar.x1 = (short)(window_x0 + 430);
		bbar.y1 = (short)(window_y1 - 1);
		draw_themed_panel(_forge_ui_area_bottom_bar, &bbar, 0);
		snprintf(buf, sizeof(buf),
			"[F1] Forge Mode  [F5] %s",
			g_forge.has_saved_map_once ? "Quick Update Save" : "Save Map As...");
		draw_themed_str(_forge_ui_area_bottom_bar, font_index, (short)(bbar.x0 + 8), (short)(bbar.y0 + 1), buf, bst->text_color);
	}
	else if (g_forge.mode == _forge_mode_3d_monitor)
	{
		/* Clean Unarmed Monitor Reticle at screen center */
		short cx = (short)((window_x0 + window_x1) / 2);
		short cy = (short)((window_y0 + window_y1) / 2);
		uint32_t ret_col = (g_forge.grabbed_live_index >= 0 || g_forge.hovered_live_index >= 0 || g_forge.hovered_object_index != NONE || g_forge.hovered_object_label[0])
			? 0xE040FF78u : 0xD858B8FFu;
		struct forge_ui_area_style const *tst = &s_ui_theme.areas[_forge_ui_area_top_bar];
		struct forge_ui_area_style const *bst = &s_ui_theme.areas[_forge_ui_area_bottom_bar];
		struct forge_ui_area_style const *mst = &s_ui_theme.areas[_forge_ui_area_markers];
		rectangle2d rt;
		rectangle2d pill;

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

		/* Show hovered object/marker label right below reticle when aiming at any map/Forge object */
		if (g_forge.grabbed_live_index < 0 && g_forge.hovered_object_label[0])
		{
			snprintf(buf, sizeof(buf), "[F / Click] Grab '%s'", g_forge.hovered_object_label);
			draw_themed_str(_forge_ui_area_markers, font_index, (short)(cx - 80), (short)(cy + 16), buf, mst->selected_color);
		}

		/* Compact Halo 3-style top-right budget & Forge status pill (Point 8: AI team removed) */
		pill.x0 = (short)(window_x1 - 246);
		pill.y0 = (short)(window_y0 + 10);
		pill.x1 = (short)(window_x1 - 12);
		pill.y1 = (short)(window_y0 + 38);
		draw_themed_panel(_forge_ui_area_top_bar, &pill, 0);

		snprintf(buf, sizeof(buf), "FORGE MODE  /  ITEMS: %d / %d",
			g_forge.live_instance_count, FORGE_MAX_LIVE_INSTANCES);
		draw_themed_str(_forge_ui_area_top_bar, font_index, (short)(pill.x0 + 8), (short)(pill.y0 + 3), buf, tst->title_color);

		snprintf(buf, sizeof(buf), "Snap[G]: %s  /  Invulnerable",
			snap_names[g_forge.snap_mode]);
		draw_themed_str(_forge_ui_area_top_bar, font_index, (short)(pill.x0 + 8), (short)(pill.y0 + 15), buf, tst->text_color);

		/* Point 9 & Point 13: Bottom control strip positioned below the radar height on the UI */
		if (g_forge.active_modal == _forge_modal_none)
		{
			rectangle2d bbar;
			bbar.x0 = (short)(window_x0 + 118);
			bbar.y0 = (short)(window_y1 - 17);
			bbar.x1 = (short)(window_x1 - 12);
			bbar.y1 = (short)(window_y1 - 1);
			draw_themed_panel(_forge_ui_area_bottom_bar, &bbar, 0);

			if (g_forge.grabbed_live_index >= 0)
			{
				snprintf(buf, sizeof(buf),
					"[Click/Enter] Place  [Scroll] Dist(%.0fm)  [Hold Z+Click] Floor  [R] Rot  [X] Opts  [Del]",
					g_forge.grab_distance);
			}
			else
			{
				snprintf(buf, sizeof(buf),
					"[F2] Menu  [Click/F] Grab(<=20m)  [F4] 3rd-P  [F5] %s  [F1] Play",
					g_forge.has_saved_map_once ? "Quick Save" : "Save Map As...");
			}
			draw_themed_str(_forge_ui_area_bottom_bar, font_index, (short)(bbar.x0 + 8), (short)(bbar.y0 + 1), buf, bst->text_color);
		}
	}

	/* ========================================================================
	 * HALO 3-STYLE COMPACT LEFT-ALIGNED FORGE MENU (F2)
	 * ======================================================================== */
	if (g_forge.active_modal == _forge_modal_spawn_palette)
	{
		struct forge_ui_area_style const *st = &s_ui_theme.areas[_forge_ui_area_spawn_menu];
		short px0 = (short)(window_x0 + 16);
		short py0 = (short)(window_y0 + 44);
		short px1 = (short)(window_x0 + 276);
		short py1 = (short)(window_y0 + 260);
		rectangle2d panel, ftr;
		short y_cur;

		if (g_forge.h3_menu_stage == 0)
		{
			int c;
			/* 6 active categories (Bipeds & AI Squads hidden) */
			py1 = (short)(py0 + 36 + 6 * 18 + 22);
			panel.x0 = px0; panel.y0 = py0; panel.x1 = px1; panel.y1 = py1;
			draw_themed_panel(_forge_ui_area_spawn_menu, &panel, 24);
			draw_themed_str(_forge_ui_area_spawn_menu, font_index, (short)(px0 + 10), (short)(py0 + 5),
				"FORGE OBJECT PALETTE", st->title_color);

			y_cur = (short)(py0 + 30);
			for (c = 0; c < NUMBER_OF_FORGE_PALETTE_CATEGORIES; c++)
			{
				int sel, cnt;
				if (c == _forge_cat_bipeds || c == _forge_cat_ai_squads)
					continue;
				sel = ((int)g_forge.current_category == c);
				cnt = get_filtered_catalog_indices_for_cat((enum forge_palette_category)c, NULL, 512);
				if (sel)
				{
					rectangle2d hi;
					hi.x0 = (short)(px0 + 4);
					hi.y0 = (short)(y_cur - 1);
					hi.x1 = (short)(px1 - 4);
					hi.y1 = (short)(y_cur + 15);
					draw_quad(&hi, st->highlight_color);
					draw_box_border(hi.x0, hi.y0, hi.x1, hi.y1, 1, st->border_color);
				}
				snprintf(buf, sizeof(buf), "%-22s (%d)", cat_names[c], cnt);
				draw_themed_str(_forge_ui_area_spawn_menu, font_index, (short)(px0 + 12), y_cur,
					buf, sel ? st->selected_color : st->text_color);
				y_cur = (short)(y_cur + 18);
			}

			ftr.x0 = px0; ftr.y0 = (short)(py1 - 20); ftr.x1 = px1; ftr.y1 = py1;
			draw_quad(&ftr, 0x9008101Eu);
			draw_themed_str(_forge_ui_area_spawn_menu, font_index, (short)(px0 + 10), (short)(py1 - 16),
				"[Enter] Select  [Up/Down] Move  [F2] Close", st->title_color);
		}
		else
		{
			/* Stage 1: Halo 3 Item List inside Selected Category (9 visible rows + Live 3D Preview) */
			int filtered[512];
			int fcount = get_filtered_catalog_indices(filtered, 512);
			int r, max_rows = 9;

			py1 = (short)(py0 + 34 + max_rows * 17 + 26);
			panel.x0 = px0; panel.y0 = py0; panel.x1 = px1; panel.y1 = py1;
			draw_themed_panel(_forge_ui_area_spawn_menu, &panel, 24);

			snprintf(buf, sizeof(buf), "< %s (%d/%d)",
				cat_names[g_forge.current_category],
				fcount > 0 ? (g_forge.palette_cursor + 1) : 0,
				fcount);
			draw_themed_str(_forge_ui_area_spawn_menu, font_index, (short)(px0 + 10), (short)(py0 + 5),
				buf, st->title_color);

			y_cur = (short)(py0 + 30);
			for (r = 0; r < max_rows && (g_forge.palette_scroll + r) < fcount; r++)
			{
				int idx = g_forge.palette_scroll + r;
				struct forge_catalog_entry const *ce = &g_forge.catalog[filtered[idx]];
				int sel = (idx == g_forge.palette_cursor);
				if (sel)
				{
					rectangle2d hi;
					hi.x0 = (short)(px0 + 4);
					hi.y0 = (short)(y_cur - 1);
					hi.x1 = (short)(px1 - 4);
					hi.y1 = (short)(y_cur + 15);
					draw_quad(&hi, st->highlight_color);
					draw_box_border(hi.x0, hi.y0, hi.x1, hi.y1, 1, st->border_color);
				}
				snprintf(buf, sizeof(buf), "%s", ce->display_name);
				draw_themed_str(_forge_ui_area_spawn_menu, font_index, (short)(px0 + 12), y_cur,
					buf, sel ? st->selected_color : st->text_color);
				y_cur = (short)(y_cur + 17);
			}

			ftr.x0 = px0; ftr.y0 = (short)(py1 - 20); ftr.x1 = px1; ftr.y1 = py1;
			draw_quad(&ftr, 0x9008101Eu);
			draw_themed_str(_forge_ui_area_spawn_menu, font_index, (short)(px0 + 10), (short)(py1 - 16),
				"[Enter] Confirm & Place  [Backspace] Back", st->title_color);

			/* Live 3D Item Preview card below the list */
			if (fcount > 0 && g_forge.palette_cursor < fcount)
			{
				struct forge_ui_area_style const *pst = &s_ui_theme.areas[_forge_ui_area_preview_panel];
				struct forge_catalog_entry const *sel_ce = &g_forge.catalog[filtered[g_forge.palette_cursor]];
				rectangle2d pcard;
				pcard.x0 = px0;
				pcard.y0 = (short)(py1 + 6);
				pcard.x1 = px1;
				pcard.y1 = (short)(py1 + 54);
				draw_themed_panel(_forge_ui_area_preview_panel, &pcard, 0);
				snprintf(buf, sizeof(buf), "3D PREVIEW: %s", sel_ce->display_name);
				draw_themed_str(_forge_ui_area_preview_panel, font_index, (short)(pcard.x0 + 8), (short)(pcard.y0 + 4),
					buf, pst->title_color);
				snprintf(buf, sizeof(buf), "Tag: %s", sel_ce->tag_path);
				draw_themed_str(_forge_ui_area_preview_panel, font_index, (short)(pcard.x0 + 8), (short)(pcard.y0 + 18),
					buf, pst->text_color);
				draw_themed_str(_forge_ui_area_preview_panel, font_index, (short)(pcard.x0 + 8), (short)(pcard.y0 + 32),
					"Press [Enter] to confirm item & position it", pst->selected_color);
			}
		}
	}

	/* ========================================================================
	 * HALO 3-STYLE OBJECT PROPERTIES PANEL (X when holding an object)
	 * ======================================================================== */
	if (g_forge.active_modal == _forge_modal_object_properties && g_forge.grabbed_live_index >= 0)
	{
		struct forge_ui_area_style const *pst = &s_ui_theme.areas[_forge_ui_area_properties_menu];
		struct forge_live_instance const *inst = &g_forge.live_instances[g_forge.grabbed_live_index];
		short px0 = (short)(window_x0 + 16);
		short py0 = (short)(window_y0 + 44);
		short px1 = (short)(window_x0 + 310);
		short py1 = (short)(py0 + 148);
		rectangle2d panel;
		char rows[5][80];
		int r;

		panel.x0 = px0; panel.y0 = py0; panel.x1 = px1; panel.y1 = py1;
		draw_themed_panel(_forge_ui_area_properties_menu, &panel, 24);
		draw_themed_str(_forge_ui_area_properties_menu, font_index, (short)(px0 + 10), (short)(py0 + 5),
			"OBJECT, SPAWN & OBJECTIVE OPTIONS", pst->title_color);

		if (inst->group_tag == 'spwn')
		{
			char const *t_lbl = (inst->team_designator == 1) ? "BLUE TEAM (Blue)" :
				(inst->team_designator == 2 ? "FFA (Yellow)" : "RED TEAM (Red)");
			snprintf(rows[0], sizeof(rows[0]), "Spawn Team: < %s >", t_lbl);
		}
		else if (inst->group_tag == 'flag' &&
			(inst->netgame_flag_type == _netgame_flag_ctf_flag || inst->netgame_flag_type == _netgame_flag_ctf_vehicle))
		{
			snprintf(rows[0], sizeof(rows[0]), "CTF Team: < %s >",
				inst->team_designator == 1 ? "BLUE TEAM (1)" : "RED TEAM (0)");
		}
		else if (inst->group_tag == 'flag' &&
			(inst->netgame_flag_type == _netgame_flag_race_track || inst->netgame_flag_type == _netgame_flag_race_vehicle))
		{
			snprintf(rows[0], sizeof(rows[0]), "Race Order / Node ID: < #%d >", (int)inst->team_designator);
		}
		else
		{
			snprintf(rows[0], sizeof(rows[0]), "Ref / Hill / Link ID: < #%d >", (int)inst->team_designator);
		}
		snprintf(rows[1], sizeof(rows[1]), "Spawn Mode: < %s >", gametype_flags_label(inst->gametype_flags));
		snprintf(rows[2], sizeof(rows[2]), "Rotation Snap: < %s >", snap_names[g_forge.snap_mode]);
		snprintf(rows[3], sizeof(rows[3]), "Snap To Ground    [Hold Z + Click]");
		snprintf(rows[4], sizeof(rows[4]), "Delete Object     [Enter / Del]");

		for (r = 0; r < 5; r++)
		{
			short ry = (short)(py0 + 30 + r * 21);
			int sel = (g_forge.props_cursor == r);
			if (sel)
			{
				rectangle2d hi;
				hi.x0 = (short)(px0 + 4);
				hi.y0 = (short)(ry - 1);
				hi.x1 = (short)(px1 - 4);
				hi.y1 = (short)(ry + 15);
				draw_quad(&hi, pst->highlight_color);
			}
			draw_themed_str(_forge_ui_area_properties_menu, font_index, (short)(px0 + 12), ry,
				rows[r], sel ? pst->selected_color : pst->text_color);
		}
	}

	/* Point 10: Modal: "Add Tags to Cache" (MAPS) -> customforgeasset.map + credits.md */
	if (g_forge.active_modal == _forge_modal_inject_custom_tags)
	{
		struct forge_ui_area_style const *mst = &s_ui_theme.areas[_forge_ui_area_maps_modal];
		rectangle2d panel;
		int r;
		short py;

		panel.x0 = (short)(window_x0 + 24);
		panel.y0 = (short)(window_y0 + 48);
		panel.x1 = (short)(window_x0 + 380);
		panel.y1 = (short)(window_y0 + 310);
		draw_themed_panel(_forge_ui_area_maps_modal, &panel, 24);
		draw_themed_str(_forge_ui_area_maps_modal, font_index, (short)(panel.x0 + 10), (short)(panel.y0 + 5),
			"MAPS: ADD TAGS TO CACHE", mst->title_color);

		py = (short)(panel.y0 + 30);
		for (r = 0; r < g_forge.donor_map_count && r < 13; r++)
		{
			int sel = (r == g_forge.donor_map_cursor);
			if (sel)
			{
				rectangle2d hi;
				hi.x0 = (short)(panel.x0 + 4);
				hi.y0 = (short)(py - 1);
				hi.x1 = (short)(panel.x1 - 4);
				hi.y1 = (short)(py + 15);
				draw_quad(&hi, mst->highlight_color);
			}
			snprintf(buf, sizeof(buf), "%s", g_forge.donor_map_files[r]);
			draw_themed_str(_forge_ui_area_maps_modal, font_index, (short)(panel.x0 + 12), py,
				buf, sel ? mst->selected_color : mst->text_color);
			py = (short)(py + 17);
		}
	}

	/* Point 11: Modal: Initial Save Prompt ("Name Custom Forge Map" -> forge_maps/<name>/<name>.map) */
	if (g_forge.active_modal == _forge_modal_save_copy_as)
	{
		struct forge_ui_area_style const *sst = &s_ui_theme.areas[_forge_ui_area_save_modal];
		rectangle2d panel;
		panel.x0 = (short)(window_x0 + 110);
		panel.y0 = (short)(window_y0 + 145);
		panel.x1 = (short)(window_x1 - 110);
		panel.y1 = (short)(window_y0 + 250);
		draw_themed_panel(_forge_ui_area_save_modal, &panel, 24);
		draw_themed_str(_forge_ui_area_save_modal, font_index, (short)(panel.x0 + 12), (short)(panel.y0 + 5),
			"INITIAL SAVE: NAME YOUR CUSTOM FORGE MAP", sst->title_color);

		snprintf(buf, sizeof(buf), "Custom Map Name: %s_  (forge_maps/%s/%s.map)",
			g_forge.save_map_name, g_forge.save_map_name, g_forge.save_map_name);
		draw_themed_str(_forge_ui_area_save_modal, font_index, (short)(panel.x0 + 12), (short)(panel.y0 + 34),
			buf, sst->selected_color);

		draw_themed_str(_forge_ui_area_save_modal, font_index, (short)(panel.x0 + 12), (short)(panel.y0 + 54),
			"Packaged into forge_maps/<name>/ (keeps custom_maps/ clean).", sst->text_color);

		draw_themed_str(_forge_ui_area_save_modal, font_index, (short)(panel.x0 + 12), (short)(panel.y0 + 74),
			"[Enter] Save Map    [Backspace] Edit Name    [Esc] Cancel", sst->title_color);
	}

	/* Compact Status Toast Banner */
	if (g_forge.status_message[0] && (uint32_t)system_milliseconds() < g_forge.status_message_until_ms)
	{
		struct forge_ui_area_style const *tst = &s_ui_theme.areas[_forge_ui_area_toast_bar];
		rectangle2d sbar;
		sbar.x0 = (short)(window_x0 + 118);
		sbar.y0 = (short)(window_y1 - 35);
		sbar.x1 = (short)(window_x1 - 12);
		sbar.y1 = (short)(window_y1 - 19);
		draw_themed_panel(_forge_ui_area_toast_bar, &sbar, 0);
		draw_themed_str(_forge_ui_area_toast_bar, font_index, (short)(sbar.x0 + 8), (short)(sbar.y0 + 1),
			g_forge.status_message, tst->text_color);
	}
}



