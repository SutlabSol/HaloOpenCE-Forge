/*
 * OpenCE : Forge ("halo.exe")
 * Runtime Tag Catalog, Cached Tag Folder System (OpenCE Forge/cached_tags/),
 * Dynamic Runtime Tag Table Expansion, Multiplayer Asset Guard, Custom Tag Extraction,
 * Standalone Map Baking, Dynamic Network Version & 343 Guilty Spark Hardware Model Renderer.
 * Strictly modular inside FORGE CODE/ with zero modifications to source/ or port/.
 */

#include "cseries.h"
#include "halo_port_limits.h" /* HALO_PORT_NETWORK_VERSION: OpenCE's protocol for standard sessions */
#include "game/game.h"
#include "objects/objects.h"
#include "objects/object_definitions.h"
#include "units/units.h"
#include "units/unit_definitions.h"
#include "models/models.h"
#include "models/model_definitions.h"
#include "physics/physics_definitions.h"
#include "items/weapon_definitions.h"
#include "items/projectile_definitions.h"
#include "math/real_math.h"
#include "bitmaps/bitmaps.h"
#include "bitmaps/bitmap_group.h"
#include "game/players.h"
#include "rasterizer/rasterizer.h"
#include "rasterizer/rasterizer_geometry.h"
#include "rasterizer/rasterizer_model_types.h"
#include "rasterizer/rasterizer_models.h"
#include "rasterizer/rasterizer_swizzle.h"
#include "render/render.h"
#include "shaders/shader_definitions.h"
#include "shaders/shaders.h"
#include "sound/sound_definitions.h"
#include "tag_files/tag_groups.h"
#include "tag_files/tag_files.h"
#include "cache/cache_files.h"
#include "custom_edition_cache.h"
#include "tag_schema.h"
#include "tag_bundle.h"
#include "tag_names.h"
#include "tag_preview.h"
#include "../src/port_tag_profiles.h"
#include "forge_marker_ftags_embed.h"
extern int compress2(unsigned char *dest, unsigned long *destLen, const unsigned char *source, unsigned long sourceLen, int level);
#include <xtl.h>
#include "rasterizer/xbox/rasterizer_xbox.h"
#include "rasterizer/xbox/rasterizer_xbox_pixel_shader.h"
#include "zlib_prefixed.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "forge_engine.h"

#undef fopen
#undef fclose
#undef fread
#undef fwrite
#undef fseek
#undef ftell
#undef remove
#undef malloc
#undef calloc
#undef free
#undef realloc

extern unsigned long system_milliseconds(void);
extern void rasterizer_set_frustum_z(real z_near, real z_far);
extern void *cache_files_tag_instances(long *count);
extern void cache_files_set_tag_instances(void *instances, long count);
extern void progress_bar_begin(boolean skip_frame_capture);
extern void progress_bar_display(real progress);
extern void progress_bar_end(void);
extern void progress_bar_eachframe(void);
extern void shell_idle(void);
extern void sound_render(void);
extern void main_present_frame(void);
extern long halo_screen_width(void);
extern void platform_translate_path(const char *xbox_path, char *host_path, unsigned long host_path_size);
extern long bitmap_format_to_d3d_format(short format, word flags);
extern long bitmap_format_to_d3d_linear_format(short format, word flags);
extern void models_fix_transparent_part_links(void);
extern boolean custom_edition_cache_tags_loaded(void);
extern long port_tag_profiles_scan(void);
extern long port_tag_profiles_count(void);
extern long port_tag_profiles_find(const char *folder);
extern long port_tag_profiles_tag_count(long profile);
extern unsigned long port_tag_profiles_tag_group(long profile, long tag);
extern const char *port_tag_profiles_tag_name(long profile, long tag);
extern int port_tag_profiles_tag_enabled(long profile, long tag);
extern const char *port_tag_profiles_tag_option(long profile, long tag, const char *key);
extern void halo_custom_edition_texels_channels(void const *address, unsigned char order);
extern boolean orig_mod_sounds_read(struct sound_permutation const *permutation, void *buffer);
extern void orig_render_model(
	long model_index,
	real level_of_detail_pixels,
	struct real_matrix4x3 const *node_matrices,
	char const *region_permutation_indices,
	union real_rgb_color const *change_colors,
	real const *function_values,
	struct render_lighting const *lighting,
	union real_point3d const *centroid,
	real radius,
	struct render_model_effect const *model_effect,
	long unique_identifier,
	short forced_shader_permutation_index,
	unsigned long flags);

struct forge_progress_bar_mode_mirror
{
	long state;
	boolean capture_frame;
	unsigned char padding0[3];
	IDirect3DTexture8 *texture0;
	IDirect3DTexture8 *texture1;
	boolean active;
	unsigned char padding1[3];
};
extern struct forge_progress_bar_mode_mirror progress_bar_mode;
extern real last_t;

static int s_forge_loading_active = 0;
static float s_forge_loading_map_base = 0.0f;
static float s_forge_loading_map_span = 0.0f;
static unsigned long s_forge_loading_last_ms = 0;

static uint8_t sample_dxt5_halo_logo_lum(uint8_t const *dxt5_raw, int sx, int sy)
{
	int bx = sx >> 2;
	int by = sy >> 2;
	int p_idx = ((sy & 3) << 2) | (sx & 3);
	size_t bidx = ((size_t)by * 256u + (size_t)bx) * 16u;
	uint8_t a0 = dxt5_raw[bidx + 0];
	uint8_t a1 = dxt5_raw[bidx + 1];
	uint64_t abits =
		(uint64_t)dxt5_raw[bidx + 2] |
		((uint64_t)dxt5_raw[bidx + 3] << 8) |
		((uint64_t)dxt5_raw[bidx + 4] << 16) |
		((uint64_t)dxt5_raw[bidx + 5] << 24) |
		((uint64_t)dxt5_raw[bidx + 6] << 32) |
		((uint64_t)dxt5_raw[bidx + 7] << 40);
	uint32_t acode = (uint32_t)((abits >> (p_idx * 3)) & 7u);
	uint32_t alpha;
	uint16_t c0 = *(uint16_t const *)(dxt5_raw + bidx + 8);
	uint16_t c1 = *(uint16_t const *)(dxt5_raw + bidx + 10);
	uint32_t cbits = *(uint32_t const *)(dxt5_raw + bidx + 12);
	uint32_t ccode = (cbits >> (p_idx * 2)) & 3u;
	uint32_t r0 = ((c0 >> 11) & 31u) * 255u / 31u;
	uint32_t g0 = ((c0 >> 5) & 63u) * 255u / 63u;
	uint32_t b0 = (c0 & 31u) * 255u / 31u;
	uint32_t r1 = ((c1 >> 11) & 31u) * 255u / 31u;
	uint32_t g1 = ((c1 >> 5) & 63u) * 255u / 63u;
	uint32_t b1 = (c1 & 31u) * 255u / 31u;
	uint32_t l0 = r0 > g0 ? (r0 > b0 ? r0 : b0) : (g0 > b0 ? g0 : b0);
	uint32_t l1 = r1 > g1 ? (r1 > b1 ? r1 : b1) : (g1 > b1 ? g1 : b1);
	uint32_t lum;

	if (acode == 0) alpha = a0;
	else if (acode == 1) alpha = a1;
	else if (a0 > a1) alpha = ((8u - acode) * a0 + (acode - 1u) * a1) / 7u;
	else if (acode < 6u) alpha = ((6u - acode) * a0 + (acode - 1u) * a1) / 5u;
	else if (acode == 6u) alpha = 0u;
	else alpha = 255u;

	if (ccode == 0u) lum = l0;
	else if (ccode == 1u) lum = l1;
	else if (ccode == 2u) lum = (2u * l0 + l1) / 3u;
	else lum = (l0 + 2u * l1) / 3u;

	return (uint8_t)((lum * alpha) / 255u);
}

static void forge_ensure_xbox_loading_tga(void)
{
	char z_tga_host[1024];
	FILE *zf;
	FILE *mf;
	uint8_t *tga_buf;
	uint8_t *dxt5_raw = NULL;
	int have_ui_logo = 0;
	int x, y, slot;

	platform_translate_path("z:\\maps\\loading.tga", z_tga_host, sizeof(z_tga_host));
#ifdef _WIN32
	{
		char z_maps_dir[1024];
		platform_translate_path("z:\\maps", z_maps_dir, sizeof(z_maps_dir));
		CreateDirectoryA(z_maps_dir, NULL);
	}
#endif

	zf = fopen(z_tga_host, "rb");
	if (zf)
	{
		long zsz = 0;
		fseek(zf, 0, SEEK_END);
		zsz = ftell(zf);
		fclose(zf);
		if (zsz == 230418L)
			return;
	}

	mf = fopen("maps/loading.tga", "rb");
	if (mf)
	{
		long msz = 0;
		fseek(mf, 0, SEEK_END);
		msz = ftell(mf);
		if (msz == 230418L)
		{
			tga_buf = (uint8_t *)malloc(230418u);
			if (tga_buf)
			{
				fseek(mf, 0, SEEK_SET);
				if (fread(tga_buf, 1, 230418u, mf) == 230418u)
				{
					FILE *out_z = fopen(z_tga_host, "wb");
					if (out_z)
					{
						fwrite(tga_buf, 1, 230418u, out_z);
						fclose(out_z);
					}
					free(tga_buf);
					fclose(mf);
					return;
				}
				free(tga_buf);
			}
		}
		fclose(mf);
	}

	/* Extract the Halo ring + logo from the player's own Xbox ui.map (ui\shell\main_menu\halo_logo) */
	{
		long logo_tag = tag_loaded('bitm', "ui\\shell\\main_menu\\halo_logo");
		long pix_off = 0x590E00L;
		if (logo_tag != NONE)
		{
			struct bitmap_group *bgrp = (struct bitmap_group *)tag_get('bitm', logo_tag);
			if (bgrp && bgrp->bitmaps.count > 0 && bgrp->bitmaps.address)
			{
				struct bitmap_data *bmp = (struct bitmap_data *)bgrp->bitmaps.address;
				if (bmp->pixels_offset > 2048L && bmp->pixels_size >= 262144L)
					pix_off = bmp->pixels_offset;
			}
		}

		for (slot = 0; slot < 6 && !have_ui_logo; slot++)
		{
			char z_xbox[64];
			char z_cache[1024];
			FILE *cf;
			snprintf(z_xbox, sizeof(z_xbox), "z:\\cache00%d.map", slot);
			platform_translate_path(z_xbox, z_cache, sizeof(z_cache));
			cf = fopen(z_cache, "rb");
			if (cf)
			{
				uint8_t chdr[2048];
				if (fread(chdr, 1, sizeof(chdr), cf) == sizeof(chdr) &&
					*(uint32_t const *)(chdr + 0x00) == 0x68656164u &&
					_stricmp((char const *)(chdr + 0x20), "ui") == 0)
				{
					dxt5_raw = (uint8_t *)malloc(262144u);
					if (dxt5_raw && fseek(cf, pix_off, SEEK_SET) == 0 &&
						fread(dxt5_raw, 1, 262144u, cf) == 262144u)
					{
						have_ui_logo = 1;
					}
				}
				fclose(cf);
			}
		}
	}

	tga_buf = (uint8_t *)calloc(1, 230418u);
	if (!tga_buf)
	{
		free(dxt5_raw);
		return;
	}

	/* 18-byte uncompressed 24-bit 320x240 TGA header */
	tga_buf[2] = 2;
	tga_buf[12] = (uint8_t)(320 & 0xFF);
	tga_buf[13] = (uint8_t)(320 >> 8);
	tga_buf[14] = (uint8_t)(240 & 0xFF);
	tga_buf[15] = (uint8_t)(240 >> 8);
	tga_buf[16] = 24;

	for (y = 0; y < 240; y++)
	{
		for (x = 0; x < 320; x++)
		{
			uint8_t grey = 0;
			uint8_t *dst = tga_buf + 18u + ((size_t)(239 - y) * 320u + (size_t)x) * 3u;

			if (have_ui_logo && dxt5_raw && y >= 56 && y < 184)
			{
				int sx = x * 2;
				int sy = (y - 56) * 2;
				uint8_t v0 = sample_dxt5_halo_logo_lum(dxt5_raw, sx, sy);
				uint8_t v1 = sample_dxt5_halo_logo_lum(dxt5_raw, sx + 1, sy);
				uint8_t v2 = sample_dxt5_halo_logo_lum(dxt5_raw, sx, sy + 1);
				uint8_t v3 = sample_dxt5_halo_logo_lum(dxt5_raw, sx + 1, sy + 1);
				uint8_t vmax = v0 > v1 ? v0 : v1;
				if (v2 > vmax) vmax = v2;
				if (v3 > vmax) vmax = v3;
				grey = vmax;
			}
			/* Subtle horizontal progress track beneath the Halo logo so the sweeping glow bar is always visible */
			if (y >= 190 && y <= 194 && x >= 32 && x < 288)
			{
				if (grey < 110)
					grey = 110;
			}
			dst[0] = grey;
			dst[1] = grey;
			dst[2] = grey;
		}
	}

	{
		FILE *out_z = fopen(z_tga_host, "wb");
		if (out_z)
		{
			fwrite(tga_buf, 1, 230418u, out_z);
			fclose(out_z);
		}
	}

	free(tga_buf);
	free(dxt5_raw);
}

static void forge_xbox_loading_screen_render_frame(float progress)
{
	struct rasterizer_frame_begin_parameters frame_parameters;
	struct rasterizer_window_begin_parameters window_parameters;

	if (!global_d3d_device)
		return;
	if (progress < 0.001f)
		progress = 0.001f;
	if (progress > 1.0f)
		progress = 1.0f;

	shell_idle();
	sound_render();

	memset(&frame_parameters, 0, sizeof(frame_parameters));
	rasterizer_frame_begin(&frame_parameters);
	rasterizer_windows_begin();

	memset(&window_parameters, 0, sizeof(window_parameters));
	window_parameters.camera.position.x = 0.0f;
	window_parameters.camera.position.y = 0.0f;
	window_parameters.camera.position.z = 0.0f;
	window_parameters.camera.forward.i = 0.0f;
	window_parameters.camera.forward.j = 0.0f;
	window_parameters.camera.forward.k = 1.0f;
	window_parameters.camera.up.i = 0.0f;
	window_parameters.camera.up.j = 1.0f;
	window_parameters.camera.up.k = 0.0f;
	window_parameters.camera.mirrored = FALSE;
	window_parameters.camera.vertical_field_of_view = 1.1f;
	window_parameters.camera.viewport_bounds.x0 = 0;
	window_parameters.camera.viewport_bounds.y0 = 0;
	window_parameters.camera.viewport_bounds.x1 = (short)halo_screen_width();
	window_parameters.camera.viewport_bounds.y1 = 480;
	window_parameters.camera.window_bounds = window_parameters.camera.viewport_bounds;
	window_parameters.camera.z_near = 0.01f;
	window_parameters.camera.z_far = 1.0f;
	render_camera_build_frustum(&window_parameters.camera, NULL, &window_parameters.frustum, TRUE);
	window_parameters.rasterizer_target = 0;

	rasterizer_window_begin(&window_parameters);
	progress_bar_display(progress);
	rasterizer_window_end();
	rasterizer_windows_end();
	rasterizer_frame_end();
	main_present_frame();
	s_forge_loading_last_ms = system_milliseconds();
}

static void forge_xbox_loading_screen_begin(void)
{
	if (!global_d3d_device)
		return;
	forge_ensure_xbox_loading_tga();
	progress_bar_eachframe();
	if (progress_bar_mode.texture0)
	{
		IDirect3DTexture8_Release(progress_bar_mode.texture0);
		progress_bar_mode.texture0 = NULL;
	}
	if (progress_bar_mode.texture1)
	{
		IDirect3DTexture8_Release(progress_bar_mode.texture1);
		progress_bar_mode.texture1 = NULL;
	}
	last_t = 0.0f;
	s_forge_loading_active = 1;
	progress_bar_begin(TRUE);
	forge_xbox_loading_screen_render_frame(0.001f);
}

static void forge_xbox_loading_screen_update(float progress)
{
	int catchup = 0;
	if (!global_d3d_device || !s_forge_loading_active)
		return;
	if (progress < 0.01f)
		progress = 0.01f;
	if (progress > 1.0f)
		progress = 1.0f;

	forge_xbox_loading_screen_render_frame(progress);
	while (last_t + 0.09f < progress && catchup < 3)
	{
		Sleep(10);
		forge_xbox_loading_screen_render_frame(progress);
		catchup++;
	}
}

static void forge_xbox_loading_screen_end(void)
{
	int step;
	if (!global_d3d_device || !s_forge_loading_active)
		return;
	for (step = 0; step < 12 && last_t < 0.96f; step++)
	{
		forge_xbox_loading_screen_render_frame(1.0f);
		Sleep(10);
	}
	forge_xbox_loading_screen_render_frame(1.0f);
	progress_bar_end();
	s_forge_loading_active = 0;
	s_forge_loading_map_base = 0.0f;
	s_forge_loading_map_span = 0.0f;
}

struct forge_cache_file_tag_instance
{
	long group_tag;
	long parent_group_tags[2];
	long tag_index;
	char *name;
	void *base_address;
	unsigned long unused[2];
};

struct forge_runtime_state g_forge;
int g_forge_session_enabled = 0;

long forge_get_network_version(void)
{
	return g_forge_session_enabled ? FORGE_NETWORK_VERSION : HALO_PORT_NETWORK_VERSION;
}

void forge_set_status(char const *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(g_forge.status_message, sizeof(g_forge.status_message), fmt, ap);
	va_end(ap);
	g_forge.status_message_until_ms = (uint32_t)system_milliseconds() + 6000u;
}

void forge_draw_screen_rect(short x0, short y0, short x1, short y1, uint32_t argb)
{
	if (!global_d3d_device || x1 <= x0 || y1 <= y0)
		return;

	_rasterizer_debug_immediate_begin_screenspace();
	IDirect3DDevice8_SetRenderState(global_d3d_device, D3DRS_ALPHABLENDENABLE, TRUE);
	IDirect3DDevice8_SetRenderState(global_d3d_device, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
	IDirect3DDevice8_SetRenderState(global_d3d_device, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
	IDirect3DDevice8_SetRenderState(global_d3d_device, D3DRS_BLENDOP, D3DBLENDOP_ADD);
	pixel_shader.final_combiner_inputs_efg = 0x1400;
	rasterizer_set_pixel_shader(&pixel_shader);

	IDirect3DDevice8_Begin(global_d3d_device, D3DPT_TRIANGLEFAN);
	IDirect3DDevice8_SetVertexDataColor(global_d3d_device, 9, argb);
	IDirect3DDevice8_SetVertexData2s(global_d3d_device, 0, x0, y0);
	IDirect3DDevice8_SetVertexData2s(global_d3d_device, 0, x1, y0);
	IDirect3DDevice8_SetVertexData2s(global_d3d_device, 0, x1, y1);
	IDirect3DDevice8_SetVertexData2s(global_d3d_device, 0, x0, y1);
	IDirect3DDevice8_End(global_d3d_device);
	_rasterizer_debug_immediate_end_screenspace();
}

int forge_is_multiplayer_session(void)
{
	short conn = game_connection();
	return (conn == _game_connection_network_client || conn == _game_connection_network_server) ? 1 : 0;
}

static int is_allowed_forge_light_or_device_tag(uint32_t grp, char const *tag_path)
{
	static struct
	{
		uint32_t grp;
		char const *path;
	} const s_allowed[] = {
		/* Functioning Beam Emitters ('mach', animated emitter + beam effect + hum sound + light) */
		{ 'mach', "levels\\a30\\devices\\beam emitter\\beam emitter" },
		{ 'mach', "levels\\a30\\devices\\beam emitter\\beam emitter red" },
		/* 12 Light Fixtures ('lifi') with 3D fixture models, lens flares, and dynamic illumination */
		{ 'lifi', "levels\\a50\\devices\\light_ceiling\\bright_light_ceiling" },
		{ 'lifi', "levels\\a50\\devices\\light_ceiling\\light_ceiling" },
		{ 'lifi', "levels\\a50\\devices\\light_ceiling_large\\light_ceiling_large" },
		{ 'lifi', "levels\\a50\\devices\\light_ground\\light_ground" },
		{ 'lifi', "levels\\b30\\devices\\light fixtures\\light fixture ceiling1\\light fixture ceiling1" },
		{ 'lifi', "levels\\b30\\devices\\light fixtures\\light fixture floor1\\light fixture floor1" },
		{ 'lifi', "levels\\b30\\devices\\light fixtures\\light fixture wall1\\light fixture wall1" },
		{ 'lifi', "levels\\b40\\devices\\b40_spotlight\\b40_spotlight" },
		{ 'lifi', "levels\\c10\\devices\\ground_bright\\ground_bright" },
		{ 'lifi', "levels\\test\\carousel\\devices\\car_spotlight\\car_spotlight" },
		{ 'lifi', "levels\\test\\damnation\\devices\\dam_spotlight\\dam_spotlight" },
		{ 'lifi', "levels\\test\\damnation\\devices\\light_ceiling\\mp bright_light_ceiling" },
		/* 5 Dynamic Point Light Scenery tags ('scen') with 3D meshes and colored dynamic lights */
		{ 'scen', "levels\\c10\\scenery\\cyan point light\\light cyan dynamic" },
		{ 'scen', "levels\\c10\\scenery\\green point light\\light green dynamic" },
		{ 'scen', "levels\\c10\\scenery\\orange point light\\light orange dynamic" },
		{ 'scen', "levels\\c10\\scenery\\red point light\\light red dynamic" },
		{ 'scen', "levels\\b40\\scenery\\b40_ctorch\\ctorch" }
	};
	size_t i;

	if (!tag_path || !tag_path[0])
		return 0;
	if ((grp == 0 || grp == 'mach') &&
		(strstr(tag_path, "door") != NULL || strstr(tag_path, "Door") != NULL || strstr(tag_path, "DOOR") != NULL))
	{
		if (strstr(tag_path, "doorblinker") == NULL && strstr(tag_path, "door_blast_collision") == NULL)
			return 1;
	}
	for (i = 0; i < sizeof(s_allowed) / sizeof(s_allowed[0]); i++)
	{
		if ((grp == 0 || grp == s_allowed[i].grp) &&
			_stricmp(tag_path, s_allowed[i].path) == 0)
		{
			return 1;
		}
	}
	return 0;
}

static int is_forge_door_tag(uint32_t grp, char const *tag_path)
{
	if (!tag_path || !tag_path[0])
		return 0;
	if ((grp == 0 || grp == 'mach') &&
		(strstr(tag_path, "door") != NULL || strstr(tag_path, "Door") != NULL || strstr(tag_path, "DOOR") != NULL))
	{
		if (strstr(tag_path, "doorblinker") == NULL && strstr(tag_path, "door_blast_collision") == NULL)
			return 1;
	}
	return 0;
}

static int is_forge_light_beacon_emitter_tag(uint32_t grp, char const *tag_path)
{
	char lower_buf[260];
	size_t i;

	if (!tag_path || !tag_path[0])
		return 0;
	if (is_forge_door_tag(grp, tag_path))
		return 0;
	if (grp == 'lifi')
		return 1;
	if (grp != 0 && grp != 'scen' && grp != 'mach')
		return 0;

	for (i = 0; tag_path[i] && i + 1 < sizeof(lower_buf); i++)
	{
		char ch = tag_path[i];
		if (ch >= 'A' && ch <= 'Z')
			ch = (char)(ch - 'A' + 'a');
		lower_buf[i] = ch;
	}
	lower_buf[i] = '\0';

	if (strstr(lower_buf, "light") != NULL ||
		strstr(lower_buf, "spotlight") != NULL ||
		strstr(lower_buf, "beacon") != NULL ||
		strstr(lower_buf, "emitter") != NULL ||
		strstr(lower_buf, "ctorch") != NULL ||
		strstr(lower_buf, "torch") != NULL)
	{
		return 1;
	}
	return 0;
}

static int is_excluded_scenery_or_effect_tag(char const *tag_path)
{
	static char const *const s_excluded_substrings[] = {
		"emitter",
		"particle",
		"effect",
		"explosion",
		"exhaust",
		"lightning",
		"point light",
		"lens flare",
		"dynamic light",
		"nomodel",
		"hologram",
		"holo control",
		"doorblinker",
		"nutblocker",
		"door_blast_collision",
		"countdown timer",
		"temp critical",
		"teleporter_shield",
		"smoke",
		"spark",
		"steam",
		"flame",
		"flare",
		"blood_pool",
		"floor_arrow",
		"cutscene_",
		"lifepod_atmosphere_entry",
		"\\devices\\",
		"scenery\\cortana_chip\\cortana_chip",
		"scenery\\baton\\baton",
		"scenery\\tubewire\\tubewire",
		"scenery\\debris\\rocks_smoldering\\rocks_smoldering",
		"levels\\b40\\scenery\\b40_cpanel\\b40_cpanel",
		"scenery\\helmet_chip\\helmet_chip",
		"scenery\\lockpick\\lockpick",
		"levels\\c10\\scenery\\c10_smallplant\\c10_smallplant",
		"levels\\c10\\scenery\\c10_lilypad\\c10_lilypad",
		"levels\\c10\\scenery\\c10_lilypadbunch\\c10_lilypadbunch",
		"levels\\c10\\scenery\\c10_lilypadcluster\\c10_lilypadcluster",
		"levels\\c10\\scenery\\c10_plantclump\\c10_plantclump",
		"levels\\c10\\scenery\\c10_lilypadbunchb\\c10_lilypadbunchb",
		"levels\\c10\\scenery\\c10_lilypadclusterb\\c10_lilypadclusterb",
		"scenery\\baton\\baton dynamic",
		"scenery\\index\\index",
		"scenery\\index_hold\\index_hold",
		"levels\\c40\\scenery\\c40_snowbushsmall_dark\\c40_snowbushsmall_dark",
		"scenery\\hilltop\\hilltop",
		"scenery\\captain_implant\\captain_implant",
		"flood_captain"
	};
	char lower_buf[260];
	size_t i, k;

	if (!tag_path || !tag_path[0])
		return 1;
	if (is_allowed_forge_light_or_device_tag(0, tag_path))
		return 0;
	if (_strnicmp(tag_path, "cinematics\\", 11) == 0)
		return 1;

	for (i = 0; tag_path[i] && i + 1 < sizeof(lower_buf); i++)
	{
		char ch = tag_path[i];
		if (ch >= 'A' && ch <= 'Z')
			ch = (char)(ch - 'A' + 'a');
		lower_buf[i] = ch;
	}
	lower_buf[i] = '\0';

	for (k = 0; k < sizeof(s_excluded_substrings) / sizeof(s_excluded_substrings[0]); k++)
	{
		if (strstr(lower_buf, s_excluded_substrings[k]) != NULL)
			return 1;
	}
	return 0;
}

static int is_excluded_weapon_or_equipment_tag(char const *tag_path)
{
	static char const *const s_excluded_weap_eqip[] = {
		"gravity rifle",
		"gravity_rifle",
		"full-spectrum",
		"full spectrum",
		"full_spectrum",
		"plasma_cannon",
		"plasma cannon",
		"fuel rod",
		"fuel_rod",
		"double speed",
		"double_speed",
		"target_laser",
		"target laser",
		"energy sword",
		"energy_sword",
		"\\flag",
		"flag\\",
		"\\ball",
		"ball\\"
	};
	char lower_buf[260];
	size_t i, k;

	if (!tag_path || !tag_path[0])
		return 1;

	for (i = 0; tag_path[i] && i + 1 < sizeof(lower_buf); i++)
	{
		char ch = tag_path[i];
		if (ch >= 'A' && ch <= 'Z')
			ch = (char)(ch - 'A' + 'a');
		lower_buf[i] = ch;
	}
	lower_buf[i] = '\0';

	for (k = 0; k < sizeof(s_excluded_weap_eqip) / sizeof(s_excluded_weap_eqip[0]); k++)
	{
		if (strstr(lower_buf, s_excluded_weap_eqip[k]) != NULL)
			return 1;
	}
	return 0;
}

int forge_catalog_entry_allowed(struct forge_catalog_entry const *entry)
{
	if (!entry)
		return 0;
	if (entry->source_cache_id == 2)
		return 1;
	/* Disable default stock bipeds, AI characters, and control devices */
	if (entry->category == _forge_cat_bipeds ||
	    entry->category == _forge_cat_ai_squads ||
	    entry->group_tag == 'bipd' ||
	    entry->group_tag == 'actv' ||
	    entry->group_tag == 'ctrl')
	{
		return 0;
	}
	/* Doors have their own dedicated DOORS category (_forge_cat_barriers_devices) */
	if (entry->category == _forge_cat_barriers_devices)
	{
		return is_forge_door_tag(entry->group_tag, entry->tag_path);
	}
	if (is_forge_door_tag(entry->group_tag, entry->tag_path))
	{
		return (entry->category == _forge_cat_barriers_devices) ? 1 : 0;
	}
	/* Lights, beacons, and emitters have their own dedicated LIGHTS category (_forge_cat_lights) */
	if (entry->category == _forge_cat_lights)
	{
		if (!is_forge_light_beacon_emitter_tag(entry->group_tag, entry->tag_path))
			return 0;
	}
	else if (is_forge_light_beacon_emitter_tag(entry->group_tag, entry->tag_path))
	{
		return 0;
	}
	if (entry->group_tag == 'mach' || entry->group_tag == 'lifi')
	{
		if (entry->source_cache_id == 2 || entry->source_cache_id == 1)
			return 1;
		return is_allowed_forge_light_or_device_tag(entry->group_tag, entry->tag_path);
	}
	/* Allow all custom .ocetags packages (source_cache_id == 2) and injected donor map tags (source_cache_id == 1) */
	if (entry->source_cache_id == 2 || entry->source_cache_id == 1)
		return 1;
	/* Exclude gravity rifle, full spectrum vision, fuel rod (plasma_cannon), and double speed */
	if ((entry->group_tag == 'weap' || entry->group_tag == 'eqip' ||
	     entry->category == _forge_cat_weapons || entry->category == _forge_cat_equipment) &&
	    is_excluded_weapon_or_equipment_tag(entry->tag_path))
	{
		return 0;
	}
	/* Exclude all emitters, particle effects, point lights, lens flares, and device scenery props */
	if (entry->group_tag == 'scen' && is_excluded_scenery_or_effect_tag(entry->tag_path))
	{
		return 0;
	}
	/* Allow standard vehicles and Pelican in the vehicle spawn list */
	if (entry->group_tag == 'vehi' || entry->category == _forge_cat_vehicles)
	{
		static char const *const s_allowed_vehicles[] = {
			"vehicles\\warthog\\warthog",
			"vehicles\\warthog\\mp_warthog",
			"vehicles\\rwarthog\\rwarthog",
			"vehicles\\ghost\\ghost",
			"vehicles\\banshee\\banshee",
			"vehicles\\banshee\\banshee_mp",
			"vehicles\\scorpion\\scorpion",
			"vehicles\\pelican\\pelican",
			"vehicles\\c_gun_turret\\c_gun_turret",
			"vehicles\\c_gun_turret\\c_gun_turret_mp",
			"vehicles\\c gun turret\\c gun turret",
			"vehicles\\c gun turret\\c gun turret_mp"
		};
		int v, allowed = 0;
		for (v = 0; v < (int)(sizeof(s_allowed_vehicles) / sizeof(s_allowed_vehicles[0])); v++)
		{
			if (_stricmp(entry->tag_path, s_allowed_vehicles[v]) == 0)
			{
				allowed = 1;
				break;
			}
		}
		if (!allowed)
			return 0;
	}
	return 1;
}

static void make_short_display_name(char const *tag_path, char const *prefix, char *out, size_t out_sz)
{
	char const *leaf = tag_path;
	char const *p;
	for (p = tag_path; *p; p++)
	{
		if (*p == '\\' || *p == '/')
			leaf = p + 1;
	}
	if (prefix && (_stricmp(prefix, "[c]") == 0))
		snprintf(out, out_sz, "[C] %s", leaf);
	else if (prefix && prefix[0] == '[')
		snprintf(out, out_sz, "%s", prefix);
	else if (prefix && prefix[0])
		snprintf(out, out_sz, "%s %s", prefix, leaf);
	else
		snprintf(out, out_sz, "%s", leaf);
}

static void add_catalog_entry(
	uint32_t group_tag,
	uint8_t category,
	uint8_t source_cache_id,
	char const *tag_path,
	char const *display_prefix)
{
	int i;
	struct forge_catalog_entry *e;

	if (!tag_path || !tag_path[0] || g_forge.catalog_count >= FORGE_MAX_CATALOG_ENTRIES)
		return;

	for (i = 0; i < g_forge.catalog_count; i++)
	{
		if (g_forge.catalog[i].group_tag == group_tag &&
		    g_forge.catalog[i].category == category &&
		    _stricmp(g_forge.catalog[i].tag_path, tag_path) == 0)
		{
			if (source_cache_id == 2)
			{
				g_forge.catalog[i].source_cache_id = 2;
				make_short_display_name(tag_path, display_prefix, g_forge.catalog[i].display_name, sizeof(g_forge.catalog[i].display_name));
			}
			return;
		}
	}

	e = &g_forge.catalog[g_forge.catalog_count++];
	e->group_tag = group_tag;
	e->category = category;
	e->source_cache_id = source_cache_id;
	e->pad = 0;
	snprintf(e->tag_path, sizeof(e->tag_path), "%s", tag_path);
	make_short_display_name(tag_path, display_prefix, e->display_name, sizeof(e->display_name));
}

static void scan_cached_tags_manifest(char const *manifest_path)
{
	FILE *f = fopen(manifest_path, "r");
	char line[512];
	if (!f)
		return;
	while (fgets(line, sizeof(line), f))
	{
		char grp_str[16];
		int cat_id = 0;
		int src_id = 0;
		char tag_path[256];
		char custom_label[128];
		char prefix_buf[140];
		uint32_t grp = 0;
		char *nl;
		char *bar;

		nl = strchr(line, '\r');
		if (nl) *nl = '\0';
		nl = strchr(line, '\n');
		if (nl) *nl = '\0';
		if (line[0] == '#' || line[0] == '\0')
			continue;

		custom_label[0] = '\0';
		if (sscanf(line, "%4s|%d|%d|%255[^\n]", grp_str, &cat_id, &src_id, tag_path) == 4)
		{
			bar = strchr(tag_path, '|');
			if (bar)
			{
				*bar = '\0';
				snprintf(custom_label, sizeof(custom_label), "%s", bar + 1);
			}
			if (strlen(grp_str) == 4 && tag_path[0] != '\0')
			{
				grp = ((uint32_t)(uint8_t)grp_str[0] << 24) |
				      ((uint32_t)(uint8_t)grp_str[1] << 16) |
				      ((uint32_t)(uint8_t)grp_str[2] << 8) |
				      ((uint32_t)(uint8_t)grp_str[3]);
				if (grp == 'ctrl')
					continue;
				if ((grp == 'mach' || grp == 'lifi') &&
					src_id != 2 && src_id != 1 &&
					!is_allowed_forge_light_or_device_tag(grp, tag_path))
				{
					continue;
				}
				if (src_id != 2 && grp == 'scen' && is_excluded_scenery_or_effect_tag(tag_path))
					continue;
				if (src_id != 2 && grp == 'vehi' && _stricmp(tag_path, "vehicles\\wraith\\wraith") == 0)
					continue;
				if (src_id != 2 && (grp == 'weap' || grp == 'eqip') && is_excluded_weapon_or_equipment_tag(tag_path))
					continue;

				/* Map group tag directly to the separated WEAPONS vs EQUIPMENT vs VEHICLES vs SCENERY vs DOORS vs LIGHTS tabs */
				if (grp == 'weap')
					cat_id = _forge_cat_weapons;
				else if (grp == 'eqip')
					cat_id = _forge_cat_equipment;
				else if (grp == 'vehi')
					cat_id = _forge_cat_vehicles;
				else if (is_forge_door_tag(grp, tag_path))
					cat_id = _forge_cat_barriers_devices;
				else if (is_forge_light_beacon_emitter_tag(grp, tag_path))
					cat_id = _forge_cat_lights;
				else if (grp == 'scen' || grp == 'mach' || grp == 'lifi')
					cat_id = _forge_cat_scenery;
				else if (grp == 'bipd')
					cat_id = _forge_cat_bipeds;
				else if (grp == 'actv')
					cat_id = _forge_cat_ai_squads;
				else
					continue;

				if (src_id == 2)
				{
					if (custom_label[0])
					{
						if (_strnicmp(custom_label, "[c]", 3) == 0)
							snprintf(prefix_buf, sizeof(prefix_buf), "%s", custom_label);
						else
							snprintf(prefix_buf, sizeof(prefix_buf), "[C] %s", custom_label);
					}
					else
					{
						snprintf(prefix_buf, sizeof(prefix_buf), "[C]");
					}
					add_catalog_entry(grp, (uint8_t)cat_id, 2, tag_path, prefix_buf);
				}
				else
				{
					add_catalog_entry(
						grp,
						(uint8_t)cat_id,
						(uint8_t)src_id,
						tag_path,
						src_id ? (grp == 'actv' ? "[CUSTOM AI]" : "[CUSTOM]") : (grp == 'actv' ? "[AI]" : ""));
				}
			}
		}
	}
	fclose(f);
}

static void scan_donor_maps_directory(char const *dir_path)
{
	char pattern[512];
	WIN32_FIND_DATAA fd;
	HANDLE h;

	snprintf(pattern, sizeof(pattern), "%s\\*.map", dir_path);
	h = FindFirstFileA(pattern, &fd);
	if (h == INVALID_HANDLE_VALUE)
		return;
	do
	{
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			continue;
		if (_stricmp(fd.cFileName, "bitmaps.map") == 0 ||
			_stricmp(fd.cFileName, "sounds.map") == 0 ||
			_stricmp(fd.cFileName, "loc.map") == 0 ||
			_stricmp(fd.cFileName, "forgeasset.map") == 0 ||
			_stricmp(fd.cFileName, "customforgeasset.map") == 0)
		{
			continue;
		}
		if (g_forge.donor_map_count < FORGE_MAX_DONOR_MAPS_LIST)
		{
			snprintf(
				g_forge.donor_map_files[g_forge.donor_map_count++],
				sizeof(g_forge.donor_map_files[0]),
				"%s/%s",
				dir_path,
				fd.cFileName);
		}
	} while (FindNextFileA(h, &fd));
	CloseHandle(h);
}

static int forge_scan_and_rip_ocetags_folders(int force_reextract);

static void scan_ocetags_directory_into_donor_list(char const *dir_path)
{
	char pattern[512];
	WIN32_FIND_DATAA fd;
	HANDLE h;

	snprintf(pattern, sizeof(pattern), "%s\\*.ocetags", dir_path);
	h = FindFirstFileA(pattern, &fd);
	if (h == INVALID_HANDLE_VALUE)
		return;
	do
	{
		char cand[256];
		int i, dup = 0;
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			continue;
		snprintf(cand, sizeof(cand), "%s/%s", dir_path, fd.cFileName);
		for (i = 0; i < g_forge.donor_map_count; i++)
		{
			char const *s1 = strrchr(g_forge.donor_map_files[i], '/');
			char const *s2 = strrchr(g_forge.donor_map_files[i], '\\');
			char const *leaf = s1 > s2 ? s1 + 1 : (s2 ? s2 + 1 : g_forge.donor_map_files[i]);
			if (_stricmp(leaf, fd.cFileName) == 0)
			{
				dup = 1;
				break;
			}
		}
		if (!dup && g_forge.donor_map_count < FORGE_MAX_DONOR_MAPS_LIST)
		{
			snprintf(
				g_forge.donor_map_files[g_forge.donor_map_count++],
				sizeof(g_forge.donor_map_files[0]),
				"%s",
				cand);
		}
	} while (FindNextFileA(h, &fd));
	CloseHandle(h);
}

static void scan_faber_tag_vault_into_catalog(void)
{
	long prof_count, p, tag_count, t;

	port_tag_profiles_scan();
	prof_count = port_tag_profiles_count();
	for (p = 0; p < prof_count; p++)
	{
		tag_count = port_tag_profiles_tag_count(p);
		for (t = 0; t < tag_count; t++)
		{
			uint32_t grp;
			char const *tag_path;
			char const *custom_name;
			char prefix_buf[140];
			uint8_t cat_id;

			if (!port_tag_profiles_tag_enabled(p, t))
				continue;
			grp = (uint32_t)port_tag_profiles_tag_group(p, t);
			tag_path = port_tag_profiles_tag_name(p, t);
			if (!tag_path || !tag_path[0])
				continue;

			if (grp == 'weap')
				cat_id = _forge_cat_weapons;
			else if (grp == 'eqip')
				cat_id = _forge_cat_equipment;
			else if (grp == 'vehi')
				cat_id = _forge_cat_vehicles;
			else if (grp == 'bipd')
				cat_id = _forge_cat_bipeds;
			else if (grp == 'actv')
				cat_id = _forge_cat_ai_squads;
			else if (grp == 'lifi')
				cat_id = _forge_cat_lights;
			else if (grp == 'mach' || grp == 'ctrl')
				cat_id = is_forge_light_beacon_emitter_tag(grp, tag_path) ? _forge_cat_lights : _forge_cat_barriers_devices;
			else if (grp == 'scen')
				cat_id = is_forge_light_beacon_emitter_tag(grp, tag_path) ? _forge_cat_lights : _forge_cat_scenery;
			else
				continue;

			custom_name = port_tag_profiles_tag_option(p, t, "name");
			if (custom_name && custom_name[0])
			{
				if (_strnicmp(custom_name, "[c]", 3) == 0)
					snprintf(prefix_buf, sizeof(prefix_buf), "%s", custom_name);
				else
					snprintf(prefix_buf, sizeof(prefix_buf), "[C] %s", custom_name);
			}
			else
			{
				snprintf(prefix_buf, sizeof(prefix_buf), "[C]");
			}
			add_catalog_entry(grp, cat_id, 2, tag_path, prefix_buf);
		}
	}
}

void forge_runtime_reload_catalogs(void)
{
	g_forge.catalog_count = 0;

	/* Built-in Spawns & Teleporters */
	add_catalog_entry('spwn', _forge_cat_spawns_teleporters, 0, "spawn\\player_red", "[SPAWN] Red Team Spawn");
	add_catalog_entry('spwn', _forge_cat_spawns_teleporters, 0, "spawn\\player_blue", "[SPAWN] Blue Team Spawn");
	add_catalog_entry('spwn', _forge_cat_spawns_teleporters, 0, "spawn\\player_ffa", "[SPAWN] FFA / Neutral Spawn");
	add_catalog_entry('tele', _forge_cat_spawns_teleporters, 0, "teleporter\\origin", "[TELEPORTER] Origin Node");
	add_catalog_entry('tele', _forge_cat_spawns_teleporters, 0, "teleporter\\destination", "[TELEPORTER] Destination Node");

	/* Built-in Netgame Objectives */
	add_catalog_entry('flag', _forge_cat_objectives, 0, "objective\\ctf_flag_red", "[CTF] Red Flag Base");
	add_catalog_entry('flag', _forge_cat_objectives, 0, "objective\\ctf_flag_blue", "[CTF] Blue Flag Base");
	add_catalog_entry('flag', _forge_cat_objectives, 0, "objective\\oddball_spawn", "[ODDBALL] Ball Spawn Marker");
	add_catalog_entry('flag', _forge_cat_objectives, 0, "objective\\koth_hill", "[KOTH] Hill Marker");
	add_catalog_entry('flag', _forge_cat_objectives, 0, "objective\\race_track", "[RACE] Race Marker / Checkpoint");

	/* 1. Scan custom .ocetags packages in OpenCE Forge/custom_tags/ (if not yet indexed) */
	forge_scan_and_rip_ocetags_folders(0);

	/* 2. Load Custom .ocetags catalog first (so [c] items appear at the top of each category), then Xbox default catalog */
	(void)scan_donor_maps_directory;
	scan_faber_tag_vault_into_catalog();
	scan_cached_tags_manifest("OpenCE Forge/cached_tags/custom_catalog.txt");
	scan_cached_tags_manifest("OpenCE Forge/cached_tags/xbox_catalog.txt");

	/* 3. Stable-sort custom .ocetags entries (source_cache_id == 2) to the front of g_forge.catalog */
	if (g_forge.catalog_count > 10)
	{
		struct forge_catalog_entry *sorted =
			(struct forge_catalog_entry *)malloc((size_t)g_forge.catalog_count * sizeof(struct forge_catalog_entry));
		if (sorted)
		{
			int i, out_n = 0;
			for (i = 0; i < g_forge.catalog_count; i++)
			{
				if (g_forge.catalog[i].source_cache_id == 2)
					sorted[out_n++] = g_forge.catalog[i];
			}
			for (i = 0; i < g_forge.catalog_count; i++)
			{
				if (g_forge.catalog[i].source_cache_id == 1)
					sorted[out_n++] = g_forge.catalog[i];
			}
			for (i = 0; i < g_forge.catalog_count; i++)
			{
				if (g_forge.catalog[i].source_cache_id != 2 && g_forge.catalog[i].source_cache_id != 1)
					sorted[out_n++] = g_forge.catalog[i];
			}
			memcpy(g_forge.catalog, sorted, (size_t)out_n * sizeof(struct forge_catalog_entry));
			free(sorted);
		}
	}

	/* Refresh custom_tags .ocetags list */
	g_forge.donor_map_count = 0;
	snprintf(
		g_forge.donor_map_files[g_forge.donor_map_count++],
		sizeof(g_forge.donor_map_files[0]),
		"__OCETAGS_FOLDER__");
	scan_ocetags_directory_into_donor_list("OpenCE Forge/custom_tags");
	scan_ocetags_directory_into_donor_list("custom_tags");
}

#ifdef _WIN32
int __stdcall CreateDirectoryA(char const *lpPathName, void *lpSecurityAttributes);
__declspec(dllimport) int __stdcall MessageBoxA(void *hWnd, char const *lpText, char const *lpCaption, unsigned int uType);
__declspec(dllimport) void *__stdcall LoadLibraryA(char const *lpLibFileName);
__declspec(dllimport) void *__stdcall GetProcAddress(void *hModule, char const *lpProcName);

struct forge_browseinfo_a
{
	void *hwndOwner;
	void *pidlRoot;
	char *pszDisplayName;
	char const *lpszTitle;
	unsigned int ulFlags;
	void *lpfn;
	long lParam;
	int iImage;
};

static int forge_browse_for_folder(char const *title, char const *default_dir, char *out_dir, size_t out_sz)
{
	typedef void *(__stdcall *sh_browse_fn)(struct forge_browseinfo_a *);
	typedef int (__stdcall *sh_get_path_fn)(void *, char *);
	void *shell32 = LoadLibraryA("shell32.dll");
	if (shell32)
	{
		sh_browse_fn pBrowse = (sh_browse_fn)GetProcAddress(shell32, "SHBrowseForFolderA");
		sh_get_path_fn pGetPath = (sh_get_path_fn)GetProcAddress(shell32, "SHGetPathFromIDListA");
		if (pBrowse && pGetPath)
		{
			char disp[260];
			char chosen[260];
			struct forge_browseinfo_a bi;
			void *pidl;
			memset(disp, 0, sizeof(disp));
			memset(chosen, 0, sizeof(chosen));
			memset(&bi, 0, sizeof(bi));
			bi.pszDisplayName = disp;
			bi.lpszTitle = title;
			bi.ulFlags = 0x00000001u | 0x00000040u; /* BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE */
			pidl = pBrowse(&bi);
			if (pidl && pGetPath(pidl, chosen) && chosen[0])
			{
				snprintf(out_dir, out_sz, "%s", chosen);
				return 1;
			}
		}
	}
	snprintf(out_dir, out_sz, "%s", default_dir);
	return 0;
}

#endif

static int is_xbox_version5_map_file(char const *map_path)
{
	FILE *fp = fopen(map_path, "rb");
	uint32_t hdr[2];
	if (!fp)
		return 0;
	if (fread(hdr, 1, sizeof(hdr), fp) != sizeof(hdr))
	{
		fclose(fp);
		return 0;
	}
	fclose(fp);
	return (hdr[0] == 0x68656164u /* 'head' */ && hdr[1] == 5u) ? 1 : 0;
}

/*
 * Checks if a pre-existing decompressed Xbox cache file already exists for `map_path`
 * in OpenCE's Xbox hard-drive cache (z:\cache000.map..z:\cache005.map or z:\cache_donor.map)
 * or if `map_path` itself is already uncompressed on disk.
 * If not, decompresses the Xbox v5 map payload using zlib into OpenCE's z:\cache_donor.map.
 */
static int ensure_xbox_map_decompressed_cache(
	char const *map_path,
	char *out_cache_path,
	size_t out_cache_sz)
{
	FILE *in_fp;
	uint8_t hdr[2048];
	uint32_t head_magic, map_version, decomp_len, tag_off, tag_sz;
	char map_internal_name[36];
	int slot;

	if (!map_path || !map_path[0] || !out_cache_path || out_cache_sz == 0)
		return 0;
	out_cache_path[0] = 0;

	in_fp = fopen(map_path, "rb");
	if (!in_fp)
		return 0;
	if (fread(hdr, 1, sizeof(hdr), in_fp) != sizeof(hdr))
	{
		fclose(in_fp);
		return 0;
	}

	head_magic = *(uint32_t const *)(hdr + 0x00);
	map_version = *(uint32_t const *)(hdr + 0x04);
	decomp_len = *(uint32_t const *)(hdr + 0x08);
	tag_off = *(uint32_t const *)(hdr + 0x10);
	tag_sz = *(uint32_t const *)(hdr + 0x14);
	memset(map_internal_name, 0, sizeof(map_internal_name));
	memcpy(map_internal_name, hdr + 0x20, 31);

	if (head_magic != 0x68656164u || map_version != 5u ||
		tag_off < 2048u || tag_sz < 64u || tag_sz > 64u * 1024u * 1024u)
	{
		fclose(in_fp);
		return 0;
	}

	/* 0. Check if OpenCE Forge/cached_tags/xbox_cache/<map_internal_name>.map already holds this decompressed Xbox map! */
	if (map_internal_name[0])
	{
		char xcache_path[512];
		FILE *xf;
		snprintf(xcache_path, sizeof(xcache_path), "OpenCE Forge/cached_tags/xbox_cache/%s.map", map_internal_name);
		xf = fopen(xcache_path, "rb");
		if (xf)
		{
			uint8_t xhdr[2048];
			long xsz = 0;
			fseek(xf, 0, SEEK_END);
			xsz = ftell(xf);
			fseek(xf, 0, SEEK_SET);
			if (xsz >= (long)(tag_off + tag_sz) && fread(xhdr, 1, sizeof(xhdr), xf) == sizeof(xhdr))
			{
				if (*(uint32_t const *)(xhdr + 0x00) == 0x68656164u &&
					*(uint32_t const *)(xhdr + 0x04) == 5u &&
					*(uint32_t const *)(xhdr + 0x10) == tag_off &&
					*(uint32_t const *)(xhdr + 0x14) == tag_sz)
				{
					fclose(xf);
					fclose(in_fp);
					snprintf(out_cache_path, out_cache_sz, "%s", xcache_path);
					return 1;
				}
			}
			fclose(xf);
		}
	}

	/* 1. Check if OpenCE's Xbox hard-drive cache (z:\cache000.map .. z:\cache005.map or z:\cache_donor.map)
	 * already holds this decompressed Xbox map; if so, use it directly without duplicating! */
	for (slot = 0; slot <= 6; slot++)
	{
		char z_xbox[64];
		char z_path[512];
		FILE *zf;
		if (slot < 6)
			snprintf(z_xbox, sizeof(z_xbox), "z:\\cache00%d.map", slot);
		else
			snprintf(z_xbox, sizeof(z_xbox), "z:\\cache_donor.map");
		platform_translate_path(z_xbox, z_path, sizeof(z_path));
		zf = fopen(z_path, "rb");
		if (zf)
		{
			uint8_t zhdr[2048];
			long zsz = 0;
			fseek(zf, 0, SEEK_END);
			zsz = ftell(zf);
			fseek(zf, 0, SEEK_SET);
			if (zsz >= (long)(tag_off + tag_sz) && fread(zhdr, 1, sizeof(zhdr), zf) == sizeof(zhdr))
			{
				if (*(uint32_t const *)(zhdr + 0x00) == 0x68656164u &&
					*(uint32_t const *)(zhdr + 0x04) == 5u &&
					*(uint32_t const *)(zhdr + 0x10) == tag_off &&
					*(uint32_t const *)(zhdr + 0x14) == tag_sz &&
					_strnicmp((char const *)(zhdr + 0x20), map_internal_name, 31) == 0)
				{
					fclose(zf);
					fclose(in_fp);
					snprintf(out_cache_path, out_cache_sz, "%s", z_path);
					return 1;
				}
			}
			fclose(zf);
		}
	}

	/* 2. Check if map_path itself is already uncompressed on disk; if so, read it directly in-place! */
	{
		long in_sz = 0;
		fseek(in_fp, 0, SEEK_END);
		in_sz = ftell(in_fp);
		if (decomp_len >= tag_off + tag_sz && in_sz >= (long)decomp_len)
		{
			uint32_t tag_sig = 0;
			if (fseek(in_fp, (long)(tag_off + 0x20u), SEEK_SET) == 0 &&
				fread(&tag_sig, 4, 1, in_fp) == 1 &&
				tag_sig == 0x626C616Du /* 'blam' */)
			{
				fclose(in_fp);
				snprintf(out_cache_path, out_cache_sz, "%s", map_path);
				return 1;
			}
		}
		fseek(in_fp, 2048L, SEEK_SET);
	}

	/* 3. Stream-decompress Xbox v5 map into OpenCE Forge/cached_tags/xbox_cache/<map_internal_name>.map
	 * so each Xbox map is only decompressed once and never overwrites a single shared donor file! */
#ifdef _WIN32
	CreateDirectoryA("OpenCE Forge", NULL);
	CreateDirectoryA("OpenCE Forge/cached_tags", NULL);
	CreateDirectoryA("OpenCE Forge/cached_tags/xbox_cache", NULL);
#endif
	if (map_internal_name[0])
		snprintf(out_cache_path, out_cache_sz, "OpenCE Forge/cached_tags/xbox_cache/%s.map", map_internal_name);
	else
		platform_translate_path("z:\\cache_donor.map", out_cache_path, out_cache_sz);
	{
		FILE *tmp_out = fopen(out_cache_path, "wb");
		z_stream strm;
		uint8_t *in_chunk;
		uint8_t *out_chunk;
		size_t const chunk_sz = 262144u;
		int zret = Z_OK;
		int decomp_ok = 0;

		if (!tmp_out)
		{
			platform_translate_path("z:\\cache_donor.map", out_cache_path, out_cache_sz);
			tmp_out = fopen(out_cache_path, "wb");
		}
		if (!tmp_out)
		{
			fclose(in_fp);
			return 0;
		}
		fwrite(hdr, 1, sizeof(hdr), tmp_out);

		in_chunk = (uint8_t *)malloc(chunk_sz);
		out_chunk = (uint8_t *)malloc(chunk_sz);
		memset(&strm, 0, sizeof(strm));
		if (in_chunk && out_chunk && inflateInit(&strm) == Z_OK)
		{
			decomp_ok = 1;
			do
			{
				strm.avail_in = (uInt)fread(in_chunk, 1, chunk_sz, in_fp);
				if (ferror(in_fp))
				{
					decomp_ok = 0;
					break;
				}
				if (strm.avail_in == 0)
					break;
				strm.next_in = in_chunk;
				do
				{
					size_t have;
					strm.avail_out = (uInt)chunk_sz;
					strm.next_out = out_chunk;
					zret = inflate(&strm, Z_NO_FLUSH);
					if (zret == Z_NEED_DICT || zret == Z_DATA_ERROR || zret == Z_MEM_ERROR)
					{
						decomp_ok = 0;
						break;
					}
					have = chunk_sz - (size_t)strm.avail_out;
					if (have > 0 && fwrite(out_chunk, 1, have, tmp_out) != have)
					{
						decomp_ok = 0;
						break;
					}
					if (s_forge_loading_active && s_forge_loading_map_span > 0.0f)
					{
						unsigned long now_ms = system_milliseconds();
						if (now_ms - s_forge_loading_last_ms >= 15u)
						{
							float map_frac = (decomp_len > 2048u)
								? ((float)strm.total_out / (float)(decomp_len - 2048u))
								: 0.5f;
							if (map_frac < 0.0f) map_frac = 0.0f;
							if (map_frac > 0.99f) map_frac = 0.99f;
							forge_xbox_loading_screen_render_frame(
								s_forge_loading_map_base + map_frac * s_forge_loading_map_span);
						}
					}
				} while (strm.avail_out == 0);
			} while (decomp_ok && zret != Z_STREAM_END);
			inflateEnd(&strm);
		}
		free(in_chunk);
		free(out_chunk);
		fclose(tmp_out);
		fclose(in_fp);

		if (!decomp_ok)
		{
			remove(out_cache_path);
			out_cache_path[0] = 0;
			return 0;
		}
	}
	return 1;
}

static int forge_index_xbox_map_file_to_catalog(
	char const *map_path,
	FILE *xbox_manifest_fp,
	FILE *legacy_manifest_fp,
	FILE *credits_fp)
{
	char cache_path[512];
	FILE *f;
	uint8_t hdr[2048];
	uint32_t tag_off, tag_sz, inst_addr, tag_base, inst_off;
	int32_t tag_count, i;
	uint8_t *tag_buf;
	int indexed_count = 0;
	char map_internal_name[64];

	if (!ensure_xbox_map_decompressed_cache(map_path, cache_path, sizeof(cache_path)))
		return 0;

	f = fopen(cache_path, "rb");
	if (!f)
		return 0;
	if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr))
	{
		fclose(f);
		return 0;
	}
	tag_off = *(uint32_t const *)(hdr + 0x10);
	tag_sz = *(uint32_t const *)(hdr + 0x14);
	memset(map_internal_name, 0, sizeof(map_internal_name));
	memcpy(map_internal_name, hdr + 0x20, 31);

	if (tag_off < 2048u || tag_sz < 64u || tag_sz > 64u * 1024u * 1024u)
	{
		fclose(f);
		return 0;
	}
	tag_buf = (uint8_t *)malloc(tag_sz);
	if (!tag_buf || fseek(f, (long)tag_off, SEEK_SET) != 0 || fread(tag_buf, 1, tag_sz, f) != tag_sz)
	{
		free(tag_buf);
		fclose(f);
		return 0;
	}
	fclose(f);

	inst_addr = *(uint32_t const *)(tag_buf + 0x00);
	tag_count = *(int32_t const *)(tag_buf + 0x0C);
	tag_base = inst_addr - 0x24u;
	inst_off = 0x24u;
	if (tag_count <= 0 || tag_count > 16384 || inst_off + (uint32_t)tag_count * 32u > tag_sz)
	{
		free(tag_buf);
		return 0;
	}

	if (credits_fp)
	{
		fprintf(credits_fp, "## Xbox Map: `%s` (`%s`)\n",
			map_internal_name[0] ? map_internal_name : map_path, cache_path);
	}

	for (i = 0; i < tag_count; i++)
	{
		uint32_t io = inst_off + (uint32_t)i * 32u;
		uint32_t grp = *(uint32_t const *)(tag_buf + io + 0x00);
		uint32_t name_addr = *(uint32_t const *)(tag_buf + io + 0x10);
		uint32_t data_addr = *(uint32_t const *)(tag_buf + io + 0x14);
		char const *tname;
		int cat_id = -1;
		char grp_str[5];

		if (name_addr < tag_base || (name_addr - tag_base) >= tag_sz)
			continue;
		tname = (char const *)(tag_buf + (name_addr - tag_base));
		if (!tname[0] || is_excluded_scenery_or_effect_tag(tname))
			continue;

		if (grp == 'weap')
		{
			if (!is_excluded_weapon_or_equipment_tag(tname))
				cat_id = _forge_cat_weapons;
		}
		else if (grp == 'eqip')
		{
			if (!is_excluded_weapon_or_equipment_tag(tname))
				cat_id = _forge_cat_equipment;
		}
		else if (grp == 'vehi')
		{
			if (_stricmp(tname, "vehicles\\wraith\\wraith") != 0)
				cat_id = _forge_cat_vehicles;
		}
		else if (is_forge_door_tag(grp, tname))
			cat_id = _forge_cat_barriers_devices;
		else if (is_forge_light_beacon_emitter_tag(grp, tname) &&
				 (grp == 'scen' || is_allowed_forge_light_or_device_tag(grp, tname)))
			cat_id = _forge_cat_lights;
		else if (grp == 'scen')
			cat_id = _forge_cat_scenery;
		else if ((grp == 'mach' || grp == 'lifi') && is_allowed_forge_light_or_device_tag(grp, tname))
			cat_id = _forge_cat_scenery;

		if (cat_id < 0 || data_addr < tag_base || (data_addr - tag_base) + 0x38u > tag_sz)
			continue;

		/* Verify the object has a valid 'mode' model reference in the Xbox map (or is a point-light scenery tag) */
		if (!is_allowed_forge_light_or_device_tag(grp, tname))
		{
			uint32_t obj_off = data_addr - tag_base;
			int32_t mod_idx = *(int32_t const *)(tag_buf + obj_off + 0x34);
			if (mod_idx == -1 || (mod_idx & 0xFFFF) >= tag_count)
				continue;
		}

		grp_str[0] = (char)((grp >> 24) & 0xFF);
		grp_str[1] = (char)((grp >> 16) & 0xFF);
		grp_str[2] = (char)((grp >> 8) & 0xFF);
		grp_str[3] = (char)(grp & 0xFF);
		grp_str[4] = '\0';

		if (xbox_manifest_fp)
			fprintf(xbox_manifest_fp, "%s|%d|0|%s\n", grp_str, cat_id, tname);
		if (credits_fp)
			fprintf(credits_fp, "- `%s` (`%s`)\n", tname, grp_str);
		indexed_count++;

		if (grp == 'mach' && _stricmp(tname, "levels\\a30\\devices\\beam emitter\\beam emitter") == 0)
		{
			char const *red_tname = "levels\\a30\\devices\\beam emitter\\beam emitter red";
			if (xbox_manifest_fp)
				fprintf(xbox_manifest_fp, "mach|%d|0|%s\n", cat_id, red_tname);
			if (credits_fp)
				fprintf(credits_fp, "- `%s` (`mach`)\n", red_tname);
			indexed_count++;
		}
	}

	if (credits_fp)
		fprintf(credits_fp, "\n");
	free(tag_buf);
	(void)legacy_manifest_fp;
	return indexed_count;
}

static int forge_rip_maps_folder_to_cached_tags(char const *maps_dir)
{
#ifdef _WIN32
	char pattern[512];
	WIN32_FIND_DATAA fd;
	HANDLE h;
	FILE *xbox_manifest_fp;
	FILE *credits_fp;
	char map_files[64][512];
	int map_file_count = 0;
	int m;
	int ripped_maps = 0;
	int total_assets = 0;

	snprintf(pattern, sizeof(pattern), "%s\\*.map", maps_dir);
	h = FindFirstFileA(pattern, &fd);
	if (h == INVALID_HANDLE_VALUE)
		return 0;

	do
	{
		char full_path[512];
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			continue;
		if (_stricmp(fd.cFileName, "bitmaps.map") == 0 ||
			_stricmp(fd.cFileName, "sounds.map") == 0 ||
			_stricmp(fd.cFileName, "loc.map") == 0 ||
			_stricmp(fd.cFileName, "ui.map") == 0 ||
			_stricmp(fd.cFileName, "forgeasset.map") == 0 ||
			_stricmp(fd.cFileName, "customforgeasset.map") == 0 ||
			_stricmp(fd.cFileName, "_temp_xbox_decomp.map") == 0)
		{
			continue;
		}
		snprintf(full_path, sizeof(full_path), "%s/%s", maps_dir, fd.cFileName);
		/* Strictly pull default tags from Xbox maps (version == 5) ONLY! */
		if (!is_xbox_version5_map_file(full_path))
			continue;
		if (map_file_count < 64)
			snprintf(map_files[map_file_count++], sizeof(map_files[0]), "%s", full_path);
	} while (FindNextFileA(h, &fd));
	CloseHandle(h);

	if (map_file_count <= 0)
		return 0;

	remove("OpenCE Forge/cached_tags/catalog.txt");
	xbox_manifest_fp = fopen("OpenCE Forge/cached_tags/xbox_catalog.txt", "wb");
	if (!xbox_manifest_fp)
		return 0;
	fprintf(xbox_manifest_fp, "# OpenCE : Forge - Native Xbox Maps Tag Catalog (Built from Xbox maps in %s)\n", maps_dir);

	credits_fp = fopen("OpenCE Forge/credits.md", "wb");
	if (credits_fp)
	{
		fprintf(credits_fp, "# OpenCE : Forge - Native Xbox Map Tag Catalog\n\n");
		fprintf(credits_fp, "Indexed natively from Xbox maps in `%s/`.\n\n", maps_dir);
	}

	forge_xbox_loading_screen_begin();
	for (m = 0; m < map_file_count; m++)
	{
		int count_from_map;
		s_forge_loading_map_base = (float)m / (float)map_file_count;
		s_forge_loading_map_span = 1.0f / (float)map_file_count;
		forge_xbox_loading_screen_update(s_forge_loading_map_base + 0.05f * s_forge_loading_map_span);
		count_from_map = forge_index_xbox_map_file_to_catalog(
			map_files[m], xbox_manifest_fp, NULL, credits_fp);
		forge_xbox_loading_screen_update((float)(m + 1) / (float)map_file_count);
		if (count_from_map > 0)
		{
			ripped_maps++;
			total_assets += count_from_map;
		}
	}
	forge_xbox_loading_screen_end();

	{
		char donor_cache[512];
		platform_translate_path("z:\\cache_donor.map", donor_cache, sizeof(donor_cache));
		remove(donor_cache);
	}

	if (credits_fp)
		fclose(credits_fp);
	fclose(xbox_manifest_fp);
	return (ripped_maps > 0 && total_assets > 0) ? total_assets : 0;
#else
	(void)maps_dir;
	return 0;
#endif
}

static void forge_delete_files_matching(char const *dir_path, char const *pattern)
{
#ifdef _WIN32
	char search_pat[512];
	WIN32_FIND_DATAA fd;
	HANDLE h;
	snprintf(search_pat, sizeof(search_pat), "%s\\%s", dir_path, pattern);
	h = FindFirstFileA(search_pat, &fd);
	if (h == INVALID_HANDLE_VALUE)
		return;
	do
	{
		if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
		{
			char full[512];
			snprintf(full, sizeof(full), "%s/%s", dir_path, fd.cFileName);
			remove(full);
		}
	} while (FindNextFileA(h, &fd));
	CloseHandle(h);
#else
	(void)dir_path;
	(void)pattern;
#endif
}

static void close_runtime_xbox_donor(void);

int forge_reset_stock_tag_cache(void)
{
	int slot;

	close_runtime_xbox_donor();
	remove("OpenCE Forge/cached_tags/xbox_catalog.txt");
	remove("OpenCE Forge/cached_tags/catalog.txt");
	remove("OpenCE Forge/credits.md");
	forge_delete_files_matching("OpenCE Forge/cached_tags/xbox_cache", "*.map");
	forge_delete_files_matching("OpenCE Forge/cached_tags/markers", "m_*.*");

	for (slot = 0; slot <= 6; slot++)
	{
		char z_xbox[64];
		char z_path[512];
		if (slot < 6)
			snprintf(z_xbox, sizeof(z_xbox), "z:\\cache00%d.map", slot);
		else
			snprintf(z_xbox, sizeof(z_xbox), "z:\\cache_donor.map");
		platform_translate_path(z_xbox, z_path, sizeof(z_path));
		remove(z_path);
	}

	forge_runtime_reload_catalogs();
	forge_set_status("Stock Tag Cache Cleared!");
	return 1;
}

int forge_clear_custom_tag_cache(void)
{
	/* Delete all custom .ocetags packages and custom catalog cache */
	{
		long safety = 0;
		port_tag_profiles_scan();
		while (port_tag_profiles_count() > 0 && safety++ < 128)
		{
			if (!port_tag_profiles_delete(0))
				break;
		}
	}
	forge_delete_files_matching("OpenCE Forge/custom_tags", "*.fxtg");
	forge_delete_files_matching("custom_tags", "*.fxtg");
	forge_delete_files_matching("OpenCE Forge/custom_tags", "*.ocetags");
	forge_delete_files_matching("custom_tags", "*.ocetags");
	forge_delete_files_matching("OpenCE Forge/ocetags", "*.ocetags");
	forge_delete_files_matching("OpenCE Forge/cached_tags", "c_*.*");
	forge_delete_files_matching("OpenCE Forge/cached_tags/markers", "c_*.*");
	remove("OpenCE Forge/cached_tags/custom_catalog.txt");

	forge_runtime_reload_catalogs();
	forge_set_status("Cleared Custom Tag Cache! All custom .ocetags packages deleted.");
	return 1;
}

#ifdef _WIN32
struct forge_startupinfo_a
{
	uint32_t cb;
	char *lpReserved;
	char *lpDesktop;
	char *lpTitle;
	uint32_t dwX;
	uint32_t dwY;
	uint32_t dwXSize;
	uint32_t dwYSize;
	uint32_t dwXCountChars;
	uint32_t dwYCountChars;
	uint32_t dwFillAttribute;
	uint32_t dwFlags;
	uint16_t wShowWindow;
	uint16_t cbReserved2;
	uint8_t *lpReserved2;
	void *hStdInput;
	void *hStdOutput;
	void *hStdError;
};

struct forge_process_information
{
	void *hProcess;
	void *hThread;
	uint32_t dwProcessId;
	uint32_t dwThreadId;
};

extern uint32_t __stdcall GetModuleFileNameA(void *hModule, char *lpFilename, uint32_t nSize);
extern int __stdcall CreateProcessA(
	char const *lpApplicationName,
	char *lpCommandLine,
	void *lpProcessAttributes,
	void *lpThreadAttributes,
	int bInheritHandles,
	uint32_t dwCreationFlags,
	void *lpEnvironment,
	char const *lpCurrentDirectory,
	struct forge_startupinfo_a *lpStartupInfo,
	struct forge_process_information *lpProcessInformation);
extern void __stdcall ExitProcess(uint32_t uExitCode);
#endif

int forge_reset_all_tag_caches_and_restart(void)
{
#ifdef _WIN32
	FILE *flag_fp;
	char exe_path[512];
	char cmd_line[1024];
	struct forge_startupinfo_a si;
	struct forge_process_information pi;
	int slot;

	/* Clear all Xbox and custom tag catalogs & decompressed Xbox map cache */
	remove("OpenCE Forge/cached_tags/xbox_catalog.txt");
	remove("OpenCE Forge/cached_tags/catalog.txt");
	remove("OpenCE Forge/cached_tags/custom_catalog.txt");
	forge_delete_files_matching("OpenCE Forge/cached_tags/xbox_cache", "*.map");
	forge_delete_files_matching("OpenCE Forge/cached_tags", "c_*.*");

	/* Also clear OpenCE's 6-slot z:\cache000.map..z:\cache005.map ring buffer and z:\cache_donor.map */
	for (slot = 0; slot <= 6; slot++)
	{
		char z_xbox[64];
		char z_path[512];
		if (slot < 6)
			snprintf(z_xbox, sizeof(z_xbox), "z:\\cache00%d.map", slot);
		else
			snprintf(z_xbox, sizeof(z_xbox), "z:\\cache_donor.map");
		platform_translate_path(z_xbox, z_path, sizeof(z_path));
		remove(z_path);
	}

	CreateDirectoryA("OpenCE Forge", NULL);
	CreateDirectoryA("OpenCE Forge/cached_tags", NULL);
	flag_fp = fopen("OpenCE Forge/cached_tags/ask_rebuild_on_launch.flag", "wb");
	if (flag_fp)
	{
		fprintf(flag_fp, "1\n");
		fclose(flag_fp);
	}

	memset(exe_path, 0, sizeof(exe_path));
	if (GetModuleFileNameA(NULL, exe_path, (uint32_t)(sizeof(exe_path) - 1)) == 0 || !exe_path[0])
		snprintf(exe_path, sizeof(exe_path), "halo.exe");

	snprintf(cmd_line, sizeof(cmd_line),
		"cmd.exe /c \"ping 127.0.0.1 -n 2 >nul & start \"\" \"%s\"\"",
		exe_path);

	memset(&si, 0, sizeof(si));
	si.cb = (uint32_t)sizeof(si);
	memset(&pi, 0, sizeof(pi));
	if (CreateProcessA(
			NULL,
			cmd_line,
			NULL,
			NULL,
			0,
			0x08000000u /* CREATE_NO_WINDOW */,
			NULL,
			NULL,
			&si,
			&pi))
	{
		/* Do not call CloseHandle here: halo_windows_prefix.h maps CloseHandle
		 * to halo_xbox_CloseHandle, which expects an Xbox HANDLE, whereas
		 * ExitProcess(0) immediately closes all Win32 process/thread handles. */
	}
	ExitProcess(0);
	return 1;
#else
	return 0;
#endif
}

static void forge_ensure_local_tag_cache_built(void)
{
	int ask_before_build = 0;
	{
		FILE *flag_f = fopen("OpenCE Forge/cached_tags/ask_rebuild_on_launch.flag", "rb");
		if (flag_f)
		{
			fclose(flag_f);
			remove("OpenCE Forge/cached_tags/ask_rebuild_on_launch.flag");
			ask_before_build = 1;
		}
	}

	if (!ask_before_build)
	{
		FILE *f = fopen("OpenCE Forge/cached_tags/xbox_catalog.txt", "r");
		if (f)
		{
			char line[512];
			int has_base_tags = 0;
			int has_lights_and_door = 0;
			while (fgets(line, sizeof(line), f))
			{
				if (strstr(line, "|0|") != NULL)
					has_base_tags = 1;
				if (strstr(line, "beam emitter red") != NULL)
					has_lights_and_door = 1;
			}
			fclose(f);
			if (has_base_tags && has_lights_and_door)
				return;
		}
	}

#ifdef _WIN32
	CreateDirectoryA("OpenCE Forge", NULL);
	CreateDirectoryA("OpenCE Forge/custom_tags", NULL);
	CreateDirectoryA("OpenCE Forge/cached_tags", NULL);

	if (ask_before_build)
	{
		int resp = MessageBoxA(
			NULL,
			"OpenCE : Forge - Tag Cache Reset\n\n"
			"The Xbox tag cache has been cleared.\n\n"
			"Would you like to build the default Xbox map tag cache now?",
			"OpenCE : Forge - Build Xbox Tag Cache",
			0x00000004u | 0x00000040u /* MB_YESNO | MB_ICONINFORMATION */);
		if (resp != 6 /* IDYES */)
			return;
	}

	{
		int extracted_total = forge_rip_maps_folder_to_cached_tags("maps");
		if (extracted_total <= 0)
			extracted_total = forge_rip_maps_folder_to_cached_tags("FORGE CODE/GAME MAPS/XBOX MAPS");
		if (extracted_total <= 0)
			extracted_total = forge_rip_maps_folder_to_cached_tags("../FORGE CODE/GAME MAPS/XBOX MAPS");

		if (extracted_total <= 0)
		{
			int resp = MessageBoxA(
				NULL,
				"OpenCE : Forge - Startup Xbox Map Tag Cache\n\n"
				"No pre-existing Xbox map cache was found.\n\n"
				"Click Yes to select your Xbox 'maps/' folder and index the default Xbox Vehicle, Scenery, Weapon & Equipment tags.",
				"OpenCE : Forge - Xbox Map Tag Cache",
				0x00000004u | 0x00000040u /* MB_YESNO | MB_ICONINFORMATION */);
			if (resp == 6 /* IDYES */)
			{
				char maps_dir[260];
				snprintf(maps_dir, sizeof(maps_dir), "maps");
				forge_browse_for_folder(
					"Select your Halo Xbox 'maps' folder (containing Xbox .map files):",
					"maps",
					maps_dir,
					sizeof(maps_dir));
				extracted_total = forge_rip_maps_folder_to_cached_tags(maps_dir);
			}
		}
	}
#endif
}

void forge_runtime_init(void)
{
	if (g_forge.initialized)
		return;
	memset(&g_forge, 0, sizeof(g_forge));
	g_forge.initialized = 1;
	g_forge.mode = _forge_mode_play;
	g_forge.active_modal = _forge_modal_none;
	g_forge.current_category = _forge_cat_vehicles;
	g_forge.snap_mode = _forge_snap_15_deg;
	g_forge.ai_team_mode = _forge_ai_team_enemy_covenant;
	g_forge.cam_speed = 12.0f;
	g_forge.grabbed_object_index = NONE;
	g_forge.grabbed_live_index = -1;
	g_forge.hovered_object_index = NONE;
	g_forge.hovered_live_index = -1;
	g_forge.grab_distance = 5.0f;
	snprintf(g_forge.save_map_name, sizeof(g_forge.save_map_name), "forge_custom_arena");
	g_forge.save_map_name_len = (int)strlen(g_forge.save_map_name);

#ifdef _WIN32
	CreateDirectoryA("OpenCE Forge", NULL);
	CreateDirectoryA("OpenCE Forge/custom_tags", NULL);
#endif
	forge_ensure_local_tag_cache_built();
	forge_runtime_reload_catalogs();
}

/* ============================================================================
 * NATIVE XBOX MAP TAG LOADER & .OCETAGS CUSTOM TAG LOADER
 * Default tags are loaded natively from Xbox maps (using OpenCE's exact tag
 * schema & hardware vertex/triangle/texture registration).
 * Extra custom tags are loaded natively from .ocetags in custom_tags/.
 * ============================================================================ */

#define FORGE_MAX_HW_PARTS       48
#define FORGE_MAX_DYNAMIC_TAGS   1024
#define FORGE_MAX_PHYS_ALLOCS    8192
#define FORGE_MAX_DYN_SOUND_PERMS 4096

struct forge_hw_part
{
	struct vertex_buffer vertex_buffer;
	struct triangle_buffer triangle_buffer;
	char shader_hint[128];
	float u_scale;
	float v_scale;
};

struct forge_dynamic_tag_entry
{
	uint32_t group_tag;
	char tag_path[256];
	long definition_index;
	float bounding_radius;
	int part_count;
	struct forge_hw_part parts[FORGE_MAX_HW_PARTS];
};

static struct
{
	void *saved_base_instances;
	long saved_base_count;
	void *allocated_instances;
	int count;
	struct forge_dynamic_tag_entry entries[FORGE_MAX_DYNAMIC_TAGS];
} s_dyn_tags;

static struct
{
	int count;
	void *ptrs[FORGE_MAX_PHYS_ALLOCS];
} s_forge_phys_allocs;

static struct
{
	int count;
	struct sound_permutation const *perms[FORGE_MAX_DYN_SOUND_PERMS];
} s_dyn_sound_perms;

static void *forge_phys_alloc(size_t bytes)
{
	void *p;
	size_t alloc_sz = (bytes > 64u) ? bytes : 64u;
	p = XPhysicalAlloc((unsigned long)alloc_sz, (unsigned long)-1, 0, PAGE_READWRITE);
	if (p)
	{
		memset(p, 0, alloc_sz);
		if (s_forge_phys_allocs.count < FORGE_MAX_PHYS_ALLOCS)
			s_forge_phys_allocs.ptrs[s_forge_phys_allocs.count++] = p;
		return p;
	}
	return calloc(1, alloc_sz);
}

static void forge_register_dyn_sound_perm(struct sound_permutation const *perm)
{
	int i;
	if (!perm || !perm->samples.address || perm->samples.size <= 0)
		return;
	for (i = 0; i < s_dyn_sound_perms.count; i++)
	{
		if (s_dyn_sound_perms.perms[i] == perm)
			return;
	}
	if (s_dyn_sound_perms.count < FORGE_MAX_DYN_SOUND_PERMS)
		s_dyn_sound_perms.perms[s_dyn_sound_perms.count++] = perm;
}

boolean mod_sounds_read(
	struct sound_permutation const *permutation,
	void *buffer)
{
	int i;
	if (permutation && buffer)
	{
		for (i = 0; i < s_dyn_sound_perms.count; i++)
		{
			if (s_dyn_sound_perms.perms[i] == permutation &&
				permutation->samples.address &&
				permutation->samples.size > 0)
			{
				memcpy(buffer, permutation->samples.address, (size_t)permutation->samples.size);
				return TRUE;
			}
		}
	}
	return orig_mod_sounds_read(permutation, buffer);
}

static int s_marker_meshes_loaded_flag = 0;
static long s_monitor_bipd_handle = NONE;
static int s_monitor_resolved = 0;
static int s_local_monitor_drawn_this_frame = 0;
static int s_last_preview_chosen_idx = -1;
static unsigned long s_last_preview_cursor_ms = 0;

static struct
{
	char path[512];
	FILE *fp;
	uint8_t *tag_buf;
	uint32_t tag_off;
	uint32_t tag_sz;
} s_runtime_xbox_donor;

static void close_runtime_xbox_donor(void)
{
	if (s_runtime_xbox_donor.fp)
		fclose(s_runtime_xbox_donor.fp);
	if (s_runtime_xbox_donor.tag_buf)
		free(s_runtime_xbox_donor.tag_buf);
	memset(&s_runtime_xbox_donor, 0, sizeof(s_runtime_xbox_donor));
}

void forge_runtime_on_map_unload(void)
{
	int i;

	close_runtime_xbox_donor();

	/* Dispose injected Custom Edition model buffers FIRST while their gbxmodel
	 * part definitions in s_forge_phys_allocs are still mapped in memory! */
	custom_edition_injected_models_dispose();

	for (i = 0; i < s_forge_phys_allocs.count; i++)
	{
		if (s_forge_phys_allocs.ptrs[i])
			XPhysicalFree(s_forge_phys_allocs.ptrs[i]);
	}
	s_forge_phys_allocs.count = 0;
	s_dyn_sound_perms.count = 0;
	s_marker_meshes_loaded_flag = 0;
	s_monitor_bipd_handle = NONE;
	s_monitor_resolved = 0;
	s_local_monitor_drawn_this_frame = 0;
	s_last_preview_chosen_idx = -1;
	s_last_preview_cursor_ms = 0;

	if (s_dyn_tags.saved_base_instances)
	{
		cache_files_set_tag_instances(s_dyn_tags.saved_base_instances, s_dyn_tags.saved_base_count);
		s_dyn_tags.saved_base_instances = NULL;
		s_dyn_tags.saved_base_count = 0;
	}
	if (s_dyn_tags.allocated_instances)
	{
		free(s_dyn_tags.allocated_instances);
		s_dyn_tags.allocated_instances = NULL;
	}
	s_dyn_tags.count = 0;
	{
		char donor_cache[512];
		platform_translate_path("z:\\cache_donor.map", donor_cache, sizeof(donor_cache));
		remove(donor_cache);
	}
}

static void build_hw_part_from_arrays(
	struct forge_hw_part *dst_part,
	uint8_t *uncomp_verts,
	int32_t vtx_count,
	uint8_t const *idx_bytes,
	int32_t tri_count,
	char const *shader_hint,
	float scale_factor,
	float z_offset)
{
	uint32_t uncomp_sz;
	uint32_t comp_sz;
	uint32_t idx_sz;
	void *hw_verts;
	void *hw_tris;
	int v, c;

	/* Strip trailing 0xFFFF padding indices (from .gbxmodel 3-index model_triangle blocks) */
	if (idx_bytes && vtx_count >= 3)
	{
		uint16_t const *raw_idx = (uint16_t const *)idx_bytes;
		while (tri_count > 1 && raw_idx[tri_count + 1] >= (uint16_t)vtx_count)
			tri_count--;
	}
	if (vtx_count < 3 || tri_count < 1)
		return;

	uncomp_sz = (uint32_t)vtx_count * 68u;
	comp_sz = (uint32_t)vtx_count * 32u;
	idx_sz = ((uint32_t)tri_count + 2u) * 2u;

	for (v = 0; v < vtx_count; v++)
	{
		uint8_t *vp = uncomp_verts + (uint32_t)v * 68u;
		if (scale_factor != 1.0f || z_offset != 0.0f)
		{
			*(float *)(vp + 0) *= scale_factor;
			*(float *)(vp + 4) *= scale_factor;
			*(float *)(vp + 8) = (*(float *)(vp + 8) * scale_factor) + z_offset;
		}
		for (c = 0; c < 9; c++)
		{
			float val = *(float *)(vp + 12 + c * 4);
			if (!(val >= -1.0f && val <= 1.0f))
				*(float *)(vp + 12 + c * 4) = (c % 3 == 0) ? 1.0f : 0.0f;
		}
		*(int16_t *)(vp + 56) = 0;
		*(int16_t *)(vp + 58) = 0;
		*(float *)(vp + 60) = 1.0f;
		*(float *)(vp + 64) = 0.0f;
	}

	hw_verts = XPhysicalAlloc(comp_sz, (unsigned long)-1, 0, PAGE_READWRITE);
	if (!hw_verts)
		hw_verts = malloc(comp_sz);
	hw_tris = XPhysicalAlloc(idx_sz, (unsigned long)-1, 0, PAGE_READWRITE);
	if (!hw_tris)
		hw_tris = malloc(idx_sz);
	if (!hw_verts || !hw_tris)
		return;

	memset(dst_part, 0, sizeof(*dst_part));
	dst_part->u_scale = 1.0f;
	dst_part->v_scale = 1.0f;
	if (shader_hint && shader_hint[0])
	{
		char const *bar1 = strchr(shader_hint, '|');
		char const *bar2 = bar1 ? strchr(bar1 + 1, '|') : NULL;
		if (bar2)
		{
			float us = 1.0f, vs = 1.0f;
			size_t copy_len = (size_t)(bar2 - shader_hint);
			if (copy_len >= sizeof(dst_part->shader_hint))
				copy_len = sizeof(dst_part->shader_hint) - 1;
			memcpy(dst_part->shader_hint, shader_hint, copy_len);
			dst_part->shader_hint[copy_len] = '\0';
			if (sscanf(bar2 + 1, "%f,%f", &us, &vs) == 2)
			{
				if (us > 0.0001f && us < 10000.0f)
					dst_part->u_scale = us;
				if (vs > 0.0001f && vs < 10000.0f)
					dst_part->v_scale = vs;
			}
		}
		else
		{
			snprintf(dst_part->shader_hint, sizeof(dst_part->shader_hint), "%s", shader_hint);
		}
	}

	rasterizer_geometry_compress_vertices(
		_rasterizer_vertex_type_model_uncompressed,
		vtx_count,
		hw_verts,
		(long)comp_sz,
		uncomp_verts,
		(long)uncomp_sz);
	memcpy(hw_tris, idx_bytes, idx_sz);
	{
		uint16_t *tris_u16 = (uint16_t *)hw_tris;
		int32_t total_idx = tri_count + 2;
		int32_t k;
		for (k = 0; k < total_idx; k++)
		{
			if (tris_u16[k] >= (uint16_t)vtx_count)
				tris_u16[k] = (k > 0 && tris_u16[k - 1] < (uint16_t)vtx_count) ? tris_u16[k - 1] : 0u;
		}
	}

	rasterizer_vertex_buffer_new(
		&dst_part->vertex_buffer,
		_rasterizer_vertex_type_model_compressed,
		vtx_count,
		hw_verts,
		(long)comp_sz);

	rasterizer_triangle_buffer_new(
		&dst_part->triangle_buffer,
		_triangle_buffer_type_precompiled_strip,
		tri_count,
		hw_tris);
}

static int load_hw_parts_from_ftag_file(
	char const *ftag_path,
	struct forge_hw_part *out_parts,
	int max_parts,
	int *out_part_count,
	float *out_radius,
	float scale_factor,
	float z_offset)
{
	FILE *f = fopen(ftag_path, "rb");
	char magic[4];
	uint32_t version = 0, grp = 0, part_count = 0, p;
	float radius = 1.5f;
	char tag_name[256];

	if (!f)
		return 0;
	if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "FTAG", 4) != 0)
	{
		fclose(f);
		return 0;
	}
	if (fread(&version, 4, 1, f) != 1 || version != 1 ||
		fread(&grp, 4, 1, f) != 1 ||
		fread(&radius, 4, 1, f) != 1 ||
		fread(tag_name, 1, 256, f) != 256 ||
		fread(&part_count, 4, 1, f) != 1)
	{
		fclose(f);
		return 0;
	}

	*out_part_count = 0;
	if (out_radius)
		*out_radius = (radius > 0.1f && radius < 100.0f) ? radius : 1.5f;

	for (p = 0; p < part_count && *out_part_count < max_parts; p++)
	{
		int32_t vtx_count = 0, tri_count = 0;
		char shader_hint[128];
		uint32_t uncomp_sz, idx_sz;
		uint8_t *uncomp_verts;
		uint8_t *idx_bytes;

		if (fread(&vtx_count, 4, 1, f) != 1 ||
			fread(&tri_count, 4, 1, f) != 1 ||
			fread(shader_hint, 1, 128, f) != 128)
		{
			break;
		}
		if (vtx_count <= 0 || tri_count <= 0 || vtx_count > 32768 || tri_count > 32768)
			break;

		uncomp_sz = (uint32_t)vtx_count * 68u;
		idx_sz = ((uint32_t)tri_count + 2u) * 2u;
		uncomp_verts = (uint8_t *)malloc(uncomp_sz);
		idx_bytes = (uint8_t *)malloc(idx_sz);
		if (!uncomp_verts || !idx_bytes)
		{
			free(uncomp_verts);
			free(idx_bytes);
			break;
		}
		if (fread(uncomp_verts, 1, uncomp_sz, f) != uncomp_sz ||
			fread(idx_bytes, 1, idx_sz, f) != idx_sz)
		{
			free(uncomp_verts);
			free(idx_bytes);
			break;
		}

		build_hw_part_from_arrays(
			&out_parts[(*out_part_count)++],
			uncomp_verts,
			vtx_count,
			idx_bytes,
			tri_count,
			shader_hint,
			scale_factor,
			z_offset);

		free(uncomp_verts);
		free(idx_bytes);
	}

	fclose(f);
	return (*out_part_count > 0) ? 1 : 0;
}

static long append_runtime_tag_instance(
	long group_tag,
	long parent0,
	long parent1,
	char const *tag_path,
	void *base_address)
{
	long existing_count = 0;
	struct forge_cache_file_tag_instance *instances;
	struct forge_cache_file_tag_instance *grown;
	long max_salt = 0xE174;
	long i, new_abs_idx, new_tag_index;
	char *name_copy;

	instances = (struct forge_cache_file_tag_instance *)cache_files_tag_instances(&existing_count);
	if (!instances || existing_count <= 0 || existing_count >= 65000)
		return NONE;

	if (!s_dyn_tags.saved_base_instances)
	{
		s_dyn_tags.saved_base_instances = instances;
		s_dyn_tags.saved_base_count = existing_count;
	}

	grown = (struct forge_cache_file_tag_instance *)malloc((size_t)(existing_count + 1) * sizeof(*grown));
	name_copy = (char *)malloc(strlen(tag_path) + 1);
	if (!grown || !name_copy)
	{
		free(grown);
		free(name_copy);
		return NONE;
	}
	memcpy(grown, instances, (size_t)existing_count * sizeof(*grown));
	strcpy(name_copy, tag_path);

	for (i = 0; i < existing_count; i++)
	{
		if (instances[i].tag_index != NONE)
		{
			long s = (long)(((unsigned long)instances[i].tag_index >> 16) & 0x7FFFu);
			if (s > max_salt)
				max_salt = s;
		}
	}

	new_abs_idx = existing_count;
	new_tag_index = ((max_salt + 1) << 16) | (new_abs_idx & 0xFFFF);

	memset(&grown[new_abs_idx], 0, sizeof(grown[new_abs_idx]));
	grown[new_abs_idx].group_tag = group_tag;
	grown[new_abs_idx].parent_group_tags[0] = parent0;
	grown[new_abs_idx].parent_group_tags[1] = parent1;
	grown[new_abs_idx].tag_index = new_tag_index;
	grown[new_abs_idx].name = name_copy;
	grown[new_abs_idx].base_address = base_address;

	cache_files_set_tag_instances(grown, existing_count + 1);
	if (s_dyn_tags.allocated_instances)
		free(s_dyn_tags.allocated_instances);
	s_dyn_tags.allocated_instances = grown;

	return new_tag_index;
}

uint32_t forge_get_base_map_tag_count(void)
{
	long existing_count = 0;
	if (s_dyn_tags.saved_base_count > 0)
		return (uint32_t)s_dyn_tags.saved_base_count;
	if (cache_files_tag_instances(&existing_count) && existing_count > 0)
		return (uint32_t)existing_count;
	return 0;
}

static int is_valid_cyborg_driver_seat_label(char const *label)
{
	if (!label || !label[0])
		return 0;
	if (_stricmp(label, "W-driver") == 0 ||
		_stricmp(label, "G-driver") == 0 ||
		_stricmp(label, "B-driver") == 0 ||
		_stricmp(label, "S-driver") == 0 ||
		_stricmp(label, "R-driver") == 0 ||
		_stricmp(label, "scorpion-driver") == 0 ||
		_stricmp(label, "CT-driver") == 0)
		return 1;
	return 0;
}

static void patch_vehicle_tag_for_player_driving(long vehi_tag_idx, char const *tag_path)
{
	struct unit_definition *ud;

	if (vehi_tag_idx == NONE)
		return;

	ud = (struct unit_definition *)tag_get('vehi', vehi_tag_idx);
	if (ud)
	{
		ud->unit.flags |= FLAG(_unit_has_entrance_points_inside_bounding_sphere_bit);
		if (ud->unit.seats.count > 0 && ud->unit.seats.address)
		{
			struct unit_seat *seat0 = (struct unit_seat *)ud->unit.seats.address;
			seat0->flags &= ~FLAG(_unit_seat_invisible_bit);
			seat0->flags |= FLAG(_unit_seat_driver_bit) |
							FLAG(_unit_seat_third_person_camera_bit) |
							FLAG(_unit_seat_third_person_on_enter_bit);
			if (tag_path && strstr(tag_path, "wraith"))
				seat0->flags |= FLAG(_unit_seat_gunner_bit);
			if (!is_valid_cyborg_driver_seat_label(seat0->label))
				snprintf(seat0->label, sizeof(seat0->label), "W-driver");
		}
	}
}

static void patch_light_definition_buffer(uint8_t *lbuf)
{
	if (!lbuf)
		return;
	/* Bit 0 (_light_definition_dynamic_bit = 1) must be set and radius_modifier_upper_bound > 0
	 * for Halo's object_lights.c to illuminate surrounding world geometry and objects! */
	*(uint32_t *)(lbuf + 0x00) |= 1u;
	if (*(float const *)(lbuf + 0x04) < 3.5f)
		*(float *)(lbuf + 0x04) = 4.5f;
	if (*(float const *)(lbuf + 0x08) == 0.0f && *(float const *)(lbuf + 0x0C) == 0.0f)
	{
		*(float *)(lbuf + 0x08) = 1.0f;
		*(float *)(lbuf + 0x0C) = 1.0f;
	}
	if (*(float const *)(lbuf + 0x4C) == 0.0f &&
		*(float const *)(lbuf + 0x50) == 0.0f &&
		*(float const *)(lbuf + 0x54) == 0.0f)
	{
		*(float *)(lbuf + 0x38) = 1.0f;
		*(float *)(lbuf + 0x3C) = 1.0f;
		*(float *)(lbuf + 0x40) = 1.0f;
		*(float *)(lbuf + 0x44) = 1.0f;
		*(float *)(lbuf + 0x48) = 1.0f;
		*(float *)(lbuf + 0x4C) = 1.0f;
		*(float *)(lbuf + 0x50) = 1.0f;
		*(float *)(lbuf + 0x54) = 1.0f;
	}
}

static void patch_forge_device_and_light_tag_runtime(long tag_idx, uint32_t grp, char const *tag_path)
{
	uint8_t *obuf;
	int32_t att_cnt, a;
	uint8_t *att_base;

	(void)tag_path;
	if (tag_idx == NONE)
		return;
	if (grp != 'scen' && grp != 'mach' && grp != 'lifi' && grp != 'ctrl')
		return;

	obuf = (uint8_t *)tag_get((long)grp, tag_idx);
	if (!obuf)
		return;

	/* Ensure point lights and fixtures have a non-zero bounding radius so Forge raycasting and visibility work */
	if (*(float const *)(obuf + 0x04u) < 0.45f)
		*(float *)(obuf + 0x04u) = 0.45f;
	if (*(float const *)(obuf + 0x104u) < 1.5f)
		*(float *)(obuf + 0x104u) = 1.5f;

	if (grp == 'mach' && tag_path &&
		(_stricmp(tag_path, "levels\\a30\\devices\\beam emitter\\beam emitter") == 0 ||
		 _stricmp(tag_path, "levels\\a30\\devices\\beam emitter\\beam emitter red") == 0))
	{
		*(float *)(obuf + 0x04u) = 65.0f;  /* bounding_radius */
		*(float *)(obuf + 0x10u) = 0.0f;   /* bounding_offset.z */
		*(float *)(obuf + 0x104u) = 65.0f; /* render_bounding_radius */
	}

	/* Patch attached 'ligh' tags so placed lights, spotlights, and point lights cast dynamic illumination */
	att_cnt = *(int32_t const *)(obuf + 0x138u);
	att_base = *(uint8_t *const *)(obuf + 0x13Cu);
	for (a = 0; a < att_cnt && att_base; a++)
	{
		uint8_t *att = att_base + (size_t)a * 72u;
		uint32_t agrp = *(uint32_t const *)(att + 0x00u);
		int32_t aidx = *(int32_t const *)(att + 0x0Cu);
		if (agrp == 'ligh' && aidx != NONE)
		{
			uint8_t *lbuf = (uint8_t *)tag_get('ligh', aidx);
			if (lbuf)
				patch_light_definition_buffer(lbuf);
		}
	}

	/* Configure 'mach' doors to open/close automatically when players or vehicles approach */
	if (grp == 'mach')
	{
		int16_t mtype = *(int16_t const *)(obuf + 0x290u);
		if (mtype == 0 /* _machine_type_door */ ||
			(tag_path && (strstr(tag_path, "door") != NULL || strstr(tag_path, "Door") != NULL || strstr(tag_path, "DOOR") != NULL)))
		{
			*(int16_t *)(obuf + 0x290u) = 0;    /* _machine_type_door */
			*(float *)(obuf + 0x21Cu) = 2.5f;   /* device.automatic_activation_radius */
			*(uint16_t *)(obuf + 0x292u) |= 0x0003u; /* _machine_is_pathfinding_obstacle_bit | _machine_is_not_pathfinding_obstacle_when_open_bit */
			*(float *)(obuf + 0x294u) = 3.0f;   /* machine.door_open_time */
			*(int32_t *)(obuf + 0x320u) = 90;   /* machine.runtime_door_open_ticks (3.0f * 30) */
		}
	}
}

static long find_native_or_mp_variant_tag(uint32_t group_tag, char const *tag_path)
{
	long idx;
	long i;

	idx = tag_loaded((long)group_tag, tag_path);
	if (idx != NONE)
	{
		if (group_tag == 'vehi')
			patch_vehicle_tag_for_player_driving(idx, tag_path);
		else if (group_tag == 'scen' || group_tag == 'mach' || group_tag == 'lifi' || group_tag == 'ctrl')
			patch_forge_device_and_light_tag_runtime(idx, group_tag, tag_path);
		return idx;
	}

	/* Map between SP and MP vehicle/weapon names natively present in MP maps */
	if (group_tag == 'vehi')
	{
		static struct { char const *a; char const *b; } const vehi_pairs[] = {
			{ "vehicles\\warthog\\warthog", "vehicles\\warthog\\mp_warthog" },
			{ "vehicles\\ghost\\ghost", "vehicles\\ghost\\ghost_mp" },
			{ "vehicles\\scorpion\\scorpion", "vehicles\\scorpion\\scorpion_mp" },
			{ "vehicles\\banshee\\banshee", "vehicles\\banshee\\banshee_mp" },
			{ "vehicles\\c_gun_turret\\c_gun_turret", "vehicles\\c_gun_turret\\c_gun_turret_mp" },
			{ "vehicles\\c gun turret\\c gun turret", "vehicles\\c gun turret\\c gun turret_mp" },
			{ "vehicles\\c_gun_turret\\c_gun_turret", "vehicles\\c gun turret\\c gun turret_mp" },
			{ "vehicles\\c gun turret\\c gun turret", "vehicles\\c_gun_turret\\c_gun_turret_mp" }
		};
		for (i = 0; i < (long)(sizeof(vehi_pairs) / sizeof(vehi_pairs[0])); i++)
		{
			if (_stricmp(tag_path, vehi_pairs[i].a) == 0)
			{
				idx = tag_loaded('vehi', vehi_pairs[i].b);
				if (idx != NONE) return idx;
			}
			else if (_stricmp(tag_path, vehi_pairs[i].b) == 0)
			{
				idx = tag_loaded('vehi', vehi_pairs[i].a);
				if (idx != NONE) return idx;
			}
		}
	}
	return NONE;
}

static struct tag_schema_group const *forge_runtime_schema_group_get(uint32_t group_tag)
{
	struct tag_schema_group const *const *list;
	struct tag_schema_group const *group;

	if (group_tag == 'mod2')
	{
		for (group = tag_schema_custom_edition_groups; group->group_tag != 0; group++)
		{
			if (group->group_tag == 'mod2')
				return group;
		}
	}

	for (list = tag_schema_group_lists; *list != NULL; list++)
	{
		for (group = *list; group->group_tag != 0; group++)
		{
			if (group->group_tag == (unsigned long)group_tag)
				return group;
		}
	}
	return NULL;
}

static long load_native_ocetags_package_runtime(uint32_t req_group, char const *req_path, uint32_t *out_actual_group);

struct xbox_runtime_closure
{
	uint8_t const *tag_buf;
	uint32_t tag_sz;
	uint32_t tag_base;
	uint32_t inst_off;
	int32_t tag_count;
	uint8_t *visited;
	int32_t *order;
	int32_t order_count;
	long *runtime_handles;
	uint8_t **cloned_roots;
	int is_red_beam_variant;
};

static void xbox_collect_schema_dependencies(
	struct xbox_runtime_closure *cl,
	struct tag_schema_definition const *def,
	uint32_t base_off,
	int32_t elem_count,
	int depth);

static void xbox_visit_tag_index(struct xbox_runtime_closure *cl, int32_t abs_idx)
{
	uint32_t io, grp, name_addr, data_addr;
	char const *tname;
	struct tag_schema_group const *schema;

	if (abs_idx < 0 || abs_idx >= cl->tag_count)
		return;
	if (cl->visited[abs_idx])
		return;
	cl->visited[abs_idx] = 1;

	io = cl->inst_off + (uint32_t)abs_idx * 32u;
	if (io + 32u > cl->tag_sz)
		return;
	grp = *(uint32_t const *)(cl->tag_buf + io + 0x00);
	name_addr = *(uint32_t const *)(cl->tag_buf + io + 0x10);
	data_addr = *(uint32_t const *)(cl->tag_buf + io + 0x14);
	if (name_addr < cl->tag_base || (name_addr - cl->tag_base) >= cl->tag_sz)
		return;
	tname = (char const *)(cl->tag_buf + (name_addr - cl->tag_base));

	if (grp == 'udlg' || grp == 'sbsp' || grp == 'scnr')
	{
		cl->runtime_handles[abs_idx] = NONE;
		return;
	}

	/* Reuse any dependency tag that was already dynamically injected during this session,
	 * or reuse shared non-visual/non-audio base-map dependencies on stock Xbox maps. */
	if (cl->order_count > 0 && !cl->is_red_beam_variant)
	{
		long ex = tag_loaded((long)grp, tname);
		if (ex != NONE)
		{
			long base_cnt = (long)forge_get_base_map_tag_count();
			if ((base_cnt > 0 && (ex & 0xFFFF) >= base_cnt) ||
				(!custom_edition_cache_tags_loaded() &&
				 grp != 'bitm' && grp != 'mode' && grp != 'snd!' && grp != 'lsnd' &&
				 grp != 'shdr' && grp != 'senv' && grp != 'soso' && grp != 'sotr' &&
				 grp != 'schi' && grp != 'scex' && grp != 'swat' && grp != 'sgla' &&
				 grp != 'smet' && grp != 'spla'))
			{
				cl->runtime_handles[abs_idx] = ex;
				return;
			}
		}
	}

	if (data_addr < cl->tag_base || (data_addr - cl->tag_base) >= cl->tag_sz)
		return;

	schema = forge_runtime_schema_group_get(grp);
	if (!schema || !schema->definition || schema->definition->size <= 0)
		return;
	if ((data_addr - cl->tag_base) + (uint32_t)schema->definition->size > cl->tag_sz)
		return;

	cl->order[cl->order_count++] = abs_idx;
	xbox_collect_schema_dependencies(cl, schema->definition, data_addr - cl->tag_base, 1, 1);
}

static void xbox_collect_schema_dependencies(
	struct xbox_runtime_closure *cl,
	struct tag_schema_definition const *def,
	uint32_t base_off,
	int32_t elem_count,
	int depth)
{
	int32_t elem_idx;
	struct tag_schema_field const *field;
	uint32_t elem_size;

	if (!def || !def->fields || elem_count <= 0 || def->size <= 0 || depth > 10)
		return;
	elem_size = (uint32_t)def->size;
	if ((uint64_t)base_off + (uint64_t)elem_count * elem_size > cl->tag_sz)
		return;

	for (elem_idx = 0; elem_idx < elem_count; elem_idx++)
	{
		uint32_t elem_off = base_off + (uint32_t)elem_idx * elem_size;
		for (field = def->fields; field->type != _tag_schema_terminator; field++)
		{
			int16_t k;
			int16_t fcount = field->count > 0 ? field->count : 1;
			uint32_t fstride = field->size > 0 ? (uint32_t)field->size : 0;

			for (k = 0; k < fcount; k++)
			{
				uint32_t f_off = elem_off + (uint32_t)field->offset + (uint32_t)k * fstride;
				if (field->type == _tag_schema_struct)
				{
					struct tag_schema_definition const *sub_def =
						(struct tag_schema_definition const *)field->definition;
					if (sub_def && f_off + (uint32_t)sub_def->size <= elem_off + elem_size)
						xbox_collect_schema_dependencies(cl, sub_def, f_off, 1, depth + 1);
				}
				else if (field->type == _tag_schema_block)
				{
					struct tag_schema_definition const *child_def =
						(struct tag_schema_definition const *)field->definition;
					if (child_def && child_def->size > 0 && f_off + 12u <= elem_off + elem_size)
					{
						int32_t cnt = *(int32_t const *)(cl->tag_buf + f_off + 0u);
						uint32_t addr = *(uint32_t const *)(cl->tag_buf + f_off + 4u);
						int32_t max_cnt = (field->maximum > 0) ? (int32_t)field->maximum : 32768;
						if (cnt > 0 && cnt <= max_cnt && addr >= cl->tag_base &&
							(uint64_t)(addr - cl->tag_base) + (uint64_t)cnt * (uint32_t)child_def->size <= cl->tag_sz)
						{
							xbox_collect_schema_dependencies(cl, child_def, addr - cl->tag_base, cnt, depth + 1);
						}
					}
				}
				else if (field->type == _tag_schema_reference)
				{
					if (f_off + 16u <= elem_off + elem_size)
					{
						int32_t ref_idx = *(int32_t const *)(cl->tag_buf + f_off + 12u);
						if (ref_idx != -1 && (ref_idx & 0xFFFF) < cl->tag_count)
							xbox_visit_tag_index(cl, ref_idx & 0xFFFF);
					}
				}
			}
		}
	}
}

static void xbox_relocate_schema_elements(
	struct xbox_runtime_closure *cl,
	struct tag_schema_definition const *def,
	uint8_t *dst_base,
	uint32_t src_base_off,
	int32_t elem_count,
	int depth)
{
	int32_t elem_idx;
	struct tag_schema_field const *field;
	uint32_t elem_size;

	if (!def || !def->fields || !dst_base || elem_count <= 0 || def->size <= 0 || depth > 10)
		return;
	elem_size = (uint32_t)def->size;

	for (elem_idx = 0; elem_idx < elem_count; elem_idx++)
	{
		uint8_t *elem_ptr = dst_base + (size_t)elem_idx * elem_size;
		uint32_t src_elem_off = src_base_off + (uint32_t)elem_idx * elem_size;

		for (field = def->fields; field->type != _tag_schema_terminator; field++)
		{
			int16_t k;
			int16_t fcount = field->count > 0 ? field->count : 1;
			uint32_t fstride = field->size > 0 ? (uint32_t)field->size : 0;

			for (k = 0; k < fcount; k++)
			{
				uint32_t f_rel = (uint32_t)field->offset + (uint32_t)k * fstride;
				uint32_t src_f_off = src_elem_off + f_rel;

				if (field->type == _tag_schema_struct)
				{
					struct tag_schema_definition const *sub_def =
						(struct tag_schema_definition const *)field->definition;
					if (sub_def && f_rel + (uint32_t)sub_def->size <= elem_size)
					{
						xbox_relocate_schema_elements(
							cl, sub_def, elem_ptr + f_rel, src_f_off, 1, depth + 1);
					}
				}
				else if (field->type == _tag_schema_block)
				{
					struct tag_schema_definition const *child_def =
						(struct tag_schema_definition const *)field->definition;
					if (child_def && child_def->size > 0 && f_rel + 12u <= elem_size)
					{
						struct tag_block *blk = (struct tag_block *)(elem_ptr + f_rel);
						int32_t cnt = *(int32_t const *)(cl->tag_buf + src_f_off + 0u);
						uint32_t addr = *(uint32_t const *)(cl->tag_buf + src_f_off + 4u);
						uint32_t sub_elem_sz = (uint32_t)child_def->size;
						int32_t max_cnt = (field->maximum > 0) ? (int32_t)field->maximum : 32768;

						blk->definition = NULL;
						if (cnt > 0 && cnt <= max_cnt && addr >= cl->tag_base &&
							(uint64_t)(addr - cl->tag_base) + (uint64_t)cnt * sub_elem_sz <= cl->tag_sz)
						{
							size_t copy_bytes = (size_t)cnt * sub_elem_sz;
							uint8_t *new_sub = (uint8_t *)calloc(1, copy_bytes);
							if (new_sub)
							{
								memcpy(new_sub, cl->tag_buf + (addr - cl->tag_base), copy_bytes);
								blk->count = cnt;
								blk->address = new_sub;
								xbox_relocate_schema_elements(
									cl, child_def, new_sub, addr - cl->tag_base, cnt, depth + 1);
							}
							else
							{
								blk->count = 0;
								blk->address = NULL;
							}
						}
						else
						{
							blk->count = 0;
							blk->address = NULL;
						}
					}
				}
				else if (field->type == _tag_schema_data)
				{
					if (f_rel + 20u <= elem_size)
					{
						struct tag_data *td = (struct tag_data *)(elem_ptr + f_rel);
						int32_t sz = *(int32_t const *)(cl->tag_buf + src_f_off + 0u);
						uint32_t addr = *(uint32_t const *)(cl->tag_buf + src_f_off + 12u);
						int32_t max_sz = (field->maximum > 0) ? (int32_t)field->maximum : (16 * 1024 * 1024);

						td->definition = NULL;
						if (sz > 0 && sz <= max_sz && addr >= cl->tag_base &&
							(uint64_t)(addr - cl->tag_base) + (uint32_t)sz <= cl->tag_sz)
						{
							void *new_td = malloc((size_t)sz);
							if (new_td)
							{
								memcpy(new_td, cl->tag_buf + (addr - cl->tag_base), (size_t)sz);
								td->size = sz;
								td->address = new_td;
							}
							else
							{
								td->size = 0;
								td->address = NULL;
							}
						}
						else
						{
							td->size = 0;
							td->address = NULL;
						}
					}
				}
				else if (field->type == _tag_schema_reference)
				{
					if (f_rel + 16u <= elem_size)
					{
						struct tag_reference *ref = (struct tag_reference *)(elem_ptr + f_rel);
						uint32_t name_addr = *(uint32_t const *)(cl->tag_buf + src_f_off + 4u);
						int32_t ref_idx = *(int32_t const *)(cl->tag_buf + src_f_off + 12u);

						ref->name = NULL;
						ref->name_length = 0;
						if (name_addr >= cl->tag_base && (name_addr - cl->tag_base) < cl->tag_sz)
						{
							char const *rname = (char const *)(cl->tag_buf + (name_addr - cl->tag_base));
							size_t rlen = strlen(rname);
							char *ncopy = (char *)malloc(rlen + 1u);
							if (ncopy)
							{
								memcpy(ncopy, rname, rlen + 1u);
								ref->name = ncopy;
							}
						}
						if (ref_idx != -1 && (ref_idx & 0xFFFF) < cl->tag_count &&
							cl->runtime_handles[ref_idx & 0xFFFF] != NONE)
						{
							long hdl = cl->runtime_handles[ref_idx & 0xFFFF];
							ref->index = hdl;
							ref->group_tag = tag_get_group_tag(hdl);
						}
						else
						{
							ref->index = NONE;
						}
					}
				}
				else if (field->type == _tag_schema_tag_index)
				{
					if (f_rel + 4u <= elem_size)
					{
						int32_t ref_idx = *(int32_t const *)(cl->tag_buf + src_f_off);
						if (ref_idx != -1 && (ref_idx & 0xFFFF) < cl->tag_count &&
							cl->runtime_handles[ref_idx & 0xFFFF] != NONE)
						{
							*(int32_t *)(elem_ptr + f_rel) = cl->runtime_handles[ref_idx & 0xFFFF];
						}
						else
						{
							*(int32_t *)(elem_ptr + f_rel) = NONE;
						}
					}
				}
			}
		}
	}
}

static void runtime_tint_dxt1_blue_to_red(uint8_t *pix, uint32_t byte_count)
{
	uint32_t off;
	if (!pix)
		return;
	for (off = 0; off + 8u <= byte_count; off += 8u)
	{
		uint16_t c0 = *(uint16_t const *)(pix + off + 0u);
		uint16_t c1 = *(uint16_t const *)(pix + off + 2u);
		uint32_t lut = *(uint32_t const *)(pix + off + 4u);
		uint16_t r0 = (uint16_t)((c0 >> 11) & 0x1Fu);
		uint16_t g0 = (uint16_t)((c0 >> 5) & 0x3Fu);
		uint16_t b0 = (uint16_t)(c0 & 0x1Fu);
		uint16_t r1 = (uint16_t)((c1 >> 11) & 0x1Fu);
		uint16_t g1 = (uint16_t)((c1 >> 5) & 0x3Fu);
		uint16_t b1 = (uint16_t)(c1 & 0x1Fu);
		uint16_t nr0 = (b0 > r0) ? b0 : r0;
		uint16_t nb0 = (r0 < b0) ? (uint16_t)(r0 >> 1) : (uint16_t)(b0 >> 1);
		uint16_t ng0 = (uint16_t)(g0 >> 2);
		uint16_t nr1 = (b1 > r1) ? b1 : r1;
		uint16_t nb1 = (r1 < b1) ? (uint16_t)(r1 >> 1) : (uint16_t)(b1 >> 1);
		uint16_t ng1 = (uint16_t)(g1 >> 2);
		uint16_t nc0 = (uint16_t)((nr0 << 11) | (ng0 << 5) | nb0);
		uint16_t nc1 = (uint16_t)((nr1 << 11) | (ng1 << 5) | nb1);

		if (c0 > c1)
		{
			if (nc0 < nc1)
			{
				uint16_t tmp = nc0;
				nc0 = nc1;
				nc1 = tmp;
				lut ^= 0x55555555u;
			}
			else if (nc0 == nc1)
			{
				if (nc0 < 0xFFFFu) nc0++;
				else nc1--;
			}
		}
		else
		{
			if (nc0 > nc1)
			{
				uint16_t tmp = nc0;
				uint32_t new_lut = 0;
				int p;
				nc0 = nc1;
				nc1 = tmp;
				for (p = 0; p < 16; p++)
				{
					uint32_t idx = (lut >> (p * 2)) & 3u;
					if (idx == 0u) idx = 1u;
					else if (idx == 1u) idx = 0u;
					new_lut |= (idx << (p * 2));
				}
				lut = new_lut;
			}
		}
		*(uint16_t *)(pix + off + 0u) = nc0;
		*(uint16_t *)(pix + off + 2u) = nc1;
		*(uint32_t *)(pix + off + 4u) = lut;
	}
}

static long load_native_xbox_tag_from_decompressed_map(
	char const *cache_map_path,
	uint32_t req_group,
	char const *req_path,
	uint32_t *out_actual_group)
{
	FILE *f = NULL;
	uint8_t hdr[2048];
	uint32_t tag_off = 0, tag_sz = 0, inst_addr;
	int32_t tag_count, i, root_abs_idx = -1;
	uint8_t *tag_buf = NULL;
	int keep_cached = 0;
	struct xbox_runtime_closure cl;
	long root_handle = NONE;
	int is_red_beam_variant = (req_group == 'mach' &&
		_stricmp(req_path, "levels\\a30\\devices\\beam emitter\\beam emitter red") == 0) ? 1 : 0;
	char const *lookup_path = is_red_beam_variant
		? "levels\\a30\\devices\\beam emitter\\beam emitter"
		: req_path;

	if (!cache_map_path || !cache_map_path[0])
		return NONE;

	if (s_runtime_xbox_donor.fp && s_runtime_xbox_donor.tag_buf &&
		_stricmp(s_runtime_xbox_donor.path, cache_map_path) == 0)
	{
		f = s_runtime_xbox_donor.fp;
		tag_buf = s_runtime_xbox_donor.tag_buf;
		tag_off = s_runtime_xbox_donor.tag_off;
		tag_sz = s_runtime_xbox_donor.tag_sz;
		keep_cached = 1;
	}
	else
	{
		f = fopen(cache_map_path, "rb");
		if (!f)
			return NONE;
		if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr))
		{
			fclose(f);
			return NONE;
		}
		if (*(uint32_t const *)(hdr + 0x00) != 0x68656164u || *(uint32_t const *)(hdr + 0x04) != 5u)
		{
			fclose(f);
			return NONE;
		}
		tag_off = *(uint32_t const *)(hdr + 0x10);
		tag_sz = *(uint32_t const *)(hdr + 0x14);
		if (tag_off < 2048u || tag_sz < 64u || tag_sz > 64u * 1024u * 1024u)
		{
			fclose(f);
			return NONE;
		}

		tag_buf = (uint8_t *)malloc(tag_sz);
		if (!tag_buf || fseek(f, (long)tag_off, SEEK_SET) != 0 || fread(tag_buf, 1, tag_sz, f) != tag_sz)
		{
			free(tag_buf);
			fclose(f);
			return NONE;
		}
		if (strstr(cache_map_path, "cache_donor.map") == NULL)
		{
			close_runtime_xbox_donor();
			snprintf(s_runtime_xbox_donor.path, sizeof(s_runtime_xbox_donor.path), "%s", cache_map_path);
			s_runtime_xbox_donor.fp = f;
			s_runtime_xbox_donor.tag_buf = tag_buf;
			s_runtime_xbox_donor.tag_off = tag_off;
			s_runtime_xbox_donor.tag_sz = tag_sz;
			keep_cached = 1;
		}
	}

	inst_addr = *(uint32_t const *)(tag_buf + 0x00);
	tag_count = *(int32_t const *)(tag_buf + 0x0C);
	memset(&cl, 0, sizeof(cl));
	cl.tag_buf = tag_buf;
	cl.tag_sz = tag_sz;
	cl.tag_base = inst_addr - 0x24u;
	cl.inst_off = 0x24u;
	cl.tag_count = tag_count;
	cl.is_red_beam_variant = is_red_beam_variant;

	if (tag_count <= 0 || tag_count > 16384 || cl.inst_off + (uint32_t)tag_count * 32u > tag_sz)
	{
		if (!keep_cached)
		{
			free(tag_buf);
			fclose(f);
		}
		return NONE;
	}

	/* Locate the requested tag in this Xbox map's tag table */
	for (i = 0; i < tag_count; i++)
	{
		uint32_t io = cl.inst_off + (uint32_t)i * 32u;
		uint32_t grp = *(uint32_t const *)(tag_buf + io + 0x00);
		uint32_t name_addr = *(uint32_t const *)(tag_buf + io + 0x10);
		char const *tname;
		if (name_addr < cl.tag_base || (name_addr - cl.tag_base) >= tag_sz)
			continue;
		tname = (char const *)(tag_buf + (name_addr - cl.tag_base));
		if ((grp == req_group || (req_group == 'mod2' && grp == 'mode')) &&
			_stricmp(tname, lookup_path) == 0)
		{
			root_abs_idx = i;
			break;
		}
	}

	if (root_abs_idx < 0)
	{
		if (!keep_cached)
		{
			free(tag_buf);
			fclose(f);
		}
		return NONE;
	}

	cl.visited = (uint8_t *)calloc((size_t)tag_count, sizeof(uint8_t));
	cl.order = (int32_t *)calloc((size_t)tag_count, sizeof(int32_t));
	cl.runtime_handles = (long *)malloc((size_t)tag_count * sizeof(long));
	cl.cloned_roots = (uint8_t **)calloc((size_t)tag_count, sizeof(uint8_t *));
	if (!cl.visited || !cl.order || !cl.runtime_handles || !cl.cloned_roots)
	{
		free(cl.visited);
		free(cl.order);
		free(cl.runtime_handles);
		free(cl.cloned_roots);
		if (!keep_cached)
		{
			free(tag_buf);
			fclose(f);
		}
		return NONE;
	}
	for (i = 0; i < tag_count; i++)
		cl.runtime_handles[i] = NONE;

	/* 1. Collect transitive tag dependency closure via OpenCE's tag_schema_group_lists */
	xbox_visit_tag_index(&cl, root_abs_idx);

	/* 2. Allocate root tag definitions and register them in global_tag_instances */
	for (i = 0; i < cl.order_count; i++)
	{
		int32_t abs_idx = cl.order[i];
		uint32_t io = cl.inst_off + (uint32_t)abs_idx * 32u;
		uint32_t grp = *(uint32_t const *)(tag_buf + io + 0x00);
		uint32_t p0 = *(uint32_t const *)(tag_buf + io + 0x04);
		uint32_t p1 = *(uint32_t const *)(tag_buf + io + 0x08);
		uint32_t name_addr = *(uint32_t const *)(tag_buf + io + 0x10);
		uint32_t data_addr = *(uint32_t const *)(tag_buf + io + 0x14);
		char const *tname = (char const *)(tag_buf + (name_addr - cl.tag_base));
		char reg_name[256];
		struct tag_schema_group const *schema = forge_runtime_schema_group_get(grp);
		size_t root_sz;
		uint8_t *root_buf;

		if (!schema || !schema->definition || schema->definition->size <= 0)
			continue;
		root_sz = (size_t)schema->definition->size;
		root_buf = (uint8_t *)calloc(1, root_sz < 64u ? 64u : root_sz);
		if (!root_buf)
			continue;
		memcpy(root_buf, tag_buf + (data_addr - cl.tag_base), root_sz);
		cl.cloned_roots[abs_idx] = root_buf;
		if (is_red_beam_variant)
		{
			if (abs_idx == root_abs_idx)
				snprintf(reg_name, sizeof(reg_name), "%s", req_path);
			else
				snprintf(reg_name, sizeof(reg_name), "%s__red", tname);
		}
		else if (abs_idx != root_abs_idx && tag_loaded((long)grp, tname) != NONE)
		{
			long ex_cnt = 0;
			cache_files_tag_instances(&ex_cnt);
			snprintf(reg_name, sizeof(reg_name), "%s__f%ld", tname, ex_cnt);
		}
		else
		{
			snprintf(reg_name, sizeof(reg_name), "%s", tname);
		}
		cl.runtime_handles[abs_idx] = append_runtime_tag_instance(
			(long)grp,
			schema->parent_group_tags[0] ? (long)schema->parent_group_tags[0] : (long)p0,
			schema->parent_group_tags[1] ? (long)schema->parent_group_tags[1] : (long)p1,
			reg_name,
			root_buf);
	}

	/* 3. Relocate all nested tag_blocks, tag_data, and tag_references */
	for (i = 0; i < cl.order_count; i++)
	{
		int32_t abs_idx = cl.order[i];
		uint32_t io = cl.inst_off + (uint32_t)abs_idx * 32u;
		uint32_t grp = *(uint32_t const *)(tag_buf + io + 0x00);
		uint32_t data_addr = *(uint32_t const *)(tag_buf + io + 0x14);
		struct tag_schema_group const *schema = forge_runtime_schema_group_get(grp);
		if (cl.cloned_roots[abs_idx] && schema && schema->definition)
		{
			xbox_relocate_schema_elements(
				&cl,
				schema->definition,
				cl.cloned_roots[abs_idx],
				data_addr - cl.tag_base,
				1,
				1);
		}
	}

	/* 4. Initialize Xbox 'bitm' hardware textures and 'mode' hardware vertex/triangle buffers using OpenCE's native API */
	for (i = 0; i < cl.order_count; i++)
	{
		int32_t abs_idx = cl.order[i];
		uint32_t io = cl.inst_off + (uint32_t)abs_idx * 32u;
		uint32_t grp = *(uint32_t const *)(tag_buf + io + 0x00);
		uint32_t data_addr = *(uint32_t const *)(tag_buf + io + 0x14);
		uint8_t *root_buf = cl.cloned_roots[abs_idx];

		if (!root_buf || cl.runtime_handles[abs_idx] == NONE)
			continue;

		if (grp == 'bitm')
		{
			struct bitmap_group *bgrp = (struct bitmap_group *)root_buf;
			int b_idx;
			for (b_idx = 0; b_idx < bgrp->bitmaps.count && bgrp->bitmaps.address; b_idx++)
			{
				struct bitmap_data *bmp = (struct bitmap_data *)((uint8_t *)bgrp->bitmaps.address + (size_t)b_idx * 48u);
				long req_bytes;
				uint32_t read_bytes, alloc_bytes;
				uint8_t *pix_mem;
				D3DBaseTexture *tex;

				/* Clear _bitmap_cached_bit (bit 7 = 0x80) so _texture_cache_bitmap_get_hardware_format returns bmp->hardware_format directly! */
				bmp->flags &= (uint16_t)~(1u << 7);
				bmp->tag_index = cl.runtime_handles[abs_idx];
				bmp->cache_block_index = NONE;
				bmp->base_address = NULL;
				bmp->hardware_format = NULL;

				if (bmp->width <= 0 || bmp->height <= 0 || bmp->depth <= 0 || bmp->pixels_offset < 2048)
					continue;
				req_bytes = rasterizer_xbox_bitmap_get_pixel_data_size(bmp);
				if (req_bytes <= 0)
					continue;
				read_bytes = (bmp->pixels_size > 0 && bmp->pixels_size < req_bytes) ? (uint32_t)bmp->pixels_size : (uint32_t)req_bytes;
				alloc_bytes = (uint32_t)req_bytes;
				if ((uint32_t)bmp->pixels_size > alloc_bytes && bmp->pixels_size <= 16 * 1024 * 1024)
					alloc_bytes = (uint32_t)bmp->pixels_size;

				pix_mem = (uint8_t *)forge_phys_alloc(alloc_bytes);
				tex = (D3DBaseTexture *)calloc(1, sizeof(D3DBaseTexture));
				if (!pix_mem || !tex)
				{
					free(tex);
					continue;
				}
				if (fseek(f, (long)bmp->pixels_offset, SEEK_SET) != 0 ||
					fread(pix_mem, 1, read_bytes, f) != read_bytes)
				{
					free(tex);
					continue;
				}
				if (is_red_beam_variant && bmp->format == 14)
				{
					uint32_t name_addr = *(uint32_t const *)(tag_buf + io + 0x10);
					char const *btname = (name_addr >= cl.tag_base && (name_addr - cl.tag_base) < tag_sz)
						? (char const *)(tag_buf + (name_addr - cl.tag_base))
						: "";
					if (strstr(btname, "jackal shield") != NULL || strstr(btname, "static blue") != NULL)
						runtime_tint_dxt1_blue_to_red(pix_mem, read_bytes);
				}
				bmp->base_address = pix_mem;
				halo_custom_edition_texels_channels(bmp->base_address, 0 /* _custom_edition_channels_xbox */);

				/* Exact OpenCE texture_cache_initialize_hardware_format (source/cache/xbox_texture_cache.c:613-647) */
				tex->Data = 0;
				tex->Lock = 0;
				tex->Common = D3DCOMMON_TYPE_TEXTURE | 1;
				if (TEST_FLAG(bmp->flags, 4 /* _bitmap_linear_bit */))
				{
					tex->Format =
						(bitmap_format_to_d3d_linear_format(bmp->format, bmp->flags) << D3DFORMAT_FORMAT_SHIFT) |
						(1 << D3DFORMAT_MIPMAP_SHIFT) |
						(2 << D3DFORMAT_DIMENSION_SHIFT) |
						D3DFORMAT_BORDERSOURCE_COLOR |
						D3DFORMAT_DMACHANNEL_A;
					tex->Size =
						(((bitmap_mipmap_get_row_pitch(bmp, 0) + D3DTEXTURE_PITCH_ALIGNMENT - 1) / D3DTEXTURE_PITCH_ALIGNMENT - 1)
							<< D3DSIZE_PITCH_SHIFT) |
						((bmp->height - 1) << D3DSIZE_HEIGHT_SHIFT) |
						(bmp->width - 1);
				}
				else
				{
					tex->Format =
						(floor_log2(bmp->depth) << D3DFORMAT_PSIZE_SHIFT) |
						(floor_log2(bmp->height) << D3DFORMAT_VSIZE_SHIFT) |
						(floor_log2(bmp->width) << D3DFORMAT_USIZE_SHIFT) |
						(bitmap_format_to_d3d_format(bmp->format, bmp->flags) << D3DFORMAT_FORMAT_SHIFT) |
						((bmp->type == 1 /* _bitmap_type_3d */ ? 3 : 2) << D3DFORMAT_DIMENSION_SHIFT) |
						((rasterizer_xbox_bitmap_get_max_mipmap_count(bmp) + 1) << D3DFORMAT_MIPMAP_SHIFT) |
						(bmp->type == 2 /* _bitmap_type_cube_map */ ? D3DFORMAT_CUBEMAP : 0) |
						D3DFORMAT_BORDERSOURCE_COLOR |
						D3DFORMAT_DMACHANNEL_A;
					tex->Size = 0;
				}
				IDirect3DBaseTexture8_Register(tex, bmp->base_address);
				bmp->hardware_format = tex;
			}
		}
		else if (grp == 'senv' || grp == 'soso' || grp == 'sotr' || grp == 'schi' ||
				 grp == 'scex' || grp == 'swat' || grp == 'sgla' || grp == 'smet' || grp == 'spla')
		{
			int16_t stype = 4;
			if (grp == 'senv') stype = 3;
			else if (grp == 'soso') stype = 4;
			else if (grp == 'sotr') stype = 5;
			else if (grp == 'schi' || grp == 'scex') stype = 6;
			else if (grp == 'swat') stype = 7;
			else if (grp == 'sgla') stype = 8;
			else if (grp == 'smet') stype = 9;
			else if (grp == 'spla') stype = 10;
			*(int16_t *)(root_buf + 0x24u) = stype;
		}
		else if (grp == 'snd!')
		{
			struct sound_definition *sdef = (struct sound_definition *)root_buf;
			int pr_i, pm_i;
			sdef->promotion_sound.index = NONE;
			sdef->promotion_count = 0;
			for (pr_i = 0; pr_i < sdef->pitch_ranges.count && sdef->pitch_ranges.address; pr_i++)
			{
				struct sound_pitch_range *pr = (struct sound_pitch_range *)((uint8_t *)sdef->pitch_ranges.address + (size_t)pr_i * sizeof(struct sound_pitch_range));
				pr->played_permutation_mask = 0xFFFFFFFFu;
				pr->previous_permutation_index = NONE;
				pr->forced_permutation_index = NONE;
				for (pm_i = 0; pm_i < pr->permutations.count && pr->permutations.address; pm_i++)
				{
					struct sound_permutation *perm = (struct sound_permutation *)((uint8_t *)pr->permutations.address + (size_t)pm_i * sizeof(struct sound_permutation));
					perm->unknown0 = NONE;
					perm->unknown1 = 0;
					perm->unknown2 = (unsigned long)cl.runtime_handles[abs_idx];
					perm->unknown3 = (unsigned long)cl.runtime_handles[abs_idx];
					perm->samples.address = NULL;
					if (perm->samples.size > 0 && perm->samples.size <= MAXIMUM_SOUND_DATA_SIZE &&
						perm->samples.file_offset >= 2048)
					{
						void *sbuf = malloc((size_t)perm->samples.size);
						if (sbuf && fseek(f, (long)perm->samples.file_offset, SEEK_SET) == 0 &&
							fread(sbuf, 1, (size_t)perm->samples.size, f) == (size_t)perm->samples.size)
						{
							perm->samples.address = sbuf;
							perm->samples.file_offset = 0;
							forge_register_dyn_sound_perm(perm);
						}
						else
						{
							free(sbuf);
							perm->samples.size = 0;
						}
					}
					else
					{
						perm->samples.size = 0;
					}
				}
			}
		}
		else if (grp == 'mode')
		{
			uint32_t mod_off = data_addr - cl.tag_base;
			int32_t geom_cnt = *(int32_t const *)(root_buf + 0xD0u);
			uint8_t *geom_base = *(uint8_t **)(root_buf + 0xD4u);
			int32_t shdr_cnt = *(int32_t const *)(root_buf + 0xDCu);
			uint8_t *shdr_base = *(uint8_t **)(root_buf + 0xE0u);
			uint32_t src_geom_addr = *(uint32_t const *)(tag_buf + mod_off + 0xD4u);
			int g_idx, s_idx;

			if (!(*(float const *)(root_buf + 0x30u) > 0.0001f && *(float const *)(root_buf + 0x30u) < 10000.0f))
				*(float *)(root_buf + 0x30u) = 1.0f;
			if (!(*(float const *)(root_buf + 0x34u) > 0.0001f && *(float const *)(root_buf + 0x34u) < 10000.0f))
				*(float *)(root_buf + 0x34u) = 1.0f;

			/* Shift beam emitter root node down by 6.5 world units (~10m in Forge HUD) so its flare collar sits at the placement origin */
			if (is_red_beam_variant || _stricmp(req_path, "levels\\a30\\devices\\beam emitter\\beam emitter") == 0)
			{
				int32_t node_cnt = *(int32_t const *)(root_buf + 0xB8u);
				uint8_t *node_base = *(uint8_t **)(root_buf + 0xBCu);
				if (node_cnt > 0 && node_base)
					*(float *)(node_base + 48u) = -6.5f;
			}

			for (s_idx = 0; s_idx < shdr_cnt && shdr_base; s_idx++)
			{
				int16_t *perm_idx_ptr = (int16_t *)(shdr_base + (size_t)s_idx * 32u + 16u);
				if (*perm_idx_ptr < 0)
					*perm_idx_ptr = 0;
			}

			if (geom_cnt > 0 && geom_base && src_geom_addr >= cl.tag_base &&
				(uint64_t)(src_geom_addr - cl.tag_base) + (uint64_t)geom_cnt * 48u <= tag_sz)
			{
				for (g_idx = 0; g_idx < geom_cnt; g_idx++)
				{
					uint8_t *dst_geom = geom_base + (size_t)g_idx * 48u;
					uint32_t src_g_off = (src_geom_addr - cl.tag_base) + (uint32_t)g_idx * 48u;
					int32_t part_cnt = *(int32_t const *)(dst_geom + 36u);
					uint8_t *dst_parts = *(uint8_t **)(dst_geom + 40u);
					uint32_t src_part_addr = *(uint32_t const *)(tag_buf + src_g_off + 40u);
					int p_idx;

					if (part_cnt <= 0 || !dst_parts || src_part_addr < cl.tag_base ||
						(uint64_t)(src_part_addr - cl.tag_base) + (uint64_t)part_cnt * 104u > tag_sz)
						continue;

					for (p_idx = 0; p_idx < part_cnt; p_idx++)
					{
						uint8_t *dst_part = dst_parts + (size_t)p_idx * 104u;
						uint32_t p_off = (src_part_addr - cl.tag_base) + (uint32_t)p_idx * 104u;
						int16_t tri_type = *(int16_t const *)(tag_buf + p_off + 68u);
						int32_t tri_count = *(int32_t const *)(tag_buf + p_off + 72u);
						uint32_t tb_hw_addr = *(uint32_t const *)(tag_buf + p_off + 80u);
						int16_t vtx_type = *(int16_t const *)(tag_buf + p_off + 84u);
						int32_t vtx_count = *(int32_t const *)(tag_buf + p_off + 88u);
						uint32_t vb_hw_addr = *(uint32_t const *)(tag_buf + p_off + 100u);

						memset(dst_part + 68u, 0, 36u);
						((struct vertex_buffer *)(dst_part + 84u))->type = _rasterizer_vertex_type_model_compressed;
						if (tri_count > 0 && vtx_count > 0 &&
							tb_hw_addr >= cl.tag_base && (tb_hw_addr - cl.tag_base) + 12u <= tag_sz &&
							vb_hw_addr >= cl.tag_base && (vb_hw_addr - cl.tag_base) + 12u <= tag_sz)
						{
							uint32_t ib_data_addr = *(uint32_t const *)(tag_buf + (tb_hw_addr - cl.tag_base) + 4u);
							uint32_t vb_data_addr = *(uint32_t const *)(tag_buf + (vb_hw_addr - cl.tag_base) + 4u);
							uint32_t comp_sz = (uint32_t)vtx_count * 32u;
							uint32_t idx_sz = (tri_type == _triangle_buffer_type_precompiled_strip)
								? ((uint32_t)tri_count + 2u) * 2u
								: (uint32_t)tri_count * 6u;

							if (ib_data_addr >= cl.tag_base && (ib_data_addr - cl.tag_base) + idx_sz <= tag_sz &&
								vb_data_addr >= cl.tag_base && (vb_data_addr - cl.tag_base) + comp_sz <= tag_sz)
							{
								void *hw_verts = forge_phys_alloc(comp_sz);
								void *hw_tris = forge_phys_alloc(idx_sz);
								if (hw_verts && hw_tris)
								{
									memcpy(hw_verts, tag_buf + (vb_data_addr - cl.tag_base), comp_sz);
									memcpy(hw_tris, tag_buf + (ib_data_addr - cl.tag_base), idx_sz);
									rasterizer_vertex_buffer_new(
										(struct vertex_buffer *)(dst_part + 84u),
										vtx_type ? vtx_type : _rasterizer_vertex_type_model_compressed,
										vtx_count,
										hw_verts,
										(long)comp_sz);
									rasterizer_triangle_buffer_new(
										(struct triangle_buffer *)(dst_part + 68u),
										tri_type,
										tri_count,
										hw_tris);
								}
							}
						}
					}
				}
			}
			models_fix_transparent_part_links();
		}
		else if (grp == 'vehi')
		{
			struct unit_definition *ud = (struct unit_definition *)root_buf;
			ud->unit.flags |= FLAG(_unit_has_entrance_points_inside_bounding_sphere_bit);
		}
		else if (grp == 'antr')
		{
			/* Clear _animation_world_relative_bit (0x0002) on scenery/character animations (e.g. flood_captain in d20)
			 * so placed objects render at their placed world position rather than teleporting to campaign cutscene coordinates. */
			int32_t anim_cnt = *(int32_t const *)(root_buf + 0x74u);
			uint8_t *anim_base = *(uint8_t **)(root_buf + 0x78u);
			float n0_tx = 0.0f, n0_ty = 0.0f, n0_tz = 0.0f;
			int m_k, a_idx;

			for (m_k = 0; m_k < cl.order_count; m_k++)
			{
				int m_abs = cl.order[m_k];
				uint32_t m_io = cl.inst_off + (uint32_t)m_abs * 32u;
				if (*(uint32_t const *)(tag_buf + m_io + 0x00) == 'mode' && cl.cloned_roots[m_abs])
				{
					uint8_t const *mod_buf = cl.cloned_roots[m_abs];
					int32_t n_cnt = *(int32_t const *)(mod_buf + 0xB8u);
					uint8_t const *n_base = *(uint8_t *const *)(mod_buf + 0xBCu);
					if (n_cnt > 0 && n_base)
					{
						float tx = *(float const *)(n_base + 40u);
						float ty = *(float const *)(n_base + 44u);
						float tz = *(float const *)(n_base + 48u);
						if (tx > -10.0f && tx < 10.0f && ty > -10.0f && ty < 10.0f && tz > -10.0f && tz < 10.0f)
						{
							n0_tx = tx;
							n0_ty = ty;
							n0_tz = tz;
						}
					}
					break;
				}
			}

			for (a_idx = 0; a_idx < anim_cnt && anim_base; a_idx++)
			{
				uint8_t *anim = anim_base + (size_t)a_idx * 180u;
				uint16_t aflags = *(uint16_t const *)(anim + 0x3Au);
				if (aflags & 0x0002u)
				{
					*(uint16_t *)(anim + 0x3Au) = (uint16_t)(aflags & ~0x0002u);
					if (!(aflags & 0x0001u))
					{
						uint32_t trans_flags0 = *(uint32_t const *)(anim + 0x5Cu);
						uint32_t rot_flags0 = *(uint32_t const *)(anim + 0x64u);
						uint32_t n0_trans_off = (rot_flags0 & 1u) ? 0u : 8u;
						if (!(trans_flags0 & 1u))
						{
							int32_t def_sz = *(int32_t const *)(anim + 0x80u);
							uint8_t *def_ptr = *(uint8_t **)(anim + 0x88u);
							if (def_ptr && def_sz >= (int32_t)(n0_trans_off + 12u))
							{
								*(float *)(def_ptr + n0_trans_off + 0u) = n0_tx;
								*(float *)(def_ptr + n0_trans_off + 4u) = n0_ty;
								*(float *)(def_ptr + n0_trans_off + 8u) = n0_tz;
							}
						}
						else
						{
							int16_t frame_cnt = *(int16_t const *)(anim + 0x24u);
							int16_t frame_sz = *(int16_t const *)(anim + 0x26u);
							int32_t fdata_sz = *(int32_t const *)(anim + 0x94u);
							uint8_t *fdata_ptr = *(uint8_t **)(anim + 0x9Cu);
							int f_i;
							for (f_i = 0; f_i < frame_cnt && frame_sz >= (int16_t)(n0_trans_off + 12u) && fdata_ptr; f_i++)
							{
								uint32_t f_off = (uint32_t)f_i * (uint32_t)frame_sz + n0_trans_off;
								if (f_off + 12u <= (uint32_t)fdata_sz)
								{
									*(float *)(fdata_ptr + f_off + 0u) = n0_tx;
									*(float *)(fdata_ptr + f_off + 4u) = n0_ty;
									*(float *)(fdata_ptr + f_off + 8u) = n0_tz;
								}
							}
						}
					}
				}
			}
		}
		else if (grp == 'ligh')
		{
			patch_light_definition_buffer(root_buf);
			if (is_red_beam_variant)
			{
				*(float *)(root_buf + 0x38u) = 1.0f;
				*(float *)(root_buf + 0x3Cu) = 1.0f;
				*(float *)(root_buf + 0x40u) = 0.12f;
				*(float *)(root_buf + 0x44u) = 0.08f;
				*(float *)(root_buf + 0x48u) = 1.0f;
				*(float *)(root_buf + 0x4Cu) = 1.0f;
				*(float *)(root_buf + 0x50u) = 0.18f;
				*(float *)(root_buf + 0x54u) = 0.10f;
			}
		}
		else if (grp == 'lens' && is_red_beam_variant)
		{
			int32_t refl_cnt = *(int32_t const *)(root_buf + 0xC4u);
			uint8_t *refl_base = *(uint8_t *const *)(root_buf + 0xC8u);
			int r_idx;
			for (r_idx = 0; r_idx < refl_cnt && refl_base; r_idx++)
			{
				uint8_t *rf = refl_base + (size_t)r_idx * 128u;
				*(float *)(rf + 0x40u) = 1.0f;
				*(float *)(rf + 0x44u) = 1.0f;
				*(float *)(rf + 0x48u) = 0.12f;
				*(float *)(rf + 0x4Cu) = 0.08f;
				*(float *)(rf + 0x50u) = 1.0f;
				*(float *)(rf + 0x54u) = 1.0f;
				*(float *)(rf + 0x58u) = 0.18f;
				*(float *)(rf + 0x5Cu) = 0.10f;
				*(float *)(rf + 0x60u) = 1.0f;
				*(float *)(rf + 0x64u) = 1.0f;
				*(float *)(rf + 0x68u) = 0.12f;
				*(float *)(rf + 0x6Cu) = 0.08f;
			}
		}
	}

	root_handle = cl.runtime_handles[root_abs_idx];
	if (root_handle != NONE)
	{
		if (req_group == 'vehi')
			patch_vehicle_tag_for_player_driving(root_handle, req_path);
		else if (req_group == 'scen' || req_group == 'mach' || req_group == 'lifi' || req_group == 'ctrl')
			patch_forge_device_and_light_tag_runtime(root_handle, req_group, req_path);
		if (s_dyn_tags.count < FORGE_MAX_DYNAMIC_TAGS)
		{
			struct forge_dynamic_tag_entry *entry = &s_dyn_tags.entries[s_dyn_tags.count++];
			memset(entry, 0, sizeof(*entry));
			entry->group_tag = req_group;
			snprintf(entry->tag_path, sizeof(entry->tag_path), "%s", req_path);
			entry->definition_index = root_handle;
			entry->part_count = 0; /* 0 = rendered natively by Halo's render_model! */
			entry->bounding_radius = cl.cloned_roots[root_abs_idx]
				? *(float const *)(cl.cloned_roots[root_abs_idx] + 0x04u)
				: 1.5f;
			if (!(entry->bounding_radius > 0.1f && entry->bounding_radius < 100.0f))
				entry->bounding_radius = 1.5f;
		}
		if (out_actual_group)
			*out_actual_group = (uint32_t)tag_get_group_tag(root_handle);
	}

	free(cl.visited);
	free(cl.order);
	free(cl.runtime_handles);
	free(cl.cloned_roots);
	if (!keep_cached)
	{
		free(tag_buf);
		fclose(f);
	}
	return root_handle;
}

static int find_donor_map_stems_from_credits(
	uint32_t req_group,
	char const *req_path,
	char out_stems[][64],
	int max_stems)
{
	FILE *fp;
	char line[512];
	char cur_stem[64];
	char grp_str[5];
	char const *lookup_path;
	int count = 0;

	if (!req_path || !req_path[0] || !out_stems || max_stems <= 0)
		return 0;

	lookup_path = (req_group == 'mach' &&
		_stricmp(req_path, "levels\\a30\\devices\\beam emitter\\beam emitter red") == 0)
		? "levels\\a30\\devices\\beam emitter\\beam emitter"
		: req_path;

	grp_str[0] = (char)((req_group >> 24) & 0xFF);
	grp_str[1] = (char)((req_group >> 16) & 0xFF);
	grp_str[2] = (char)((req_group >> 8) & 0xFF);
	grp_str[3] = (char)(req_group & 0xFF);
	grp_str[4] = '\0';

	fp = fopen("OpenCE Forge/credits.md", "r");
	if (!fp)
		return 0;

	cur_stem[0] = '\0';
	while (fgets(line, sizeof(line), fp))
	{
		if (strncmp(line, "## Xbox Map: `", 14) == 0)
		{
			char const *p = line + 14;
			char const *q = strchr(p, '`');
			if (q && q > p)
			{
				size_t len = (size_t)(q - p);
				if (len >= sizeof(cur_stem))
					len = sizeof(cur_stem) - 1;
				memcpy(cur_stem, p, len);
				cur_stem[len] = '\0';
			}
		}
		else if (cur_stem[0] && strncmp(line, "- `", 3) == 0)
		{
			char const *p1 = line + 3;
			char const *q1 = strchr(p1, '`');
			if (q1 && q1 > p1)
			{
				char tag_buf[256];
				size_t tlen = (size_t)(q1 - p1);
				char const *p2 = strchr(q1 + 1, '`');
				char const *q2 = p2 ? strchr(p2 + 1, '`') : NULL;
				if (tlen < sizeof(tag_buf) && p2 && q2 && (q2 - (p2 + 1)) == 4)
				{
					char gbuf[5];
					memcpy(tag_buf, p1, tlen);
					tag_buf[tlen] = '\0';
					memcpy(gbuf, p2 + 1, 4);
					gbuf[4] = '\0';
					if (_stricmp(tag_buf, lookup_path) == 0 &&
						(_stricmp(gbuf, grp_str) == 0 || (req_group == 'mod2' && _stricmp(gbuf, "mode") == 0)))
					{
						int i, dup = 0;
						for (i = 0; i < count; i++)
						{
							if (_stricmp(out_stems[i], cur_stem) == 0)
							{
								dup = 1;
								break;
							}
						}
						if (!dup && count < max_stems)
						{
							snprintf(out_stems[count++], 64, "%s", cur_stem);
							if (count >= max_stems)
								break;
						}
					}
				}
			}
		}
	}
	fclose(fp);
	return count;
}

static long load_native_xbox_tag_runtime(
	uint32_t req_group,
	char const *req_path,
	uint32_t *out_actual_group)
{
#ifdef _WIN32
	static char const *const s_xbox_map_dirs[] = {
		"maps",
		"FORGE CODE/GAME MAPS/XBOX MAPS",
		"../FORGE CODE/GAME MAPS/XBOX MAPS"
	};
	char candidate_stems[8][64];
	int stem_count, s, slot, d;

	if (!req_path || !req_path[0])
		return NONE;

	/* 0. If the active in-memory donor map already contains this tag, load from it without re-reading disk! */
	if (s_runtime_xbox_donor.fp && s_runtime_xbox_donor.tag_buf && s_runtime_xbox_donor.path[0])
	{
		long hdl = load_native_xbox_tag_from_decompressed_map(
			s_runtime_xbox_donor.path, req_group, req_path, out_actual_group);
		if (hdl != NONE)
			return hdl;
	}

	/* 1. Check exact donor map(s) indexed in OpenCE Forge/credits.md FIRST so we never scan unrelated maps */
	stem_count = find_donor_map_stems_from_credits(req_group, req_path, candidate_stems, 8);
	for (s = 0; s < stem_count; s++)
	{
		char xcache_cand[512];
		snprintf(xcache_cand, sizeof(xcache_cand), "OpenCE Forge/cached_tags/xbox_cache/%s.map", candidate_stems[s]);
		{
			long hdl = load_native_xbox_tag_from_decompressed_map(
				xcache_cand, req_group, req_path, out_actual_group);
			if (hdl != NONE)
				return hdl;
		}
		for (d = 0; d < (int)(sizeof(s_xbox_map_dirs) / sizeof(s_xbox_map_dirs[0])); d++)
		{
			char full_path[512];
			char cache_path[512];
			snprintf(full_path, sizeof(full_path), "%s/%s.map", s_xbox_map_dirs[d], candidate_stems[s]);
			if (is_xbox_version5_map_file(full_path) &&
				ensure_xbox_map_decompressed_cache(full_path, cache_path, sizeof(cache_path)))
			{
				long hdl = load_native_xbox_tag_from_decompressed_map(cache_path, req_group, req_path, out_actual_group);
				if (hdl != NONE)
					return hdl;
			}
		}
	}

	/* 2. Check OpenCE's Xbox hard-drive cache (z:\cache000.map .. z:\cache005.map and z:\cache_donor.map) */
	for (slot = 0; slot <= 6; slot++)
	{
		char z_xbox[64];
		char z_path[512];
		long hdl;
		if (slot < 6)
			snprintf(z_xbox, sizeof(z_xbox), "z:\\cache00%d.map", slot);
		else
			snprintf(z_xbox, sizeof(z_xbox), "z:\\cache_donor.map");
		platform_translate_path(z_xbox, z_path, sizeof(z_path));
		hdl = load_native_xbox_tag_from_decompressed_map(z_path, req_group, req_path, out_actual_group);
		if (hdl != NONE)
			return hdl;
	}

	/* 3. Fallback: scan Xbox maps in maps/ */
	for (d = 0; d < (int)(sizeof(s_xbox_map_dirs) / sizeof(s_xbox_map_dirs[0])); d++)
	{
		char pattern[512];
		WIN32_FIND_DATAA fd;
		HANDLE h;
		snprintf(pattern, sizeof(pattern), "%s\\*.map", s_xbox_map_dirs[d]);
		h = FindFirstFileA(pattern, &fd);
		if (h != INVALID_HANDLE_VALUE)
		{
			do
			{
				char full_path[512];
				char cache_path[512];
				if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
					continue;
				if (_stricmp(fd.cFileName, "bitmaps.map") == 0 ||
					_stricmp(fd.cFileName, "sounds.map") == 0 ||
					_stricmp(fd.cFileName, "loc.map") == 0 ||
					_stricmp(fd.cFileName, "ui.map") == 0)
					continue;
				snprintf(full_path, sizeof(full_path), "%s/%s", s_xbox_map_dirs[d], fd.cFileName);
				if (is_xbox_version5_map_file(full_path) &&
					ensure_xbox_map_decompressed_cache(full_path, cache_path, sizeof(cache_path)))
				{
					long hdl = load_native_xbox_tag_from_decompressed_map(cache_path, req_group, req_path, out_actual_group);
					if (hdl != NONE)
					{
						CloseHandle(h);
						return hdl;
					}
				}
			} while (FindNextFileA(h, &fd));
			CloseHandle(h);
		}
	}
#else
	(void)req_group;
	(void)req_path;
	(void)out_actual_group;
#endif
	return NONE;
}

struct faber_bundle_read_ctx
{
	long handle;
	unsigned long file_data_offset;
	unsigned long file_data_size;
};

static boolean faber_bundle_file_read_cb(
	void *context,
	unsigned long offset,
	unsigned long size,
	void *buffer)
{
	struct faber_bundle_read_ctx const *ctx = (struct faber_bundle_read_ctx const *)context;
	if (!ctx || ctx->handle == NONE)
		return FALSE;
	if (offset > ctx->file_data_size || size > ctx->file_data_size - offset)
		return FALSE;
	return port_tag_profiles_bundle_read_at(
		ctx->handle,
		ctx->file_data_offset + offset,
		buffer,
		size) ? TRUE : FALSE;
}

static void faber_retarget_schema_fields(
	struct tag_bundle_loaded const *loaded,
	long const *datums,
	uint8_t *element,
	struct tag_schema_definition const *definition,
	int depth)
{
	struct tag_schema_field const *field;

	if (depth >= 24 || !element || !definition || !definition->fields)
		return;
	for (field = definition->fields; field->type != _tag_schema_terminator; field++)
	{
		int16_t item;
		int16_t fcount = (field->count > 0) ? field->count : 1;
		uint32_t fstride = (field->size > 0) ? (uint32_t)field->size : 0;

		for (item = 0; item < fcount; item++)
		{
			uint8_t *place = element + (uint32_t)field->offset + (uint32_t)item * fstride;

			switch (field->type)
			{
			case _tag_schema_reference:
			{
				struct tag_reference *ref = (struct tag_reference *)place;
				long b_idx = ref->index;
				long datum = (b_idx >= 0 && b_idx < loaded->tag_count) ? datums[b_idx] : NONE;

				ref->index = datum;
				if (ref->group_tag == 'mod2')
					ref->group_tag = 'mode';
				else if (ref->group_tag == 'scex')
					ref->group_tag = 'schi';
				if (datum == NONE)
				{
					ref->name = (char *)loaded->names + loaded->names_size - 1;
					ref->name_length = 0;
				}
				else
				{
					char const *tname = tag_get_name(datum);
					ref->group_tag = tag_get_group_tag(datum);
					ref->name = (char *)tname;
					ref->name_length = tname ? (long)strlen(tname) : 0;
				}
				break;
			}
			case _tag_schema_tag_index:
			{
				long b_idx = *(long const *)place;
				*(long *)place = (b_idx >= 0 && b_idx < loaded->tag_count) ? datums[b_idx] : NONE;
				break;
			}
			case _tag_schema_struct:
				faber_retarget_schema_fields(
					loaded,
					datums,
					place,
					(struct tag_schema_definition const *)field->definition,
					depth + 1);
				break;
			case _tag_schema_block:
			{
				struct tag_block *block = (struct tag_block *)place;
				struct tag_schema_definition const *elem_def =
					(struct tag_schema_definition const *)field->definition;
				long k;
				for (k = 0; elem_def && block->address && k < block->count; k++)
				{
					faber_retarget_schema_fields(
						loaded,
						datums,
						(uint8_t *)block->address + (size_t)k * (size_t)elem_def->size,
						elem_def,
						depth + 1);
				}
				break;
			}
			default:
				break;
			}
		}
	}
}

static long load_faber_vault_tag_bundle_runtime(
	uint32_t req_group,
	char const *req_path,
	uint32_t *out_actual_group)
{
	long prof_count, p, t, found_p = -1, found_t = -1;
	unsigned long bundle_size = 0;
	long b_handle = NONE;
	byte header_bytes[TAG_BUNDLE_HEADER_BYTES];
	unsigned long tags_size;
	byte *bundle_bytes = NULL;
	struct tag_bundle_loaded loaded;
	long *datums = NULL;
	uint8_t *is_new_tag = NULL;
	struct faber_bundle_read_ctx read_ctx;
	long ti;
	long root_handle = NONE;

	if (!req_path || !req_path[0])
		return NONE;

	prof_count = port_tag_profiles_count();
	if (prof_count <= 0)
	{
		port_tag_profiles_scan();
		prof_count = port_tag_profiles_count();
	}
	for (p = 0; p < prof_count && found_p < 0; p++)
	{
		long tc = port_tag_profiles_tag_count(p);
		for (t = 0; t < tc; t++)
		{
			unsigned long grp;
			char const *tname;
			if (!port_tag_profiles_tag_enabled(p, t))
				continue;
			grp = port_tag_profiles_tag_group(p, t);
			tname = port_tag_profiles_tag_name(p, t);
			if ((grp == (unsigned long)req_group || (req_group == 'mode' && grp == 'mod2')) &&
				tname && _stricmp(tname, req_path) == 0)
			{
				found_p = p;
				found_t = t;
				break;
			}
		}
	}
	if (found_p < 0)
		return NONE;

	b_handle = port_tag_profiles_bundle_open(found_p, found_t, &bundle_size);
	if (b_handle == NONE || bundle_size < TAG_BUNDLE_HEADER_BYTES ||
		!port_tag_profiles_bundle_read_at(b_handle, 0, header_bytes, TAG_BUNDLE_HEADER_BYTES))
	{
		if (b_handle != NONE)
			port_tag_profiles_bundle_close(b_handle);
		return NONE;
	}
	tags_size = tag_bundle_file_data_offset(header_bytes);
	if (tags_size < TAG_BUNDLE_HEADER_BYTES || tags_size > bundle_size || tags_size > 64u * 1024u * 1024u)
	{
		port_tag_profiles_bundle_close(b_handle);
		return NONE;
	}

	bundle_bytes = (byte *)forge_phys_alloc(tags_size);
	if (!bundle_bytes || !port_tag_profiles_bundle_read_at(b_handle, 0, bundle_bytes, tags_size))
	{
		port_tag_profiles_bundle_close(b_handle);
		return NONE;
	}
	memset(&loaded, 0, sizeof(loaded));
	if (tag_bundle_load_tags(bundle_bytes, tags_size, bundle_size, &loaded) != _tag_bundle_status_ok ||
		loaded.tag_count <= 0)
	{
		port_tag_profiles_bundle_close(b_handle);
		return NONE;
	}

	datums = (long *)malloc((size_t)loaded.tag_count * sizeof(long));
	is_new_tag = (uint8_t *)calloc((size_t)loaded.tag_count, 1);
	if (!datums || !is_new_tag)
	{
		free(datums);
		free(is_new_tag);
		port_tag_profiles_bundle_close(b_handle);
		return NONE;
	}
	for (ti = 0; ti < loaded.tag_count; ti++)
		datums[ti] = NONE;

	read_ctx.handle = b_handle;
	read_ctx.file_data_offset = loaded.file_data_offset;
	read_ctx.file_data_size = loaded.file_data_size;

	/* 1. Convert Custom Edition 'mod2' models and register all bundle tags */
	for (ti = 0; ti < loaded.tag_count; ti++)
	{
		byte const *entry = loaded.tags + ti * TAG_BUNDLE_TAG_BYTES;
		uint32_t grp = *(uint32_t const *)(entry + TAG_BUNDLE_TAG_GROUP_OFFSET);
		uint32_t p0 = *(uint32_t const *)(entry + TAG_BUNDLE_TAG_PARENT_GROUPS_OFFSET);
		uint32_t p1 = *(uint32_t const *)(entry + TAG_BUNDLE_TAG_PARENT_GROUPS_OFFSET + 4);
		char const *tname = loaded.names + *(uint32_t const *)(entry + TAG_BUNDLE_TAG_NAME_OFFSET);
		uint8_t *root_buf = loaded.data + *(uint32_t const *)(entry + TAG_BUNDLE_TAG_ROOT_OFFSET);
		uint32_t actual_grp = (grp == 'mod2') ? 'mode' : ((grp == 'scex') ? 'schi' : grp);

		if (grp == 'scnr' || grp == 'sbsp' || grp == 'matg' || grp == 'udlg')
			continue;

		if (ti > 0 && custom_edition_cache_tags_loaded() &&
			grp != 'bitm' && grp != 'mod2' && grp != 'mode' && grp != 'snd!' && grp != 'lsnd' &&
			grp != 'shdr' && grp != 'senv' && grp != 'soso' && grp != 'sotr' &&
			grp != 'schi' && grp != 'scex' && grp != 'swat' && grp != 'sgla' &&
			grp != 'smet' && grp != 'spla')
		{
			long ex = tag_loaded((long)actual_grp, tname);
			if (ex != NONE)
			{
				datums[ti] = ex;
				continue;
			}
		}

		if (grp == 'mod2')
		{
			if (!custom_edition_injected_model_convert(
					(struct model *)root_buf,
					tname,
					loaded.file_data_size,
					faber_bundle_file_read_cb,
					&read_ctx))
			{
				continue;
			}
		}
		else if (grp == 'scex')
		{
			p0 = 'shdr';
			p1 = NONE;
		}

		{
			char reg_name[256];
			if (ti > 0 && tag_loaded((long)actual_grp, tname) != NONE)
			{
				long ex_cnt = 0;
				cache_files_tag_instances(&ex_cnt);
				snprintf(reg_name, sizeof(reg_name), "%s__f%ld", tname, ex_cnt);
			}
			else
			{
				snprintf(reg_name, sizeof(reg_name), "%s", tname);
			}
			datums[ti] = append_runtime_tag_instance(
				(long)actual_grp,
				(long)p0,
				(long)p1,
				reg_name,
				root_buf);
		}
		if (datums[ti] != NONE)
			is_new_tag[ti] = 1;
	}

	/* 2. Retarget all tag_references and tag_indices across the bundle */
	for (ti = 0; ti < loaded.tag_count; ti++)
	{
		byte const *entry = loaded.tags + ti * TAG_BUNDLE_TAG_BYTES;
		uint32_t grp = *(uint32_t const *)(entry + TAG_BUNDLE_TAG_GROUP_OFFSET);
		uint8_t *root_buf = loaded.data + *(uint32_t const *)(entry + TAG_BUNDLE_TAG_ROOT_OFFSET);
		struct tag_schema_group const *schema_grp;

		if (!is_new_tag[ti] || datums[ti] == NONE)
			continue;
		schema_grp = forge_runtime_schema_group_get(grp == 'mod2' ? 'mode' : grp);
		if (schema_grp && schema_grp->definition)
			faber_retarget_schema_fields(&loaded, datums, root_buf, schema_grp->definition, 0);
	}

	/* 3. Initialize bitmaps, sounds, shaders, lights, and vehicles */
	for (ti = 0; ti < loaded.tag_count; ti++)
	{
		byte const *entry = loaded.tags + ti * TAG_BUNDLE_TAG_BYTES;
		uint32_t grp = *(uint32_t const *)(entry + TAG_BUNDLE_TAG_GROUP_OFFSET);
		uint8_t *root_buf = loaded.data + *(uint32_t const *)(entry + TAG_BUNDLE_TAG_ROOT_OFFSET);

		if (!is_new_tag[ti] || datums[ti] == NONE)
			continue;

		if (grp == 'bitm')
		{
			struct bitmap_group *bgrp = (struct bitmap_group *)root_buf;
			unsigned char chan_order = 0;
			long k;
			int bi;

			*(uint32_t *)(root_buf + 0x38u) = 0;
			*(void **)(root_buf + 0x44u) = NULL;

			{
				int used_mp = 0, used_rgba = 0;
				for (k = 0; k < loaded.tag_count; k++)
				{
					byte const *s_ent = loaded.tags + k * TAG_BUNDLE_TAG_BYTES;
					uint32_t s_grp = *(uint32_t const *)(s_ent + TAG_BUNDLE_TAG_GROUP_OFFSET);
					uint8_t const *s_buf = loaded.data + *(uint32_t const *)(s_ent + TAG_BUNDLE_TAG_ROOT_OFFSET);
					if (s_grp != 'soso')
						continue;
					if (*(int32_t const *)(s_buf + 0xC8u) == (int32_t)datums[ti])
						used_mp = 1;
					if (*(int32_t const *)(s_buf + 0xB0u) == (int32_t)datums[ti] ||
						*(int32_t const *)(s_buf + 0xE8u) == (int32_t)datums[ti] ||
						*(int32_t const *)(s_buf + 0x170u) == (int32_t)datums[ti])
						used_rgba = 1;
				}
				if (used_mp && !used_rgba)
					chan_order = 1;
			}

			for (bi = 0; bi < bgrp->bitmaps.count && bgrp->bitmaps.address; bi++)
			{
				struct bitmap_data *bmp = (struct bitmap_data *)((uint8_t *)bgrp->bitmaps.address + (size_t)bi * sizeof(struct bitmap_data));
				unsigned long pix_off = (unsigned long)bmp->pixels_offset;
				size_t src_len = (bmp->pixels_size > 0) ? (size_t)bmp->pixels_size : 0u;
				short max_mips;
				long req_bytes;
				size_t alloc_bytes;
				void *pix_copy;
				D3DBaseTexture *tex;

				bmp->base_address = NULL;
				bmp->hardware_format = NULL;
				bmp->cache_block_index = NONE;
				bmp->tag_index = datums[ti];
				bmp->signature = 'bitm';
				bmp->flags &= (unsigned short)~(0x80u | 0x08u | 0x10u | 0x100u);
				if (bmp->width > 0 && (bmp->width & (bmp->width - 1)) == 0 &&
					bmp->height > 0 && (bmp->height & (bmp->height - 1)) == 0)
					bmp->flags |= 0x01u;
				if (bmp->format >= 14 && bmp->format <= 16)
					bmp->flags |= 0x02u;
				else
					bmp->flags &= (unsigned short)~0x02u;
				if (bmp->format == 17)
					bmp->flags |= 0x04u;

				if (src_len == 0 || bmp->width <= 0 || bmp->height <= 0 || bmp->depth <= 0 ||
					pix_off > loaded.file_data_size || src_len > loaded.file_data_size - pix_off)
					continue;

				max_mips = rasterizer_xbox_bitmap_get_max_mipmap_count(bmp);
				if (bmp->mipmap_count < 0)
					bmp->mipmap_count = 0;
				if (bmp->mipmap_count > max_mips)
					bmp->mipmap_count = max_mips;
				while (bmp->mipmap_count > 0 && (size_t)bitmap_get_pixel_data_size(bmp) > src_len)
					bmp->mipmap_count--;

				req_bytes = rasterizer_xbox_bitmap_get_pixel_data_size(bmp);
				if (req_bytes <= 0)
					continue;
				alloc_bytes = ((size_t)req_bytes > src_len) ? (size_t)req_bytes : src_len;
				pix_copy = forge_phys_alloc(alloc_bytes);
				tex = (D3DBaseTexture *)calloc(1, sizeof(D3DBaseTexture));
				if (!pix_copy || !tex ||
					!faber_bundle_file_read_cb(&read_ctx, pix_off, (unsigned long)src_len, pix_copy))
				{
					free(tex);
					continue;
				}
				bmp->base_address = pix_copy;
				bmp->pixels_offset = 0;
				bmp->pixels_size = (long)alloc_bytes;

				halo_custom_edition_texels_channels(bmp->base_address, chan_order);
				rasterizer_xbox_bitmap_rebuild_hardware_format(bmp);

				tex->Data = 0;
				tex->Lock = 0;
				tex->Common = D3DCOMMON_TYPE_TEXTURE | 1;
				if (TEST_FLAG(bmp->flags, 4))
				{
					tex->Format =
						(bitmap_format_to_d3d_linear_format(bmp->format, bmp->flags) << D3DFORMAT_FORMAT_SHIFT) |
						(1 << D3DFORMAT_MIPMAP_SHIFT) |
						(2 << D3DFORMAT_DIMENSION_SHIFT) |
						D3DFORMAT_BORDERSOURCE_COLOR |
						D3DFORMAT_DMACHANNEL_A;
					tex->Size =
						(((bitmap_mipmap_get_row_pitch(bmp, 0) + D3DTEXTURE_PITCH_ALIGNMENT - 1) / D3DTEXTURE_PITCH_ALIGNMENT - 1)
							<< D3DSIZE_PITCH_SHIFT) |
						((bmp->height - 1) << D3DSIZE_HEIGHT_SHIFT) |
						(bmp->width - 1);
				}
				else
				{
					tex->Format =
						(floor_log2(bmp->depth) << D3DFORMAT_PSIZE_SHIFT) |
						(floor_log2(bmp->height) << D3DFORMAT_VSIZE_SHIFT) |
						(floor_log2(bmp->width) << D3DFORMAT_USIZE_SHIFT) |
						(bitmap_format_to_d3d_format(bmp->format, bmp->flags) << D3DFORMAT_FORMAT_SHIFT) |
						((bmp->type == 1 ? 3 : 2) << D3DFORMAT_DIMENSION_SHIFT) |
						((rasterizer_xbox_bitmap_get_max_mipmap_count(bmp) + 1) << D3DFORMAT_MIPMAP_SHIFT) |
						(bmp->type == 2 ? D3DFORMAT_CUBEMAP : 0) |
						D3DFORMAT_BORDERSOURCE_COLOR |
						D3DFORMAT_DMACHANNEL_A;
					tex->Size = 0;
				}
				IDirect3DBaseTexture8_Register(tex, bmp->base_address);
				bmp->hardware_format = tex;
			}
		}
		else if (grp == 'snd!')
		{
			struct sound_definition *sdef = (struct sound_definition *)root_buf;
			int pr_i, pm_i;
			sdef->promotion_sound.index = NONE;
			sdef->promotion_count = 0;
			for (pr_i = 0; pr_i < sdef->pitch_ranges.count && sdef->pitch_ranges.address; pr_i++)
			{
				struct sound_pitch_range *pr = (struct sound_pitch_range *)((uint8_t *)sdef->pitch_ranges.address + (size_t)pr_i * sizeof(struct sound_pitch_range));
				pr->played_permutation_mask = 0xFFFFFFFFu;
				pr->previous_permutation_index = NONE;
				pr->forced_permutation_index = NONE;
				for (pm_i = 0; pm_i < pr->permutations.count && pr->permutations.address; pm_i++)
				{
					struct sound_permutation *perm = (struct sound_permutation *)((uint8_t *)pr->permutations.address + (size_t)pm_i * sizeof(struct sound_permutation));
					unsigned long s_off = (unsigned long)perm->samples.file_offset;
					unsigned long s_sz = (perm->samples.size > 0) ? (unsigned long)perm->samples.size : 0u;
					perm->unknown0 = NONE;
					perm->unknown1 = 0;
					perm->unknown2 = (unsigned long)datums[ti];
					perm->unknown3 = (unsigned long)datums[ti];
					perm->samples.address = NULL;
					if (s_sz > 0 && s_sz <= MAXIMUM_SOUND_DATA_SIZE &&
						s_off <= loaded.file_data_size && s_sz <= loaded.file_data_size - s_off)
					{
						void *sbuf = forge_phys_alloc(s_sz);
						if (sbuf && faber_bundle_file_read_cb(&read_ctx, s_off, s_sz, sbuf))
						{
							perm->samples.address = sbuf;
							perm->samples.file_offset = 0;
							forge_register_dyn_sound_perm(perm);
						}
						else
						{
							perm->samples.size = 0;
						}
					}
					else
					{
						perm->samples.size = 0;
					}
				}
			}
		}
		else if (grp == 'ligh')
		{
			patch_light_definition_buffer(root_buf);
		}
	}

	root_handle = datums[0];
	if (root_handle != NONE)
	{
		if (req_group == 'vehi')
			patch_vehicle_tag_for_player_driving(root_handle, req_path);
		else if (req_group == 'scen' || req_group == 'mach' || req_group == 'lifi' || req_group == 'ctrl')
			patch_forge_device_and_light_tag_runtime(root_handle, req_group, req_path);
		if (s_dyn_tags.count < FORGE_MAX_DYNAMIC_TAGS)
		{
			struct forge_dynamic_tag_entry *d_entry = &s_dyn_tags.entries[s_dyn_tags.count++];
			uint8_t const *root0 = loaded.data + *(uint32_t const *)(loaded.tags + TAG_BUNDLE_TAG_ROOT_OFFSET);
			memset(d_entry, 0, sizeof(*d_entry));
			d_entry->group_tag = req_group;
			snprintf(d_entry->tag_path, sizeof(d_entry->tag_path), "%s", req_path);
			d_entry->definition_index = root_handle;
			d_entry->part_count = 0;
			d_entry->bounding_radius = *(float const *)(root0 + 0x04u);
			if (!(d_entry->bounding_radius > 0.1f && d_entry->bounding_radius < 100.0f))
				d_entry->bounding_radius = 1.5f;
		}
		if (out_actual_group)
			*out_actual_group = (uint32_t)tag_get_group_tag(root_handle);
	}

	free(datums);
	free(is_new_tag);
	port_tag_profiles_bundle_close(b_handle);
	return root_handle;
}

struct fxtg_bundle_header
{
	uint32_t magic;
	uint32_t group_tag;
	uint32_t uncompressed_size;
	uint32_t compressed_size;
	char tag_path[256];
	char custom_name[64];
	char source_map[64];
};

static long load_compressed_xbox_bundle_from_file(
	char const *fxtg_path,
	uint32_t req_group,
	char const *req_path,
	uint32_t *out_actual_group)
{
	FILE *fp;
	struct fxtg_bundle_header hdr;
	uint8_t *comp_buf;
	uint8_t *uncomp_buf;
	unsigned long dest_len;
	char donor_path[512];
	FILE *out_fp;
	long hdl;

	fp = fopen(fxtg_path, "rb");
	if (!fp)
		return NONE;
	if (fread(&hdr, 1, sizeof(hdr), fp) != sizeof(hdr) ||
		hdr.magic != 0x31545846u /* 'FXT1' */ ||
		hdr.group_tag != req_group ||
		_stricmp(hdr.tag_path, req_path) != 0 ||
		hdr.compressed_size == 0 || hdr.compressed_size > 64u * 1024u * 1024u ||
		hdr.uncompressed_size < 2048u || hdr.uncompressed_size > 128u * 1024u * 1024u)
	{
		fclose(fp);
		return NONE;
	}
	comp_buf = (uint8_t *)malloc(hdr.compressed_size);
	uncomp_buf = (uint8_t *)malloc(hdr.uncompressed_size);
	if (!comp_buf || !uncomp_buf ||
		fread(comp_buf, 1, hdr.compressed_size, fp) != hdr.compressed_size)
	{
		free(comp_buf);
		free(uncomp_buf);
		fclose(fp);
		return NONE;
	}
	fclose(fp);

	dest_len = hdr.uncompressed_size;
	if (z_uncompress(uncomp_buf, &dest_len, comp_buf, hdr.compressed_size) != Z_OK ||
		dest_len != hdr.uncompressed_size)
	{
		free(comp_buf);
		free(uncomp_buf);
		return NONE;
	}
	free(comp_buf);

	platform_translate_path("z:\\cache_donor.map", donor_path, sizeof(donor_path));
	out_fp = fopen(donor_path, "wb");
	if (!out_fp)
	{
		free(uncomp_buf);
		return NONE;
	}
	fwrite(uncomp_buf, 1, hdr.uncompressed_size, out_fp);
	fclose(out_fp);
	free(uncomp_buf);

	hdl = load_native_xbox_tag_from_decompressed_map(donor_path, req_group, req_path, out_actual_group);
	remove(donor_path);
	return hdl;
}

static long load_compressed_xbox_bundle_runtime(
	uint32_t req_group,
	char const *req_path,
	uint32_t *out_actual_group)
{
#ifdef _WIN32
	static char const *const dirs[] = {
		"OpenCE Forge/custom_tags",
		"custom_tags"
	};
	int d;
	for (d = 0; d < 2; d++)
	{
		char pat[512];
		WIN32_FIND_DATAA fd;
		HANDLE h;
		snprintf(pat, sizeof(pat), "%s\\*.fxtg", dirs[d]);
		h = FindFirstFileA(pat, &fd);
		if (h == INVALID_HANDLE_VALUE)
			continue;
		do
		{
			if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
			{
				char full[512];
				long hdl;
				snprintf(full, sizeof(full), "%s/%s", dirs[d], fd.cFileName);
				hdl = load_compressed_xbox_bundle_from_file(full, req_group, req_path, out_actual_group);
				if (hdl != NONE)
				{
					CloseHandle(h);
					return hdl;
				}
			}
		} while (FindNextFileA(h, &fd));
		CloseHandle(h);
	}
#else
	(void)req_group;
	(void)req_path;
	(void)out_actual_group;
#endif
	return NONE;
}

static long register_dynamic_cached_tag(uint32_t group_tag, char const *tag_path, uint32_t *out_actual_group)
{
	int i;
	long native_idx;

	/* Check if already registered (or negative-cached) for the current map */
	for (i = 0; i < s_dyn_tags.count; i++)
	{
		if (s_dyn_tags.entries[i].group_tag == group_tag &&
			_stricmp(s_dyn_tags.entries[i].tag_path, tag_path) == 0)
		{
			if (out_actual_group && s_dyn_tags.entries[i].definition_index != NONE)
				*out_actual_group = (uint32_t)tag_get_group_tag(s_dyn_tags.entries[i].definition_index);
			return s_dyn_tags.entries[i].definition_index;
		}
	}

	if (s_dyn_tags.count >= FORGE_MAX_DYNAMIC_TAGS)
		return NONE;

	/* 0a. Check compressed Faber Tag Vault (.fbrtag + faber_vault.bin) in OpenCE Forge/tag_vault/ */
	native_idx = load_faber_vault_tag_bundle_runtime(group_tag, tag_path, out_actual_group);
	if (native_idx != NONE)
		return native_idx;

	/* 0b. Check compressed Xbox tag bundle (.fxtg) in OpenCE Forge/custom_tags/ */
	native_idx = load_compressed_xbox_bundle_runtime(group_tag, tag_path, out_actual_group);
	if (native_idx != NONE)
		return native_idx;

	/* 1. Check if an exact .ocetags package exists in custom_tags/ and load its complete tag tree natively! */
	native_idx = load_native_ocetags_package_runtime(group_tag, tag_path, out_actual_group);
	if (native_idx != NONE)
		return native_idx;

	/* 2. Load default Xbox tag natively from Xbox map cache using OpenCE's exact tag schema & hardware format! */
	native_idx = load_native_xbox_tag_runtime(group_tag, tag_path, out_actual_group);
	if (native_idx != NONE)
		return native_idx;

	/* Negative-cache missing tag so we never re-scan disk on every frame */
	if (s_dyn_tags.count < FORGE_MAX_DYNAMIC_TAGS)
	{
		struct forge_dynamic_tag_entry *entry = &s_dyn_tags.entries[s_dyn_tags.count++];
		memset(entry, 0, sizeof(*entry));
		entry->group_tag = group_tag;
		snprintf(entry->tag_path, sizeof(entry->tag_path), "%s", tag_path);
		entry->definition_index = NONE;
	}

	return NONE;
}

static long forge_try_get_loaded_engine_tag(
	uint32_t group_tag,
	char const *tag_path,
	uint32_t *out_actual_group,
	int *out_known_missing)
{
	long idx;
	int i;

	if (out_actual_group)
		*out_actual_group = group_tag;
	if (out_known_missing)
		*out_known_missing = 0;
	if (!tag_path || !tag_path[0])
		return NONE;

	for (i = 0; i < s_dyn_tags.count; i++)
	{
		if (s_dyn_tags.entries[i].group_tag == group_tag &&
			_stricmp(s_dyn_tags.entries[i].tag_path, tag_path) == 0)
		{
			if (s_dyn_tags.entries[i].definition_index == NONE)
			{
				if (out_known_missing)
					*out_known_missing = 1;
				return NONE;
			}
			if (out_actual_group)
				*out_actual_group = (uint32_t)tag_get_group_tag(s_dyn_tags.entries[i].definition_index);
			return s_dyn_tags.entries[i].definition_index;
		}
	}

	idx = find_native_or_mp_variant_tag(group_tag, tag_path);
	if (idx != NONE)
	{
		if (group_tag == 'vehi')
			patch_vehicle_tag_for_player_driving(idx, tag_path);
		return idx;
	}
	return NONE;
}

long forge_resolve_engine_tag(uint32_t group_tag, char const *tag_path, uint32_t *out_actual_group)
{
	long idx;
	int known_missing = 0;

	idx = forge_try_get_loaded_engine_tag(group_tag, tag_path, out_actual_group, &known_missing);
	if (idx != NONE || known_missing)
		return idx;

	if (!tag_path || !tag_path[0])
		return NONE;

	/* Dynamically load from custom_tags\*.ocetags or Xbox map cache into global_tag_instances! */
	return register_dynamic_cached_tag(group_tag, tag_path, out_actual_group);
}

static int ocetags_json_find_key_range(char const *buf, size_t start, size_t end, char const *key, size_t *out_val_pos)
{
	char pat[128];
	size_t pat_len;
	size_t p;

	snprintf(pat, sizeof(pat), "\"%s\"", key);
	pat_len = strlen(pat);
	if (end <= start || end - start < pat_len)
		return 0;
	for (p = start; p + pat_len <= end; p++)
	{
		if (memcmp(buf + p, pat, pat_len) == 0)
		{
			size_t q = p + pat_len;
			while (q < end && (buf[q] == ' ' || buf[q] == '\t' || buf[q] == '\r' || buf[q] == '\n'))
				q++;
			if (q < end && buf[q] == ':')
			{
				q++;
				while (q < end && (buf[q] == ' ' || buf[q] == '\t' || buf[q] == '\r' || buf[q] == '\n'))
					q++;
				*out_val_pos = q;
				return 1;
			}
		}
	}
	return 0;
}

static int ocetags_json_get_str(char const *buf, size_t start, size_t end, char const *key, char *out, size_t out_sz)
{
	size_t vpos = 0;
	size_t i = 0;

	if (!out || out_sz == 0)
		return 0;
	out[0] = 0;
	if (!ocetags_json_find_key_range(buf, start, end, key, &vpos))
		return 0;
	if (vpos >= end || buf[vpos] != '"')
		return 0;
	vpos++;
	while (vpos < end && buf[vpos] != '"' && i + 1 < out_sz)
	{
		if (buf[vpos] == '\\' && vpos + 1 < end)
		{
			vpos++;
			if (buf[vpos] == '\\' || buf[vpos] == '"' || buf[vpos] == '/')
				out[i++] = buf[vpos];
			else if (buf[vpos] == 'n')
				out[i++] = '\n';
			else
				out[i++] = buf[vpos];
		}
		else
		{
			out[i++] = buf[vpos];
		}
		vpos++;
	}
	out[i] = 0;
	return 1;
}

static int ocetags_json_get_u32(char const *buf, size_t start, size_t end, char const *key, uint32_t *out_val)
{
	size_t vpos = 0;
	if (!ocetags_json_find_key_range(buf, start, end, key, &vpos))
		return 0;
	if (vpos >= end)
		return 0;
	*out_val = (uint32_t)strtoul(buf + vpos, NULL, 10);
	return 1;
}

static int ocetags_find_matching_brace(char const *buf, size_t open_pos, size_t limit, char open_ch, char close_ch, size_t *out_close)
{
	int depth = 0;
	int in_str = 0;
	size_t p;

	for (p = open_pos; p < limit; p++)
	{
		char c = buf[p];
		if (in_str)
		{
			if (c == '\\' && p + 1 < limit)
				p++;
			else if (c == '"')
				in_str = 0;
		}
		else
		{
			if (c == '"')
				in_str = 1;
			else if (c == open_ch)
				depth++;
			else if (c == close_ch)
			{
				depth--;
				if (depth == 0)
				{
					*out_close = p;
					return 1;
				}
			}
		}
	}
	return 0;
}

static int ocetags_json_skip_value(char const *buf, size_t pos, size_t end, size_t *out_next)
{
	size_t close_pos = 0;
	while (pos < end && (buf[pos] == ' ' || buf[pos] == '\t' || buf[pos] == '\r' || buf[pos] == '\n'))
		pos++;
	if (pos >= end)
		return 0;
	if (buf[pos] == '{')
	{
		if (!ocetags_find_matching_brace(buf, pos, end, '{', '}', &close_pos))
			return 0;
		if (out_next)
			*out_next = close_pos + 1;
		return 1;
	}
	if (buf[pos] == '[')
	{
		if (!ocetags_find_matching_brace(buf, pos, end, '[', ']', &close_pos))
			return 0;
		if (out_next)
			*out_next = close_pos + 1;
		return 1;
	}
	if (buf[pos] == '"')
	{
		pos++;
		while (pos < end)
		{
			if (buf[pos] == '\\' && pos + 1 < end)
				pos += 2;
			else if (buf[pos] == '"')
			{
				pos++;
				break;
			}
			else
				pos++;
		}
		if (out_next)
			*out_next = pos;
		return 1;
	}
	while (pos < end && buf[pos] != ',' && buf[pos] != '}' && buf[pos] != ']' &&
		   buf[pos] != ' ' && buf[pos] != '\t' && buf[pos] != '\r' && buf[pos] != '\n')
		pos++;
	if (out_next)
		*out_next = pos;
	return 1;
}

struct ocetags_chunk_entry
{
	char id[64];
	char group[8];
	char path[256];
	char kind[48];
	uint32_t offset;
	uint32_t size;
	uint32_t vertex_count;
	uint32_t index_count;
	uint32_t width;
	uint32_t height;
	uint32_t format_code;
};

#define OCETAGS_MAX_CHUNKS 256

static uint32_t ocetags_group_str_to_fourcc(char const *grp)
{
	static struct { char const *name; uint32_t fourcc; } const s_map[] = {
		{ "actor", 'actr' },
		{ "actor_variant", 'actv' },
		{ "antenna", 'ant!' },
		{ "model_animations", 'antr' },
		{ "biped", 'bipd' },
		{ "bitmap", 'bitm' },
		{ "continuous_damage_effect", 'cdmg' },
		{ "model_collision_geometry", 'coll' },
		{ "color_table", 'colo' },
		{ "contrail", 'cont' },
		{ "device_control", 'ctrl' },
		{ "decal", 'deca' },
		{ "ui_widget_definition", 'DeLa' },
		{ "input_device_defaults", 'devc' },
		{ "device", 'devi' },
		{ "detail_object_collection", 'dobc' },
		{ "effect", 'effe' },
		{ "equipment", 'eqip' },
		{ "flag", 'flag' },
		{ "fog", 'fog ' },
		{ "font", 'font' },
		{ "material_effects", 'foot' },
		{ "garbage", 'garb' },
		{ "glow", 'glw!' },
		{ "grenade_hud_interface", 'grhi' },
		{ "hud_message_text", 'hmt ' },
		{ "hud_number", 'hud#' },
		{ "hud_globals", 'hudg' },
		{ "item", 'item' },
		{ "item_collection", 'itmc' },
		{ "damage_effect", 'jpt!' },
		{ "lens_flare", 'lens' },
		{ "lightning", 'elec' },
		{ "device_light_fixture", 'lifi' },
		{ "light", 'ligh' },
		{ "sound_looping", 'lsnd' },
		{ "device_machine", 'mach' },
		{ "globals", 'matg' },
		{ "meter", 'metr' },
		{ "light_volume", 'mgs2' },
		{ "gbxmodel", 'mod2' },
		{ "model", 'mode' },
		{ "multiplayer_scenario_description", 'mply' },
		{ "preferences_network_game", 'ngpr' },
		{ "object", 'obje' },
		{ "particle", 'part' },
		{ "particle_system", 'pctl' },
		{ "physics", 'phys' },
		{ "placeholder", 'plac' },
		{ "point_physics", 'pphy' },
		{ "projectile", 'proj' },
		{ "weather_particle_system", 'rain' },
		{ "scenario_structure_bsp", 'sbsp' },
		{ "scenery", 'scen' },
		{ "shader_transparent_chicago_extended", 'scex' },
		{ "shader_transparent_chicago", 'schi' },
		{ "scenario", 'scnr' },
		{ "shader_environment", 'senv' },
		{ "shader_transparent_glass", 'sgla' },
		{ "shader", 'shdr' },
		{ "sky", 'sky ' },
		{ "shader_transparent_meter", 'smet' },
		{ "sound", 'snd!' },
		{ "sound_environment", 'snde' },
		{ "shader_model", 'soso' },
		{ "shader_transparent_generic", 'sotr' },
		{ "ui_widget_collection", 'Soul' },
		{ "shader_transparent_plasma", 'spla' },
		{ "sound_scenery", 'ssce' },
		{ "string_list", 'str#' },
		{ "shader_transparent_water", 'swat' },
		{ "tag_collection", 'tagc' },
		{ "camera_track", 'trak' },
		{ "dialogue", 'udlg' },
		{ "unit_hud_interface", 'unhi' },
		{ "unit", 'unit' },
		{ "unicode_string_list", 'ustr' },
		{ "virtual_keyboard", 'vcky' },
		{ "vehicle", 'vehi' },
		{ "weapon", 'weap' },
		{ "wind", 'wind' },
		{ "weapon_hud_interface", 'wphi' }
	};
	size_t i;

	if (!grp || !grp[0])
		return 0;
	for (i = 0; i < sizeof(s_map) / sizeof(s_map[0]); i++)
	{
		if (_stricmp(grp, s_map[i].name) == 0)
			return s_map[i].fourcc;
	}
	if (strlen(grp) == 4)
	{
		return ((uint32_t)(uint8_t)grp[0] << 24) |
		       ((uint32_t)(uint8_t)grp[1] << 16) |
		       ((uint32_t)(uint8_t)grp[2] << 8) |
		       (uint32_t)(uint8_t)grp[3];
	}
	return 0;
}

static int ocetags_is_shader_fourcc(uint32_t fourcc)
{
	return (fourcc == 'soso' || fourcc == 'senv' || fourcc == 'schi' || fourcc == 'scex' ||
	        fourcc == 'sgla' || fourcc == 'smet' || fourcc == 'spla' || fourcc == 'swat' ||
	        fourcc == 'shdr');
}

static uint8_t *ocetags_json_decode_base64_key(
	char const *buf,
	size_t start,
	size_t end,
	char const *key,
	size_t *out_len)
{
	size_t vpos = 0;
	size_t b64_start, b64_len, cap;
	uint8_t *out;
	size_t o = 0;
	uint32_t val = 0;
	int valb = -8;
	size_t p;

	if (out_len)
		*out_len = 0;
	if (!ocetags_json_find_key_range(buf, start, end, key, &vpos))
		return NULL;
	if (vpos >= end || buf[vpos] != '"')
		return NULL;
	vpos++;
	b64_start = vpos;
	while (vpos < end && buf[vpos] != '"')
	{
		if (buf[vpos] == '\\' && vpos + 1 < end)
			vpos += 2;
		else
			vpos++;
	}
	b64_len = vpos - b64_start;
	cap = (b64_len * 3u) / 4u + 16u;
	out = (uint8_t *)malloc(cap);
	if (!out)
		return NULL;

	for (p = b64_start; p < vpos; p++)
	{
		unsigned char c = (unsigned char)buf[p];
		int d = -1;
		if (c >= 'A' && c <= 'Z') d = c - 'A';
		else if (c >= 'a' && c <= 'z') d = c - 'a' + 26;
		else if (c >= '0' && c <= '9') d = c - '0' + 52;
		else if (c == '+') d = 62;
		else if (c == '/') d = 63;
		else if (c == '=') break;
		else continue;

		val = (val << 6) | (uint32_t)d;
		valb += 6;
		if (valb >= 0)
		{
			if (o < cap)
				out[o++] = (uint8_t)((val >> valb) & 0xFFu);
			valb -= 8;
		}
	}
	if (out_len)
		*out_len = o;
	return out;
}

static int catalog_file_has_ocetags_entry(char const *gstr, char const *tag_path)
{
	static char const *const s_cat_files[2] = {
		"OpenCE Forge/cached_tags/custom_catalog.txt",
		"OpenCE Forge/cached_tags/xbox_catalog.txt"
	};
	char line[512];
	char needle[300];
	int f_idx;

	snprintf(needle, sizeof(needle), "|2|%s", tag_path);
	for (f_idx = 0; f_idx < 2; f_idx++)
	{
		FILE *f = fopen(s_cat_files[f_idx], "r");
		if (!f)
			continue;
		while (fgets(line, sizeof(line), f))
		{
			if (_strnicmp(line, gstr, 4) == 0 && strstr(line, needle) != NULL)
			{
				fclose(f);
				return 1;
			}
		}
		fclose(f);
	}
	return 0;
}

struct ocetags_v5_tag_span
{
	uint32_t fourcc;
	char group[32];
	char path[256];
	size_t obj_start;
	size_t obj_end;
	int is_2d_bitmap;
};

#define OCETAGS_V5_MAX_TAGS 512

static int forge_rip_ocetags_v5_json_to_cached_tags(
	char const *ocetags_path,
	char const *json,
	size_t json_len,
	int force_overwrite,
	FILE *manifest_fp,
	FILE *credits_fp)
{
	size_t tags_pos = 0, tags_end = 0;
	char custom_item_name[64];
	char root_group_str[32];
	uint32_t root_group = 0;
	char root_tag_path[256];
	size_t cur, obj_end = 0;
	char gstr[5];
	int cat_id;

	(void)force_overwrite;
	custom_item_name[0] = 0;
	root_group_str[0] = 0;
	root_tag_path[0] = 0;
	ocetags_json_get_str(json, 0, json_len, "itemName", custom_item_name, sizeof(custom_item_name));
	if (!custom_item_name[0])
		ocetags_json_get_str(json, 0, json_len, "displayName", custom_item_name, sizeof(custom_item_name));

	if (!ocetags_json_find_key_range(json, 0, json_len, "tags", &tags_pos) ||
	    tags_pos >= json_len || json[tags_pos] != '[' ||
	    !ocetags_find_matching_brace(json, tags_pos, json_len, '[', ']', &tags_end))
	{
		return 0;
	}

	cur = tags_pos + 1;
	while (cur < tags_end)
	{
		if (json[cur] == '{' && ocetags_find_matching_brace(json, cur, tags_end, '{', '}', &obj_end))
		{
			ocetags_json_get_str(json, cur, obj_end + 1, "group", root_group_str, sizeof(root_group_str));
			ocetags_json_get_str(json, cur, obj_end + 1, "path", root_tag_path, sizeof(root_tag_path));
			root_group = ocetags_group_str_to_fourcc(root_group_str);
			if (!custom_item_name[0])
			{
				ocetags_json_get_str(json, cur, obj_end + 1, "itemName", custom_item_name, sizeof(custom_item_name));
				if (!custom_item_name[0])
					ocetags_json_get_str(json, cur, obj_end + 1, "displayName", custom_item_name, sizeof(custom_item_name));
			}
			break;
		}
		cur++;
	}

	if (!root_group || !root_tag_path[0])
		return 0;

	cat_id = is_forge_light_beacon_emitter_tag(root_group, root_tag_path) ? _forge_cat_lights : _forge_cat_scenery;
	if (root_group == 'vehi') cat_id = _forge_cat_vehicles;
	else if (root_group == 'weap') cat_id = _forge_cat_weapons;
	else if (root_group == 'eqip') cat_id = _forge_cat_equipment;
	else if (root_group == 'bipd') cat_id = _forge_cat_bipeds;
	else if (root_group != 'scen') return 0;

	gstr[0] = (char)((root_group >> 24) & 0xFF);
	gstr[1] = (char)((root_group >> 16) & 0xFF);
	gstr[2] = (char)((root_group >> 8) & 0xFF);
	gstr[3] = (char)(root_group & 0xFF);
	gstr[4] = 0;

	if (catalog_file_has_ocetags_entry(gstr, root_tag_path))
		return 0;

	if (manifest_fp)
	{
		if (custom_item_name[0])
			fprintf(manifest_fp, "%s|%d|2|%s|%s\n", gstr, cat_id, root_tag_path, custom_item_name);
		else
			fprintf(manifest_fp, "%s|%d|2|%s\n", gstr, cat_id, root_tag_path);
		fflush(manifest_fp);
	}
	if (credits_fp)
	{
		fprintf(credits_fp, "- **`%s`** (`%s`) — native `.ocetags` package `%s`\r\n",
			root_tag_path, gstr, ocetags_path);
		fflush(credits_fp);
	}
	return 1;
}

struct ocetags_runtime_pkg_tag
{
	uint32_t fourcc;
	uint32_t actual_group;
	uint32_t orig_index;
	uint16_t orig_low;
	char path[256];
	size_t obj_start;
	size_t obj_end;
	uint8_t *tag_buf;
	size_t raw_len;
	long runtime_handle;
	int is_reused_existing;
};

static void ocetags_runtime_rebase_and_link_schema(
	struct tag_schema_definition const *def,
	uint8_t *elem_base,
	int32_t elem_count,
	uint8_t *tag_buf,
	size_t raw_len,
	uint32_t root_size,
	struct ocetags_runtime_pkg_tag const *pkg_tags,
	int pkg_tag_count,
	int depth)
{
	int32_t e;
	struct tag_schema_field const *field;
	uint32_t elem_size;

	if (!def || !def->fields || !elem_base || elem_count <= 0 || def->size <= 0 || depth > 10)
		return;

	elem_size = (uint32_t)def->size;
	for (e = 0; e < elem_count; e++)
	{
		uint8_t *elem_ptr = elem_base + (size_t)e * elem_size;
		for (field = def->fields; field->type != _tag_schema_terminator; field++)
		{
			int16_t k;
			int16_t fcount = (field->count > 0) ? field->count : 1;
			uint32_t fstride = (field->size > 0) ? (uint32_t)field->size : 0;

			for (k = 0; k < fcount; k++)
			{
				uint32_t f_off = (uint32_t)field->offset + (uint32_t)k * fstride;
				uint8_t *f_ptr = elem_ptr + f_off;

				if (field->type == _tag_schema_struct)
				{
					struct tag_schema_definition const *sub_def =
						(struct tag_schema_definition const *)field->definition;
					if (sub_def && f_off + (uint32_t)sub_def->size <= elem_size)
					{
						ocetags_runtime_rebase_and_link_schema(
							sub_def, f_ptr, 1, tag_buf, raw_len, root_size,
							pkg_tags, pkg_tag_count, depth + 1);
					}
				}
				else if (field->type == _tag_schema_block)
				{
					struct tag_schema_definition const *child_def =
						(struct tag_schema_definition const *)field->definition;
					if (child_def && child_def->size > 0 && f_off + 12u <= elem_size)
					{
						int32_t cnt = *(int32_t const *)(f_ptr + 0);
						uint32_t rel = *(uint32_t const *)(f_ptr + 4);
						uint32_t child_sz = (uint32_t)child_def->size;
						if (cnt > 0 && cnt <= 65536 &&
							rel >= root_size &&
							(uint64_t)rel + (uint64_t)cnt * child_sz <= (uint64_t)raw_len)
						{
							uint8_t *child_base = tag_buf + rel;
							*(int32_t *)(f_ptr + 0) = cnt;
							*(void **)(f_ptr + 4) = child_base;
							*(void **)(f_ptr + 8) = NULL;
							ocetags_runtime_rebase_and_link_schema(
								child_def, child_base, cnt, tag_buf, raw_len, root_size,
								pkg_tags, pkg_tag_count, depth + 1);
						}
						else
						{
							memset(f_ptr, 0, 12);
						}
					}
				}
				else if (field->type == _tag_schema_data)
				{
					if (f_off + 20u <= elem_size)
					{
						int32_t sz = *(int32_t const *)(f_ptr + 0);
						uint32_t rel = *(uint32_t const *)(f_ptr + 12);
						if (sz > 0 && sz <= 16 * 1024 * 1024 &&
							rel >= root_size &&
							(uint64_t)rel + (uint64_t)sz <= (uint64_t)raw_len)
						{
							*(int32_t *)(f_ptr + 0) = sz;
							*(uint32_t *)(f_ptr + 4) = 0;
							*(uint32_t *)(f_ptr + 8) = 0;
							*(void **)(f_ptr + 12) = tag_buf + rel;
							*(void **)(f_ptr + 16) = NULL;
						}
						else
						{
							memset(f_ptr, 0, 20);
						}
					}
				}
				else if (field->type == _tag_schema_reference)
				{
					if (f_off + 16u <= elem_size)
					{
						int32_t ref_idx = *(int32_t const *)(f_ptr + 12);
						long resolved_handle = NONE;
						uint32_t resolved_grp = *(uint32_t const *)(f_ptr + 0);
						if (ref_idx != -1 && ref_idx != 0)
						{
							uint16_t orig_low = (uint16_t)((uint32_t)ref_idx & 0xFFFFu);
							int m;
							for (m = 0; m < pkg_tag_count; m++)
							{
								if ((uint32_t)ref_idx == pkg_tags[m].orig_index || orig_low == pkg_tags[m].orig_low)
								{
									resolved_handle = pkg_tags[m].runtime_handle;
									resolved_grp = pkg_tags[m].actual_group;
									break;
								}
							}
						}
						if (resolved_grp == 'mod2') resolved_grp = 'mode';
						else if (resolved_grp == 'scex') resolved_grp = 'schi';
						*(uint32_t *)(f_ptr + 0) = (resolved_handle != NONE) ? resolved_grp : 0u;
						*(char const **)(f_ptr + 4) = NULL;
						*(int32_t *)(f_ptr + 8) = 0;
						*(int32_t *)(f_ptr + 12) = (int32_t)resolved_handle;
					}
				}
				else if (field->type == _tag_schema_tag_index)
				{
					if (f_off + 4u <= elem_size)
					{
						int32_t ref_idx = *(int32_t const *)f_ptr;
						long resolved_handle = NONE;
						if (ref_idx != -1 && ref_idx != 0)
						{
							uint16_t orig_low = (uint16_t)((uint32_t)ref_idx & 0xFFFFu);
							int m;
							for (m = 0; m < pkg_tag_count; m++)
							{
								if ((uint32_t)ref_idx == pkg_tags[m].orig_index || orig_low == pkg_tags[m].orig_low)
								{
									resolved_handle = pkg_tags[m].runtime_handle;
									break;
								}
							}
						}
						*(int32_t *)f_ptr = (int32_t)resolved_handle;
					}
				}
			}
		}
	}
}

static long load_native_ocetags_package_from_json(
	char const *json,
	size_t json_len,
	uint32_t req_group,
	char const *req_path,
	uint32_t *out_actual_group)
{
	size_t fmt_pos = 0, tags_pos = 0, tags_end = 0;
	struct ocetags_runtime_pkg_tag *pkg_tags = NULL;
	int pkg_tag_count = 0;
	int i;
	uint8_t **raw_assets = NULL;
	size_t *raw_asset_lens = NULL;
	int raw_asset_count = 0;
	long root_handle = NONE;

	if (!json || json_len < 32 || !req_path || !req_path[0])
		return NONE;
	if (!ocetags_json_find_key_range(json, 0, json_len, "formatVersion", &fmt_pos))
		return NONE;
	if (!ocetags_json_find_key_range(json, 0, json_len, "tags", &tags_pos) ||
		!ocetags_json_skip_value(json, tags_pos, json_len, &tags_end))
		return NONE;

	pkg_tags = (struct ocetags_runtime_pkg_tag *)calloc(OCETAGS_V5_MAX_TAGS, sizeof(*pkg_tags));
	if (!pkg_tags)
		return NONE;

	/* 1. Scan tags[] array and check if tags[0] matches (req_group, req_path) */
	{
		size_t p = tags_pos + 1;
		while (p < tags_end && pkg_tag_count < OCETAGS_V5_MAX_TAGS)
		{
			size_t obj_start, obj_end;
			while (p < tags_end && json[p] != '{' && json[p] != ']')
				p++;
			if (p >= tags_end || json[p] != '{')
				break;
			obj_start = p;
			if (!ocetags_json_skip_value(json, obj_start, tags_end, &obj_end))
				break;
			{
				struct ocetags_runtime_pkg_tag *pt = &pkg_tags[pkg_tag_count];
				char gstr[64];
				gstr[0] = 0;
				ocetags_json_get_str(json, obj_start, obj_end, "group", gstr, sizeof(gstr));
				ocetags_json_get_str(json, obj_start, obj_end, "path", pt->path, sizeof(pt->path));
				ocetags_json_get_u32(json, obj_start, obj_end, "originalIndex", &pt->orig_index);
				pt->orig_low = (uint16_t)(pt->orig_index & 0xFFFFu);
				pt->fourcc = ocetags_group_str_to_fourcc(gstr);
				pt->actual_group = (pt->fourcc == 'mod2') ? 'mode' : ((pt->fourcc == 'scex') ? 'schi' : pt->fourcc);
				pt->obj_start = obj_start;
				pt->obj_end = obj_end;
				pt->runtime_handle = NONE;
				if (pkg_tag_count == 0)
				{
					/* Match root tag! */
					if (pt->fourcc != req_group || _stricmp(pt->path, req_path) != 0)
					{
						free(pkg_tags);
						return NONE;
					}
				}
				pkg_tag_count++;
			}
			p = obj_end;
		}
	}

	if (pkg_tag_count <= 0)
	{
		free(pkg_tags);
		return NONE;
	}

	/* 2. Decode each tag's raw binary data and register in global_tag_instances.
	 * Strictly keep Custom Edition .ocetags packages using their own Custom Edition bitmaps,
	 * shaders, models, and sounds from the .ocetags package! */
	for (i = 0; i < pkg_tag_count; i++)
	{
		struct ocetags_runtime_pkg_tag *pt = &pkg_tags[i];
		struct tag_schema_group const *schema_grp = forge_runtime_schema_group_get(pt->fourcc);
		long p0 = schema_grp ? schema_grp->parent_group_tags[0] : NONE;
		long p1 = schema_grp ? schema_grp->parent_group_tags[1] : NONE;
		uint8_t *decoded = NULL;
		size_t decoded_len = 0;
		size_t alloc_sz;

		if (pt->fourcc == 'scex')
		{
			p0 = 'shdr';
			p1 = NONE;
		}

		decoded = ocetags_json_decode_base64_key(json, pt->obj_start, pt->obj_end, "data", &decoded_len);
		if (!decoded || decoded_len == 0)
		{
			free(decoded);
			/* Only reuse an existing map tag if the .ocetags entry has no embedded raw data AND we are on a CE map */
			if (i > 0 && custom_edition_cache_tags_loaded())
			{
				long ex = tag_loaded((long)pt->actual_group, pt->path);
				if (ex != NONE)
				{
					pt->runtime_handle = ex;
					pt->is_reused_existing = 1;
				}
			}
			continue;
		}

		alloc_sz = (decoded_len > 4096u) ? (decoded_len + 256u) : 4096u;
		pt->tag_buf = (uint8_t *)calloc(1, alloc_sz);
		if (!pt->tag_buf)
		{
			free(decoded);
			continue;
		}
		memcpy(pt->tag_buf, decoded, decoded_len);
		pt->raw_len = decoded_len;
		free(decoded);

		pt->runtime_handle = append_runtime_tag_instance(
			(long)pt->actual_group, p0, p1, pt->path, pt->tag_buf);
	}

	/* 4. Rebase internal tag_blocks/tag_data and link all tag_references across the package */
	for (i = 0; i < pkg_tag_count; i++)
	{
		struct ocetags_runtime_pkg_tag *pt = &pkg_tags[i];
		struct tag_schema_group const *schema_grp;
		uint32_t root_sz;

		if (!pt->tag_buf || pt->is_reused_existing || pt->runtime_handle == NONE)
			continue;
		schema_grp = forge_runtime_schema_group_get(pt->fourcc);
		if (!schema_grp || !schema_grp->definition || schema_grp->definition->size <= 0)
			continue;
		root_sz = (uint32_t)schema_grp->definition->size;
		if (pt->raw_len < root_sz)
			continue;

		ocetags_runtime_rebase_and_link_schema(
			schema_grp->definition,
			pt->tag_buf,
			1,
			pt->tag_buf,
			pt->raw_len,
			root_sz,
			pkg_tags,
			pkg_tag_count,
			0);
	}

	/* 5. Decode per-tag embeddedRawDataAssets & embeddedRawDataPatches for Custom Edition 'bitm' and 'snd!' tags */
	for (i = 0; i < pkg_tag_count; i++)
	{
		struct ocetags_runtime_pkg_tag *pt = &pkg_tags[i];
		if (!pt->tag_buf || pt->is_reused_existing || pt->runtime_handle == NONE)
			continue;

		if (pt->fourcc == 'bitm' && pt->raw_len >= 0x6Cu)
		{
			struct bitmap_group *bgrp = (struct bitmap_group *)pt->tag_buf;
			uint8_t **tag_assets = NULL;
			size_t *tag_asset_lens = NULL;
			int tag_asset_count = 0;
			size_t assets_pos = 0, assets_end = 0;
			size_t patches_pos = 0, patches_end = 0;
			unsigned char chan_order = 0;
			int k, bi, ai;

			*(uint32_t *)(pt->tag_buf + 0x38u) = 0; /* clear processed_pixel_data.size */
			*(void **)(pt->tag_buf + 0x44u) = NULL;

			/* Check if any shader_model ('soso') uses this bitmap as multipurpose_map (+0xBC + 12 = +0xC8)
			 * without also using it as base_map (+0xB0), detail_map (+0xE8), or reflection_cube_map (+0x170) */
			{
				int used_mp = 0, used_rgba = 0;
				for (k = 0; k < pkg_tag_count; k++)
				{
					struct ocetags_runtime_pkg_tag *st = &pkg_tags[k];
					if (!st->tag_buf || st->fourcc != 'soso' || st->raw_len < 0x178u)
						continue;
					if (*(int32_t const *)(st->tag_buf + 0xC8u) == (int32_t)pt->runtime_handle)
						used_mp = 1;
					if (*(int32_t const *)(st->tag_buf + 0xB0u) == (int32_t)pt->runtime_handle ||
						*(int32_t const *)(st->tag_buf + 0xE8u) == (int32_t)pt->runtime_handle ||
						*(int32_t const *)(st->tag_buf + 0x170u) == (int32_t)pt->runtime_handle)
						used_rgba = 1;
				}
				if (used_mp && !used_rgba)
					chan_order = 1; /* _custom_edition_channels_multipurpose */
			}

			/* Decode per-tag embeddedRawDataAssets[] inside pt->obj_start .. pt->obj_end */
			if (ocetags_json_find_key_range(json, pt->obj_start, pt->obj_end, "embeddedRawDataAssets", &assets_pos) &&
				ocetags_json_skip_value(json, assets_pos, pt->obj_end, &assets_end))
			{
				size_t p = assets_pos + 1;
				tag_assets = (uint8_t **)calloc(64, sizeof(uint8_t *));
				tag_asset_lens = (size_t *)calloc(64, sizeof(size_t));
				while (tag_assets && tag_asset_lens && p < assets_end && tag_asset_count < 64)
				{
					size_t a_start, a_end;
					while (p < assets_end && json[p] != '{' && json[p] != ']')
						p++;
					if (p >= assets_end || json[p] != '{')
						break;
					a_start = p;
					if (!ocetags_json_skip_value(json, a_start, assets_end, &a_end))
						break;
					tag_assets[tag_asset_count] = ocetags_json_decode_base64_key(
						json, a_start, a_end, "data", &tag_asset_lens[tag_asset_count]);
					tag_asset_count++;
					p = a_end;
				}
			}

			for (bi = 0; bi < bgrp->bitmaps.count && bgrp->bitmaps.address; bi++)
			{
				struct bitmap_data *bmp = (struct bitmap_data *)((uint8_t *)bgrp->bitmaps.address + (size_t)bi * sizeof(struct bitmap_data));
				bmp->base_address = NULL;
				bmp->hardware_format = NULL;
				bmp->cache_block_index = NONE;
				bmp->tag_index = pt->runtime_handle;
			}

			/* Apply per-tag embeddedRawDataPatches[] (supports both patchOffset and offsetFieldOffset) */
			if (tag_asset_count > 0 &&
				ocetags_json_find_key_range(json, pt->obj_start, pt->obj_end, "embeddedRawDataPatches", &patches_pos) &&
				ocetags_json_skip_value(json, patches_pos, pt->obj_end, &patches_end))
			{
				size_t p = patches_pos + 1;
				while (p < patches_end)
				{
					size_t p_start, p_end;
					uint32_t a_idx = 0xFFFFFFFFu, patch_off = 0xFFFFFFFFu;
					while (p < patches_end && json[p] != '{' && json[p] != ']')
						p++;
					if (p >= patches_end || json[p] != '{')
						break;
					p_start = p;
					if (!ocetags_json_skip_value(json, p_start, patches_end, &p_end))
						break;
					if (ocetags_json_get_u32(json, p_start, p_end, "assetIndex", &a_idx) &&
						(ocetags_json_get_u32(json, p_start, p_end, "patchOffset", &patch_off) ||
						 ocetags_json_get_u32(json, p_start, p_end, "offsetFieldOffset", &patch_off)) &&
						a_idx < (uint32_t)tag_asset_count &&
						tag_assets[a_idx] && tag_asset_lens[a_idx] > 0 &&
						patch_off >= 24u && patch_off + 24u <= pt->raw_len)
					{
						struct bitmap_data *bmp = (struct bitmap_data *)(pt->tag_buf + (patch_off - 24u));
						bmp->base_address = tag_assets[a_idx];
						bmp->pixels_size = (long)tag_asset_lens[a_idx];
					}
					p = p_end;
				}
			}

			/* Finalize each bitmap_data entry: allocate in Xbox contiguous memory via forge_phys_alloc,
			 * convert Custom Edition texel layout via rasterizer_xbox_bitmap_rebuild_hardware_format,
			 * and initialize D3DBaseTexture hardware_format! */
			for (bi = 0; bi < bgrp->bitmaps.count && bgrp->bitmaps.address; bi++)
			{
				struct bitmap_data *bmp = (struct bitmap_data *)((uint8_t *)bgrp->bitmaps.address + (size_t)bi * sizeof(struct bitmap_data));
				uint8_t const *src_pixels = (uint8_t const *)bmp->base_address;
				size_t src_len = (bmp->pixels_size > 0) ? (size_t)bmp->pixels_size : 0u;
				short max_mips;
				long req_bytes;
				size_t alloc_bytes;
				void *pix_copy;
				D3DBaseTexture *tex;

				if ((!src_pixels || src_len == 0) && bi < tag_asset_count && tag_assets[bi] && tag_asset_lens[bi] > 0)
				{
					src_pixels = tag_assets[bi];
					src_len = tag_asset_lens[bi];
				}
				bmp->base_address = NULL;
				bmp->hardware_format = NULL;

				bmp->signature = 'bitm';
				bmp->flags &= (unsigned short)~(0x80u | 0x08u | 0x10u | 0x100u);
				if (bmp->width > 0 && (bmp->width & (bmp->width - 1)) == 0 &&
					bmp->height > 0 && (bmp->height & (bmp->height - 1)) == 0)
					bmp->flags |= 0x01u; /* _bitmap_power_of_two_dimensions_bit */
				if (bmp->format >= 14 && bmp->format <= 16)
					bmp->flags |= 0x02u; /* _bitmap_compressed_bit */
				else
					bmp->flags &= (unsigned short)~0x02u;
				if (bmp->format == 17)
					bmp->flags |= 0x04u; /* _bitmap_palettized_bit */

				if (!src_pixels || src_len == 0 || bmp->width <= 0 || bmp->height <= 0 || bmp->depth <= 0)
					continue;

				max_mips = rasterizer_xbox_bitmap_get_max_mipmap_count(bmp);
				if (bmp->mipmap_count < 0)
					bmp->mipmap_count = 0;
				if (bmp->mipmap_count > max_mips)
					bmp->mipmap_count = max_mips;
				while (bmp->mipmap_count > 0 &&
					   (size_t)bitmap_get_pixel_data_size(bmp) > src_len)
				{
					bmp->mipmap_count--;
				}
				req_bytes = rasterizer_xbox_bitmap_get_pixel_data_size(bmp);
				if (req_bytes <= 0)
					continue;
				alloc_bytes = ((size_t)req_bytes > src_len) ? (size_t)req_bytes : src_len;
				pix_copy = forge_phys_alloc(alloc_bytes);
				tex = (D3DBaseTexture *)calloc(1, sizeof(D3DBaseTexture));
				if (!pix_copy || !tex)
				{
					free(tex);
					continue;
				}
				memcpy(pix_copy, src_pixels, src_len);
				bmp->base_address = pix_copy;
				bmp->pixels_offset = 0;
				bmp->pixels_size = (long)alloc_bytes;
				bmp->tag_index = pt->runtime_handle;
				bmp->cache_block_index = NONE;

				halo_custom_edition_texels_channels(bmp->base_address, chan_order);
				rasterizer_xbox_bitmap_rebuild_hardware_format(bmp);

				tex->Data = 0;
				tex->Lock = 0;
				tex->Common = D3DCOMMON_TYPE_TEXTURE | 1;
				if (TEST_FLAG(bmp->flags, 4 /* _bitmap_linear_bit */))
				{
					tex->Format =
						(bitmap_format_to_d3d_linear_format(bmp->format, bmp->flags) << D3DFORMAT_FORMAT_SHIFT) |
						(1 << D3DFORMAT_MIPMAP_SHIFT) |
						(2 << D3DFORMAT_DIMENSION_SHIFT) |
						D3DFORMAT_BORDERSOURCE_COLOR |
						D3DFORMAT_DMACHANNEL_A;
					tex->Size =
						(((bitmap_mipmap_get_row_pitch(bmp, 0) + D3DTEXTURE_PITCH_ALIGNMENT - 1) / D3DTEXTURE_PITCH_ALIGNMENT - 1)
							<< D3DSIZE_PITCH_SHIFT) |
						((bmp->height - 1) << D3DSIZE_HEIGHT_SHIFT) |
						(bmp->width - 1);
				}
				else
				{
					tex->Format =
						(floor_log2(bmp->depth) << D3DFORMAT_PSIZE_SHIFT) |
						(floor_log2(bmp->height) << D3DFORMAT_VSIZE_SHIFT) |
						(floor_log2(bmp->width) << D3DFORMAT_USIZE_SHIFT) |
						(bitmap_format_to_d3d_format(bmp->format, bmp->flags) << D3DFORMAT_FORMAT_SHIFT) |
						((bmp->type == 1 /* _bitmap_type_3d */ ? 3 : 2) << D3DFORMAT_DIMENSION_SHIFT) |
						((rasterizer_xbox_bitmap_get_max_mipmap_count(bmp) + 1) << D3DFORMAT_MIPMAP_SHIFT) |
						(bmp->type == 2 /* _bitmap_type_cube_map */ ? D3DFORMAT_CUBEMAP : 0) |
						D3DFORMAT_BORDERSOURCE_COLOR |
						D3DFORMAT_DMACHANNEL_A;
					tex->Size = 0;
				}
				IDirect3DBaseTexture8_Register(tex, bmp->base_address);
				bmp->hardware_format = tex;
			}

			if (tag_assets)
			{
				for (ai = 0; ai < tag_asset_count; ai++)
					free(tag_assets[ai]);
				free(tag_assets);
			}
			free(tag_asset_lens);
		}
		else if (pt->fourcc == 'snd!' && pt->raw_len >= 0xA4u)
		{
			struct sound_definition *sdef = (struct sound_definition *)pt->tag_buf;
			uint8_t **tag_assets = NULL;
			size_t *tag_asset_lens = NULL;
			int tag_asset_count = 0;
			size_t assets_pos = 0, assets_end = 0;
			size_t patches_pos = 0, patches_end = 0;
			long channels = (sdef->encoding == 1) ? 2 : 1;
			long rate = (sdef->sample_rate == 1) ? 44100 : 22050;
			long encoded_rate = (channels == 1) ? 22050 : rate;
			int pr_i, pm_i, ai, flat_perm_idx = 0, valid_perms = 0;

			/* Decode per-tag embeddedRawDataAssets[] inside pt->obj_start .. pt->obj_end */
			if (ocetags_json_find_key_range(json, pt->obj_start, pt->obj_end, "embeddedRawDataAssets", &assets_pos) &&
				ocetags_json_skip_value(json, assets_pos, pt->obj_end, &assets_end))
			{
				size_t p = assets_pos + 1;
				tag_assets = (uint8_t **)calloc(128, sizeof(uint8_t *));
				tag_asset_lens = (size_t *)calloc(128, sizeof(size_t));
				while (tag_assets && tag_asset_lens && p < assets_end && tag_asset_count < 128)
				{
					size_t a_start, a_end;
					while (p < assets_end && json[p] != '{' && json[p] != ']')
						p++;
					if (p >= assets_end || json[p] != '{')
						break;
					a_start = p;
					if (!ocetags_json_skip_value(json, a_start, assets_end, &a_end))
						break;
					tag_assets[tag_asset_count] = ocetags_json_decode_base64_key(
						json, a_start, a_end, "data", &tag_asset_lens[tag_asset_count]);
					tag_asset_count++;
					p = a_end;
				}
			}

			sdef->promotion_sound.index = NONE;
			sdef->promotion_count = 0;
			if (channels == 1 && sdef->sample_rate == 1)
				sdef->sample_rate = 0; /* Xbox plays mono sounds at 22 kHz */
			if (sdef->minimum_distance <= 0.0f) sdef->minimum_distance = 1.0f;
			if (sdef->maximum_distance <= 0.0f) sdef->maximum_distance = 30.0f;
			if (sdef->random_pitch_bounds.lower <= 0.0f) sdef->random_pitch_bounds.lower = 1.0f;
			if (sdef->random_pitch_bounds.upper <= 0.0f) sdef->random_pitch_bounds.upper = 1.0f;
			if (sdef->zero_skip_fraction_modifier == 0.0f && sdef->one_skip_fraction_modifier == 0.0f)
			{
				sdef->zero_skip_fraction_modifier = 1.0f;
				sdef->one_skip_fraction_modifier = 1.0f;
			}
			if (sdef->zero_gain_modifier == 0.0f && sdef->one_gain_modifier == 0.0f)
			{
				sdef->zero_gain_modifier = 1.0f;
				sdef->one_gain_modifier = 1.0f;
			}
			if (sdef->zero_pitch_modifier == 0.0f && sdef->one_pitch_modifier == 0.0f)
			{
				sdef->zero_pitch_modifier = 1.0f;
				sdef->one_pitch_modifier = 1.0f;
			}

			/* Match embeddedRawDataPatches to permutations */
			if (tag_asset_count > 0 &&
				ocetags_json_find_key_range(json, pt->obj_start, pt->obj_end, "embeddedRawDataPatches", &patches_pos) &&
				ocetags_json_skip_value(json, patches_pos, pt->obj_end, &patches_end))
			{
				size_t p = patches_pos + 1;
				while (p < patches_end)
				{
					size_t p_start, p_end;
					uint32_t a_idx = 0xFFFFFFFFu, patch_off = 0xFFFFFFFFu;
					while (p < patches_end && json[p] != '{' && json[p] != ']')
						p++;
					if (p >= patches_end || json[p] != '{')
						break;
					p_start = p;
					if (!ocetags_json_skip_value(json, p_start, patches_end, &p_end))
						break;
					if (ocetags_json_get_u32(json, p_start, p_end, "assetIndex", &a_idx) &&
						(ocetags_json_get_u32(json, p_start, p_end, "patchOffset", &patch_off) ||
						 ocetags_json_get_u32(json, p_start, p_end, "offsetFieldOffset", &patch_off)) &&
						a_idx < (uint32_t)tag_asset_count &&
						tag_assets[a_idx] && tag_asset_lens[a_idx] > 0)
					{
						for (pr_i = 0; pr_i < sdef->pitch_ranges.count && sdef->pitch_ranges.address; pr_i++)
						{
							struct sound_pitch_range *pr = (struct sound_pitch_range *)((uint8_t *)sdef->pitch_ranges.address + (size_t)pr_i * sizeof(struct sound_pitch_range));
							for (pm_i = 0; pm_i < pr->permutations.count && pr->permutations.address; pm_i++)
							{
								struct sound_permutation *perm = (struct sound_permutation *)((uint8_t *)pr->permutations.address + (size_t)pm_i * sizeof(struct sound_permutation));
								uint32_t perm_off = (uint32_t)((uint8_t *)perm - pt->tag_buf);
								if (patch_off >= perm_off + 0x40u && patch_off < perm_off + 0x54u)
								{
									perm->samples.address = tag_assets[a_idx];
									perm->samples.size = (long)tag_asset_lens[a_idx];
								}
							}
						}
					}
					p = p_end;
				}
			}

			for (pr_i = 0; pr_i < sdef->pitch_ranges.count && sdef->pitch_ranges.address; pr_i++)
			{
				struct sound_pitch_range *pr = (struct sound_pitch_range *)((uint8_t *)sdef->pitch_ranges.address + (size_t)pr_i * sizeof(struct sound_pitch_range));
				if (pr->natural_pitch <= 0.0f) pr->natural_pitch = 1.0f;
				pr->playback_rate = 1.0f / pr->natural_pitch;
				pr->played_permutation_mask = 0xFFFFFFFFu;
				pr->previous_permutation_index = NONE;
				pr->forced_permutation_index = NONE;
				if (pr->actual_permutation_count <= 0 || pr->actual_permutation_count > pr->permutations.count)
					pr->actual_permutation_count = (short)pr->permutations.count;

				for (pm_i = 0; pm_i < pr->permutations.count && pr->permutations.address; pm_i++, flat_perm_idx++)
				{
					struct sound_permutation *perm = (struct sound_permutation *)((uint8_t *)pr->permutations.address + (size_t)pm_i * sizeof(struct sound_permutation));
					uint8_t const *src_snd = (uint8_t const *)perm->samples.address;
					size_t src_snd_len = (perm->samples.size > 0) ? (size_t)perm->samples.size : 0u;
					unsigned long encoded_bytes = 0;
					void *encoded_buf = NULL;

					if ((!src_snd || src_snd_len == 0) && flat_perm_idx < tag_asset_count &&
						tag_assets[flat_perm_idx] && tag_asset_lens[flat_perm_idx] > 0)
					{
						src_snd = tag_assets[flat_perm_idx];
						src_snd_len = tag_asset_lens[flat_perm_idx];
					}
					perm->samples.address = NULL;
					perm->samples.size = 0;
					perm->samples.file_offset = 0;
					perm->unknown0 = NONE;
					perm->unknown1 = 0;
					perm->unknown2 = (unsigned long)pt->runtime_handle;
					perm->unknown3 = (unsigned long)pt->runtime_handle;
					if (perm->gain <= 0.0f)
						perm->gain = 1.0f;

					if (src_snd && src_snd_len > 0)
					{
						if (perm->compression == 1 /* Xbox ADPCM */ && rate == encoded_rate)
						{
							encoded_buf = malloc(src_snd_len);
							if (encoded_buf)
							{
								memcpy(encoded_buf, src_snd, src_snd_len);
								encoded_bytes = (unsigned long)src_snd_len;
							}
						}
						else
						{
							encoded_buf = custom_edition_sounds_encode(
								src_snd,
								(long)src_snd_len,
								perm->compression,
								FALSE /* .ocetags raw data is little-endian */,
								channels,
								rate,
								encoded_rate,
								&encoded_bytes);
						}
						if (encoded_buf && encoded_bytes > 0 && encoded_bytes <= MAXIMUM_SOUND_DATA_SIZE)
						{
							perm->samples.address = encoded_buf;
							perm->samples.size = (long)encoded_bytes;
							perm->compression = 1; /* SOUND_COMPRESSION_XBOX_ADPCM */
							perm->sample_buffer_size = 0;
							forge_register_dyn_sound_perm(perm);
							valid_perms++;
						}
						else
						{
							free(encoded_buf);
						}
					}
				}
			}
			sdef->compression = 1;
			if (valid_perms == 0)
			{
				sdef->pitch_ranges.count = 0;
				sdef->pitch_ranges.address = NULL;
			}

			if (tag_assets)
			{
				for (ai = 0; ai < tag_asset_count; ai++)
					free(tag_assets[ai]);
				free(tag_assets);
			}
			free(tag_asset_lens);
		}
	}

	/* 6. Perform OpenCE runtime conversions on models ('mod2'->'mode'), shaders, animations, weapons, HUD, collision, and vehicles */
	for (i = 0; i < pkg_tag_count; i++)
	{
		struct ocetags_runtime_pkg_tag *pt = &pkg_tags[i];
		if (!pt->tag_buf || pt->is_reused_existing || pt->runtime_handle == NONE)
			continue;

		/* Shader runtime type and scex->schi conversion */
		if (ocetags_is_shader_fourcc(pt->fourcc) && pt->raw_len >= 0x28u)
		{
			int16_t stype = 4;
			if (pt->fourcc == 'senv') stype = 3;
			else if (pt->fourcc == 'soso') stype = 4;
			else if (pt->fourcc == 'sotr') stype = 5;
			else if (pt->fourcc == 'schi' || pt->fourcc == 'scex') stype = 6;
			else if (pt->fourcc == 'swat') stype = 7;
			else if (pt->fourcc == 'sgla') stype = 8;
			else if (pt->fourcc == 'smet') stype = 9;
			else if (pt->fourcc == 'spla') stype = 10;
			*(int16_t *)(pt->tag_buf + 0x24u) = stype;

			if (pt->fourcc == 'scex' && pt->raw_len >= 0x78u)
			{
				if (*(int32_t const *)(pt->tag_buf + 0x54u) == 0 && *(int32_t const *)(pt->tag_buf + 0x60u) > 0)
				{
					memcpy(pt->tag_buf + 0x54u, pt->tag_buf + 0x60u, 12);
					*(uint32_t *)(pt->tag_buf + 0x60u) = *(uint32_t const *)(pt->tag_buf + 0x6Cu);
					memset(pt->tag_buf + 0x64u, 0, 8);
				}
			}
		}
		else if (pt->fourcc == 'mod2' && pt->raw_len >= 0xE8u)
		{
			uint8_t *mbuf = pt->tag_buf;
			uint32_t mflags = *(uint32_t const *)(mbuf + 0x00u);
			int has_local_nodes = ((mflags >> 1) & 1u) != 0;
			int32_t node_cnt = *(int32_t const *)(mbuf + 0xB8u);
			uint8_t *node_base = *(uint8_t **)(mbuf + 0xBCu);
			int32_t geom_cnt = *(int32_t const *)(mbuf + 0xD0u);
			uint8_t *geom_base = *(uint8_t **)(mbuf + 0xD4u);
			int32_t shdr_cnt = *(int32_t const *)(mbuf + 0xDCu);
			uint8_t *shdr_base = *(uint8_t **)(mbuf + 0xE0u);
			int n, s_idx;

			if (!(*(float const *)(mbuf + 0x30u) > 0.0001f && *(float const *)(mbuf + 0x30u) < 10000.0f))
				*(float *)(mbuf + 0x30u) = 1.0f;
			if (!(*(float const *)(mbuf + 0x34u) > 0.0001f && *(float const *)(mbuf + 0x34u) < 10000.0f))
				*(float *)(mbuf + 0x34u) = 1.0f;

			for (s_idx = 0; s_idx < shdr_cnt && shdr_base; s_idx++)
			{
				int16_t *perm_idx_ptr = (int16_t *)(shdr_base + (size_t)s_idx * 32u + 16u);
				if (*perm_idx_ptr < 0)
					*perm_idx_ptr = 0;
			}

			/* Ensure model nodes have valid quaternions and runtime_default_inverse_matrix */
			if (node_cnt > 0 && node_base)
			{
				struct real_matrix4x3 default_node_mats[64];
				int need_inv_calc = 0;

				for (n = 0; n < node_cnt; n++)
				{
					uint8_t *mn = node_base + (size_t)n * 156u;
					float qi = *(float const *)(mn + 0x34u);
					float qj = *(float const *)(mn + 0x38u);
					float qk = *(float const *)(mn + 0x3Cu);
					float qw = *(float const *)(mn + 0x40u);
					float mscale = *(float const *)(mn + 0x68u);
					if (qi == 0.0f && qj == 0.0f && qk == 0.0f && qw == 0.0f)
						*(float *)(mn + 0x40u) = 1.0f;
					if (!(mscale > 0.0001f && mscale < 1000.0f))
						need_inv_calc = 1;
				}
				if (need_inv_calc)
				{
					memset(default_node_mats, 0, sizeof(default_node_mats));
					model_get_node_matrices((struct model const *)mbuf, default_node_mats, NULL, NULL, NULL);
					for (n = 0; n < node_cnt && n < 64; n++)
					{
						uint8_t *mn = node_base + (size_t)n * 156u;
						float mscale = *(float const *)(mn + 0x68u);
						if (!(mscale > 0.0001f && mscale < 1000.0f))
						{
							if (default_node_mats[n].scale > 0.0001f)
							{
								matrix4x3_inverse(
									&default_node_mats[n],
									(struct real_matrix4x3 *)(mn + 0x68u));
							}
							else
							{
								memset(mn + 0x68u, 0, 52);
								*(float *)(mn + 0x68u) = 1.0f;
								*(float *)(mn + 0x6Cu) = 1.0f;
								*(float *)(mn + 0x7Cu) = 1.0f;
								*(float *)(mn + 0x8Cu) = 1.0f;
							}
						}
					}
				}
				for (n = 0; n < 5; n++)
				{
					int16_t *cutoff = (int16_t *)(mbuf + 0x1Cu + (size_t)n * 2u);
					if (*cutoff <= 0 || *cutoff > node_cnt)
						*cutoff = (int16_t)(node_cnt - 1);
				}
			}

			/* Convert 132-byte custom_edition_model_part elements in-place to 104-byte model_geometry_part
			 * and build hardware vertex/triangle buffers from gbxmodelGeometryParts[] */
			{
				size_t gparts_arr = 0, gparts_end = 0;
				int has_gparts = (ocetags_json_find_key_range(json, pt->obj_start, pt->obj_end, "gbxmodelGeometryParts", &gparts_arr) &&
								  ocetags_json_skip_value(json, gparts_arr, pt->obj_end, &gparts_end));
				int g_idx;

				for (g_idx = 0; g_idx < geom_cnt && geom_base; g_idx++)
				{
					uint8_t *geom = geom_base + (size_t)g_idx * 48u;
					int32_t part_cnt = *(int32_t const *)(geom + 36u);
					uint8_t *parts_base = *(uint8_t **)(geom + 40u);
					int p_idx;

					if (!parts_base || part_cnt <= 0)
						continue;

					for (p_idx = 0; p_idx < part_cnt; p_idx++)
					{
						uint8_t ce_part[132];
						uint8_t xbox_part[104];
						uint8_t *verts_raw = NULL;
						size_t verts_bytes = 0;
						uint8_t *tris_raw = NULL;
						size_t tris_bytes = 0;

						memcpy(ce_part, parts_base + (size_t)p_idx * 132u, 132u);
						memset(xbox_part, 0, sizeof(xbox_part));
						memcpy(xbox_part, ce_part, 32u);
						*(uint32_t *)(xbox_part + 0u) &= ~0x02u; /* clear _model_geometry_part_local_nodes_bit */
						xbox_part[6] = 0xFFu; /* previous_part_index = -1 */
						xbox_part[7] = 0xFFu; /* next_part_index = -1 */
						if (has_local_nodes)
						{
							int16_t lnc = (int16_t)ce_part[107u];
							uint8_t const *ln = ce_part + 108u;
							int16_t c0 = *(int16_t const *)(xbox_part + 8u);
							int16_t c1 = *(int16_t const *)(xbox_part + 10u);
							if (lnc > 0 && lnc <= 22)
							{
								if (c0 >= 0 && c0 < lnc) *(int16_t *)(xbox_part + 8u) = (int16_t)ln[c0];
								if (c1 >= 0 && c1 < lnc) *(int16_t *)(xbox_part + 10u) = (int16_t)ln[c1];
							}
						}
						((struct vertex_buffer *)(xbox_part + 84u))->type = _rasterizer_vertex_type_model_compressed;

						/* Find matching entry in gbxmodelGeometryParts[] */
						if (has_gparts)
						{
							size_t gp = gparts_arr + 1;
							while (gp < gparts_end)
							{
								size_t gp_start, gp_end;
								uint32_t j_geom = 0xFFFFFFFFu, j_part = 0xFFFFFFFFu;
								while (gp < gparts_end && json[gp] != '{' && json[gp] != ']')
									gp++;
								if (gp >= gparts_end || json[gp] != '{')
									break;
								gp_start = gp;
								if (!ocetags_json_skip_value(json, gp_start, gparts_end, &gp_end))
									break;
								if (ocetags_json_get_u32(json, gp_start, gp_end, "geometryIndex", &j_geom) &&
									ocetags_json_get_u32(json, gp_start, gp_end, "partIndex", &j_part) &&
									j_geom == (uint32_t)g_idx && j_part == (uint32_t)p_idx)
								{
									verts_raw = ocetags_json_decode_base64_key(json, gp_start, gp_end, "vertexData", &verts_bytes);
									if (!verts_raw)
										verts_raw = ocetags_json_decode_base64_key(json, gp_start, gp_end, "vertices", &verts_bytes);
									tris_raw = ocetags_json_decode_base64_key(json, gp_start, gp_end, "triangleData", &tris_bytes);
									if (!tris_raw)
										tris_raw = ocetags_json_decode_base64_key(json, gp_start, gp_end, "triangles", &tris_bytes);
									break;
								}
								gp = gp_end;
							}
						}

						if (verts_raw && tris_raw && verts_bytes >= 3u * 68u && tris_bytes >= 6u)
						{
							int32_t vtx_count = (int32_t)(verts_bytes / 68u);
							int32_t idx_count = (int32_t)(tris_bytes / 2u);
							uint16_t const *raw_ibuf = (uint16_t const *)tris_raw;
							int16_t local_node_count = (int16_t)ce_part[107u];
							uint8_t const *local_nodes = ce_part + 108u;
							int v, c;

							while (idx_count > 3 && raw_ibuf[idx_count - 1] >= (uint16_t)vtx_count)
								idx_count--;

							for (v = 0; v < vtx_count; v++)
							{
								uint8_t *vp = verts_raw + (size_t)v * 68u;
								int16_t n0 = *(int16_t const *)(vp + 56u);
								int16_t n1 = *(int16_t const *)(vp + 58u);
								float w0 = *(float const *)(vp + 60u);
								float w1 = *(float const *)(vp + 64u);

								for (c = 0; c < 9; c++)
								{
									float val = *(float const *)(vp + 12u + (size_t)c * 4u);
									if (!(val >= -1.0f && val <= 1.0f))
										*(float *)(vp + 12u + (size_t)c * 4u) = (c % 3 == 0) ? 1.0f : 0.0f;
								}

								if (has_local_nodes && local_node_count > 0 && local_node_count <= 22)
								{
									n0 = (n0 >= 0 && n0 < local_node_count) ? (int16_t)local_nodes[n0] : 0;
									n1 = (n1 >= 0 && n1 < local_node_count) ? (int16_t)local_nodes[n1] : 0;
								}
								if (node_cnt > 0)
								{
									if (n0 < 0 || n0 >= node_cnt) n0 = 0;
									if (n1 < 0 || n1 >= node_cnt) n1 = 0;
								}
								else
								{
									n0 = 0;
									n1 = 0;
								}
								if (!(w0 >= 0.0f && w0 <= 1.0f) || !(w1 >= 0.0f && w1 <= 1.0f))
								{
									w0 = 1.0f;
									w1 = 0.0f;
								}
								*(int16_t *)(vp + 56u) = n0;
								*(int16_t *)(vp + 58u) = n1;
								*(float *)(vp + 60u) = w0;
								*(float *)(vp + 64u) = w1;
							}

							if (vtx_count >= 3 && idx_count >= 3)
							{
								uint32_t uncomp_sz = (uint32_t)vtx_count * 68u;
								uint32_t comp_sz = (uint32_t)vtx_count * 32u;
								uint32_t idx_sz = (uint32_t)idx_count * 2u;
								void *hw_verts = forge_phys_alloc(comp_sz);
								void *hw_tris = forge_phys_alloc(idx_sz);
								if (hw_verts && hw_tris)
								{
									uint16_t *tris_u16 = (uint16_t *)hw_tris;
									int32_t k;
									rasterizer_geometry_compress_vertices(
										_rasterizer_vertex_type_model_uncompressed,
										vtx_count,
										hw_verts,
										(long)comp_sz,
										verts_raw,
										(long)uncomp_sz);
									memcpy(hw_tris, tris_raw, idx_sz);
									for (k = 0; k < idx_count; k++)
									{
										if (tris_u16[k] >= (uint16_t)vtx_count)
											tris_u16[k] = (k > 0 && tris_u16[k - 1] < (uint16_t)vtx_count) ? tris_u16[k - 1] : 0u;
									}
									rasterizer_vertex_buffer_new(
										(struct vertex_buffer *)(xbox_part + 84u),
										_rasterizer_vertex_type_model_compressed,
										vtx_count,
										hw_verts,
										(long)comp_sz);
									rasterizer_triangle_buffer_new(
										(struct triangle_buffer *)(xbox_part + 68u),
										_triangle_buffer_type_precompiled_strip,
										idx_count - 2,
										hw_tris);
								}
							}
						}

						free(verts_raw);
						free(tris_raw);
						memcpy(parts_base + (size_t)p_idx * 104u, xbox_part, 104u);
					}
				}
			}

			/* Clear _gbxmodel_parts_have_local_nodes_bit (bit 1) now that local nodes are resolved */
			*(uint32_t *)(mbuf + 0x00u) = mflags & ~(1u << 1);
			models_fix_transparent_part_links();
		}
		else if (pt->fourcc == 'antr' && pt->raw_len >= 0x80u)
		{
			/* Sanitize animation_graph object_overlays (+0x00, stride 20) and clamp animations node_count */
			int32_t ov_cnt = *(int32_t const *)(pt->tag_buf + 0x00u);
			uint8_t *ov_base = *(uint8_t **)(pt->tag_buf + 0x04u);
			int32_t anim_cnt = *(int32_t const *)(pt->tag_buf + 0x74u);
			uint8_t *anim_base = *(uint8_t **)(pt->tag_buf + 0x78u);
			int32_t node_cnt = *(int32_t const *)(pt->tag_buf + 0x5Cu);
			int a;

			if (ov_cnt > 0 && ov_base)
			{
				for (a = 0; a < ov_cnt; a++)
				{
					uint8_t *ov = ov_base + (size_t)a * 20u;
					int16_t fn_ctrl = *(int16_t const *)(ov + 0x04u);
					int16_t fn_idx = *(int16_t const *)(ov + 0x06u);
					if (fn_ctrl < 0 || fn_ctrl > 3 || fn_idx < 0 || fn_idx >= 4)
					{
						*(int16_t *)(ov + 0x02u) = -1;
						*(int16_t *)(ov + 0x04u) = 0;
						*(int16_t *)(ov + 0x06u) = 0;
					}
				}
			}
			if (anim_cnt > 0 && anim_base)
			{
				for (a = 0; a < anim_cnt; a++)
				{
					uint8_t *an = anim_base + (size_t)a * 180u;
					int16_t nc = *(int16_t const *)(an + 0x3Au);
					if (nc < 0 || nc > 64 || (node_cnt > 0 && nc > node_cnt))
						*(int16_t *)(an + 0x3Au) = (int16_t)((node_cnt > 0 && node_cnt <= 64) ? node_cnt : 1);
				}
			}
		}
		else if (pt->fourcc == 'weap' && pt->raw_len >= 0x508u)
		{
			/* Convert Custom Edition weapon exported function modes at +0x330 (17->15, 18->16) */
			int f;
			for (f = 0; f < 4; f++)
			{
				int16_t *fn_mode = (int16_t *)(pt->tag_buf + 0x330u + (size_t)f * 2u);
				if (*fn_mode == 17) *fn_mode = 15;
				else if (*fn_mode == 18) *fn_mode = 16;
			}
		}
		else if (pt->fourcc == 'coll' && pt->raw_len >= 0x298u)
		{
			/* Sanitize collision pathfinding box and Mantle 0x0BDEA5CE vertex offsets */
			int pf_i, n;
			int32_t reg_cnt = *(int32_t const *)(pt->tag_buf + 0x240u);
			int32_t cn_cnt = *(int32_t const *)(pt->tag_buf + 0x28Cu);
			uint8_t *cn_base = *(uint8_t **)(pt->tag_buf + 0x290u);

			for (pf_i = 0; pf_i < 6; pf_i++)
			{
				float pfv = *(float const *)(pt->tag_buf + 0x268u + (size_t)pf_i * 4u);
				if (!(pfv >= -500.0f && pfv <= 500.0f))
					*(float *)(pt->tag_buf + 0x268u + (size_t)pf_i * 4u) = (pf_i & 1) ? 2.5f : -2.5f;
			}
			if (cn_cnt > 0 && cn_base)
			{
				for (n = 0; n < cn_cnt; n++)
				{
					uint8_t *cn = cn_base + (size_t)n * 64u;
					int16_t r_idx = *(int16_t const *)(cn + 0x20u);
					int32_t bsp_cnt = *(int32_t const *)(cn + 0x34u);
					uint8_t *bsp_base = *(uint8_t **)(cn + 0x38u);
					int b;

					if (r_idx != -1 && (r_idx < 0 || r_idx >= reg_cnt))
						*(int16_t *)(cn + 0x20u) = (reg_cnt > 0) ? 0 : -1;
					for (b = 0; b < bsp_cnt && bsp_base; b++)
					{
						uint8_t *bsp = bsp_base + (size_t)b * 96u;
						int32_t v_cnt = *(int32_t const *)(bsp + 7u * 12u + 0u);
						uint8_t *v_base = *(uint8_t **)(bsp + 7u * 12u + 4u);
						int v_i, c_i;
						for (v_i = 0; v_i < v_cnt && v_base; v_i++)
						{
							uint8_t *cv = v_base + (size_t)v_i * 16u;
							for (c_i = 0; c_i < 3; c_i++)
							{
								float val = *(float const *)(cv + (size_t)c_i * 4u);
								if (!(val >= -500.0f && val <= 500.0f))
								{
									uint32_t raw_bits = *(uint32_t const *)(cv + (size_t)c_i * 4u);
									uint32_t fixed_bits = raw_bits - 0x0BDEA5CEu;
									float fixed_val = 0.0f;
									if (fixed_bits >= 0x40440000u && fixed_bits <= 0x41B40000u)
										memcpy(&fixed_val, &fixed_bits, 4);
									*(float *)(cv + (size_t)c_i * 4u) = fixed_val;
								}
							}
						}
					}
				}
			}
		}
		else if (pt->fourcc == 'vehi' && pt->raw_len >= 0x3F0u)
		{
			patch_vehicle_tag_for_player_driving(pt->runtime_handle, pt->path);
		}
	}

	root_handle = pkg_tags[0].runtime_handle;
	if (root_handle != NONE && s_dyn_tags.count < FORGE_MAX_DYNAMIC_TAGS)
	{
		struct forge_dynamic_tag_entry *entry = &s_dyn_tags.entries[s_dyn_tags.count++];
		memset(entry, 0, sizeof(*entry));
		entry->group_tag = req_group;
		snprintf(entry->tag_path, sizeof(entry->tag_path), "%s", req_path);
		entry->definition_index = root_handle;
		entry->part_count = 0; /* 0 = let Halo's native render_model render the exact gbxmodel with all nodes/regions/shaders! */
		entry->bounding_radius = (pkg_tags[0].raw_len >= 0x08u) ? *(float const *)(pkg_tags[0].tag_buf + 0x04u) : 1.5f;
		if (!(entry->bounding_radius > 0.1f && entry->bounding_radius < 100.0f))
			entry->bounding_radius = 1.5f;
	}

	if (out_actual_group && root_handle != NONE)
		*out_actual_group = pkg_tags[0].actual_group;

	if (raw_assets)
	{
		for (i = 0; i < raw_asset_count; i++)
			free(raw_assets[i]);
		free(raw_assets);
	}
	free(raw_asset_lens);
	free(pkg_tags);
	return root_handle;
}

static long load_native_ocetags_package_runtime(
	uint32_t req_group,
	char const *req_path,
	uint32_t *out_actual_group)
{
#ifdef _WIN32
	static char const *const s_ocetags_dirs[] = {
		"OpenCE Forge/custom_tags",
		"custom_tags",
		"OpenCE Forge/ocetags",
		"OpenCE Forge/cached_tags",
		"OpenCE Forge",
		"ocetags",
		"custom_maps",
		"maps",
		"mods"
	};
	int d;

	if (!req_path || !req_path[0])
		return NONE;

	for (d = 0; d < (int)(sizeof(s_ocetags_dirs) / sizeof(s_ocetags_dirs[0])); d++)
	{
		char pattern[512];
		WIN32_FIND_DATAA fd;
		HANDLE h;
		snprintf(pattern, sizeof(pattern), "%s/*.ocetags", s_ocetags_dirs[d]);
		h = FindFirstFileA(pattern, &fd);
		if (h != INVALID_HANDLE_VALUE)
		{
			do
			{
				if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
				{
					char full_path[512];
					FILE *fp;
					snprintf(full_path, sizeof(full_path), "%s/%s", s_ocetags_dirs[d], fd.cFileName);
					fp = fopen(full_path, "rb");
					if (fp)
					{
						long fsz;
						fseek(fp, 0, SEEK_END);
						fsz = ftell(fp);
						fseek(fp, 0, SEEK_SET);
						if (fsz > 32 && fsz <= 128 * 1024 * 1024)
						{
							/* Read a 2048-byte prefix first to quickly check if tags[0].path matches req_path */
							char hdr_peek[2049];
							size_t peek_n = fread(hdr_peek, 1, 2048, fp);
							hdr_peek[peek_n] = 0;
							if (strstr(hdr_peek, "\"formatVersion\"") != NULL)
							{
								char *json_buf;
								fseek(fp, 0, SEEK_SET);
								json_buf = (char *)malloc((size_t)fsz + 1u);
								if (json_buf)
								{
									if (fread(json_buf, 1, (size_t)fsz, fp) == (size_t)fsz)
									{
										long hdl;
										json_buf[fsz] = 0;
										hdl = load_native_ocetags_package_from_json(
											json_buf, (size_t)fsz, req_group, req_path, out_actual_group);
										if (hdl != NONE)
										{
											free(json_buf);
											fclose(fp);
											CloseHandle(h);
											return hdl;
										}
									}
									free(json_buf);
								}
							}
						}
						fclose(fp);
					}
				}
			} while (FindNextFileA(h, &fd));
			CloseHandle(h);
		}
	}
#else
	(void)req_group;
	(void)req_path;
	(void)out_actual_group;
#endif
	return NONE;
}

/* Index a standalone .ocetags package file (JSON formatVersion 5 or OCETAG1\0 header)
 * in custom_tags/ into custom_catalog.txt without generating any .ftag/.fcol/.fbtm files */
static int forge_rip_ocetags_file_to_cached_tags(
	char const *ocetags_path,
	int force_overwrite,
	FILE *manifest_fp,
	FILE *credits_fp)
{
	FILE *fp;
	uint8_t *file_buf = NULL;
	size_t file_size = 0;
	uint64_t manifest_off = 0, manifest_sz = 0, payload_off = 0, payload_sz = 0;
	char const *json;
	size_t json_len;
	char root_group_str[32];
	char root_tag_path[256];
	char custom_item_name[64];
	uint32_t root_group = 0;
	char gstr[5];
	int cat_id;

	if (!ocetags_path || !ocetags_path[0])
		return 0;
	fp = fopen(ocetags_path, "rb");
	if (!fp)
		return 0;
	fseek(fp, 0, SEEK_END);
	file_size = (size_t)ftell(fp);
	fseek(fp, 0, SEEK_SET);
	if (file_size < 64u || file_size > 128u * 1024u * 1024u)
	{
		fclose(fp);
		return 0;
	}
	file_buf = (uint8_t *)malloc(file_size + 1u);
	if (!file_buf)
	{
		fclose(fp);
		return 0;
	}
	if (fread(file_buf, 1, file_size, fp) != file_size)
	{
		free(file_buf);
		fclose(fp);
		return 0;
	}
	fclose(fp);
	file_buf[file_size] = 0;

	/* Check for JSON formatVersion 5 (.ocetags starting with '{' or UTF-8 BOM + '{') */
	{
		size_t jstart = 0;
		if (file_size >= 3 && file_buf[0] == 0xEF && file_buf[1] == 0xBB && file_buf[2] == 0xBF)
			jstart = 3;
		while (jstart < file_size && (file_buf[jstart] == ' ' || file_buf[jstart] == '\t' || file_buf[jstart] == '\r' || file_buf[jstart] == '\n'))
			jstart++;
		if (jstart < file_size && file_buf[jstart] == '{')
		{
			int rc = forge_rip_ocetags_v5_json_to_cached_tags(
				ocetags_path,
				(char const *)(file_buf + jstart),
				file_size - jstart,
				force_overwrite,
				manifest_fp,
				credits_fp);
			free(file_buf);
			return rc;
		}
	}

	if (memcmp(file_buf, "OCETAG1\0", 8) != 0)
	{
		free(file_buf);
		return 0;
	}
	memcpy(&manifest_off, file_buf + 16, sizeof(uint64_t));
	memcpy(&manifest_sz, file_buf + 24, sizeof(uint64_t));
	memcpy(&payload_off, file_buf + 32, sizeof(uint64_t));
	memcpy(&payload_sz, file_buf + 40, sizeof(uint64_t));
	if (manifest_off + manifest_sz > file_size || payload_off + payload_sz > file_size)
	{
		free(file_buf);
		return 0;
	}

	json = (char const *)(file_buf + (size_t)manifest_off);
	json_len = (size_t)manifest_sz;

	root_group_str[0] = 0;
	root_tag_path[0] = 0;
	custom_item_name[0] = 0;
	{
		size_t root_pos = 0, root_end = 0;
		if (ocetags_json_find_key_range(json, 0, json_len, "root", &root_pos) &&
			root_pos < json_len && json[root_pos] == '{' &&
			ocetags_find_matching_brace(json, root_pos, json_len, '{', '}', &root_end))
		{
			ocetags_json_get_str(json, root_pos, root_end + 1, "group", root_group_str, sizeof(root_group_str));
			ocetags_json_get_str(json, root_pos, root_end + 1, "path", root_tag_path, sizeof(root_tag_path));
			ocetags_json_get_str(json, root_pos, root_end + 1, "itemName", custom_item_name, sizeof(custom_item_name));
			if (!custom_item_name[0])
				ocetags_json_get_str(json, root_pos, root_end + 1, "displayName", custom_item_name, sizeof(custom_item_name));
			if (!custom_item_name[0])
				ocetags_json_get_str(json, root_pos, root_end + 1, "name", custom_item_name, sizeof(custom_item_name));
		}
		if (!custom_item_name[0])
			ocetags_json_get_str(json, 0, json_len, "itemName", custom_item_name, sizeof(custom_item_name));
		if (!custom_item_name[0])
			ocetags_json_get_str(json, 0, json_len, "displayName", custom_item_name, sizeof(custom_item_name));
	}

	root_group = ocetags_group_str_to_fourcc(root_group_str);
	free(file_buf);

	if (!root_group || !root_tag_path[0])
		return 0;

	cat_id = is_forge_light_beacon_emitter_tag(root_group, root_tag_path) ? _forge_cat_lights : _forge_cat_scenery;
	if (root_group == 'vehi') cat_id = _forge_cat_vehicles;
	else if (root_group == 'weap') cat_id = _forge_cat_weapons;
	else if (root_group == 'eqip') cat_id = _forge_cat_equipment;
	else if (root_group == 'bipd') cat_id = _forge_cat_bipeds;
	else if (root_group != 'scen') return 0;

	gstr[0] = (char)((root_group >> 24) & 0xFF);
	gstr[1] = (char)((root_group >> 16) & 0xFF);
	gstr[2] = (char)((root_group >> 8) & 0xFF);
	gstr[3] = (char)(root_group & 0xFF);
	gstr[4] = 0;

	if (catalog_file_has_ocetags_entry(gstr, root_tag_path))
		return 0;

	if (manifest_fp)
	{
		if (custom_item_name[0])
			fprintf(manifest_fp, "%s|%d|2|%s|%s\n", gstr, cat_id, root_tag_path, custom_item_name);
		else
			fprintf(manifest_fp, "%s|%d|2|%s\n", gstr, cat_id, root_tag_path);
		fflush(manifest_fp);
	}
	if (credits_fp)
	{
		fprintf(credits_fp, "- **`%s`** (`%s`) — native `.ocetags` package `%s`\r\n",
			root_tag_path, gstr, ocetags_path);
		fflush(credits_fp);
	}
	return 1;
}

static int forge_scan_and_rip_ocetags_folders(int force_reload)
{
	int extracted = 0;
#ifdef _WIN32
	static char const *const s_ocetags_dirs[] = {
		"OpenCE Forge/custom_tags",
		"custom_tags"
	};
	char found_files[256][512];
	int found_count = 0;
	int d, f_idx;
	FILE *manifest_fp = NULL;
	FILE *credits_fp = NULL;

	CreateDirectoryA("OpenCE Forge", NULL);
	CreateDirectoryA("OpenCE Forge/custom_tags", NULL);
	CreateDirectoryA("OpenCE Forge/cached_tags", NULL);

	for (d = 0; d < (int)(sizeof(s_ocetags_dirs) / sizeof(s_ocetags_dirs[0])); d++)
	{
		char pattern[512];
		WIN32_FIND_DATAA fd;
		HANDLE h;
		snprintf(pattern, sizeof(pattern), "%s/*.ocetags", s_ocetags_dirs[d]);
		h = FindFirstFileA(pattern, &fd);
		if (h != INVALID_HANDLE_VALUE)
		{
			do
			{
				if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && found_count < 256)
				{
					snprintf(found_files[found_count++], sizeof(found_files[0]), "%s/%s", s_ocetags_dirs[d], fd.cFileName);
				}
			} while (FindNextFileA(h, &fd));
			CloseHandle(h);
		}
	}

	manifest_fp = fopen("OpenCE Forge/cached_tags/custom_catalog.txt", "ab");
	if (!manifest_fp)
		manifest_fp = fopen("OpenCE Forge/cached_tags/custom_catalog.txt", "wb");
	credits_fp = fopen("OpenCE Forge/credits.md", "ab");

	if (force_reload)
		forge_xbox_loading_screen_begin();

	for (f_idx = 0; f_idx < found_count; f_idx++)
	{
		if (force_reload)
			forge_xbox_loading_screen_update((float)(f_idx + 1) / (float)(found_count > 0 ? found_count : 1));
		extracted += forge_rip_ocetags_file_to_cached_tags(found_files[f_idx], force_reload, manifest_fp, credits_fp);
	}

	if (force_reload)
		forge_xbox_loading_screen_end();

	if (credits_fp)
		fclose(credits_fp);
	if (manifest_fp)
		fclose(manifest_fp);
#else
	(void)force_reload;
#endif
	return extracted;
}

int forge_ui_inject_custom_map(char const *donor_map_path)
{
	int injected = 0;

	if (!donor_map_path || !donor_map_path[0])
		return 0;

	/* Rehash Custom Tags from custom_tags/ folder (with Original Xbox Loading Screen progress) */
	if (strcmp(donor_map_path, "__OCETAGS_FOLDER__") == 0)
	{
		injected = forge_scan_and_rip_ocetags_folders(1);
		forge_runtime_reload_catalogs();
		if (injected > 0)
			forge_set_status("Rehashed %d new custom .ocetags package(s) from custom_tags/ into Forge Menu!", injected);
		else
			forge_set_status("Rehash complete: All .ocetags packages in custom_tags/ are active in Forge Menu!");
		return 1;
	}

	if (strlen(donor_map_path) > 8 && _stricmp(donor_map_path + strlen(donor_map_path) - 8, ".ocetags") == 0)
	{
		FILE *manifest_fp = fopen("OpenCE Forge/cached_tags/custom_catalog.txt", "ab");
		FILE *credits_fp = fopen("OpenCE Forge/credits.md", "ab");
		forge_xbox_loading_screen_begin();
		forge_xbox_loading_screen_update(0.5f);
		injected = forge_rip_ocetags_file_to_cached_tags(donor_map_path, 1, manifest_fp, credits_fp);
		forge_xbox_loading_screen_end();
		if (credits_fp)
			fclose(credits_fp);
		if (manifest_fp)
			fclose(manifest_fp);
		forge_runtime_reload_catalogs();
		if (injected > 0)
			forge_set_status("Added custom .ocetags package '%s' to Forge Menu!", donor_map_path);
		else
			forge_set_status("Package '%s' is already in Forge Menu!", donor_map_path);
		return 1;
	}

	forge_set_status("Place custom .ocetags files in 'OpenCE Forge/custom_tags/' and click SYNC CUSTOM TAGS!");
	return 0;
}

#if 0
static void get_forge_exe_dir_prefix(char *out_dir, size_t out_sz)
{
	out_dir[0] = '\0';
#ifdef _WIN32
	{
		char mod_path[512];
		DWORD len = GetModuleFileNameA(NULL, mod_path, (DWORD)sizeof(mod_path));
		if (len > 0 && len < sizeof(mod_path))
		{
			char *slash = strrchr(mod_path, '\\');
			if (!slash)
				slash = strrchr(mod_path, '/');
			if (slash)
			{
				slash[1] = '\0';
				snprintf(out_dir, out_sz, "%s", mod_path);
			}
		}
	}
#endif
}

#endif
static int is_ce_v609_map_file(char const *path)
{
	FILE *f = fopen(path, "rb");
	uint32_t hdr[2];
	if (!f)
		return 0;
	if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr))
	{
		fclose(f);
		return 0;
	}
	fclose(f);
	return (hdr[0] == 0x68656164u && hdr[1] == 609u) ? 1 : 0;
}

static int extract_embedded_forge_base_map_stem(char const *map_path, char *out_stem, size_t out_sz)
{
	FILE *f;
	uint8_t hdr[0x200];
	uint32_t state_off = 0, state_sz = 0, sig = 0;

	if (!map_path || !out_stem || out_sz == 0)
		return 0;
	out_stem[0] = 0;
	f = fopen(map_path, "rb");
	if (!f)
		return 0;
	if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr))
	{
		fclose(f);
		return 0;
	}
	memcpy(&state_off, hdr + 0x1E4u, 4);
	memcpy(&state_sz, hdr + 0x1E8u, 4);
	memcpy(&sig, hdr + 0x1FCu, 4);
	if (sig == 0x464C4731u && state_off >= 0x800u && state_sz > 0 && state_sz < 4u * 1024u * 1024u)
	{
		uint32_t read_sz = (state_sz < 1024u) ? state_sz : 1024u;
		char buf[1025];
		if (fseek(f, (long)state_off, SEEK_SET) == 0 && fread(buf, 1, read_sz, f) == read_sz)
		{
			char const *p;
			buf[read_sz] = 0;
			p = strstr(buf, "Base Map: ");
			if (p)
			{
				size_t i = 0;
				p += 10;
				while (p[i] && p[i] != '\r' && p[i] != '\n' && i + 1 < out_sz)
				{
					out_stem[i] = p[i];
					i++;
				}
				out_stem[i] = 0;
			}
		}
	}
	fclose(f);
	return out_stem[0] ? 1 : 0;
}

static int try_resolve_clean_ce_map_by_stem(char const *stem, char *out_path, size_t out_sz)
{
	char candidate[512];
	char h_xbox[128];
	FILE *f;
	if (!stem || !stem[0])
		return 0;

	snprintf(h_xbox, sizeof(h_xbox), "h:\\maps\\%s.map", stem);
	platform_translate_path(h_xbox, candidate, sizeof(candidate));
	if (is_ce_v609_map_file(candidate)) { snprintf(out_path, out_sz, "%s", candidate); return 1; }

	snprintf(candidate, sizeof(candidate), "C:/Program Files (x86)/Microsoft Games/Halo Custom Edition/maps/%s.map", stem);
	if (is_ce_v609_map_file(candidate)) { snprintf(out_path, out_sz, "%s", candidate); return 1; }

	snprintf(candidate, sizeof(candidate), "C:/Program Files/Microsoft Games/Halo Custom Edition/maps/%s.map", stem);
	if (is_ce_v609_map_file(candidate)) { snprintf(out_path, out_sz, "%s", candidate); return 1; }

	snprintf(candidate, sizeof(candidate), "custom_maps/%s.map", stem);
	if (is_ce_v609_map_file(candidate)) { snprintf(out_path, out_sz, "%s", candidate); return 1; }

	snprintf(candidate, sizeof(candidate), "maps/%s.map", stem);
	if ((f = fopen(candidate, "rb")) != NULL) { fclose(f); snprintf(out_path, out_sz, "%s", candidate); return 1; }

	return 0;
}

static void resolve_current_base_map_path(char *out_path, size_t out_sz)
{
	char const *ce_loaded = custom_edition_cache_loaded_path();
	char const *map_leaf;
	char base_stem[64];
	FILE *f;

	if (ce_loaded && ce_loaded[0])
	{
		char const *rel_ce = ce_loaded;
		if ((rel_ce[0] == 'd' || rel_ce[0] == 'D') && rel_ce[1] == ':' && (rel_ce[2] == '\\' || rel_ce[2] == '/'))
			rel_ce += 3;
		if (is_ce_v609_map_file(rel_ce))
		{
			if (extract_embedded_forge_base_map_stem(rel_ce, base_stem, sizeof(base_stem)) &&
				_stricmp(base_stem, rel_ce) != 0 &&
				try_resolve_clean_ce_map_by_stem(base_stem, out_path, out_sz))
			{
				return;
			}
			snprintf(out_path, out_sz, "%s", rel_ce);
			return;
		}
	}

	map_leaf = main_get_map_name();
	if (map_leaf && map_leaf[0])
	{
		char const *slash = strrchr(map_leaf, '\\');
		char candidate[512];
		if (!slash)
			slash = strrchr(map_leaf, '/');
		if (slash)
			map_leaf = slash + 1;

		snprintf(candidate, sizeof(candidate), "custom_maps/%s.map", map_leaf);
		if (extract_embedded_forge_base_map_stem(candidate, base_stem, sizeof(base_stem)) &&
			_stricmp(base_stem, map_leaf) != 0 &&
			try_resolve_clean_ce_map_by_stem(base_stem, out_path, out_sz))
		{
			return;
		}
		if (is_ce_v609_map_file(candidate))
		{
			snprintf(out_path, out_sz, "%s", candidate);
			return;
		}
		if (try_resolve_clean_ce_map_by_stem(map_leaf, out_path, out_sz))
			return;
	}

	if (try_resolve_clean_ce_map_by_stem("bloodgulch", out_path, out_sz))
		return;
	if ((f = fopen("maps/bloodgulch.map", "rb")) != NULL)
	{
		fclose(f);
		snprintf(out_path, out_sz, "maps/bloodgulch.map");
		return;
	}
	snprintf(out_path, out_sz, "maps/bloodgulch.map");
}

int forge_ui_save_copy_as(char const *new_map_name)
{
	char base_map_path[512];
	char clean_name[64];
	char const *donors[2];
	int was_already_saved = g_forge.has_saved_map_once;
	int ok;

	if (!new_map_name || !new_map_name[0])
	{
		char const *cur = main_get_map_name();
		char const *slash = cur ? strrchr(cur, '\\') : NULL;
		snprintf(clean_name, sizeof(clean_name), "%s_forge", slash ? (slash + 1) : "bloodgulch");
	}
	else
	{
		snprintf(clean_name, sizeof(clean_name), "%s", new_map_name);
	}

	resolve_current_base_map_path(base_map_path, sizeof(base_map_path));
	donors[0] = "OpenCE Forge/forge_maps/forgeasset.map";
	donors[1] = "OpenCE Forge/forge_maps/customforgeasset.map";

#ifdef _WIN32
	CreateDirectoryA("custom_maps", NULL);
#endif

	ok = forge_export_scenario_map(
		base_map_path,
		"maps",
		"custom_maps",
		donors,
		2,
		&g_forge.edit_state,
		clean_name,
		"custom_maps");

	if (ok)
	{
		extern void custom_edition_maps_look_again(void);
		custom_edition_maps_look_again();
		snprintf(g_forge.save_map_name, sizeof(g_forge.save_map_name), "%s", clean_name);
		g_forge.save_map_name_len = (int)strlen(g_forge.save_map_name);
		g_forge.has_saved_map_once = 1;

		if (was_already_saved)
		{
			forge_set_status("Updated saved map 'custom_maps/%s.map' (%d placed, %d default edits)!",
				clean_name,
				(int)g_forge.edit_state.placed_object_count,
				(int)g_forge.edit_state.default_edit_count);
		}
		else
		{
			forge_set_status("Initial save complete: 'custom_maps/%s.map'! Press F5 anytime to quick-update.",
				clean_name);
		}
		return 1;
	}
	forge_set_status("ERROR: Failed to export custom_maps/%s.map", clean_name);
	return 0;
}

void forge_trigger_save_map_action(void)
{
	/* If an initial save has already been done in this session (or loaded from Edit Maps), quick-update immediately! */
	if (g_forge.has_saved_map_once && g_forge.save_map_name[0])
	{
		g_forge.active_modal = _forge_modal_none;
		forge_ui_save_copy_as(g_forge.save_map_name);
		return;
	}

	/* Otherwise, open the initial save prompt so the user can name their custom map */
	if (!g_forge.save_map_name[0])
	{
		char const *cur = main_get_map_name();
		char const *slash = cur ? strrchr(cur, '\\') : NULL;
		snprintf(g_forge.save_map_name, sizeof(g_forge.save_map_name), "%s_forge", slash ? (slash + 1) : "custom");
		g_forge.save_map_name_len = (int)strlen(g_forge.save_map_name);
	}
	g_forge.active_modal = _forge_modal_save_copy_as;
}

static void parse_forge_state_text_buffer(char const *text_buf, char const *leaf)
{
	char const *cur = text_buf;
	char line[512];
	int in_forge_state = 0;

	if (!cur)
		return;

	while (*cur)
	{
		size_t len = 0;
		while (cur[len] && cur[len] != '\r' && cur[len] != '\n')
			len++;
		if (len >= sizeof(line))
			len = sizeof(line) - 1;
		memcpy(line, cur, len);
		line[len] = 0;
		cur += len;
		while (*cur == '\r' || *cur == '\n')
			cur++;

		if (strcmp(line, "[OpenCE Forge Map]") == 0)
		{
			/* This map is already a saved Forge map: enable quick-update on F5! */
			snprintf(g_forge.save_map_name, sizeof(g_forge.save_map_name), "%s", leaf);
			g_forge.save_map_name_len = (int)strlen(g_forge.save_map_name);
			g_forge.has_saved_map_once = 1;
		}
		else if (strcmp(line, "[ForgeState]") == 0)
		{
			in_forge_state = 1;
		}
		else if (in_forge_state && strncmp(line, "OBJECT|", 7) == 0)
		{
			unsigned int grp = 0;
			int src_cache = 0, baked = 0, team = 0;
			float x = 0, y = 0, z = 0, yaw = 0, pitch = 0, roll = 0;
			char tpath[256];
			tpath[0] = 0;
			if (sscanf(line + 7, "%u|%d|%d|%f|%f|%f|%f|%f|%f|%d|%255[^\r\n]",
					&grp, &src_cache, &baked, &x, &y, &z, &yaw, &pitch, &roll, &team, tpath) == 11 && tpath[0])
			{
				if (!baked)
				{
					/* Cached tag object not baked into base_map: spawn live now! */
					forge_3d_spawn_object_at((uint32_t)grp, tpath, (uint8_t)src_cache, x, y, z, yaw, pitch, roll, (int16_t)team, 0);
				}
				else
				{
					struct object_iterator iter;
					struct object_datum *obj;
					long found_oidx = NONE;
					float best_d2 = 36.0f;

					object_iterator_new(&iter, (unsigned long)-1, 0);
					while ((obj = object_iterator_next(&iter)) != NULL)
					{
						int li, already_claimed = 0;
						float dx, dy, dz, d2;
						char const *oname = NULL;
						if (obj->object.parent_object_index != NONE)
							continue;
						if (obj->object.type == _object_type_biped &&
							((struct unit_datum *)obj)->unit.player_index != NONE)
							continue;
						for (li = 0; li < g_forge.live_instance_count; li++)
						{
							if (g_forge.live_instances[li].active &&
								g_forge.live_instances[li].object_index == iter.index)
							{
								already_claimed = 1;
								break;
							}
						}
						if (already_claimed)
							continue;
						if (obj->definition_index != NONE)
							oname = tag_get_name(obj->definition_index);
						if (oname && oname[0])
						{
							size_t tlen = strlen(tpath);
							if (_stricmp(oname, tpath) != 0 &&
								_strnicmp(oname, tpath, tlen) != 0 &&
								!(grp == 'vehi' && _strnicmp(oname, "forge\\vehicles\\placed_unit_", 27) == 0))
								continue;
						}
						dx = obj->object.position.x - x;
						dy = obj->object.position.y - y;
						dz = obj->object.position.z - z;
						/* Physics objects (vehicles/weapons) spawn with +bounding_sphere_radius*0.5f on Z */
						if (dx * dx + dy * dy > 9.0f || (float)fabs((double)dz) > 6.0f)
							continue;
						d2 = dx * dx + dy * dy + dz * dz * 0.25f;
						if (d2 < best_d2)
						{
							best_d2 = d2;
							found_oidx = iter.index;
						}
					}

					if (found_oidx == NONE)
					{
						/* Not yet spawned by scenario: spawn live and record in placed_objects */
						forge_3d_spawn_object_at((uint32_t)grp, tpath, (uint8_t)src_cache, x, y, z, yaw, pitch, roll, (int16_t)team, 0);
					}
					else if (g_forge.edit_state.placed_object_count < FORGE_MAX_PLACED_OBJECTS)
					{
						/* Record baked object in edit_state and link to its live instance */
						int32_t po_idx = g_forge.edit_state.placed_object_count++;
						struct forge_placed_object_entry *po = &g_forge.edit_state.placed_objects[po_idx];
						struct object_datum *fobj = (struct object_datum *)object_try_and_get_and_verify_type(found_oidx, (unsigned long)-1);

						memset(po, 0, sizeof(*po));
						po->active = 1;
						po->group_tag = (uint32_t)grp;
						po->source_cache_id = (uint8_t)src_cache;
						po->category = (grp == 'vehi') ? _forge_object_vehicle :
							(grp == 'weap') ? _forge_object_weapon :
							(grp == 'bipd') ? _forge_object_biped :
							(grp == 'eqip') ? _forge_object_equipment : _forge_object_scenery;
						snprintf(po->tag_path, sizeof(po->tag_path), "%s", tpath);
						po->x = x; po->y = y; po->z = z;
						po->yaw = yaw; po->pitch = pitch; po->roll = roll;
						po->team_index = (int16_t)team;

						if (fobj && fobj->definition_index != NONE)
						{
							if ((uint32_t)grp == 'vehi')
								patch_vehicle_tag_for_player_driving(fobj->definition_index, tpath);
							else
								patch_forge_device_and_light_tag_runtime(fobj->definition_index, (uint32_t)grp, tpath);
						}

						if (g_forge.live_instance_count < FORGE_MAX_LIVE_INSTANCES)
						{
							int32_t li_idx = g_forge.live_instance_count++;
							struct forge_live_instance *inst = &g_forge.live_instances[li_idx];
							memset(inst, 0, sizeof(*inst));
							inst->active = 1;
							inst->object_index = found_oidx;
							inst->actor_index = NONE;
							inst->edit_entry_index = po_idx;
							inst->group_tag = (uint32_t)grp;
							inst->source_cache_id = (uint8_t)src_cache;
							inst->is_default_scenario_object = 0;
							inst->team_designator = (int16_t)team;
							inst->x = x; inst->y = y; inst->z = z;
							inst->yaw = yaw; inst->pitch = pitch; inst->roll = roll;
							snprintf(inst->tag_path, sizeof(inst->tag_path), "%s", tpath);
							forge_3d_move_instance(li_idx, x, y, z, yaw, pitch, roll, 0);
						}
					}
				}
			}
		}
		else if (in_forge_state && strncmp(line, "AI|", 3) == 0)
		{
			float x = 0, y = 0, z = 0, yaw = 0;
			int team = 0;
			char tpath[256];
			tpath[0] = 0;
			if (sscanf(line + 3, "%f|%f|%f|%f|%d|%255[^\r\n]", &x, &y, &z, &yaw, &team, tpath) == 6 && tpath[0])
			{
				forge_3d_spawn_object_at('actv', tpath, 0, x, y, z, yaw, 0.0f, 0.0f, (int16_t)team, 0);
			}
		}
		else if (in_forge_state && strncmp(line, "DEFAULT_EDIT|", 13) == 0)
		{
			unsigned int grp = 0;
			int deleted = 0;
			float ox = 0, oy = 0, oz = 0, nx = 0, ny = 0, nz = 0, nyaw = 0, npitch = 0, nroll = 0;
			char tpath[256];
			tpath[0] = 0;
			if (sscanf(line + 13, "%u|%d|%f|%f|%f|%f|%f|%f|%f|%f|%f|%255[^\r\n]",
					&grp, &deleted, &ox, &oy, &oz, &nx, &ny, &nz, &nyaw, &npitch, &nroll, tpath) == 12 &&
				g_forge.edit_state.default_edit_count < FORGE_MAX_DEFAULT_OBJECT_EDITS)
			{
				struct forge_default_object_edit *de = &g_forge.edit_state.default_edits[g_forge.edit_state.default_edit_count++];
				struct object_iterator iter;
				struct object_datum *obj;
				long found_oidx = NONE;
				float best_d2 = 9.0f;

				memset(de, 0, sizeof(*de));
				de->active = 1;
				de->group_tag = (uint32_t)grp;
				de->deleted = (uint8_t)deleted;
				de->orig_x = ox; de->orig_y = oy; de->orig_z = oz;
				de->new_x = nx; de->new_y = ny; de->new_z = nz;
				de->new_yaw = nyaw; de->new_pitch = npitch; de->new_roll = nroll;
				snprintf(de->tag_path, sizeof(de->tag_path), "%s", tpath);

				/* Apply default edit live if the scenario object is still at its unedited origin (e.g. Xbox v5 FLG1 container) */
				object_iterator_new(&iter, (unsigned long)-1, 0);
				while ((obj = object_iterator_next(&iter)) != NULL)
				{
					float dx = obj->object.position.x - ox;
					float dy = obj->object.position.y - oy;
					float dz = obj->object.position.z - oz;
					float d2 = dx * dx + dy * dy + dz * dz * 0.25f;
					if (obj->object.parent_object_index != NONE)
						continue;
					if (obj->object.type == _object_type_biped &&
						((struct unit_datum *)obj)->unit.player_index != NONE)
						continue;
					if (d2 < best_d2)
					{
						best_d2 = d2;
						found_oidx = iter.index;
					}
				}
				if (found_oidx != NONE)
				{
					if (deleted)
					{
						object_delete(found_oidx);
					}
					else
					{
						struct object_datum *fobj = (struct object_datum *)object_try_and_get_and_verify_type(found_oidx, (unsigned long)-1);
						if (fobj)
						{
							fobj->object.position.x = nx;
							fobj->object.position.y = ny;
							fobj->object.position.z = nz;
						}
					}
				}
			}
		}
	}
}

void forge_load_saved_map_state_if_present(char const *map_name)
{
	char const *slash;
	char leaf[64];
	char map_path[512];
	char txt_path[512];
	FILE *f;

	if (!map_name || !map_name[0])
		return;

	/* Patch any baked vehicles in the loaded map so their driver seats and bounding-sphere entrance flags work in MP & Forge */
	{
		long tag_cnt = 0, ti;
		struct forge_cache_file_tag_instance *instances =
			(struct forge_cache_file_tag_instance *)cache_files_tag_instances(&tag_cnt);
		if (instances && tag_cnt > 0)
		{
			for (ti = 0; ti < tag_cnt; ti++)
			{
				if (instances[ti].group_tag == 'vehi' && instances[ti].base_address)
					patch_vehicle_tag_for_player_driving(instances[ti].tag_index, instances[ti].name);
			}
		}
	}

	slash = strrchr(map_name, '\\');
	if (!slash)
		slash = strrchr(map_name, '/');
	snprintf(leaf, sizeof(leaf), "%s", slash ? (slash + 1) : map_name);

	/* 1. Read embedded [OpenCE Forge Map] / [ForgeState] directly from custom_maps/<leaf>.map header (0x1E4 / 0x1E8) */
	snprintf(map_path, sizeof(map_path), "custom_maps/%s.map", leaf);
	f = fopen(map_path, "rb");
	if (f)
	{
		uint8_t hdr[0x200];
		if (fread(hdr, 1, sizeof(hdr), f) == sizeof(hdr))
		{
			uint32_t state_off = 0, state_sz = 0, sig = 0;
			memcpy(&state_off, hdr + 0x1E4u, 4);
			memcpy(&state_sz, hdr + 0x1E8u, 4);
			memcpy(&sig, hdr + 0x1FCu, 4);
			if (sig == 0x464C4731u)
			{
				forge_runtime_init();
				snprintf(g_forge.save_map_name, sizeof(g_forge.save_map_name), "%s", leaf);
				g_forge.save_map_name_len = (int)strlen(g_forge.save_map_name);
				g_forge.has_saved_map_once = 1;
				if (state_off >= 0x800u && state_sz > 0 && state_sz < 4u * 1024u * 1024u)
				{
					char *buf = (char *)malloc(state_sz + 1u);
					if (buf)
					{
						if (fseek(f, (long)state_off, SEEK_SET) == 0 && fread(buf, 1, state_sz, f) == state_sz)
						{
							buf[state_sz] = 0;
							parse_forge_state_text_buffer(buf, leaf);
							free(buf);
							fclose(f);
							return;
						}
						free(buf);
					}
				}
			}
		}
		fclose(f);
	}

	/* 2. Legacy fallback for maps saved before embedded state */
	snprintf(txt_path, sizeof(txt_path), "forge_maps/%s/%s.txt", leaf, leaf);
	f = fopen(txt_path, "rb");
	if (!f)
	{
		snprintf(txt_path, sizeof(txt_path), "custom_maps/%s.txt", leaf);
		f = fopen(txt_path, "rb");
	}
	if (f)
	{
		long sz;
		fseek(f, 0, SEEK_END);
		sz = ftell(f);
		fseek(f, 0, SEEK_SET);
		if (sz > 0 && sz < 4 * 1024 * 1024)
		{
			char *buf = (char *)malloc((size_t)sz + 1u);
			if (buf)
			{
				size_t rd = fread(buf, 1, (size_t)sz, f);
				buf[rd] = 0;
				parse_forge_state_text_buffer(buf, leaf);
				free(buf);
			}
		}
		fclose(f);
	}
}

/* ============================================================================
 * MONITOR ("characters\monitor\monitor") & NATIVE MODEL RENDERER
 * - Renders 343 Guilty Spark ("characters\monitor\monitor") and 3D Spawn Menu
 *   previews directly through OpenCE's native orig_render_model!
 * ============================================================================ */

static int draw_native_object_model_at(
	long obj_def_idx,
	long unique_id,
	void const *lighting_ptr,
	union real_point3d const *pos,
	union real_vector3d const *forward,
	union real_vector3d const *up,
	int is_monitor,
	int is_preview,
	unsigned long flags)
{
	uint8_t const *obj_def;
	float radius;
	long model_idx;
	struct model const *mdef;
	struct real_matrix4x3 node_matrices[64];
	static struct render_lighting studio_lighting;
	struct render_lighting const *use_lighting;
	static union real_rgb_color change_colors_mon[4];
	static union real_rgb_color change_colors_std[4];
	static real function_values[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	union real_rgb_color *change_colors = is_monitor ? change_colors_mon : change_colors_std;
	union real_point3d draw_centroid = *pos;
	int node_count, n;

	if (obj_def_idx == NONE)
		return 0;
	obj_def = (uint8_t const *)tag_get(tag_get_group_tag(obj_def_idx), obj_def_idx);
	if (!obj_def)
		return 0;

	radius = *(float const *)(obj_def + 0x04u);
	if (!(radius > 0.1f && radius < 100.0f))
		radius = 1.5f;

	model_idx = *(int32_t const *)(obj_def + 0x34u);
	if (model_idx == NONE)
		return 0;
	mdef = (struct model const *)tag_get('mode', model_idx);
	if (!mdef)
		return 0;

	node_count = (int)mdef->nodes.count;
	if (node_count <= 0)
		return 0;
	if (node_count > 64)
		node_count = 64;

	memset(node_matrices, 0, sizeof(node_matrices));
	if (is_preview)
	{
		float preview_scale = 0.52f / ((radius > 0.15f) ? radius : 0.52f);
		struct real_matrix4x3 scale_pos_mat;
		if (preview_scale < 0.035f) preview_scale = 0.035f;
		if (preview_scale > 1.35f) preview_scale = 1.35f;

		model_get_node_matrices(mdef, node_matrices, global_origin3d, forward, up);
		matrix4x3_from_point_and_vectors(&scale_pos_mat, pos, global_forward3d, global_up3d);
		scale_pos_mat.scale = preview_scale;
		for (n = 0; n < node_count; n++)
		{
			struct real_matrix4x3 tmp = node_matrices[n];
			matrix4x3_multiply(&scale_pos_mat, &tmp, &node_matrices[n]);
		}
		radius *= preview_scale;
	}
	else if (is_monitor)
	{
		union real_point3d mpos = *pos;
		struct real_matrix4x3 scale_pos_mat;
		mpos.z += 0.55f;
		draw_centroid = mpos;
		model_get_node_matrices(mdef, node_matrices, global_origin3d, forward, up);
		matrix4x3_from_point_and_vectors(&scale_pos_mat, &mpos, global_forward3d, global_up3d);
		scale_pos_mat.scale = 1.35f;
		for (n = 0; n < node_count; n++)
		{
			struct real_matrix4x3 tmp = node_matrices[n];
			matrix4x3_multiply(&scale_pos_mat, &tmp, &node_matrices[n]);
		}
		radius *= 1.35f;
	}
	else
	{
		model_get_node_matrices(mdef, node_matrices, pos, forward, up);
	}

	if (is_monitor)
	{
		change_colors[0].red = 0.28f;
		change_colors[0].green = 0.72f;
		change_colors[0].blue = 1.0f;
		change_colors[1].red = 0.65f;
		change_colors[1].green = 0.90f;
		change_colors[1].blue = 1.0f;
	}
	else
	{
		change_colors[0].red = 0.85f;
		change_colors[0].green = 0.85f;
		change_colors[0].blue = 0.85f;
		change_colors[1].red = 0.75f;
		change_colors[1].green = 0.75f;
		change_colors[1].blue = 0.75f;
	}
	change_colors[2] = change_colors[0];
	change_colors[3] = change_colors[1];

	function_values[0] = 1.0f;
	function_values[1] = 1.0f;
	function_values[2] = 1.0f;
	function_values[3] = 1.0f;

	if (lighting_ptr)
	{
		use_lighting = (struct render_lighting const *)lighting_ptr;
	}
	else
	{
		memset(&studio_lighting, 0, sizeof(studio_lighting));
		studio_lighting.ambient_color.red = 0.72f;
		studio_lighting.ambient_color.green = 0.74f;
		studio_lighting.ambient_color.blue = 0.78f;
		studio_lighting.distant_light_count = 1;
		studio_lighting.distant_lights[0].color.red = 0.95f;
		studio_lighting.distant_lights[0].color.green = 0.95f;
		studio_lighting.distant_lights[0].color.blue = 0.92f;
		studio_lighting.distant_lights[0].direction.i = -0.577f;
		studio_lighting.distant_lights[0].direction.j = -0.577f;
		studio_lighting.distant_lights[0].direction.k = -0.577f;
		use_lighting = &studio_lighting;
	}

	orig_render_model(
		model_idx,
		10000.0f,
		node_matrices,
		NULL,
		change_colors,
		function_values,
		use_lighting,
		&draw_centroid,
		radius,
		NULL,
		(is_monitor || is_preview) ? NONE : unique_id,
		0,
		is_preview ? (flags | (1u << 3)) /* _render_model_first_person_bit */ : flags);
	return 1;
}

enum forge_marker_color_preset
{
	_marker_color_ffa_yellow = 0,
	_marker_color_red_team,
	_marker_color_blue_team,
	_marker_color_tele_green,
	_marker_color_race_orange,
	_marker_color_oddball_purple,
	_marker_color_koth_gold,
	NUMBER_OF_MARKER_COLOR_PRESETS
};

enum forge_marker_mesh_kind
{
	_marker_mesh_player_spawn = 0,
	_marker_mesh_tele_origin,
	_marker_mesh_tele_dest,
	_marker_mesh_race,
	_marker_mesh_oddball,
	_marker_mesh_ctf_flag,
	_marker_mesh_koth,
	_marker_mesh_koth_beam,
	NUMBER_OF_MARKER_MESH_KINDS
};

static struct
{
	int loaded;
	struct
	{
		int valid;
		int part_count;
		float radius;
		struct forge_hw_part parts[FORGE_MAX_HW_PARTS];
	} meshes[NUMBER_OF_MARKER_MESH_KINDS];
} s_marker_meshes = { 0 };

static long get_or_create_translucent_marker_shader(int color_preset, int is_highlighted, int is_z_prime)
{
	static uint32_t const s_preset_rgb[NUMBER_OF_MARKER_COLOR_PRESETS] = {
		0x00FFE618u, /* FFA Player Spawn     - Translucent Yellow */
		0x00FF2424u, /* Red Team Spawn/Flag  - Translucent Red    */
		0x002488FFu, /* Blue Team Spawn/Flag - Translucent Blue   */
		0x0024FF54u, /* Teleporter In/Out    - Translucent Green  */
		0x00FF8C14u, /* Race Marker          - Translucent Orange */
		0x00BC38FFu, /* Oddball Marker       - Translucent Purple */
		0x00FFC824u  /* KOTH Hill Marker     - Translucent Gold   */
	};
	char shdr_name[96];
	char bitm_name[96];
	long shdr_idx, tmpl_soso_idx, bitm_idx;
	struct tag_iterator iter;
	void *tmpl_ptr;
	struct bitmap_data *bmp;
	struct bitmap_group *bgrp;
	uint8_t *soso_def;
	uint32_t rgb, alpha_byte, argb;
	uint32_t *pix;
	float r_f, g_f, b_f;
	int i;

	if (color_preset < 0 || color_preset >= NUMBER_OF_MARKER_COLOR_PRESETS)
		color_preset = _marker_color_ffa_yellow;

	snprintf(shdr_name, sizeof(shdr_name), "forge\\markers\\shdr_%d_%d_%d",
		color_preset, is_highlighted ? 1 : 0, is_z_prime ? 1 : 0);
	shdr_idx = tag_loaded('soso', shdr_name);
	if (shdr_idx != NONE)
		return shdr_idx;

	rgb = s_preset_rgb[color_preset];
	alpha_byte = is_z_prime ? 0xFFu : (is_highlighted ? 0xB8u : 0x8Cu);
	argb = (alpha_byte << 24) | rgb;
	r_f = (float)((rgb >> 16) & 0xFFu) / 255.0f;
	g_f = (float)((rgb >> 8) & 0xFFu) / 255.0f;
	b_f = (float)(rgb & 0xFFu) / 255.0f;

	snprintf(bitm_name, sizeof(bitm_name), "forge\\markers\\bitm_%d_%d_%d",
		color_preset, is_highlighted ? 1 : 0, is_z_prime ? 1 : 0);
	bitm_idx = tag_loaded('bitm', bitm_name);
	if (bitm_idx == NONE)
	{
		/* Format 11 = _bitmap_format_a8r8g8b8 (32-bit ARGB) */
		bmp = bitmap_2d_new(16, 16, 0, 11);
		if (!bmp || !bmp->base_address)
			return NONE;
		pix = (uint32_t *)bmp->base_address;
		for (i = 0; i < 16 * 16; i++)
			pix[i] = argb;
		bitmap_rebuild(bmp);

		bgrp = (struct bitmap_group *)calloc(1, sizeof(struct bitmap_group));
		if (!bgrp)
			return NONE;
		bgrp->bitmaps.count = 1;
		bgrp->bitmaps.address = bmp;
		bitm_idx = append_runtime_tag_instance('bitm', NONE, NONE, bitm_name, bgrp);
		if (bitm_idx == NONE)
			return NONE;
	}

	tag_iterator_new(&iter, 'soso');
	tmpl_soso_idx = tag_iterator_next(&iter);
	if (tmpl_soso_idx == NONE)
		return NONE;
	tmpl_ptr = shader_definition_get(tmpl_soso_idx);
	if (!tmpl_ptr)
		return NONE;

	soso_def = (uint8_t *)calloc(1, 512);
	if (!soso_def)
		return NONE;
	memcpy(soso_def, tmpl_ptr, 440);

	*(int16_t *)(soso_def + 0x24) = 4; /* _shader_type_model */
	/* Bit 1 = _shader_model_two_sided_bit, Bit 2 = _shader_model_not_alpha_tested_bit, Bit 3 = _shader_model_alpha_blended_decal_bit */
	*(uint16_t *)(soso_def + 0x28) = is_z_prime ? ((1u << 1) | (1u << 2)) : ((1u << 1) | (1u << 2) | (1u << 3));
	*(float *)(soso_def + 0x38) = is_highlighted ? 0.72f : 0.55f;
	*(int16_t *)(soso_def + 0x4C) = 0;
	*(uint16_t *)(soso_def + 0x6C) = 1u; /* _shader_model_self_illumination_no_random_phase_bit */
	*(int16_t *)(soso_def + 0x70) = 0;
	*(int16_t *)(soso_def + 0x72) = 0;
	*(float *)(soso_def + 0x74) = 1.0f;
	*(float *)(soso_def + 0x78) = r_f * 0.34f;
	*(float *)(soso_def + 0x7C) = g_f * 0.34f;
	*(float *)(soso_def + 0x80) = b_f * 0.34f;
	*(float *)(soso_def + 0x84) = r_f * 0.34f;
	*(float *)(soso_def + 0x88) = g_f * 0.34f;
	*(float *)(soso_def + 0x8C) = b_f * 0.34f;
	*(float *)(soso_def + 0x9C) = 1.0f;
	*(float *)(soso_def + 0xA0) = 1.0f;
	*(int32_t *)(soso_def + 0xA4) = 'bitm';
	*(int32_t *)(soso_def + 0xA8) = 0;
	*(int32_t *)(soso_def + 0xAC) = 0;
	*(int32_t *)(soso_def + 0xB0) = bitm_idx;
	*(int32_t *)(soso_def + 0xC8) = NONE;
	*(int16_t *)(soso_def + 0xD4) = 0;
	*(int16_t *)(soso_def + 0xD6) = 0;
	*(float *)(soso_def + 0xD8) = 1.0f;
	*(int32_t *)(soso_def + 0xE8) = NONE;
	*(float *)(soso_def + 0xEC) = 1.0f;
	*(float *)(soso_def + 0x144) = 0.0f;
	*(float *)(soso_def + 0x154) = 0.0f;
	*(int32_t *)(soso_def + 0x170) = NONE;

	return append_runtime_tag_instance('soso', 'shdr', NONE, shdr_name, soso_def);
}

static void ensure_objective_marker_meshes_loaded(void)
{
	static char const *const s_files[NUMBER_OF_MARKER_MESH_KINDS] = {
		"OpenCE Forge/cached_tags/markers/player_spawn.ftag",
		"OpenCE Forge/cached_tags/markers/teleporter_origin.ftag",
		"OpenCE Forge/cached_tags/markers/teleporter_destination.ftag",
		"OpenCE Forge/cached_tags/markers/race_marker.ftag",
		"OpenCE Forge/cached_tags/markers/oddball_marker.ftag",
		"OpenCE Forge/cached_tags/markers/ctf_flag_marker.ftag",
		"OpenCE Forge/cached_tags/markers/koth_marker.ftag",
		"OpenCE Forge/cached_tags/markers/koth_beam.ftag"
	};
	int k;
	int all_valid = 1;

	if (s_marker_meshes.loaded)
	{
		for (k = 0; k < NUMBER_OF_MARKER_MESH_KINDS; k++)
		{
			if (!s_marker_meshes.meshes[k].valid)
			{
				all_valid = 0;
				break;
			}
		}
		if (all_valid)
		{
			s_marker_meshes_loaded_flag = 1;
			return;
		}
	}
	s_marker_meshes.loaded = 1;
	s_marker_meshes_loaded_flag = 1;

	CreateDirectoryA("OpenCE Forge", NULL);
	CreateDirectoryA("OpenCE Forge/cached_tags", NULL);
	CreateDirectoryA("OpenCE Forge/cached_tags/markers", NULL);

	for (k = 0; k < NUMBER_OF_MARKER_MESH_KINDS; k++)
	{
		FILE *test_fp;

		if (s_marker_meshes.meshes[k].valid && s_marker_meshes.meshes[k].part_count > 0)
			continue;

		test_fp = fopen(s_files[k], "rb");
		if (test_fp)
		{
			fclose(test_fp);
		}
		else if (k < 8 && s_embedded_marker_ftags[k].comp_data && s_embedded_marker_ftags[k].uncomp_size > 0)
		{
			unsigned long raw_len = (unsigned long)s_embedded_marker_ftags[k].uncomp_size;
			uint8_t *raw_buf = (uint8_t *)malloc((size_t)raw_len);
			if (raw_buf)
			{
				if (z_uncompress(raw_buf, &raw_len, s_embedded_marker_ftags[k].comp_data, (unsigned long)s_embedded_marker_ftags[k].comp_size) == 0)
				{
					FILE *out_fp = fopen(s_files[k], "wb");
					if (out_fp)
					{
						fwrite(raw_buf, 1, (size_t)raw_len, out_fp);
						fclose(out_fp);
					}
				}
				free(raw_buf);
			}
		}

		s_marker_meshes.meshes[k].valid = 0;
		s_marker_meshes.meshes[k].part_count = 0;
		s_marker_meshes.meshes[k].radius = 0.45f;
		if (load_hw_parts_from_ftag_file(
				s_files[k],
				s_marker_meshes.meshes[k].parts,
				FORGE_MAX_HW_PARTS,
				&s_marker_meshes.meshes[k].part_count,
				&s_marker_meshes.meshes[k].radius,
				1.0f,
				0.0f))
		{
			s_marker_meshes.meshes[k].valid = (s_marker_meshes.meshes[k].part_count > 0) ? 1 : 0;
		}
	}
}

static void draw_translucent_marker_hw_parts(
	union real_point3d const *pos,
	union real_vector3d const *forward,
	union real_vector3d const *up,
	float radius,
	float scale,
	struct forge_hw_part *parts,
	int part_count,
	int color_preset,
	int is_highlighted,
	int is_preview)
{
	struct rasterizer_model_begin_parameters model_parameters;
	static union real_rgb_color change_colors[4];
	static real function_values[4];
	struct real_matrix4x3 node_matrix;
	long z_shdr_idx, col_shdr_idx;
	struct shader *col_shdr_def;
	int p;

	if (!parts || part_count <= 0)
		return;

	z_shdr_idx = get_or_create_translucent_marker_shader(color_preset, is_highlighted, 1);
	(void)z_shdr_idx;
	col_shdr_idx = get_or_create_translucent_marker_shader(color_preset, is_highlighted, 0);
	if (col_shdr_idx == NONE)
		return;
	col_shdr_def = shader_definition_get(col_shdr_idx);
	if (!col_shdr_def)
		return;

	matrix4x3_from_point_and_vectors(&node_matrix, pos, forward, up);
	if (scale > 0.001f && scale != 1.0f)
		node_matrix.scale = scale;

	change_colors[0].red = 1.0f;
	change_colors[0].green = 1.0f;
	change_colors[0].blue = 1.0f;
	change_colors[1] = change_colors[0];
	change_colors[2] = change_colors[0];
	change_colors[3] = change_colors[0];

	function_values[0] = 1.0f;
	function_values[1] = 1.0f;
	function_values[2] = 1.0f;
	function_values[3] = 1.0f;

	memset(&model_parameters, 0, sizeof(model_parameters));
	model_parameters.unique_identifier = NONE;
	model_parameters.lighting.ambient_color.red = is_highlighted ? 0.88f : 0.72f;
	model_parameters.lighting.ambient_color.green = is_highlighted ? 0.88f : 0.72f;
	model_parameters.lighting.ambient_color.blue = is_highlighted ? 0.88f : 0.72f;
	model_parameters.lighting.distant_light_count = 1;
	model_parameters.lighting.distant_lights[0].color.red = 0.95f;
	model_parameters.lighting.distant_lights[0].color.green = 0.95f;
	model_parameters.lighting.distant_lights[0].color.blue = 0.95f;
	model_parameters.lighting.distant_lights[0].direction.i = -0.45f;
	model_parameters.lighting.distant_lights[0].direction.j = -0.45f;
	model_parameters.lighting.distant_lights[0].direction.k = -0.77f;
	model_parameters.centroid = *pos;
	model_parameters.radius = (radius > 0.1f) ? (radius * ((scale > 0.001f) ? scale : 1.0f)) : 0.6f;
	model_parameters.animation.colors = change_colors;
	model_parameters.animation.values = function_values;
	model_parameters.skinning.node_matrices = &node_matrix;
	model_parameters.skinning.node_matrix_count = 1;
	model_parameters.geometry_flags = (1u << 2) | (is_preview ? (1u << 4) : 0u); /* _rasterizer_geometry_no_fog_bit */
	model_parameters.base_map_scale.i = 1.0f;
	model_parameters.base_map_scale.j = 1.0f;

	rasterizer_model_begin(&model_parameters, FALSE);

	for (p = 0; p < part_count; p++)
	{
		rasterizer_model_draw(
			col_shdr_def,
			0,
			&parts[p].triangle_buffer,
			NONE,
			parts[p].triangle_buffer.count,
			&parts[p].vertex_buffer,
			NONE);
	}
	rasterizer_model_end();
	if (is_preview)
	{
		rasterizer_set_stencil_mode(0);
		rasterizer_set_frustum_z(0.0f, 0.0f);
	}
}

static int resolve_marker_kind_and_color(
	uint32_t group_tag,
	char const *tag_path,
	int16_t netgame_flag_type,
	int16_t team_designator,
	int *out_mesh_kind,
	int *out_color_preset)
{
	if (group_tag == 'spwn' || (tag_path && strncmp(tag_path, "spawn\\", 6) == 0))
	{
		*out_mesh_kind = _marker_mesh_player_spawn;
		if (team_designator == 1 || (tag_path && strstr(tag_path, "blue")))
			*out_color_preset = _marker_color_blue_team;
		else if (team_designator == 2 || (tag_path && strstr(tag_path, "ffa")))
			*out_color_preset = _marker_color_ffa_yellow;
		else
			*out_color_preset = _marker_color_red_team;
		return 1;
	}
	if (group_tag == 'tele' || (tag_path && strncmp(tag_path, "teleporter\\", 11) == 0))
	{
		int is_sender = (netgame_flag_type == 6 /* _netgame_flag_teleporter_source */ ||
			(tag_path && (strstr(tag_path, "origin") || strstr(tag_path, "sender"))));
		*out_mesh_kind = is_sender ? _marker_mesh_tele_origin : _marker_mesh_tele_dest;
		*out_color_preset = _marker_color_tele_green;
		return 1;
	}
	if (group_tag == 'flag' || (tag_path && strncmp(tag_path, "objective\\", 10) == 0))
	{
		if (netgame_flag_type == 2 /* _netgame_flag_oddball_ball_spawn */ ||
			(tag_path && strstr(tag_path, "oddball")))
		{
			*out_mesh_kind = _marker_mesh_oddball;
			*out_color_preset = _marker_color_oddball_purple;
			return 1;
		}
		if (netgame_flag_type == 3 /* _netgame_flag_race_track */ ||
			netgame_flag_type == 4 /* _netgame_flag_race_vehicle */ ||
			(tag_path && strstr(tag_path, "race")))
		{
			*out_mesh_kind = _marker_mesh_race;
			*out_color_preset = _marker_color_race_orange;
			return 1;
		}
		if (netgame_flag_type == 0 /* _netgame_flag_ctf_flag */ ||
			netgame_flag_type == 1 /* _netgame_flag_ctf_vehicle */ ||
			(tag_path && strstr(tag_path, "ctf")))
		{
			*out_mesh_kind = _marker_mesh_ctf_flag;
			*out_color_preset = (team_designator == 1 || (tag_path && strstr(tag_path, "blue")))
				? _marker_color_blue_team
				: _marker_color_red_team;
			return 1;
		}
		*out_mesh_kind = _marker_mesh_koth;
		*out_color_preset = _marker_color_koth_gold;
		return 1;
	}
	return 0;
}

static void draw_translucent_koth_beam_3d(
	union real_point3d const *p0,
	union real_point3d const *p1)
{
	struct rasterizer_model_begin_parameters model_parameters;
	union real_rgb_color change_colors[4];
	real function_values[4];
	struct real_matrix4x3 node_matrix;
	long col_shdr_idx;
	struct shader *col_shdr_def;
	float dx, dy, dz, len_xy, len_3d;
	int p;

	if (!s_marker_meshes.meshes[_marker_mesh_koth_beam].valid ||
		s_marker_meshes.meshes[_marker_mesh_koth_beam].part_count <= 0)
		return;

	dx = p1->x - p0->x;
	dy = p1->y - p0->y;
	dz = p1->z - p0->z;
	len_xy = square_root(dx * dx + dy * dy);
	len_3d = square_root(dx * dx + dy * dy + dz * dz);
	if (len_3d < 0.01f)
		return;

	col_shdr_idx = get_or_create_translucent_marker_shader(_marker_color_koth_gold, 1, 0);
	if (col_shdr_idx == NONE)
		return;
	col_shdr_def = shader_definition_get(col_shdr_idx);
	if (!col_shdr_def)
		return;

	/*
	 * Stretch the unit 3D beam (x in [0..1], y in [-0.015..+0.015], z in [0.025..0.055])
	 * along forward = (p1 - p0) while keeping left and up unit-length so cross-section
	 * thickness stays constant regardless of segment length!
	 */
	memset(&node_matrix, 0, sizeof(node_matrix));
	node_matrix.scale = 1.0f;
	node_matrix.position = *p0;
	node_matrix.forward.i = dx;
	node_matrix.forward.j = dy;
	node_matrix.forward.k = dz;
	if (len_xy > 0.001f)
	{
		node_matrix.left.i = -dy / len_xy;
		node_matrix.left.j = dx / len_xy;
		node_matrix.left.k = 0.0f;
	}
	else
	{
		node_matrix.left.i = 0.0f;
		node_matrix.left.j = 1.0f;
		node_matrix.left.k = 0.0f;
	}
	node_matrix.up.i = 0.0f;
	node_matrix.up.j = 0.0f;
	node_matrix.up.k = 1.0f;

	change_colors[0].red = 1.0f;
	change_colors[0].green = 1.0f;
	change_colors[0].blue = 1.0f;
	change_colors[1] = change_colors[0];
	change_colors[2] = change_colors[0];
	change_colors[3] = change_colors[0];

	function_values[0] = 1.0f;
	function_values[1] = 1.0f;
	function_values[2] = 1.0f;
	function_values[3] = 1.0f;

	memset(&model_parameters, 0, sizeof(model_parameters));
	model_parameters.unique_identifier = NONE;
	model_parameters.lighting.ambient_color.red = 0.90f;
	model_parameters.lighting.ambient_color.green = 0.90f;
	model_parameters.lighting.ambient_color.blue = 0.90f;
	model_parameters.lighting.distant_light_count = 1;
	model_parameters.lighting.distant_lights[0].color.red = 0.95f;
	model_parameters.lighting.distant_lights[0].color.green = 0.95f;
	model_parameters.lighting.distant_lights[0].color.blue = 0.95f;
	model_parameters.lighting.distant_lights[0].direction.i = -0.45f;
	model_parameters.lighting.distant_lights[0].direction.j = -0.45f;
	model_parameters.lighting.distant_lights[0].direction.k = -0.77f;
	model_parameters.centroid.x = 0.5f * (p0->x + p1->x);
	model_parameters.centroid.y = 0.5f * (p0->y + p1->y);
	model_parameters.centroid.z = 0.5f * (p0->z + p1->z);
	model_parameters.radius = 0.5f * len_3d + 0.5f;
	model_parameters.animation.colors = change_colors;
	model_parameters.animation.values = function_values;
	model_parameters.skinning.node_matrices = &node_matrix;
	model_parameters.skinning.node_matrix_count = 1;
	model_parameters.geometry_flags = (1u << 2); /* _rasterizer_geometry_no_fog_bit */
	model_parameters.base_map_scale.i = 1.0f;
	model_parameters.base_map_scale.j = 1.0f;

	rasterizer_model_begin(&model_parameters, FALSE);
	for (p = 0; p < s_marker_meshes.meshes[_marker_mesh_koth_beam].part_count; p++)
	{
		rasterizer_model_draw(
			col_shdr_def,
			0,
			&s_marker_meshes.meshes[_marker_mesh_koth_beam].parts[p].triangle_buffer,
			NONE,
			s_marker_meshes.meshes[_marker_mesh_koth_beam].parts[p].triangle_buffer.count,
			&s_marker_meshes.meshes[_marker_mesh_koth_beam].parts[p].vertex_buffer,
			NONE);
	}
	rasterizer_model_end();
}

static void render_world_objective_and_spawn_markers(void)
{
	int i, hill_id;

	ensure_objective_marker_meshes_loaded();

	for (i = 0; i < g_forge.live_instance_count; i++)
	{
		struct forge_live_instance const *inst = &g_forge.live_instances[i];
		int mesh_kind = 0, color_preset = 0;
		int is_hi;
		float dx, dy, dz, d2;
		union real_point3d pos;
		union real_vector3d fwd, up;

		if (!inst->active)
			continue;
		if (inst->group_tag != 'spwn' && inst->group_tag != 'tele' && inst->group_tag != 'flag')
			continue;
		if (!resolve_marker_kind_and_color(
				inst->group_tag,
				inst->tag_path,
				inst->netgame_flag_type,
				inst->team_designator,
				&mesh_kind,
				&color_preset))
			continue;
		if (mesh_kind < 0 || mesh_kind >= NUMBER_OF_MARKER_MESH_KINDS ||
			!s_marker_meshes.meshes[mesh_kind].valid)
			continue;

		/* Player spawns, KOTH, Oddball, and Race markers are only visible while Forge Mode is active */
		if (g_forge.mode != _forge_mode_3d_monitor &&
			(mesh_kind == _marker_mesh_player_spawn ||
			 mesh_kind == _marker_mesh_koth ||
			 mesh_kind == _marker_mesh_oddball ||
			 mesh_kind == _marker_mesh_race))
		{
			continue;
		}

		if (mesh_kind == _marker_mesh_koth)
		{
			/* KOTH markers only highlight when actively clicked/grabbed (or during 10-node placement), never on hover/proximity */
			is_hi = (i == g_forge.grabbed_live_index) ||
					(g_forge.koth_group_placing && inst->team_designator == g_forge.koth_group_hill_id);
		}
		else
		{
			is_hi = (i == g_forge.grabbed_live_index || i == g_forge.hovered_live_index);
		}

		dx = inst->x - render.camera.position.x;
		dy = inst->y - render.camera.position.y;
		dz = inst->z - render.camera.position.z;
		d2 = dx * dx + dy * dy + dz * dz;
		if (d2 > 300.0f * 300.0f && !is_hi)
			continue;

		pos.x = inst->x;
		pos.y = inst->y;
		pos.z = inst->z;
		fwd.i = (float)cosine(inst->yaw);
		fwd.j = (float)sine(inst->yaw);
		fwd.k = 0.0f;
		up.i = 0.0f;
		up.j = 0.0f;
		up.k = 1.0f;

		draw_translucent_marker_hw_parts(
			&pos,
			&fwd,
			&up,
			s_marker_meshes.meshes[mesh_kind].radius,
			1.0f,
			s_marker_meshes.meshes[mesh_kind].parts,
			s_marker_meshes.meshes[mesh_kind].part_count,
			color_preset,
			is_hi,
			0);
	}

	/* Render 3D translucent KOTH boundary beams ONLY in Forge mode when a node of that hill is actually clicked/grabbed
	 * (or actively being placed as a 10-point hill), connecting the exact 3D marker positions. */
	if (g_forge.mode != _forge_mode_3d_monitor)
		return;
	for (hill_id = 0; hill_id < 16; hill_id++)
	{
		int node_indices[32];
		float node_angles[32];
		int ncount = 0;
		int hill_grabbed = (g_forge.koth_group_placing && g_forge.koth_group_hill_id == hill_id) ? 1 : 0;
		float cx_h = 0.0f, cy_h = 0.0f;
		int a, b;

		for (i = 0; i < g_forge.live_instance_count && ncount < 32; i++)
		{
			struct forge_live_instance const *inst = &g_forge.live_instances[i];
			if (!inst->active || inst->group_tag != 'flag')
				continue;
			if (inst->netgame_flag_type != 8 /* _netgame_flag_hill */ && strstr(inst->tag_path, "koth") == NULL)
				continue;
			if (inst->team_designator != hill_id)
				continue;
			node_indices[ncount++] = i;
			cx_h += inst->x;
			cy_h += inst->y;
			if (i == g_forge.grabbed_live_index)
				hill_grabbed = 1;
		}

		if (ncount >= 2 && hill_grabbed)
		{
			cx_h /= (float)ncount;
			cy_h /= (float)ncount;

			for (a = 0; a < ncount; a++)
			{
				struct forge_live_instance const *na = &g_forge.live_instances[node_indices[a]];
				node_angles[a] = (float)atan2((double)(na->y - cy_h), (double)(na->x - cx_h));
			}
			for (a = 0; a < ncount - 1; a++)
			{
				for (b = a + 1; b < ncount; b++)
				{
					if (node_angles[b] < node_angles[a])
					{
						float ta = node_angles[a];
						int ti = node_indices[a];
						node_angles[a] = node_angles[b];
						node_indices[a] = node_indices[b];
						node_angles[b] = ta;
						node_indices[b] = ti;
					}
				}
			}

			for (a = 0; a < ncount; a++)
			{
				int nb = (a + 1) % ncount;
				struct forge_live_instance const *inst0 = &g_forge.live_instances[node_indices[a]];
				struct forge_live_instance const *inst1 = &g_forge.live_instances[node_indices[nb]];
				union real_point3d p0, p1;
				p0.x = inst0->x;
				p0.y = inst0->y;
				p0.z = inst0->z;
				p1.x = inst1->x;
				p1.y = inst1->y;
				p1.z = inst1->z;
				draw_translucent_koth_beam_3d(&p0, &p1);
			}
		}
	}
}

static void render_spawn_palette_3d_preview(long object_index, void const *lighting_ptr)
{
	int i, match_count = 0, chosen_idx = -1;
	struct forge_catalog_entry const *entry;
	union real_point3d preview_pos;
	union real_vector3d preview_fwd, preview_up, cam_right;
	float dist, angle;
	int marker_kind = -1, marker_color = 0;
	uint32_t actual_grp = 0;
	long obj_def_idx;

	if (g_forge.active_modal != _forge_modal_spawn_palette || g_forge.h3_menu_stage != 1)
		return;

	for (i = 0; i < g_forge.catalog_count; i++)
	{
		if (g_forge.catalog[i].category == g_forge.current_category &&
			forge_catalog_entry_allowed(&g_forge.catalog[i]))
		{
			if (match_count == g_forge.palette_cursor)
			{
				chosen_idx = i;
				break;
			}
			match_count++;
		}
	}
	if (chosen_idx < 0)
		return;

	entry = &g_forge.catalog[chosen_idx];

	/* Check if this catalog entry is one of our 3D Translucent Spawn/Teleporter/Objective Markers! */
	if (resolve_marker_kind_and_color(entry->group_tag, entry->tag_path, -1, -1, &marker_kind, &marker_color))
	{
		ensure_objective_marker_meshes_loaded();
		if (marker_kind >= 0 && marker_kind < NUMBER_OF_MARKER_MESH_KINDS &&
			s_marker_meshes.meshes[marker_kind].valid)
		{
			dist = 1.45f;
			cross_product3d(&render.camera.forward, &render.camera.up, &cam_right);
			preview_pos.x = render.camera.position.x + render.camera.forward.i * dist + cam_right.i * 0.42f - render.camera.up.i * 0.18f;
			preview_pos.y = render.camera.position.y + render.camera.forward.j * dist + cam_right.j * 0.42f - render.camera.up.j * 0.18f;
			preview_pos.z = render.camera.position.z + render.camera.forward.k * dist + cam_right.k * 0.42f - render.camera.up.k * 0.18f;

			angle = (float)(system_milliseconds() % 6000u) * (6.2831853f / 6000.0f);
			preview_fwd.i = (float)cosine(angle);
			preview_fwd.j = (float)sine(angle);
			preview_fwd.k = 0.0f;
			preview_up.i = 0.0f;
			preview_up.j = 0.0f;
			preview_up.k = 1.0f;

			draw_translucent_marker_hw_parts(
				&preview_pos,
				&preview_fwd,
				&preview_up,
				s_marker_meshes.meshes[marker_kind].radius,
				1.15f,
				s_marker_meshes.meshes[marker_kind].parts,
				s_marker_meshes.meshes[marker_kind].part_count,
				marker_color,
				1,
				1);
			return;
		}
	}

	{
		int known_missing = 0;
		unsigned long now_ms = system_milliseconds();

		if (chosen_idx != s_last_preview_chosen_idx)
		{
			s_last_preview_chosen_idx = chosen_idx;
			s_last_preview_cursor_ms = now_ms;
		}

		obj_def_idx = forge_try_get_loaded_engine_tag(
			entry->group_tag,
			entry->tag_path,
			&actual_grp,
			&known_missing);
		if (obj_def_idx == NONE && !known_missing)
		{
			/* Debounce disk-backed dynamic tag loading while the user is actively scrolling
			 * through the Spawn Palette so scrolling never hitches on intermediate items! */
			if ((unsigned long)(now_ms - s_last_preview_cursor_ms) < 200u)
				return;
			obj_def_idx = forge_resolve_engine_tag(entry->group_tag, entry->tag_path, &actual_grp);
		}
	}
	if (obj_def_idx == NONE)
		return;

	dist = 1.55f;
	cross_product3d(&render.camera.forward, &render.camera.up, &cam_right);
	preview_pos.x = render.camera.position.x + render.camera.forward.i * dist + cam_right.i * 0.42f - render.camera.up.i * 0.14f;
	preview_pos.y = render.camera.position.y + render.camera.forward.j * dist + cam_right.j * 0.42f - render.camera.up.j * 0.14f;
	preview_pos.z = render.camera.position.z + render.camera.forward.k * dist + cam_right.k * 0.42f - render.camera.up.k * 0.14f;

	angle = (float)(system_milliseconds() % 6000u) * (6.2831853f / 6000.0f);
	preview_fwd.i = (float)cosine(angle);
	preview_fwd.j = (float)sine(angle);
	preview_fwd.k = 0.0f;
	preview_up.i = 0.0f;
	preview_up.j = 0.0f;
	preview_up.k = 1.0f;

	draw_native_object_model_at(
		obj_def_idx,
		object_index,
		lighting_ptr,
		&preview_pos,
		&preview_fwd,
		&preview_up,
		0,
		1,
		0);
}

void forge_render_world_3d_overlays(void const *lighting_ptr)
{
	if (!g_forge_session_enabled || !g_forge.initialized)
		return;
	if (g_forge.mode == _forge_mode_3d_monitor && g_forge.third_person_view && !s_local_monitor_drawn_this_frame)
	{
		long local_unit = player_control_get_unit_index(0);
		if (local_unit != NONE)
			forge_monitor_render_player_unit(local_unit, lighting_ptr, 0);
	}
	s_local_monitor_drawn_this_frame = 0;
	render_world_objective_and_spawn_markers();
	render_spawn_palette_3d_preview(NONE, lighting_ptr);
}

int forge_monitor_render_player_unit(long object_index, void const *lighting_ptr, unsigned long flags)
{
	struct unit_datum *unit;

	if (!g_forge_session_enabled || object_index == NONE)
		return 0;

	/* Check if this object is a player biped in Forge Monitor mode (per-player in MP!) */
	unit = (struct unit_datum *)object_try_and_get_and_verify_type(object_index, _object_mask_biped);
	if (!unit || unit->object.type != _object_type_biped || unit->unit.player_index == NONE)
		return 0;

	if (!forge_is_unit_in_monitor_mode(object_index))
		return 0;

	if (flags & (1u << 1)) /* Skip shadow pass for Monitor */
		return 1;

	if (!s_monitor_resolved || s_monitor_bipd_handle == NONE)
	{
		uint32_t actual_grp = 'bipd';
		s_monitor_resolved = 1;
		s_monitor_bipd_handle = forge_resolve_engine_tag('bipd', "characters\\monitor\\monitor", &actual_grp);
	}
	if (s_monitor_bipd_handle == NONE)
		return 0;

	if (object_index == player_control_get_unit_index(0))
	{
		union real_point3d pos;
		union real_vector3d fwd, up;
		float cy, sy, cp, sp;

		if (!g_forge.third_person_view)
			return 1;
		s_local_monitor_drawn_this_frame = 1;

		/* Place local Monitor at exact live camera anchor (draw_native_object_model_at adds +0.55f to Z) */
		pos.x = g_forge.cam_x;
		pos.y = g_forge.cam_y;
		pos.z = g_forge.cam_z - 0.55f;

		cy = (float)cosine(g_forge.cam_yaw);
		sy = (float)sine(g_forge.cam_yaw);
		cp = (float)cosine(g_forge.cam_pitch);
		sp = (float)sine(g_forge.cam_pitch);
		fwd.i = cy * cp;
		fwd.j = sy * cp;
		fwd.k = sp;
		up.i = -cy * sp;
		up.j = -sy * sp;
		up.k = cp;

		return draw_native_object_model_at(
			s_monitor_bipd_handle,
			NONE,
			lighting_ptr,
			&pos,
			&fwd,
			&up,
			1,
			0,
			flags);
	}

	{
		union real_point3d pos = unit->object.position;
		union real_vector3d fwd = unit->object.forward;
		union real_vector3d up = unit->object.up;
		int p_abs = (int)((uint32_t)unit->unit.player_index & 0xFFFFu);

		if (p_abs >= 0 && p_abs < 128 && g_forge.player_has_monitor_pos[p_abs])
		{
			float rx = g_forge.player_has_render_pos[p_abs]
				? g_forge.player_monitor_render_pos[p_abs][0]
				: g_forge.player_monitor_pos[p_abs][0];
			float ry = g_forge.player_has_render_pos[p_abs]
				? g_forge.player_monitor_render_pos[p_abs][1]
				: g_forge.player_monitor_pos[p_abs][1];
			float rz = g_forge.player_has_render_pos[p_abs]
				? g_forge.player_monitor_render_pos[p_abs][2]
				: g_forge.player_monitor_pos[p_abs][2];
			float ryaw = g_forge.player_has_render_pos[p_abs]
				? g_forge.player_monitor_render_yaw[p_abs]
				: g_forge.player_monitor_yaw[p_abs];
			float rpitch = g_forge.player_has_render_pos[p_abs]
				? g_forge.player_monitor_render_pitch[p_abs]
				: g_forge.player_monitor_pitch[p_abs];
			float cy = (float)cosine(ryaw);
			float sy = (float)sine(ryaw);
			float cp = (float)cosine(rpitch);
			float sp = (float)sine(rpitch);

			pos.x = rx;
			pos.y = ry;
			pos.z = rz - 0.55f;
			fwd.i = cy * cp;
			fwd.j = sy * cp;
			fwd.k = sp;
			up.i = -cy * sp;
			up.j = -sy * sp;
			up.k = cp;
		}

		return draw_native_object_model_at(
			s_monitor_bipd_handle,
			NONE,
			lighting_ptr,
			&pos,
			&fwd,
			&up,
			1,
			0,
			flags);
	}
}


/* ============================================================================
 * FORGE TAG OPTIONS: ADD TAGS (MAP TAG BROWSER), INSTALLED TAGS & 3D PREVIEW
 * ============================================================================ */

#define FORGE_BROWSER_NUM_CATS 5

struct browser_ce_tag_instance
{
	long group_tag;
	long parent_group_tags[2];
	long tag_index;
	char *name;
	void *base_address;
	unsigned long unused[2];
};

struct browser_ce_map_header
{
	struct browser_ce_tag_instance *instances;
	long scenario_tag_index;
	unsigned long checksum;
	long tag_count;
	long model_part_count;
	unsigned long model_data_offset;
	long model_part_count_again;
	unsigned long model_index_data_offset;
	unsigned long model_data_size;
	unsigned long signature;
};

static struct
{
	int is_open;
	int is_ce_map;
	int is_xbox_map;
	char map_path[512];
	char decomp_xbox_path[512];
	char map_name[64];
	struct browser_ce_map_header *ce_header;
	unsigned char *names_made;
	FILE *xbox_fp;
	uint8_t *xbox_tag_buf;
	uint32_t xbox_tag_sz;
	uint32_t xbox_tag_base;
	uint32_t xbox_inst_off;
	int32_t xbox_tag_count;
	int32_t *cat_tags[FORGE_BROWSER_NUM_CATS];
	int32_t *cat_sizes[FORGE_BROWSER_NUM_CATS];
	int cat_counts[FORGE_BROWSER_NUM_CATS];
} s_map_browser;

static struct
{
	char map_path[512];
	FILE *fp;
	uint8_t *tag_buf;
	uint32_t tag_sz;
	uint32_t tag_base;
	uint32_t inst_off;
	int32_t tag_count;
} s_preview_xbox_donor;

static void close_preview_xbox_donor(void)
{
	if (s_preview_xbox_donor.fp)
		fclose(s_preview_xbox_donor.fp);
	if (s_preview_xbox_donor.tag_buf)
		free(s_preview_xbox_donor.tag_buf);
	memset(&s_preview_xbox_donor, 0, sizeof(s_preview_xbox_donor));
}

static int open_preview_xbox_donor_for_tag(uint32_t req_group, char const *req_path)
{
	char stems[16][64];
	int stem_count, s, d;
	static char const *const map_dirs[] = {
		"maps",
		"custom_maps",
		"FORGE CODE/GAME MAPS/XBOX MAPS",
		"../FORGE CODE/GAME MAPS/XBOX MAPS"
	};

	if (s_preview_xbox_donor.fp && s_preview_xbox_donor.tag_buf)
	{
		int32_t i;
		char const *lookup = (req_group == 'mach' &&
			_stricmp(req_path, "levels\\a30\\devices\\beam emitter\\beam emitter red") == 0)
			? "levels\\a30\\devices\\beam emitter\\beam emitter"
			: req_path;
		for (i = 0; i < s_preview_xbox_donor.tag_count; i++)
		{
			uint32_t io = s_preview_xbox_donor.inst_off + (uint32_t)i * 32u;
			uint32_t grp = *(uint32_t const *)(s_preview_xbox_donor.tag_buf + io + 0x00);
			uint32_t name_addr = *(uint32_t const *)(s_preview_xbox_donor.tag_buf + io + 0x10);
			if (name_addr >= s_preview_xbox_donor.tag_base &&
				(name_addr - s_preview_xbox_donor.tag_base) < s_preview_xbox_donor.tag_sz)
			{
				char const *tname = (char const *)(s_preview_xbox_donor.tag_buf + (name_addr - s_preview_xbox_donor.tag_base));
				if (grp == req_group && _stricmp(tname, lookup) == 0)
					return 1;
			}
		}
	}

	stem_count = find_donor_map_stems_from_credits(req_group, req_path, stems, 16);
	for (s = 0; s < stem_count; s++)
	{
		for (d = 0; d < 4; d++)
		{
			char cand[512];
			char decomp[512];
			FILE *f;
			uint8_t hdr[2048];
			uint32_t tag_off, tag_sz, inst_addr;
			int32_t tag_count;
			uint8_t *tbuf;

			snprintf(cand, sizeof(cand), "%s/%s.map", map_dirs[d], stems[s]);
			f = fopen(cand, "rb");
			if (!f)
				continue;
			fclose(f);
			if (!is_xbox_version5_map_file(cand))
				continue;
			snprintf(decomp, sizeof(decomp), "OpenCE Forge/cached_tags/xbox_cache/%s.map", stems[s]);
#ifdef _WIN32
			CreateDirectoryA("OpenCE Forge/cached_tags/xbox_cache", NULL);
#endif
			if (!ensure_xbox_map_decompressed_cache(cand, decomp, sizeof(decomp)))
				continue;
			f = fopen(decomp, "rb");
			if (!f)
				continue;
			if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr) ||
				*(uint32_t const *)(hdr + 0x00) != 0x68656164u ||
				*(uint32_t const *)(hdr + 0x04) != 5u)
			{
				fclose(f);
				continue;
			}
			tag_off = *(uint32_t const *)(hdr + 0x10);
			tag_sz = *(uint32_t const *)(hdr + 0x14);
			if (tag_off < 2048u || tag_sz < 64u || tag_sz > 64u * 1024u * 1024u)
			{
				fclose(f);
				continue;
			}
			tbuf = (uint8_t *)malloc(tag_sz);
			if (!tbuf || fseek(f, (long)tag_off, SEEK_SET) != 0 || fread(tbuf, 1, tag_sz, f) != tag_sz)
			{
				free(tbuf);
				fclose(f);
				continue;
			}
			inst_addr = *(uint32_t const *)(tbuf + 0x00);
			tag_count = *(int32_t const *)(tbuf + 0x0C);
			if (tag_count <= 0 || tag_count > 16384 || 0x24u + (uint32_t)tag_count * 32u > tag_sz)
			{
				free(tbuf);
				fclose(f);
				continue;
			}
			close_preview_xbox_donor();
			snprintf(s_preview_xbox_donor.map_path, sizeof(s_preview_xbox_donor.map_path), "%s", decomp);
			s_preview_xbox_donor.fp = f;
			s_preview_xbox_donor.tag_buf = tbuf;
			s_preview_xbox_donor.tag_sz = tag_sz;
			s_preview_xbox_donor.tag_base = inst_addr - 0x24u;
			s_preview_xbox_donor.inst_off = 0x24u;
			s_preview_xbox_donor.tag_count = tag_count;
			return 1;
		}
	}
	return 0;
}

static int browser_group_matches_category(uint32_t grp, int category_tab)
{
	switch (category_tab)
	{
	case 0: /* SCENERY */
		return grp == 'scen';
	case 1: /* VEHICLES */
		return grp == 'vehi';
	case 2: /* DEVICES */
		return grp == 'mach' || grp == 'lifi' || grp == 'ctrl';
	case 3: /* WEAPONS */
		return grp == 'weap';
	case 4: /* EQUIPMENT */
		return grp == 'eqip';
	default:
		return 0;
	}
}

static char const *browser_group_type_label(uint32_t grp)
{
	switch (grp)
	{
	case 'scen': return "SCENERY (scen)";
	case 'vehi': return "VEHICLE (vehi)";
	case 'mach': return "DEVICE MACHINE (mach)";
	case 'lifi': return "LIGHT FIXTURE (lifi)";
	case 'ctrl': return "DEVICE CONTROL (ctrl)";
	case 'weap': return "WEAPON (weap)";
	case 'eqip': return "EQUIPMENT (eqip)";
	default:     return "OBJECT TAG";
	}
}

static void format_tag_byte_size(int32_t bytes, char *out, size_t out_sz)
{
	if (!out || out_sz == 0)
		return;
	if (bytes <= 0)
		snprintf(out, out_sz, "N/A");
	else if (bytes >= 1024 * 1024)
		snprintf(out, out_sz, "%.2f MB", (double)bytes / (1024.0 * 1024.0));
	else if (bytes >= 1024)
		snprintf(out, out_sz, "%.1f KB", (double)bytes / 1024.0);
	else
		snprintf(out, out_sz, "%d B", (int)bytes);
}

static char const *browser_tag_name_at(int32_t abs_idx)
{
	if (s_map_browser.is_ce_map && s_map_browser.ce_header)
	{
		if (abs_idx >= 0 && abs_idx < s_map_browser.ce_header->tag_count &&
			s_map_browser.ce_header->instances[abs_idx].name)
			return s_map_browser.ce_header->instances[abs_idx].name;
	}
	else if (s_map_browser.is_xbox_map && s_map_browser.xbox_tag_buf)
	{
		if (abs_idx >= 0 && abs_idx < s_map_browser.xbox_tag_count)
		{
			uint32_t io = s_map_browser.xbox_inst_off + (uint32_t)abs_idx * 32u;
			uint32_t name_addr = *(uint32_t const *)(s_map_browser.xbox_tag_buf + io + 0x10);
			if (name_addr >= s_map_browser.xbox_tag_base &&
				(name_addr - s_map_browser.xbox_tag_base) < s_map_browser.xbox_tag_sz)
				return (char const *)(s_map_browser.xbox_tag_buf + (name_addr - s_map_browser.xbox_tag_base));
		}
	}
	return "";
}

static uint32_t browser_tag_group_at(int32_t abs_idx)
{
	if (s_map_browser.is_ce_map && s_map_browser.ce_header)
	{
		if (abs_idx >= 0 && abs_idx < s_map_browser.ce_header->tag_count)
			return (uint32_t)s_map_browser.ce_header->instances[abs_idx].group_tag;
	}
	else if (s_map_browser.is_xbox_map && s_map_browser.xbox_tag_buf)
	{
		if (abs_idx >= 0 && abs_idx < s_map_browser.xbox_tag_count)
		{
			uint32_t io = s_map_browser.xbox_inst_off + (uint32_t)abs_idx * 32u;
			return *(uint32_t const *)(s_map_browser.xbox_tag_buf + io + 0x00);
		}
	}
	return 0;
}

static int browser_sort_cmp(void const *a, void const *b)
{
	int32_t ia = *(int32_t const *)a;
	int32_t ib = *(int32_t const *)b;
	return _stricmp(browser_tag_name_at(ia), browser_tag_name_at(ib));
}

void forge_ui_close_map_tag_browser(void)
{
	int c;
	for (c = 0; c < FORGE_BROWSER_NUM_CATS; c++)
	{
		if (s_map_browser.cat_tags[c])
			free(s_map_browser.cat_tags[c]);
		if (s_map_browser.cat_sizes[c])
			free(s_map_browser.cat_sizes[c]);
		s_map_browser.cat_tags[c] = NULL;
		s_map_browser.cat_sizes[c] = NULL;
		s_map_browser.cat_counts[c] = 0;
	}
	if (s_map_browser.is_ce_map)
		custom_edition_cache_bundle_close();
	tag_names_dispose();
	if (s_map_browser.names_made)
		free(s_map_browser.names_made);
	if (s_map_browser.xbox_fp)
		fclose(s_map_browser.xbox_fp);
	if (s_map_browser.xbox_tag_buf)
		free(s_map_browser.xbox_tag_buf);
	memset(&s_map_browser, 0, sizeof(s_map_browser));
	close_preview_xbox_donor();
}

int forge_ui_open_map_for_tag_browser(char const *map_path)
{
	char const *s1;
	char const *s2;
	char const *leaf;
	char *dot;
	struct browser_ce_map_header *ce_hdr;
	int c;

	forge_ui_close_map_tag_browser();
	if (!map_path || !map_path[0])
		return 0;

	snprintf(s_map_browser.map_path, sizeof(s_map_browser.map_path), "%s", map_path);
	s1 = strrchr(map_path, '/');
	s2 = strrchr(map_path, '\\');
	leaf = (s1 > s2) ? (s1 + 1) : (s2 ? (s2 + 1) : map_path);
	snprintf(s_map_browser.map_name, sizeof(s_map_browser.map_name), "%s", leaf);
	dot = strrchr(s_map_browser.map_name, '.');
	if (dot)
		*dot = '\0';

	/* 1. Try opening as a Custom Edition / PC cache map first */
	ce_hdr = is_xbox_version5_map_file(map_path) ? NULL : (struct browser_ce_map_header *)custom_edition_cache_bundle_open(map_path, NULL, 0);
	if (ce_hdr && ce_hdr->tag_count > 0 && ce_hdr->instances)
	{
		int32_t i;
		unsigned char *made = NULL;

		s_map_browser.is_open = 1;
		s_map_browser.is_ce_map = 1;
		s_map_browser.ce_header = ce_hdr;
		tag_names_restore(ce_hdr, s_map_browser.map_name, NULL);
		s_map_browser.names_made = NULL;

		for (c = 0; c < FORGE_BROWSER_NUM_CATS; c++)
		{
			int cnt = 0;
			for (i = 0; i < ce_hdr->tag_count; i++)
			{
				if (browser_group_matches_category((uint32_t)ce_hdr->instances[i].group_tag, c))
					cnt++;
			}
			if (cnt > 0)
			{
				int idx = 0;
				s_map_browser.cat_tags[c] = (int32_t *)malloc((size_t)cnt * sizeof(int32_t));
				s_map_browser.cat_sizes[c] = (int32_t *)malloc((size_t)cnt * sizeof(int32_t));
				if (s_map_browser.cat_tags[c] && s_map_browser.cat_sizes[c])
				{
					for (i = 0; i < ce_hdr->tag_count; i++)
					{
						if (browser_group_matches_category((uint32_t)ce_hdr->instances[i].group_tag, c))
						{
							s_map_browser.cat_tags[c][idx] = i;
							s_map_browser.cat_sizes[c][idx] = -1;
							idx++;
						}
					}
					s_map_browser.cat_counts[c] = idx;
					qsort(s_map_browser.cat_tags[c], (size_t)idx, sizeof(int32_t), browser_sort_cmp);
				}
			}
		}
		return 1;
	}
	if (ce_hdr)
		custom_edition_cache_bundle_close();

	/* 2. Open as a Stock Xbox v5 map (compressed or decompressed) */
	if (is_xbox_version5_map_file(map_path))
	{
		char decomp_path[512];
		FILE *fp;
		uint8_t hdr[2048];
		uint32_t tag_off, tag_sz, inst_addr;
		int32_t tag_count, i;
		uint8_t *tbuf;

#ifdef _WIN32
		CreateDirectoryA("OpenCE Forge", NULL);
		CreateDirectoryA("OpenCE Forge/cached_tags", NULL);
		CreateDirectoryA("OpenCE Forge/cached_tags/xbox_cache", NULL);
#endif
		snprintf(decomp_path, sizeof(decomp_path), "OpenCE Forge/cached_tags/xbox_cache/%s.map", s_map_browser.map_name);
		if (!ensure_xbox_map_decompressed_cache(map_path, decomp_path, sizeof(decomp_path)))
			return 0;

		fp = fopen(decomp_path, "rb");
		if (!fp)
			return 0;
		if (fread(hdr, 1, sizeof(hdr), fp) != sizeof(hdr) ||
			*(uint32_t const *)(hdr + 0x00) != 0x68656164u ||
			*(uint32_t const *)(hdr + 0x04) != 5u)
		{
			fclose(fp);
			return 0;
		}
		tag_off = *(uint32_t const *)(hdr + 0x10);
		tag_sz = *(uint32_t const *)(hdr + 0x14);
		if (tag_off < 2048u || tag_sz < 64u || tag_sz > 64u * 1024u * 1024u)
		{
			fclose(fp);
			return 0;
		}
		tbuf = (uint8_t *)malloc(tag_sz);
		if (!tbuf || fseek(fp, (long)tag_off, SEEK_SET) != 0 || fread(tbuf, 1, tag_sz, fp) != tag_sz)
		{
			free(tbuf);
			fclose(fp);
			return 0;
		}
		inst_addr = *(uint32_t const *)(tbuf + 0x00);
		tag_count = *(int32_t const *)(tbuf + 0x0C);
		if (tag_count <= 0 || tag_count > 16384 || 0x24u + (uint32_t)tag_count * 32u > tag_sz)
		{
			free(tbuf);
			fclose(fp);
			return 0;
		}

		s_map_browser.is_open = 1;
		s_map_browser.is_xbox_map = 1;
		snprintf(s_map_browser.decomp_xbox_path, sizeof(s_map_browser.decomp_xbox_path), "%s", decomp_path);
		s_map_browser.xbox_fp = fp;
		s_map_browser.xbox_tag_buf = tbuf;
		s_map_browser.xbox_tag_sz = tag_sz;
		s_map_browser.xbox_tag_base = inst_addr - 0x24u;
		s_map_browser.xbox_inst_off = 0x24u;
		s_map_browser.xbox_tag_count = tag_count;

		for (c = 0; c < FORGE_BROWSER_NUM_CATS; c++)
		{
			int cnt = 0;
			for (i = 0; i < tag_count; i++)
			{
				uint32_t io = s_map_browser.xbox_inst_off + (uint32_t)i * 32u;
				uint32_t grp = *(uint32_t const *)(tbuf + io + 0x00);
				if (browser_group_matches_category(grp, c))
					cnt++;
			}
			if (cnt > 0)
			{
				int idx = 0;
				s_map_browser.cat_tags[c] = (int32_t *)malloc((size_t)cnt * sizeof(int32_t));
				s_map_browser.cat_sizes[c] = (int32_t *)malloc((size_t)cnt * sizeof(int32_t));
				if (s_map_browser.cat_tags[c] && s_map_browser.cat_sizes[c])
				{
					for (i = 0; i < tag_count; i++)
					{
						uint32_t io = s_map_browser.xbox_inst_off + (uint32_t)i * 32u;
						uint32_t grp = *(uint32_t const *)(tbuf + io + 0x00);
						if (browser_group_matches_category(grp, c))
						{
							s_map_browser.cat_tags[c][idx] = i;
							s_map_browser.cat_sizes[c][idx] = -1;
							idx++;
						}
					}
					s_map_browser.cat_counts[c] = idx;
					qsort(s_map_browser.cat_tags[c], (size_t)idx, sizeof(int32_t), browser_sort_cmp);
				}
			}
		}
		return 1;
	}

	return 0;
}

int forge_ui_map_tag_count(int category_tab)
{
	if (!s_map_browser.is_open || category_tab < 0 || category_tab >= FORGE_BROWSER_NUM_CATS)
		return 0;
	return s_map_browser.cat_counts[category_tab];
}

static boolean bundle_count_size_cb(void *context, void const *bytes, unsigned long size)
{
	unsigned long *total = (unsigned long *)context;
	(void)bytes;
	*total += size;
	return TRUE;
}

static int32_t compute_xbox_tag_closure_bytes(
	uint8_t const *tag_buf,
	uint32_t tag_sz,
	uint32_t tag_base,
	uint32_t inst_off,
	int32_t tag_count,
	int32_t root_abs_idx)
{
	struct xbox_runtime_closure cl;
	int32_t i;
	uint32_t total = 2048u;

	if (!tag_buf || root_abs_idx < 0 || root_abs_idx >= tag_count)
		return 0;
	memset(&cl, 0, sizeof(cl));
	cl.tag_buf = tag_buf;
	cl.tag_sz = tag_sz;
	cl.tag_base = tag_base;
	cl.inst_off = inst_off;
	cl.tag_count = tag_count;
	cl.visited = (uint8_t *)calloc((size_t)tag_count, 1);
	cl.order = (int32_t *)calloc((size_t)tag_count, sizeof(int32_t));
	cl.runtime_handles = (long *)malloc((size_t)tag_count * sizeof(long));
	if (!cl.visited || !cl.order || !cl.runtime_handles)
	{
		free(cl.visited);
		free(cl.order);
		free(cl.runtime_handles);
		return 0;
	}
	for (i = 0; i < tag_count; i++)
		cl.runtime_handles[i] = NONE;
	xbox_visit_tag_index(&cl, root_abs_idx);

	for (i = 0; i < cl.order_count; i++)
	{
		int32_t abs_idx = cl.order[i];
		uint32_t io = inst_off + (uint32_t)abs_idx * 32u;
		uint32_t grp = *(uint32_t const *)(tag_buf + io + 0x00);
		uint32_t data_addr = *(uint32_t const *)(tag_buf + io + 0x14);
		struct tag_schema_group const *schema = forge_runtime_schema_group_get(grp);

		total += 64u;
		if (schema && schema->definition && schema->definition->size > 0)
			total += (uint32_t)schema->definition->size * 2u;
		if (grp == 'bitm' && data_addr >= tag_base && (data_addr - tag_base) + 0x60u <= tag_sz)
		{
			uint32_t b_off = data_addr - tag_base;
			int32_t b_cnt = *(int32_t const *)(tag_buf + b_off + 0x54u);
			uint32_t b_addr = *(uint32_t const *)(tag_buf + b_off + 0x58u);
			int b_i;
			if (b_cnt > 0 && b_cnt <= 256 && b_addr >= tag_base &&
				(b_addr - tag_base) + (uint32_t)b_cnt * 48u <= tag_sz)
			{
				for (b_i = 0; b_i < b_cnt; b_i++)
				{
					int32_t p_sz = *(int32_t const *)(tag_buf + (b_addr - tag_base) + (uint32_t)b_i * 48u + 28u);
					if (p_sz > 0 && p_sz <= 16 * 1024 * 1024)
						total += (uint32_t)p_sz;
				}
			}
		}
		else if (grp == 'mode' && data_addr >= tag_base && (data_addr - tag_base) + 0xE8u <= tag_sz)
		{
			uint32_t m_off = data_addr - tag_base;
			int32_t g_cnt = *(int32_t const *)(tag_buf + m_off + 0xD0u);
			uint32_t g_addr = *(uint32_t const *)(tag_buf + m_off + 0xD4u);
			int g_i;
			if (g_cnt > 0 && g_cnt <= 64 && g_addr >= tag_base &&
				(g_addr - tag_base) + (uint32_t)g_cnt * 48u <= tag_sz)
			{
				for (g_i = 0; g_i < g_cnt; g_i++)
				{
					uint32_t go = (g_addr - tag_base) + (uint32_t)g_i * 48u;
					int32_t p_cnt = *(int32_t const *)(tag_buf + go + 36u);
					uint32_t p_addr = *(uint32_t const *)(tag_buf + go + 40u);
					int p_i;
					if (p_cnt > 0 && p_cnt <= 128 && p_addr >= tag_base &&
						(p_addr - tag_base) + (uint32_t)p_cnt * 104u <= tag_sz)
					{
						for (p_i = 0; p_i < p_cnt; p_i++)
						{
							uint32_t po = (p_addr - tag_base) + (uint32_t)p_i * 104u;
							int32_t t_cnt = *(int32_t const *)(tag_buf + po + 72u);
							int32_t v_cnt = *(int32_t const *)(tag_buf + po + 88u);
							if (v_cnt > 0 && v_cnt <= 65536)
								total += (uint32_t)v_cnt * 32u;
							if (t_cnt > 0 && t_cnt <= 65536)
								total += ((uint32_t)t_cnt + 2u) * 2u;
						}
					}
				}
			}
		}
	}

	free(cl.visited);
	free(cl.order);
	free(cl.runtime_handles);
	return (int32_t)total;
}

int forge_ui_map_tag_get_info(
	int category_tab,
	int list_index,
	char *out_short_name,
	size_t short_sz,
	char *out_full_path,
	size_t full_sz,
	char *out_type_str,
	size_t type_sz,
	char *out_size_str,
	size_t size_sz)
{
	int32_t abs_idx;
	char const *full_path;
	char const *leaf;
	char const *p;
	uint32_t grp;
	int32_t byte_size;

	if (!s_map_browser.is_open || category_tab < 0 || category_tab >= FORGE_BROWSER_NUM_CATS ||
		list_index < 0 || list_index >= s_map_browser.cat_counts[category_tab])
		return 0;

	abs_idx = s_map_browser.cat_tags[category_tab][list_index];
	full_path = browser_tag_name_at(abs_idx);
	grp = browser_tag_group_at(abs_idx);
	leaf = full_path;
	for (p = full_path; *p; p++)
	{
		if (*p == '\\' || *p == '/')
			leaf = p + 1;
	}

	if (out_short_name && short_sz > 0)
		snprintf(out_short_name, short_sz, "%s", leaf);
	if (out_full_path && full_sz > 0)
		snprintf(out_full_path, full_sz, "%s", full_path);
	if (out_type_str && type_sz > 0)
		snprintf(out_type_str, type_sz, "%s", browser_group_type_label(grp));

	byte_size = s_map_browser.cat_sizes[category_tab][list_index];
	if (byte_size < 0 && out_size_str && size_sz > 0)
	{
		if (s_map_browser.is_ce_map)
		{
			struct tag_bundle_source source;
			struct tag_bundle_report report;
			unsigned long counted = 0;
			custom_edition_cache_bundle_source(&source);
			memset(&report, 0, sizeof(report));
			if (tag_bundle_write(&source, abs_idx, bundle_count_size_cb, &counted, &report) == _tag_bundle_status_ok)
				byte_size = (int32_t)report.size;
			else
				byte_size = 0;
		}
		else if (s_map_browser.is_xbox_map)
		{
			byte_size = compute_xbox_tag_closure_bytes(
				s_map_browser.xbox_tag_buf,
				s_map_browser.xbox_tag_sz,
				s_map_browser.xbox_tag_base,
				s_map_browser.xbox_inst_off,
				s_map_browser.xbox_tag_count,
				abs_idx);
		}
		s_map_browser.cat_sizes[category_tab][list_index] = byte_size;
	}
	if (out_size_str && size_sz > 0)
		format_tag_byte_size(byte_size, out_size_str, size_sz);
	return 1;
}

struct forge_grow_buf
{
	byte *bytes;
	unsigned long size;
	unsigned long capacity;
};

static boolean forge_grow_buf_write(void *context, void const *bytes, unsigned long size)
{
	struct forge_grow_buf *buffer = (struct forge_grow_buf *)context;
	if (size > 0x7FFFFFFFUL - buffer->size)
		return FALSE;
	if (buffer->size + size > buffer->capacity)
	{
		unsigned long cap = buffer->capacity ? buffer->capacity : 0x100000UL;
		byte *grown;
		while (cap < buffer->size + size)
		{
			if (cap > 0x3FFFFFFFUL)
				return FALSE;
			cap *= 2;
		}
		grown = buffer->bytes ? (byte *)realloc(buffer->bytes, cap) : (byte *)malloc(cap);
		if (!grown)
			return FALSE;
		buffer->bytes = grown;
		buffer->capacity = cap;
	}
	memcpy(buffer->bytes + buffer->size, bytes, size);
	buffer->size += size;
	return TRUE;
}

static void xbox_mark_schema_spans(
	struct xbox_runtime_closure const *cl,
	uint8_t *used_mask,
	struct tag_schema_definition const *def,
	uint32_t base_off,
	int32_t elem_count,
	int depth)
{
	int32_t elem_idx;
	struct tag_schema_field const *field;
	uint32_t elem_size;
	uint64_t span_bytes;

	if (!def || !def->fields || elem_count <= 0 || def->size <= 0 || depth > 12)
		return;
	elem_size = (uint32_t)def->size;
	span_bytes = (uint64_t)elem_count * elem_size;
	if ((uint64_t)base_off + span_bytes > cl->tag_sz)
		return;

	memset(used_mask + base_off, 1, (size_t)span_bytes);

	for (elem_idx = 0; elem_idx < elem_count; elem_idx++)
	{
		uint32_t elem_off = base_off + (uint32_t)elem_idx * elem_size;
		for (field = def->fields; field->type != _tag_schema_terminator; field++)
		{
			int16_t k;
			int16_t count = (field->count > 0) ? field->count : 1;
			uint32_t stride = (field->size > 0) ? (uint32_t)field->size : 0;

			for (k = 0; k < count; k++)
			{
				uint32_t f_off = elem_off + (uint32_t)field->offset + (uint32_t)k * stride;
				if (field->type == _tag_schema_struct)
				{
					struct tag_schema_definition const *sub_def =
						(struct tag_schema_definition const *)field->definition;
					if (sub_def && f_off + (uint32_t)sub_def->size <= cl->tag_sz)
						xbox_mark_schema_spans(cl, used_mask, sub_def, f_off, 1, depth + 1);
				}
				else if (field->type == _tag_schema_block)
				{
					struct tag_schema_definition const *child_def =
						(struct tag_schema_definition const *)field->definition;
					if (child_def && f_off + 12u <= cl->tag_sz)
					{
						int32_t child_cnt = *(int32_t const *)(cl->tag_buf + f_off + 0);
						uint32_t child_addr = *(uint32_t const *)(cl->tag_buf + f_off + 4);
						if (child_cnt > 0 && child_cnt <= 65536 &&
							child_addr >= cl->tag_base &&
							(uint64_t)(child_addr - cl->tag_base) + (uint64_t)child_cnt * (uint32_t)child_def->size <= cl->tag_sz)
						{
							xbox_mark_schema_spans(
								cl,
								used_mask,
								child_def,
								child_addr - cl->tag_base,
								child_cnt,
								depth + 1);
						}
					}
				}
				else if (field->type == _tag_schema_data)
				{
					if (f_off + 20u <= cl->tag_sz)
					{
						int32_t d_sz = *(int32_t const *)(cl->tag_buf + f_off + 0);
						uint32_t d_addr = *(uint32_t const *)(cl->tag_buf + f_off + 12);
						if (d_sz > 0 && d_sz <= 16 * 1024 * 1024 &&
							d_addr >= cl->tag_base &&
							(uint64_t)(d_addr - cl->tag_base) + (uint64_t)d_sz <= cl->tag_sz)
						{
							memset(used_mask + (d_addr - cl->tag_base), 1, (size_t)d_sz);
						}
					}
				}
				else if (field->type == _tag_schema_reference)
				{
					if (f_off + 16u <= cl->tag_sz)
					{
						uint32_t rname_addr = *(uint32_t const *)(cl->tag_buf + f_off + 4);
						int32_t rname_len = *(int32_t const *)(cl->tag_buf + f_off + 8);
						if (rname_len > 0 && rname_len < 256 &&
							rname_addr >= cl->tag_base &&
							(rname_addr - cl->tag_base) + (uint32_t)rname_len + 1u <= cl->tag_sz)
						{
							memset(used_mask + (rname_addr - cl->tag_base), 1, (size_t)rname_len + 1u);
						}
					}
				}
			}
		}
	}
}

static void upsert_custom_catalog_manifest_line(
	uint32_t grp,
	uint8_t cat_id,
	char const *tag_path,
	char const *custom_name)
{
	FILE *in_fp;
	char (*lines)[512];
	int line_count = 0;
	int updated = 0;
	int i;
	char grp_str[5];
	FILE *out_fp;

	lines = (char (*)[512])malloc(512u * sizeof(*lines));
	if (!lines)
		return;

	grp_str[0] = (char)((grp >> 24) & 0xFF);
	grp_str[1] = (char)((grp >> 16) & 0xFF);
	grp_str[2] = (char)((grp >> 8) & 0xFF);
	grp_str[3] = (char)(grp & 0xFF);
	grp_str[4] = '\0';

	in_fp = fopen("OpenCE Forge/cached_tags/custom_catalog.txt", "r");
	if (in_fp)
	{
		char buf[512];
		while (line_count < 510 && fgets(buf, sizeof(buf), in_fp))
		{
			char l_grp[16], l_path[256];
			int l_cat = 0, l_src = 0;
			char *nl = strchr(buf, '\r');
			if (nl) *nl = '\0';
			nl = strchr(buf, '\n');
			if (nl) *nl = '\0';
			if (sscanf(buf, "%4s|%d|%d|%255[^|\n]", l_grp, &l_cat, &l_src, l_path) == 4 &&
				_stricmp(l_grp, grp_str) == 0 && _stricmp(l_path, tag_path) == 0)
			{
				snprintf(lines[line_count++], sizeof(lines[0]), "%s|%d|2|%s|%s",
					grp_str, (int)cat_id, tag_path, custom_name);
				updated = 1;
			}
			else if (buf[0])
			{
				snprintf(lines[line_count++], sizeof(lines[0]), "%s", buf);
			}
		}
		fclose(in_fp);
	}
	if (!updated && line_count < 512)
	{
		snprintf(lines[line_count++], sizeof(lines[0]), "%s|%d|2|%s|%s",
			grp_str, (int)cat_id, tag_path, custom_name);
	}
#ifdef _WIN32
	CreateDirectoryA("OpenCE Forge", NULL);
	CreateDirectoryA("OpenCE Forge/cached_tags", NULL);
#endif
	out_fp = fopen("OpenCE Forge/cached_tags/custom_catalog.txt", "wb");
	if (out_fp)
	{
		for (i = 0; i < line_count; i++)
			fprintf(out_fp, "%s\n", lines[i]);
		fclose(out_fp);
	}
	free(lines);
}

static int extract_compressed_xbox_map_tag(
	int32_t root_abs_idx,
	uint32_t grp,
	char const *tag_path,
	char const *clean_name)
{
	struct xbox_runtime_closure cl;
	uint8_t *used_mask = NULL;
	uint8_t *masked_tag_buf = NULL;
	struct forge_grow_buf ext_buf = { NULL, 0, 0 };
	uint32_t uncomp_size, new_tag_off;
	uint8_t *uncomp_img = NULL;
	unsigned long comp_bound, comp_len;
	uint8_t *comp_img = NULL;
	struct fxtg_bundle_header fhdr;
	char safe_file[128];
	char fxtg_path[512];
	FILE *out_fp;
	uint8_t cat_id;
	int32_t i;
	size_t c_idx, w_idx = 0;

	if (!s_map_browser.xbox_fp || !s_map_browser.xbox_tag_buf ||
		root_abs_idx < 0 || root_abs_idx >= s_map_browser.xbox_tag_count)
		return 0;

	memset(&cl, 0, sizeof(cl));
	cl.tag_buf = s_map_browser.xbox_tag_buf;
	cl.tag_sz = s_map_browser.xbox_tag_sz;
	cl.tag_base = s_map_browser.xbox_tag_base;
	cl.inst_off = s_map_browser.xbox_inst_off;
	cl.tag_count = s_map_browser.xbox_tag_count;
	cl.visited = (uint8_t *)calloc((size_t)cl.tag_count, 1);
	cl.order = (int32_t *)calloc((size_t)cl.tag_count, sizeof(int32_t));
	cl.runtime_handles = (long *)malloc((size_t)cl.tag_count * sizeof(long));
	used_mask = (uint8_t *)calloc(1, cl.tag_sz);
	masked_tag_buf = (uint8_t *)calloc(1, cl.tag_sz);
	if (!cl.visited || !cl.order || !cl.runtime_handles || !used_mask || !masked_tag_buf)
	{
		free(cl.visited);
		free(cl.order);
		free(cl.runtime_handles);
		free(used_mask);
		free(masked_tag_buf);
		return 0;
	}
	for (i = 0; i < cl.tag_count; i++)
		cl.runtime_handles[i] = NONE;

	/* Visit ALL dependencies of root_abs_idx */
	xbox_visit_tag_index(&cl, root_abs_idx);

	/* Mark the tag table header (0x24 bytes) */
	memset(used_mask, 1, 0x24u);

	for (i = 0; i < cl.order_count; i++)
	{
		int32_t abs_idx = cl.order[i];
		uint32_t io = cl.inst_off + (uint32_t)abs_idx * 32u;
		uint32_t t_grp = *(uint32_t const *)(cl.tag_buf + io + 0x00);
		uint32_t name_addr = *(uint32_t const *)(cl.tag_buf + io + 0x10);
		uint32_t data_addr = *(uint32_t const *)(cl.tag_buf + io + 0x14);
		struct tag_schema_group const *schema = forge_runtime_schema_group_get(t_grp);

		memset(used_mask + io, 1, 32u);
		if (name_addr >= cl.tag_base && (name_addr - cl.tag_base) < cl.tag_sz)
		{
			char const *tn = (char const *)(cl.tag_buf + (name_addr - cl.tag_base));
			size_t tn_len = strlen(tn) + 1u;
			if ((name_addr - cl.tag_base) + tn_len <= cl.tag_sz)
				memset(used_mask + (name_addr - cl.tag_base), 1, tn_len);
		}
		if (schema && schema->definition && data_addr >= cl.tag_base &&
			(data_addr - cl.tag_base) + (uint32_t)schema->definition->size <= cl.tag_sz)
		{
			xbox_mark_schema_spans(&cl, used_mask, schema->definition, data_addr - cl.tag_base, 1, 1);
		}
		if (t_grp == 'mode' && data_addr >= cl.tag_base && (data_addr - cl.tag_base) + 0xE8u <= cl.tag_sz)
		{
			uint32_t m_off = data_addr - cl.tag_base;
			int32_t g_cnt = *(int32_t const *)(cl.tag_buf + m_off + 0xD0u);
			uint32_t g_addr = *(uint32_t const *)(cl.tag_buf + m_off + 0xD4u);
			int g_i;
			if (g_cnt > 0 && g_cnt <= 64 && g_addr >= cl.tag_base &&
				(g_addr - cl.tag_base) + (uint32_t)g_cnt * 48u <= cl.tag_sz)
			{
				for (g_i = 0; g_i < g_cnt; g_i++)
				{
					uint32_t go = (g_addr - cl.tag_base) + (uint32_t)g_i * 48u;
					int32_t p_cnt = *(int32_t const *)(cl.tag_buf + go + 36u);
					uint32_t p_addr = *(uint32_t const *)(cl.tag_buf + go + 40u);
					int p_i;
					if (p_cnt > 0 && p_cnt <= 128 && p_addr >= cl.tag_base &&
						(p_addr - cl.tag_base) + (uint32_t)p_cnt * 104u <= cl.tag_sz)
					{
						for (p_i = 0; p_i < p_cnt; p_i++)
						{
							uint32_t po = (p_addr - cl.tag_base) + (uint32_t)p_i * 104u;
							int16_t tri_type = *(int16_t const *)(cl.tag_buf + po + 68u);
							int32_t tri_cnt = *(int32_t const *)(cl.tag_buf + po + 72u);
							uint32_t tb_hw = *(uint32_t const *)(cl.tag_buf + po + 80u);
							int32_t vtx_cnt = *(int32_t const *)(cl.tag_buf + po + 88u);
							uint32_t vb_hw = *(uint32_t const *)(cl.tag_buf + po + 100u);
							if (tri_cnt > 0 && vtx_cnt > 0 &&
								tb_hw >= cl.tag_base && (tb_hw - cl.tag_base) + 12u <= cl.tag_sz &&
								vb_hw >= cl.tag_base && (vb_hw - cl.tag_base) + 12u <= cl.tag_sz)
							{
								uint32_t ib_addr = *(uint32_t const *)(cl.tag_buf + (tb_hw - cl.tag_base) + 4u);
								uint32_t vb_addr = *(uint32_t const *)(cl.tag_buf + (vb_hw - cl.tag_base) + 4u);
								uint32_t comp_sz = (uint32_t)vtx_cnt * 32u;
								uint32_t idx_sz = (tri_type == _triangle_buffer_type_precompiled_strip)
									? ((uint32_t)tri_cnt + 2u) * 2u
									: (uint32_t)tri_cnt * 6u;
								memset(used_mask + (tb_hw - cl.tag_base), 1, 12u);
								memset(used_mask + (vb_hw - cl.tag_base), 1, 12u);
								if (ib_addr >= cl.tag_base && (ib_addr - cl.tag_base) + idx_sz <= cl.tag_sz)
									memset(used_mask + (ib_addr - cl.tag_base), 1, idx_sz);
								if (vb_addr >= cl.tag_base && (vb_addr - cl.tag_base) + comp_sz <= cl.tag_sz)
									memset(used_mask + (vb_addr - cl.tag_base), 1, comp_sz);
							}
						}
					}
				}
			}
		}
	}

	/* Copy only marked bytes into masked_tag_buf (all other bytes remain 0 for maximum zlib compression!) */
	for (c_idx = 0; c_idx < cl.tag_sz; c_idx++)
	{
		if (used_mask[c_idx])
			masked_tag_buf[c_idx] = cl.tag_buf[c_idx];
	}

	/* Pack external 'bitm' pixels and 'snd!' samples contiguously starting at file offset 2048 */
	for (i = 0; i < cl.order_count; i++)
	{
		int32_t abs_idx = cl.order[i];
		uint32_t io = cl.inst_off + (uint32_t)abs_idx * 32u;
		uint32_t t_grp = *(uint32_t const *)(masked_tag_buf + io + 0x00);
		uint32_t data_addr = *(uint32_t const *)(masked_tag_buf + io + 0x14);

		if (t_grp == 'bitm' && data_addr >= cl.tag_base && (data_addr - cl.tag_base) + 0x60u <= cl.tag_sz)
		{
			uint32_t b_off = data_addr - cl.tag_base;
			int32_t b_cnt = *(int32_t const *)(masked_tag_buf + b_off + 0x54u);
			uint32_t b_addr = *(uint32_t const *)(masked_tag_buf + b_off + 0x58u);
			int b_i;
			if (b_cnt > 0 && b_cnt <= 256 && b_addr >= cl.tag_base &&
				(b_addr - cl.tag_base) + (uint32_t)b_cnt * 48u <= cl.tag_sz)
			{
				for (b_i = 0; b_i < b_cnt; b_i++)
				{
					uint8_t *bmp_raw = masked_tag_buf + (b_addr - cl.tag_base) + (uint32_t)b_i * 48u;
					struct bitmap_data tmp_bmp;
					long req_bytes;
					uint32_t orig_off, p_sz, read_bytes;
					uint8_t *pix_tmp;

					memcpy(&tmp_bmp, bmp_raw, sizeof(tmp_bmp));
					orig_off = (uint32_t)tmp_bmp.pixels_offset;
					p_sz = (tmp_bmp.pixels_size > 0) ? (uint32_t)tmp_bmp.pixels_size : 0u;
					if (tmp_bmp.width <= 0 || tmp_bmp.height <= 0 || tmp_bmp.depth <= 0 || orig_off < 2048u)
						continue;
					req_bytes = rasterizer_xbox_bitmap_get_pixel_data_size(&tmp_bmp);
					if (req_bytes <= 0)
						continue;
					read_bytes = (p_sz > 0 && p_sz < (uint32_t)req_bytes) ? p_sz : (uint32_t)req_bytes;
					pix_tmp = (uint8_t *)malloc(read_bytes);
					if (pix_tmp &&
						fseek(s_map_browser.xbox_fp, (long)orig_off, SEEK_SET) == 0 &&
						fread(pix_tmp, 1, read_bytes, s_map_browser.xbox_fp) == read_bytes)
					{
						uint32_t new_off = 2048u + (uint32_t)ext_buf.size;
						if (forge_grow_buf_write(&ext_buf, pix_tmp, read_bytes))
							*(int32_t *)(bmp_raw + 24u) = (int32_t)new_off;
					}
					free(pix_tmp);
				}
			}
		}
		else if (t_grp == 'snd!' && data_addr >= cl.tag_base && (data_addr - cl.tag_base) + 0xA4u <= cl.tag_sz)
		{
			uint32_t s_off = data_addr - cl.tag_base;
			int32_t pr_cnt = *(int32_t const *)(masked_tag_buf + s_off + 0x98u);
			uint32_t pr_addr = *(uint32_t const *)(masked_tag_buf + s_off + 0x9Cu);
			int pr_i, pm_i;
			if (pr_cnt > 0 && pr_cnt <= 64 && pr_addr >= cl.tag_base &&
				(pr_addr - cl.tag_base) + (uint32_t)pr_cnt * 72u <= cl.tag_sz)
			{
				for (pr_i = 0; pr_i < pr_cnt; pr_i++)
				{
					uint32_t pro = (pr_addr - cl.tag_base) + (uint32_t)pr_i * 72u;
					int32_t pm_cnt = *(int32_t const *)(masked_tag_buf + pro + 60u);
					uint32_t pm_addr = *(uint32_t const *)(masked_tag_buf + pro + 64u);
					if (pm_cnt > 0 && pm_cnt <= 256 && pm_addr >= cl.tag_base &&
						(pm_addr - cl.tag_base) + (uint32_t)pm_cnt * 124u <= cl.tag_sz)
					{
						for (pm_i = 0; pm_i < pm_cnt; pm_i++)
						{
							uint8_t *perm_raw = masked_tag_buf + (pm_addr - cl.tag_base) + (uint32_t)pm_i * 124u;
							int32_t s_sz = *(int32_t const *)(perm_raw + 64u);
							uint32_t s_foff = *(uint32_t const *)(perm_raw + 72u);
							if (s_sz > 0 && s_sz <= MAXIMUM_SOUND_DATA_SIZE && s_foff >= 2048u)
							{
								uint8_t *stmp = (uint8_t *)malloc((size_t)s_sz);
								if (stmp &&
									fseek(s_map_browser.xbox_fp, (long)s_foff, SEEK_SET) == 0 &&
									fread(stmp, 1, (size_t)s_sz, s_map_browser.xbox_fp) == (size_t)s_sz)
								{
									uint32_t new_off = 2048u + (uint32_t)ext_buf.size;
									if (forge_grow_buf_write(&ext_buf, stmp, (unsigned long)s_sz))
										*(uint32_t *)(perm_raw + 72u) = new_off;
								}
								free(stmp);
							}
						}
					}
				}
			}
		}
	}

	new_tag_off = 2048u + (uint32_t)((ext_buf.size + 31u) & ~31u);
	uncomp_size = new_tag_off + cl.tag_sz;
	uncomp_img = (uint8_t *)calloc(1, uncomp_size);
	if (!uncomp_img)
	{
		free(ext_buf.bytes);
		free(cl.visited);
		free(cl.order);
		free(cl.runtime_handles);
		free(used_mask);
		free(masked_tag_buf);
		return 0;
	}

	*(uint32_t *)(uncomp_img + 0x00) = 0x68656164u; /* 'head' */
	*(uint32_t *)(uncomp_img + 0x04) = 5u;          /* Xbox v5 */
	*(uint32_t *)(uncomp_img + 0x08) = uncomp_size;
	*(uint32_t *)(uncomp_img + 0x10) = new_tag_off;
	*(uint32_t *)(uncomp_img + 0x14) = cl.tag_sz;
	*(uint32_t *)(uncomp_img + 0x7FC) = 0x666F6F74u; /* 'foot' */
	if (ext_buf.bytes && ext_buf.size > 0)
		memcpy(uncomp_img + 2048u, ext_buf.bytes, ext_buf.size);
	memcpy(uncomp_img + new_tag_off, masked_tag_buf, cl.tag_sz);

	free(ext_buf.bytes);
	free(cl.visited);
	free(cl.order);
	free(cl.runtime_handles);
	free(used_mask);
	free(masked_tag_buf);

	comp_bound = uncomp_size + (uncomp_size >> 10) + 256u;
	comp_img = (uint8_t *)malloc(comp_bound);
	if (!comp_img)
	{
		free(uncomp_img);
		return 0;
	}
	comp_len = comp_bound;
	{
#undef compress2
		extern int compress2(Bytef *dest, uLongf *destLen, const Bytef *source, uLong sourceLen, int level);
		if (compress2(comp_img, &comp_len, uncomp_img, uncomp_size, 6) != Z_OK)
		{
			free(comp_img);
			free(uncomp_img);
			return 0;
		}
	}
	free(uncomp_img);

	for (c_idx = 0; tag_path[c_idx] && w_idx + 1 < sizeof(safe_file); c_idx++)
	{
		char ch = tag_path[c_idx];
		if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
			(ch >= '0' && ch <= '9') || ch == '_' || ch == '-')
			safe_file[w_idx++] = ch;
		else
			safe_file[w_idx++] = '_';
	}
	safe_file[w_idx] = '\0';

#ifdef _WIN32
	CreateDirectoryA("OpenCE Forge", NULL);
	CreateDirectoryA("OpenCE Forge/custom_tags", NULL);
#endif
	snprintf(fxtg_path, sizeof(fxtg_path), "OpenCE Forge/custom_tags/%s_%08X.fxtg", safe_file, (unsigned int)grp);
	memset(&fhdr, 0, sizeof(fhdr));
	fhdr.magic = 0x31545846u; /* 'FXT1' */
	fhdr.group_tag = grp;
	fhdr.uncompressed_size = uncomp_size;
	fhdr.compressed_size = (uint32_t)comp_len;
	snprintf(fhdr.tag_path, sizeof(fhdr.tag_path), "%s", tag_path);
	snprintf(fhdr.custom_name, sizeof(fhdr.custom_name), "%s", clean_name);
	snprintf(fhdr.source_map, sizeof(fhdr.source_map), "%s", s_map_browser.map_name);

	out_fp = fopen(fxtg_path, "wb");
	if (!out_fp)
	{
		free(comp_img);
		return 0;
	}
	fwrite(&fhdr, 1, sizeof(fhdr), out_fp);
	fwrite(comp_img, 1, comp_len, out_fp);
	fclose(out_fp);
	free(comp_img);

	if (grp == 'weap') cat_id = _forge_cat_weapons;
	else if (grp == 'eqip') cat_id = _forge_cat_equipment;
	else if (grp == 'vehi') cat_id = _forge_cat_vehicles;
	else if (is_forge_door_tag(grp, tag_path)) cat_id = _forge_cat_barriers_devices;
	else if (is_forge_light_beacon_emitter_tag(grp, tag_path)) cat_id = _forge_cat_lights;
	else cat_id = _forge_cat_scenery;

	upsert_custom_catalog_manifest_line(grp, cat_id, tag_path, clean_name);
	return 1;
}

int forge_ui_extract_selected_map_tag(int category_tab, int list_index, char const *custom_name)
{
	int32_t abs_idx;
	uint32_t grp;
	char tag_path[256];
	char clean_name[64];
	char const *name_src;
	char const *leaf;
	char const *p;

	if (!s_map_browser.is_open || category_tab < 0 || category_tab >= FORGE_BROWSER_NUM_CATS ||
		list_index < 0 || list_index >= s_map_browser.cat_counts[category_tab])
		return 0;

	abs_idx = s_map_browser.cat_tags[category_tab][list_index];
	grp = browser_tag_group_at(abs_idx);
	snprintf(tag_path, sizeof(tag_path), "%s", browser_tag_name_at(abs_idx));
	if (!tag_path[0])
		return 0;

	leaf = tag_path;
	for (p = tag_path; *p; p++)
	{
		if (*p == '\\' || *p == '/')
			leaf = p + 1;
	}
	name_src = (custom_name && custom_name[0]) ? custom_name : leaf;
	while (*name_src == ' ')
		name_src++;
	if (_strnicmp(name_src, "[c]", 3) == 0)
	{
		name_src += 3;
		while (*name_src == ' ')
			name_src++;
	}
	snprintf(clean_name, sizeof(clean_name), "%s", name_src[0] ? name_src : leaf);

	if (s_map_browser.is_ce_map && s_map_browser.ce_header)
	{
		long prof = 0;
		struct tag_bundle_source source;
		struct tag_bundle_report report;
		struct forge_grow_buf buffer = { NULL, 0, 0 };
		long *indices = NULL;
		long count = 0;
		long problem = NONE;
		long tag_count = s_map_browser.ce_header->tag_count;
		byte *sounds = NULL;
		long t_i;
		struct browser_ce_map_header *reopened;
		unsigned long tags_size;
		byte *copy_tags = NULL;
		struct tag_bundle_loaded loaded;
		struct tag_bundle_file_region *regions = NULL;
		long region_count = 0;
		static struct tag_bundle_file_region const no_region = { 0, 0 };
		int added = 0;

		port_tag_profiles_scan();
		if (port_tag_profiles_count() <= 0)
			prof = port_tag_profiles_create("Forge Custom Tags");
		else
			prof = 0;
		if (prof < 0)
			return 0;

		custom_edition_cache_bundle_source(&source);
		if (tag_bundle_tags(&source, abs_idx, &indices, &count, &problem) != _tag_bundle_status_ok || !indices)
		{
			free(indices);
			return 0;
		}
		sounds = (byte *)calloc((size_t)tag_count, 1);
		if (!sounds)
		{
			free(indices);
			return 0;
		}
		for (t_i = 0; t_i < count; t_i++)
		{
			if ((uint32_t)s_map_browser.ce_header->instances[indices[t_i]].group_tag == 'snd!')
				sounds[indices[t_i]] = TRUE;
		}
		free(indices);

		reopened = (struct browser_ce_map_header *)custom_edition_cache_bundle_open(
			s_map_browser.map_path, sounds, tag_count);
		free(sounds);
		if (!reopened)
			return 0;
		s_map_browser.ce_header = reopened;
		if (s_map_browser.names_made)
			free(s_map_browser.names_made);
		s_map_browser.names_made = NULL;
		tag_names_restore(reopened, s_map_browser.map_name, NULL);

		custom_edition_cache_bundle_source(&source);
		memset(&report, 0, sizeof(report));
		if (tag_bundle_write(&source, abs_idx, forge_grow_buf_write, &buffer, &report) != _tag_bundle_status_ok ||
			!buffer.bytes || buffer.size < TAG_BUNDLE_HEADER_BYTES)
		{
			free(buffer.bytes);
			return 0;
		}

		tags_size = tag_bundle_file_data_offset(buffer.bytes);
		if (tags_size >= TAG_BUNDLE_HEADER_BYTES && tags_size <= buffer.size)
		{
			copy_tags = (byte *)malloc(tags_size);
			if (copy_tags)
			{
				memcpy(copy_tags, buffer.bytes, tags_size);
				if (tag_bundle_load_tags_regions(
						copy_tags,
						tags_size,
						buffer.size,
						&loaded,
						&regions,
						&region_count) == _tag_bundle_status_ok)
				{
					added = port_tag_profiles_tag_add(
						prof,
						s_map_browser.map_name,
						(unsigned long)grp,
						tag_path,
						buffer.bytes,
						buffer.size,
						tags_size,
						(struct port_tag_store_region const *)(regions ? regions : &no_region),
						region_count);
				}
				free(copy_tags);
				free(regions);
			}
		}
		free(buffer.bytes);

		if (!added)
			return 0;

		{
			long t_slot = port_tag_profiles_tag_find(prof, (unsigned long)grp, tag_path);
			if (t_slot >= 0)
			{
				port_tag_profiles_tag_set_enabled(prof, t_slot, 1);
				port_tag_profiles_tag_set_option(prof, t_slot, "name", clean_name);
			}
		}
		forge_runtime_reload_catalogs();
		return 1;
	}
	else if (s_map_browser.is_xbox_map)
	{
		if (!extract_compressed_xbox_map_tag(abs_idx, grp, tag_path, clean_name))
			return 0;
		forge_runtime_reload_catalogs();
		return 1;
	}

	return 0;
}

static int catalog_entry_matches_installed_tab(struct forge_catalog_entry const *e, int category_tab)
{
	if (!e || e->group_tag == 'spwn' || e->group_tag == 'tele' || e->group_tag == 'flag')
		return 0;
	return browser_group_matches_category(e->group_tag, category_tab);
}

static int get_installed_catalog_index(int category_tab, int list_index)
{
	int i, found = 0;
	if (category_tab < 0 || category_tab >= FORGE_BROWSER_NUM_CATS || list_index < 0)
		return -1;
	for (i = 0; i < g_forge.catalog_count; i++)
	{
		if (catalog_entry_matches_installed_tab(&g_forge.catalog[i], category_tab))
		{
			if (found == list_index)
				return i;
			found++;
		}
	}
	return -1;
}

int forge_ui_installed_tag_count(int category_tab)
{
	int i, count = 0;
	if (category_tab < 0 || category_tab >= FORGE_BROWSER_NUM_CATS)
		return 0;
	for (i = 0; i < g_forge.catalog_count; i++)
	{
		if (catalog_entry_matches_installed_tab(&g_forge.catalog[i], category_tab))
			count++;
	}
	return count;
}

static int find_fxtg_file_for_tag(uint32_t grp, char const *tag_path, char *out_file, size_t out_sz, uint32_t *out_disk_bytes)
{
#ifdef _WIN32
	static char const *const dirs[] = {
		"OpenCE Forge/custom_tags",
		"custom_tags"
	};
	int d;
	for (d = 0; d < 2; d++)
	{
		char pat[512];
		WIN32_FIND_DATAA fd;
		HANDLE h;
		snprintf(pat, sizeof(pat), "%s\\*.fxtg", dirs[d]);
		h = FindFirstFileA(pat, &fd);
		if (h == INVALID_HANDLE_VALUE)
			continue;
		do
		{
			if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
			{
				char full[512];
				FILE *fp;
				struct fxtg_bundle_header hdr;
				snprintf(full, sizeof(full), "%s/%s", dirs[d], fd.cFileName);
				fp = fopen(full, "rb");
				if (fp)
				{
					if (fread(&hdr, 1, sizeof(hdr), fp) == sizeof(hdr) &&
						hdr.magic == 0x31545846u &&
						hdr.group_tag == grp &&
						_stricmp(hdr.tag_path, tag_path) == 0)
					{
						fclose(fp);
						CloseHandle(h);
						if (out_file && out_sz > 0)
							snprintf(out_file, out_sz, "%s", full);
						if (out_disk_bytes)
							*out_disk_bytes = (uint32_t)sizeof(hdr) + hdr.compressed_size;
						return 1;
					}
					fclose(fp);
				}
			}
		} while (FindNextFileA(h, &fd));
		CloseHandle(h);
	}
#else
	(void)grp;
	(void)tag_path;
	(void)out_file;
	(void)out_sz;
	(void)out_disk_bytes;
#endif
	return 0;
}

int forge_ui_installed_tag_get_info(
	int category_tab,
	int list_index,
	char *out_display_name,
	size_t disp_sz,
	char *out_full_path,
	size_t full_sz,
	char *out_type_str,
	size_t type_sz,
	char *out_size_str,
	size_t size_sz,
	int *out_is_custom)
{
	int cat_idx = get_installed_catalog_index(category_tab, list_index);
	struct forge_catalog_entry const *e;
	int32_t byte_size = 0;

	if (cat_idx < 0 || cat_idx >= g_forge.catalog_count)
		return 0;
	e = &g_forge.catalog[cat_idx];

	if (out_display_name && disp_sz > 0)
		snprintf(out_display_name, disp_sz, "%s", e->display_name);
	if (out_full_path && full_sz > 0)
		snprintf(out_full_path, full_sz, "%s", e->tag_path);
	if (out_type_str && type_sz > 0)
		snprintf(out_type_str, type_sz, "%s", browser_group_type_label(e->group_tag));
	if (out_is_custom)
		*out_is_custom = (_strnicmp(e->display_name, "[c] ", 4) == 0) ? 1 : 0;

	if (out_size_str && size_sz > 0)
	{
		long p_cnt = port_tag_profiles_count();
		long p;
		uint32_t fxtg_bytes = 0;

		for (p = 0; p < p_cnt && byte_size <= 0; p++)
		{
			long t = port_tag_profiles_tag_find(p, (unsigned long)e->group_tag, e->tag_path);
			if (t >= 0)
				byte_size = (int32_t)port_tag_profiles_tag_size(p, t);
		}
		if (byte_size <= 0 && find_fxtg_file_for_tag(e->group_tag, e->tag_path, NULL, 0, &fxtg_bytes))
		{
			byte_size = (int32_t)fxtg_bytes;
		}
		if (byte_size <= 0 && open_preview_xbox_donor_for_tag(e->group_tag, e->tag_path))
		{
			int32_t i;
			char const *lookup = (e->group_tag == 'mach' &&
				_stricmp(e->tag_path, "levels\\a30\\devices\\beam emitter\\beam emitter red") == 0)
				? "levels\\a30\\devices\\beam emitter\\beam emitter"
				: e->tag_path;
			for (i = 0; i < s_preview_xbox_donor.tag_count; i++)
			{
				uint32_t io = s_preview_xbox_donor.inst_off + (uint32_t)i * 32u;
				uint32_t grp = *(uint32_t const *)(s_preview_xbox_donor.tag_buf + io + 0x00);
				uint32_t name_addr = *(uint32_t const *)(s_preview_xbox_donor.tag_buf + io + 0x10);
				if (grp == e->group_tag && name_addr >= s_preview_xbox_donor.tag_base &&
					(name_addr - s_preview_xbox_donor.tag_base) < s_preview_xbox_donor.tag_sz)
				{
					char const *tn = (char const *)(s_preview_xbox_donor.tag_buf + (name_addr - s_preview_xbox_donor.tag_base));
					if (_stricmp(tn, lookup) == 0)
					{
						byte_size = compute_xbox_tag_closure_bytes(
							s_preview_xbox_donor.tag_buf,
							s_preview_xbox_donor.tag_sz,
							s_preview_xbox_donor.tag_base,
							s_preview_xbox_donor.inst_off,
							s_preview_xbox_donor.tag_count,
							i);
						break;
					}
				}
			}
		}
		format_tag_byte_size(byte_size, out_size_str, size_sz);
	}
	return 1;
}

static void remove_manifest_entry_from_file(char const *manifest_path, uint32_t grp, char const *tag_path)
{
	FILE *in_fp = fopen(manifest_path, "r");
	char (*lines)[512];
	int line_count = 0, i;
	char grp_str[5];
	FILE *out_fp;

	if (!in_fp)
		return;
	lines = (char (*)[512])malloc(2048u * sizeof(*lines));
	if (!lines)
	{
		fclose(in_fp);
		return;
	}
	grp_str[0] = (char)((grp >> 24) & 0xFF);
	grp_str[1] = (char)((grp >> 16) & 0xFF);
	grp_str[2] = (char)((grp >> 8) & 0xFF);
	grp_str[3] = (char)(grp & 0xFF);
	grp_str[4] = '\0';

	{
		char buf[512];
		while (line_count < 2048 && fgets(buf, sizeof(buf), in_fp))
		{
			char l_grp[16], l_path[256];
			int l_cat = 0, l_src = 0;
			char *nl = strchr(buf, '\r');
			if (nl) *nl = '\0';
			nl = strchr(buf, '\n');
			if (nl) *nl = '\0';
			if (sscanf(buf, "%4s|%d|%d|%255[^|\n]", l_grp, &l_cat, &l_src, l_path) == 4 &&
				_stricmp(l_grp, grp_str) == 0 && _stricmp(l_path, tag_path) == 0)
			{
				continue;
			}
			snprintf(lines[line_count++], sizeof(lines[0]), "%s", buf);
		}
	}
	fclose(in_fp);

	out_fp = fopen(manifest_path, "wb");
	if (out_fp)
	{
		for (i = 0; i < line_count; i++)
			fprintf(out_fp, "%s\n", lines[i]);
		fclose(out_fp);
	}
	free(lines);
}

int forge_ui_installed_tag_delete(int category_tab, int list_index)
{
	int cat_idx = get_installed_catalog_index(category_tab, list_index);
	uint32_t grp;
	char tag_path[256];
	char fxtg_file[512];
	long p_cnt, p;

	if (cat_idx < 0 || cat_idx >= g_forge.catalog_count)
		return 0;
	grp = g_forge.catalog[cat_idx].group_tag;
	snprintf(tag_path, sizeof(tag_path), "%s", g_forge.catalog[cat_idx].tag_path);

	/* 1. Remove from Faber Tag Vault profiles if present */
	port_tag_profiles_scan();
	p_cnt = port_tag_profiles_count();
	for (p = 0; p < p_cnt; p++)
	{
		long t = port_tag_profiles_tag_find(p, (unsigned long)grp, tag_path);
		if (t >= 0)
			port_tag_profiles_tag_remove(p, t);
	}

	/* 2. Remove compressed .fxtg bundle if present */
	while (find_fxtg_file_for_tag(grp, tag_path, fxtg_file, sizeof(fxtg_file), NULL))
	{
		if (remove(fxtg_file) != 0)
			break;
	}

	/* 3. Remove from custom_catalog.txt and xbox_catalog.txt */
	remove_manifest_entry_from_file("OpenCE Forge/cached_tags/custom_catalog.txt", grp, tag_path);
	remove_manifest_entry_from_file("OpenCE Forge/cached_tags/xbox_catalog.txt", grp, tag_path);

	forge_runtime_reload_catalogs();
	return 1;
}

/* ============================================================================
 * 3D SOFTWARE MESH PREVIEW BUILDER (Supports CE 'mod2', Faber '.fbrtag',
 * Stock Xbox 'mode', and Compressed Xbox '.fxtg' bundles!)
 * ============================================================================ */

#define FORGE_PREVIEW_MAX_VERTICES   16384
#define FORGE_PREVIEW_MAX_TRIANGLES  24576
#define FORGE_PREVIEW_MAX_SHADERS    64
#define FORGE_PREVIEW_TEXTURE_SIZE   128

struct forge_preview_node_xform
{
	float matrix[3][3];
	float point[3];
};

static void compute_preview_world_nodes(
	int32_t node_count,
	uint8_t const *nodes_base,
	struct forge_preview_node_xform *out_nodes)
{
	int32_t i;
	for (i = 0; i < node_count; i++)
	{
		uint8_t const *n = nodes_base + (size_t)i * 156u;
		int16_t parent = *(int16_t const *)(n + 36u);
		float tx = *(float const *)(n + 40u);
		float ty = *(float const *)(n + 44u);
		float tz = *(float const *)(n + 48u);
		float qi = *(float const *)(n + 52u);
		float qj = *(float const *)(n + 56u);
		float qk = *(float const *)(n + 60u);
		float qw = *(float const *)(n + 64u);
		float norm = qi * qi + qj * qj + qk * qk + qw * qw;
		float s = (norm > 0.0f) ? (2.0f / norm) : 0.0f;
		float xx = qi * qi * s, xy = qi * qj * s, xz = qi * qk * s, xw = qi * qw * s;
		float yy = qj * qj * s, yz = qj * qk * s, yw = qj * qw * s;
		float zz = qk * qk * s, zw = qk * qw * s;
		float loc[3][3];

		loc[0][0] = 1.0f - (yy + zz);
		loc[0][1] = xy + zw;
		loc[0][2] = xz - yw;
		loc[1][0] = xy - zw;
		loc[1][1] = 1.0f - (xx + zz);
		loc[1][2] = yz + xw;
		loc[2][0] = xz + yw;
		loc[2][1] = yz - xw;
		loc[2][2] = 1.0f - (xx + yy);

		if (parent >= 0 && parent < i)
		{
			struct forge_preview_node_xform const *p = &out_nodes[parent];
			int r, c;
			for (r = 0; r < 3; r++)
			{
				for (c = 0; c < 3; c++)
				{
					out_nodes[i].matrix[r][c] =
						loc[r][0] * p->matrix[0][c] +
						loc[r][1] * p->matrix[1][c] +
						loc[r][2] * p->matrix[2][c];
				}
			}
			for (c = 0; c < 3; c++)
			{
				out_nodes[i].point[c] = p->point[c] +
					tx * p->matrix[0][c] +
					ty * p->matrix[1][c] +
					tz * p->matrix[2][c];
			}
		}
		else
		{
			memcpy(out_nodes[i].matrix, loc, sizeof(loc));
			out_nodes[i].point[0] = tx;
			out_nodes[i].point[1] = ty;
			out_nodes[i].point[2] = tz;
		}
	}
}

static void preview_append_part_vertices_and_strip(
	struct tag_injector_mesh *mesh,
	uint8_t const *verts68,
	int32_t vtx_count,
	uint16_t const *strip_indices,
	int32_t strip_tri_count,
	struct forge_preview_node_xform const *world_nodes,
	int32_t node_count,
	int has_local_nodes,
	int16_t local_node_count,
	uint8_t const *local_node_indices,
	short material_index,
	float model_u_scale,
	float model_v_scale)
{
	long base_vtx = mesh->vertex_count;
	int32_t v, t;

	if (vtx_count <= 0 || strip_tri_count <= 0 ||
		base_vtx + vtx_count > FORGE_PREVIEW_MAX_VERTICES ||
		mesh->triangle_count + strip_tri_count > FORGE_PREVIEW_MAX_TRIANGLES)
		return;

	for (v = 0; v < vtx_count; v++)
	{
		uint8_t const *vb = verts68 + (size_t)v * 68u;
		float px = *(float const *)(vb + 0u);
		float py = *(float const *)(vb + 4u);
		float pz = *(float const *)(vb + 8u);
		float nx = *(float const *)(vb + 12u);
		float ny = *(float const *)(vb + 16u);
		float nz = *(float const *)(vb + 20u);
		float tu = *(float const *)(vb + 48u) * model_u_scale;
		float tv = *(float const *)(vb + 52u) * model_v_scale;
		int16_t n0 = *(int16_t const *)(vb + 56u);
		int16_t n1 = *(int16_t const *)(vb + 58u);
		float w0 = *(float const *)(vb + 60u);
		float w1 = *(float const *)(vb + 64u);

		if (!(px > -1000.0f && px < 1000.0f && py > -1000.0f && py < 1000.0f && pz > -1000.0f && pz < 1000.0f))
			px = py = pz = 0.0f;
		if (!(nx > -1000.0f && nx < 1000.0f && ny > -1000.0f && ny < 1000.0f && nz > -1000.0f && nz < 1000.0f))
		{
			nx = 0.0f; ny = 0.0f; nz = 1.0f;
		}
		if (!(tu > -1000.0f && tu < 1000.0f && tv > -1000.0f && tv < 1000.0f))
			tu = tv = 0.0f;

		if (world_nodes && node_count > 0)
		{
			if (has_local_nodes && local_node_indices)
			{
				n0 = (n0 >= 0 && n0 < local_node_count) ? (int16_t)local_node_indices[n0] : -1;
				n1 = (n1 >= 0 && n1 < local_node_count) ? (int16_t)local_node_indices[n1] : -1;
			}
			if (n0 >= 0 && n0 < node_count)
			{
				struct forge_preview_node_xform const *xf0 = &world_nodes[n0];
				float p0x = xf0->point[0] + px * xf0->matrix[0][0] + py * xf0->matrix[1][0] + pz * xf0->matrix[2][0];
				float p0y = xf0->point[1] + px * xf0->matrix[0][1] + py * xf0->matrix[1][1] + pz * xf0->matrix[2][1];
				float p0z = xf0->point[2] + px * xf0->matrix[0][2] + py * xf0->matrix[1][2] + pz * xf0->matrix[2][2];
				float n0x = nx * xf0->matrix[0][0] + ny * xf0->matrix[1][0] + nz * xf0->matrix[2][0];
				float n0y = nx * xf0->matrix[0][1] + ny * xf0->matrix[1][1] + nz * xf0->matrix[2][1];
				float n0z = nx * xf0->matrix[0][2] + ny * xf0->matrix[1][2] + nz * xf0->matrix[2][2];

				if (n1 >= 0 && n1 < node_count && w0 >= 0.0f && w0 <= 1.0f && w1 > 0.0f && w1 <= 1.0f)
				{
					struct forge_preview_node_xform const *xf1 = &world_nodes[n1];
					float p1x = xf1->point[0] + px * xf1->matrix[0][0] + py * xf1->matrix[1][0] + pz * xf1->matrix[2][0];
					float p1y = xf1->point[1] + px * xf1->matrix[0][1] + py * xf1->matrix[1][1] + pz * xf1->matrix[2][1];
					float p1z = xf1->point[2] + px * xf1->matrix[0][2] + py * xf1->matrix[1][2] + pz * xf1->matrix[2][2];
					float n1x = nx * xf1->matrix[0][0] + ny * xf1->matrix[1][0] + nz * xf1->matrix[2][0];
					float n1y = nx * xf1->matrix[0][1] + ny * xf1->matrix[1][1] + nz * xf1->matrix[2][1];
					float n1z = nx * xf1->matrix[0][2] + ny * xf1->matrix[1][2] + nz * xf1->matrix[2][2];
					px = w0 * p0x + w1 * p1x;
					py = w0 * p0y + w1 * p1y;
					pz = w0 * p0z + w1 * p1z;
					nx = w0 * n0x + w1 * n1x;
					ny = w0 * n0y + w1 * n1y;
					nz = w0 * n0z + w1 * n1z;
				}
				else
				{
					px = p0x; py = p0y; pz = p0z;
					nx = n0x; ny = n0y; nz = n0z;
				}
			}
		}

		mesh->positions[base_vtx + v][0] = px;
		mesh->positions[base_vtx + v][1] = py;
		mesh->positions[base_vtx + v][2] = pz;
		mesh->normals[base_vtx + v][0] = nx;
		mesh->normals[base_vtx + v][1] = ny;
		mesh->normals[base_vtx + v][2] = nz;
		mesh->texcoords[base_vtx + v][0] = tu;
		mesh->texcoords[base_vtx + v][1] = tv;
	}
	mesh->vertex_count += vtx_count;

	for (t = 0; t < strip_tri_count; t++)
	{
		uint16_t a = strip_indices[t];
		uint16_t b = strip_indices[t + 1];
		uint16_t c = strip_indices[t + 2];
		if (a == b || b == c || a == c ||
			a >= (uint16_t)vtx_count || b >= (uint16_t)vtx_count || c >= (uint16_t)vtx_count)
			continue;
		mesh->triangles[mesh->triangle_count][0] = base_vtx + (long)((t & 1) ? b : a);
		mesh->triangles[mesh->triangle_count][1] = base_vtx + (long)((t & 1) ? a : b);
		mesh->triangles[mesh->triangle_count][2] = base_vtx + (long)c;
		mesh->triangle_materials[mesh->triangle_count] = material_index;
		mesh->triangle_count++;
	}
}

enum
{
	_bitmap_format_a8 = 0,
	_bitmap_format_y8,
	_bitmap_format_ay8,
	_bitmap_format_a8y8,
	_bitmap_format_unused1,
	_bitmap_format_unused2,
	_bitmap_format_r5g6b5,
	_bitmap_format_unused3,
	_bitmap_format_a1r5g5b5,
	_bitmap_format_a4r4g4b4,
	_bitmap_format_x8r8g8b8,
	_bitmap_format_a8r8g8b8,
	_bitmap_format_unused4,
	_bitmap_format_unused5,
	_bitmap_format_dxt1,
	_bitmap_format_dxt3,
	_bitmap_format_dxt5,
	_bitmap_format_p8_bump,
	_bitmap_swizzled_bit = 3
};

static unsigned long preview_bitmap_level_bytes(short format, short bpp, short width, short height)
{
	unsigned long w = (width > 0) ? (unsigned long)width : 1ul;
	unsigned long h = (height > 0) ? (unsigned long)height : 1ul;
	if (bpp > 0)
		return w * h * (unsigned long)bpp;
	if (format == _bitmap_format_dxt1)
		return ((w + 3ul) / 4ul) * ((h + 3ul) / 4ul) * 8ul;
	if (format == _bitmap_format_dxt3 || format == _bitmap_format_dxt5)
		return ((w + 3ul) / 4ul) * ((h + 3ul) / 4ul) * 16ul;
	return 0ul;
}

static int preview_decode_bitmap_texture(
	struct bitmap_data const *bitmap,
	boolean (*read_fn)(void *ctx, unsigned long offset, unsigned long size, void *buf),
	void *read_ctx,
	struct tag_injector_texture *out_tex)
{
	short mip_w, mip_h;
	short level = 0;
	unsigned long offset;
	unsigned long size;
	unsigned char *pixels;
	short bpp = 0;

	if (!bitmap || !read_fn || !out_tex)
		return 0;
	if (bitmap->type != 0 /* _bitmap_type_2d */ ||
		bitmap->width <= 0 || bitmap->width > 4096 ||
		bitmap->height <= 0 || bitmap->height > 4096 ||
		bitmap->depth != 1 || bitmap->pixels_offset < 0 || bitmap->pixels_size <= 0)
		return 0;

	switch (bitmap->format)
	{
	case _bitmap_format_a8:
	case _bitmap_format_y8:
	case _bitmap_format_ay8:
		bpp = 1;
		break;
	case _bitmap_format_a8y8:
	case _bitmap_format_r5g6b5:
	case _bitmap_format_a1r5g5b5:
	case _bitmap_format_a4r4g4b4:
		bpp = 2;
		break;
	case _bitmap_format_x8r8g8b8:
	case _bitmap_format_a8r8g8b8:
		bpp = 4;
		break;
	case _bitmap_format_dxt1:
	case _bitmap_format_dxt3:
	case _bitmap_format_dxt5:
		break;
	default:
		return 0;
	}

	mip_w = bitmap->width;
	mip_h = bitmap->height;
	offset = (unsigned long)bitmap->pixels_offset;
	while (level < bitmap->mipmap_count &&
		   (mip_w > FORGE_PREVIEW_TEXTURE_SIZE || mip_h > FORGE_PREVIEW_TEXTURE_SIZE))
	{
		offset += preview_bitmap_level_bytes(bitmap->format, bpp, mip_w, mip_h);
		mip_w = (short)((mip_w >> 1) > 1 ? (mip_w >> 1) : 1);
		mip_h = (short)((mip_h >> 1) > 1 ? (mip_h >> 1) : 1);
		level++;
	}
	size = preview_bitmap_level_bytes(bitmap->format, bpp, mip_w, mip_h);
	if (!size || offset - (unsigned long)bitmap->pixels_offset > (unsigned long)bitmap->pixels_size ||
		size > (unsigned long)bitmap->pixels_size - (offset - (unsigned long)bitmap->pixels_offset))
		return 0;

	pixels = (unsigned char *)malloc(size);
	if (!pixels)
		return 0;
	if (!read_fn(read_ctx, offset, size, pixels))
	{
		free(pixels);
		return 0;
	}
	if (bpp > 0 && TEST_FLAG(bitmap->flags, _bitmap_swizzled_bit))
	{
		unsigned char *unswizzled = (unsigned char *)malloc(size);
		if (!unswizzled)
		{
			free(pixels);
			return 0;
		}
		{
			short sx, sy;
			for (sy = 0; sy < mip_h; sy++)
			{
				for (sx = 0; sx < mip_w; sx++)
				{
					long swz = 0;
					bitmap_swizzle_vector2d(mip_w, mip_h, sx, sy, &swz);
					if (swz >= 0 && (unsigned long)(swz + 1) * (unsigned long)bpp <= size)
						memcpy(unswizzled + ((long)sy * mip_w + sx) * bpp, pixels + swz * bpp, (size_t)bpp);
				}
			}
		}
		free(pixels);
		pixels = unswizzled;
	}

	out_tex->width = mip_w;
	out_tex->height = mip_h;
	out_tex->format = bitmap->format;
	out_tex->size = size;
	out_tex->pixels = pixels;
	return 1;
}

static boolean preview_xbox_file_read_cb(void *ctx, unsigned long offset, unsigned long size, void *buf)
{
	FILE *fp = (FILE *)ctx;
	if (!fp || offset < 2048u)
		return FALSE;
	return (fseek(fp, (long)offset, SEEK_SET) == 0 && fread(buf, 1, size, fp) == size) ? TRUE : FALSE;
}

static int build_preview_mesh_from_xbox_map_data(
	FILE *fp,
	uint8_t const *tag_buf,
	uint32_t tag_sz,
	uint32_t tag_base,
	uint32_t inst_off,
	int32_t tag_count,
	int32_t obj_abs_idx,
	struct tag_injector_mesh *mesh)
{
	uint32_t obj_io, obj_data_addr, obj_off;
	int32_t mod_ref_idx, mod_abs_idx;
	uint32_t mod_io, mod_data_addr, mod_off;
	float u_scale, v_scale;
	int32_t node_cnt, reg_cnt, geom_cnt, shdr_cnt;
	uint32_t node_addr, reg_addr, geom_addr, shdr_addr;
	struct forge_preview_node_xform *world_nodes = NULL;
	short shader_to_mat[FORGE_PREVIEW_MAX_SHADERS];
	int r_i, s_i;

	if (!tag_buf || obj_abs_idx < 0 || obj_abs_idx >= tag_count || !mesh)
		return 0;

	obj_io = inst_off + (uint32_t)obj_abs_idx * 32u;
	obj_data_addr = *(uint32_t const *)(tag_buf + obj_io + 0x14);
	if (obj_data_addr < tag_base || (obj_data_addr - tag_base) + 0x40u > tag_sz)
		return 0;
	obj_off = obj_data_addr - tag_base;

	/* object.model reference is at +0x28 (index at +0x34) */
	mod_ref_idx = *(int32_t const *)(tag_buf + obj_off + 0x34u);
	if (mod_ref_idx == -1)
		return 0;
	mod_abs_idx = mod_ref_idx & 0xFFFF;
	if (mod_abs_idx < 0 || mod_abs_idx >= tag_count)
		return 0;

	mod_io = inst_off + (uint32_t)mod_abs_idx * 32u;
	if (*(uint32_t const *)(tag_buf + mod_io + 0x00) != 'mode')
		return 0;
	mod_data_addr = *(uint32_t const *)(tag_buf + mod_io + 0x14);
	if (mod_data_addr < tag_base || (mod_data_addr - tag_base) + 0xE8u > tag_sz)
		return 0;
	mod_off = mod_data_addr - tag_base;

	u_scale = *(float const *)(tag_buf + mod_off + 0x30u);
	v_scale = *(float const *)(tag_buf + mod_off + 0x34u);
	if (!(u_scale > 0.0001f && u_scale < 1000.0f)) u_scale = 1.0f;
	if (!(v_scale > 0.0001f && v_scale < 1000.0f)) v_scale = 1.0f;

	node_cnt = *(int32_t const *)(tag_buf + mod_off + 0xB8u);
	node_addr = *(uint32_t const *)(tag_buf + mod_off + 0xBCu);
	reg_cnt = *(int32_t const *)(tag_buf + mod_off + 0xC4u);
	reg_addr = *(uint32_t const *)(tag_buf + mod_off + 0xC8u);
	geom_cnt = *(int32_t const *)(tag_buf + mod_off + 0xD0u);
	geom_addr = *(uint32_t const *)(tag_buf + mod_off + 0xD4u);
	shdr_cnt = *(int32_t const *)(tag_buf + mod_off + 0xDCu);
	shdr_addr = *(uint32_t const *)(tag_buf + mod_off + 0xE0u);

	if (geom_cnt <= 0 || geom_addr < tag_base || (geom_addr - tag_base) + (uint32_t)geom_cnt * 48u > tag_sz)
		return 0;

	mesh->positions = (float (*)[3])malloc(FORGE_PREVIEW_MAX_VERTICES * sizeof(*mesh->positions));
	mesh->normals = (float (*)[3])malloc(FORGE_PREVIEW_MAX_VERTICES * sizeof(*mesh->normals));
	mesh->texcoords = (float (*)[2])malloc(FORGE_PREVIEW_MAX_VERTICES * sizeof(*mesh->texcoords));
	mesh->triangles = (long (*)[3])malloc(FORGE_PREVIEW_MAX_TRIANGLES * sizeof(*mesh->triangles));
	mesh->triangle_materials = (short *)malloc(FORGE_PREVIEW_MAX_TRIANGLES * sizeof(short));
	mesh->materials = (struct tag_injector_material *)calloc(FORGE_PREVIEW_MAX_SHADERS, sizeof(*mesh->materials));
	mesh->textures = (struct tag_injector_texture *)calloc(FORGE_PREVIEW_MAX_SHADERS, sizeof(*mesh->textures));
	if (!mesh->positions || !mesh->normals || !mesh->texcoords ||
		!mesh->triangles || !mesh->triangle_materials || !mesh->materials || !mesh->textures)
	{
		tag_preview_mesh_free(mesh);
		return 0;
	}
	mesh->color_a[0] = mesh->color_a[1] = mesh->color_a[2] = 1.0f;
	mesh->color_b[0] = mesh->color_b[1] = mesh->color_b[2] = 1.0f;

	for (s_i = 0; s_i < FORGE_PREVIEW_MAX_SHADERS; s_i++)
		shader_to_mat[s_i] = -2;

	if (node_cnt > 0 && node_cnt <= 64 && node_addr >= tag_base &&
		(node_addr - tag_base) + (uint32_t)node_cnt * 156u <= tag_sz)
	{
		world_nodes = (struct forge_preview_node_xform *)malloc((size_t)node_cnt * sizeof(*world_nodes));
		if (world_nodes)
			compute_preview_world_nodes(node_cnt, tag_buf + (node_addr - tag_base), world_nodes);
	}

	/* Iterate regions (or geometry 0 if no regions) */
	for (r_i = 0; r_i < (reg_cnt > 0 ? reg_cnt : 1); r_i++)
	{
		int16_t geom_idx = 0;
		uint32_t go;
		int32_t part_cnt;
		uint32_t part_addr;
		int p_i;

		if (reg_cnt > 0 && reg_addr >= tag_base && (reg_addr - tag_base) + (uint32_t)reg_cnt * 76u <= tag_sz)
		{
			uint32_t ro = (reg_addr - tag_base) + (uint32_t)r_i * 76u;
			int32_t perm_cnt = *(int32_t const *)(tag_buf + ro + 64u);
			uint32_t perm_addr = *(uint32_t const *)(tag_buf + ro + 68u);
			int pm_i;
			geom_idx = -1;
			if (perm_cnt > 0 && perm_addr >= tag_base && (perm_addr - tag_base) + (uint32_t)perm_cnt * 88u <= tag_sz)
			{
				for (pm_i = 0; pm_i < perm_cnt && geom_idx < 0; pm_i++)
				{
					uint32_t pmo = (perm_addr - tag_base) + (uint32_t)pm_i * 88u;
					int lod;
					for (lod = 4; lod >= 0; lod--)
					{
						int16_t g = *(int16_t const *)(tag_buf + pmo + 64u + (uint32_t)lod * 2u);
						if (g >= 0 && g < geom_cnt)
						{
							geom_idx = g;
							break;
						}
					}
				}
			}
		}
		if (geom_idx < 0 || geom_idx >= geom_cnt)
			continue;

		go = (geom_addr - tag_base) + (uint32_t)geom_idx * 48u;
		part_cnt = *(int32_t const *)(tag_buf + go + 36u);
		part_addr = *(uint32_t const *)(tag_buf + go + 40u);
		if (part_cnt <= 0 || part_addr < tag_base || (part_addr - tag_base) + (uint32_t)part_cnt * 104u > tag_sz)
			continue;

		for (p_i = 0; p_i < part_cnt; p_i++)
		{
			uint32_t po = (part_addr - tag_base) + (uint32_t)p_i * 104u;
			int16_t shdr_idx = *(int16_t const *)(tag_buf + po + 4u);
			int16_t tri_type = *(int16_t const *)(tag_buf + po + 68u);
			int32_t tri_cnt = *(int32_t const *)(tag_buf + po + 72u);
			uint32_t tb_hw = *(uint32_t const *)(tag_buf + po + 80u);
			int32_t vtx_cnt = *(int32_t const *)(tag_buf + po + 88u);
			uint32_t vb_hw = *(uint32_t const *)(tag_buf + po + 100u);
			short mat_idx = -1;

			if (shdr_idx >= 0 && shdr_idx < shdr_cnt && shdr_idx < FORGE_PREVIEW_MAX_SHADERS &&
				shdr_addr >= tag_base && (shdr_addr - tag_base) + (uint32_t)shdr_cnt * 32u <= tag_sz)
			{
				if (shader_to_mat[shdr_idx] == -2)
				{
					uint32_t so = (shdr_addr - tag_base) + (uint32_t)shdr_idx * 32u;
					int32_t s_ref = *(int32_t const *)(tag_buf + so + 12u);
					int32_t s_abs = (s_ref != -1) ? (s_ref & 0xFFFF) : -1;
					shader_to_mat[shdr_idx] = -1;
					if (s_abs >= 0 && s_abs < tag_count && fp)
					{
						uint32_t sio = inst_off + (uint32_t)s_abs * 32u;
						uint32_t sgrp = *(uint32_t const *)(tag_buf + sio + 0x00);
						uint32_t sdata = *(uint32_t const *)(tag_buf + sio + 0x14);
						int32_t bitm_ref = -1;
						float su = 1.0f, sv = 1.0f;
						if (sgrp == 'soso' && sdata >= tag_base && (sdata - tag_base) + 0xB4u <= tag_sz)
						{
							su = *(float const *)(tag_buf + (sdata - tag_base) + 0x9Cu);
							sv = *(float const *)(tag_buf + (sdata - tag_base) + 0xA0u);
							bitm_ref = *(int32_t const *)(tag_buf + (sdata - tag_base) + 0xB0u);
						}
						else if (sgrp == 'senv' && sdata >= tag_base && (sdata - tag_base) + 0x98u <= tag_sz)
						{
							bitm_ref = *(int32_t const *)(tag_buf + (sdata - tag_base) + 0x94u);
						}
						if (bitm_ref != -1 && (bitm_ref & 0xFFFF) < tag_count)
						{
							int32_t b_abs = bitm_ref & 0xFFFF;
							uint32_t bio = inst_off + (uint32_t)b_abs * 32u;
							uint32_t bdata = *(uint32_t const *)(tag_buf + bio + 0x14);
							if (*(uint32_t const *)(tag_buf + bio + 0x00) == 'bitm' &&
								bdata >= tag_base && (bdata - tag_base) + 0x60u <= tag_sz)
							{
								int32_t b_cnt = *(int32_t const *)(tag_buf + (bdata - tag_base) + 0x54u);
								uint32_t b_addr = *(uint32_t const *)(tag_buf + (bdata - tag_base) + 0x58u);
								if (b_cnt > 0 && b_addr >= tag_base && (b_addr - tag_base) + 48u <= tag_sz)
								{
									struct bitmap_data b_elem;
									memcpy(&b_elem, tag_buf + (b_addr - tag_base), sizeof(b_elem));
									if (preview_decode_bitmap_texture(
											&b_elem,
											preview_xbox_file_read_cb,
											fp,
											&mesh->textures[mesh->texture_count]))
									{
										short m_slot = (short)mesh->material_count++;
										short t_slot = (short)mesh->texture_count++;
										mesh->materials[m_slot].texture_index = t_slot;
										mesh->materials[m_slot].color_source = 0;
										mesh->materials[m_slot].u_scale = (su > 0.0001f && su < 1000.0f) ? su : 1.0f;
										mesh->materials[m_slot].v_scale = (sv > 0.0001f && sv < 1000.0f) ? sv : 1.0f;
										mesh->materials[m_slot].base_map_alpha_Replicates = 0;
										shader_to_mat[shdr_idx] = m_slot;
									}
								}
							}
						}
					}
				}
				mat_idx = shader_to_mat[shdr_idx];
			}

			if (tri_type == _triangle_buffer_type_precompiled_strip &&
				tri_cnt > 0 && tri_cnt <= 65536 && vtx_cnt > 0 && vtx_cnt <= 16384 &&
				tb_hw >= tag_base && (tb_hw - tag_base) + 12u <= tag_sz &&
				vb_hw >= tag_base && (vb_hw - tag_base) + 12u <= tag_sz)
			{
				uint32_t ib_addr = *(uint32_t const *)(tag_buf + (tb_hw - tag_base) + 4u);
				uint32_t vb_addr = *(uint32_t const *)(tag_buf + (vb_hw - tag_base) + 4u);
				uint32_t comp_sz = (uint32_t)vtx_cnt * 32u;
				uint32_t idx_sz = ((uint32_t)tri_cnt + 2u) * 2u;
				if (ib_addr >= tag_base && (ib_addr - tag_base) + idx_sz <= tag_sz &&
					vb_addr >= tag_base && (vb_addr - tag_base) + comp_sz <= tag_sz)
				{
					uint8_t *verts68 = (uint8_t *)malloc((size_t)vtx_cnt * 68u);
					if (verts68)
					{
						rasterizer_geometry_uncompress_vertices(
							_rasterizer_vertex_type_model_compressed,
							vtx_cnt,
							verts68,
							(int)vtx_cnt * 68,
							tag_buf + (vb_addr - tag_base),
							(int)comp_sz);
						preview_append_part_vertices_and_strip(
							mesh,
							verts68,
							vtx_cnt,
							(uint16_t const *)(tag_buf + (ib_addr - tag_base)),
							tri_cnt,
							world_nodes,
							node_cnt,
							0,
							0,
							NULL,
							mat_idx,
							u_scale,
							v_scale);
						free(verts68);
					}
				}
			}
		}
	}

	free(world_nodes);
	return mesh->triangle_count > 0 ? 1 : 0;
}

struct preview_ce_source
{
	int is_bundle;
	struct tag_bundle_source map_src;
	struct browser_ce_map_header const *map_hdr;
	struct tag_bundle_loaded const *bundle_loaded;
	struct faber_bundle_read_ctx bundle_read;
};

static uint8_t const *preview_ce_get_tag(
	struct preview_ce_source const *src,
	long index,
	uint32_t *out_group)
{
	if (!src || index == NONE)
		return NULL;
	if (src->is_bundle)
	{
		if (index >= 0 && index < src->bundle_loaded->tag_count)
		{
			byte const *ent = src->bundle_loaded->tags + index * TAG_BUNDLE_TAG_BYTES;
			if (out_group)
				*out_group = *(uint32_t const *)(ent + TAG_BUNDLE_TAG_GROUP_OFFSET);
			return src->bundle_loaded->data + *(uint32_t const *)(ent + TAG_BUNDLE_TAG_ROOT_OFFSET);
		}
	}
	else if (src->map_hdr)
	{
		long abs_i = index & 0xFFFF;
		if (abs_i >= 0 && abs_i < src->map_hdr->tag_count)
		{
			if (out_group)
				*out_group = (uint32_t)src->map_hdr->instances[abs_i].group_tag;
			return (uint8_t const *)src->map_hdr->instances[abs_i].base_address;
		}
	}
	return NULL;
}

static boolean preview_ce_read_file_cb(void *ctx, unsigned long offset, unsigned long size, void *buf)
{
	struct preview_ce_source const *src = (struct preview_ce_source const *)ctx;
	if (!src)
		return FALSE;
	if (src->is_bundle)
		return faber_bundle_file_read_cb((void *)&src->bundle_read, offset, size, buf);
	return src->map_src.read(src->map_src.context, offset, size, buf);
}

static int build_preview_mesh_from_ce_source(
	struct preview_ce_source const *src,
	uint8_t const *obj_root,
	uint32_t obj_group,
	struct tag_injector_mesh *mesh)
{
	struct object_definition const *obj;
	uint32_t mod_group = 0;
	struct model const *model;
	float u_scale, v_scale;
	struct forge_preview_node_xform *world_nodes = NULL;
	short shader_to_mat[FORGE_PREVIEW_MAX_SHADERS];
	int has_local_nodes;
	long r_i, s_i;

	(void)obj_group;
	if (!src || !obj_root || !mesh)
		return 0;
	obj = (struct object_definition const *)obj_root;
	if (obj->object.model.index == NONE)
		return 0;
	model = (struct model const *)preview_ce_get_tag(src, obj->object.model.index, &mod_group);
	if (!model || (mod_group != 'mod2' && mod_group != 'mode') ||
		model->geometries.count <= 0 || !model->geometries.address)
		return 0;

	u_scale = model->base_map_scale.i;
	v_scale = model->base_map_scale.j;
	if (!(u_scale > 0.0001f && u_scale < 1000.0f)) u_scale = 1.0f;
	if (!(v_scale > 0.0001f && v_scale < 1000.0f)) v_scale = 1.0f;

	mesh->positions = (float (*)[3])malloc(FORGE_PREVIEW_MAX_VERTICES * sizeof(*mesh->positions));
	mesh->normals = (float (*)[3])malloc(FORGE_PREVIEW_MAX_VERTICES * sizeof(*mesh->normals));
	mesh->texcoords = (float (*)[2])malloc(FORGE_PREVIEW_MAX_VERTICES * sizeof(*mesh->texcoords));
	mesh->triangles = (long (*)[3])malloc(FORGE_PREVIEW_MAX_TRIANGLES * sizeof(*mesh->triangles));
	mesh->triangle_materials = (short *)malloc(FORGE_PREVIEW_MAX_TRIANGLES * sizeof(short));
	mesh->materials = (struct tag_injector_material *)calloc(FORGE_PREVIEW_MAX_SHADERS, sizeof(*mesh->materials));
	mesh->textures = (struct tag_injector_texture *)calloc(FORGE_PREVIEW_MAX_SHADERS, sizeof(*mesh->textures));
	if (!mesh->positions || !mesh->normals || !mesh->texcoords ||
		!mesh->triangles || !mesh->triangle_materials || !mesh->materials || !mesh->textures)
	{
		tag_preview_mesh_free(mesh);
		return 0;
	}
	mesh->color_a[0] = mesh->color_a[1] = mesh->color_a[2] = 1.0f;
	mesh->color_b[0] = mesh->color_b[1] = mesh->color_b[2] = 1.0f;

	for (s_i = 0; s_i < FORGE_PREVIEW_MAX_SHADERS; s_i++)
		shader_to_mat[s_i] = -2;

	if (model->nodes.count > 0 && model->nodes.count <= 256 && model->nodes.address)
	{
		world_nodes = (struct forge_preview_node_xform *)malloc((size_t)model->nodes.count * sizeof(*world_nodes));
		if (world_nodes)
			compute_preview_world_nodes(model->nodes.count, (uint8_t const *)model->nodes.address, world_nodes);
	}
	has_local_nodes = TEST_FLAG(model->flags, 1) ? 1 : 0;

	for (r_i = 0; r_i < (model->regions.count > 0 ? model->regions.count : 1); r_i++)
	{
		short geom_idx = 0;
		uint8_t const *geom_ptr;
		struct tag_block const *parts_blk;
		long p_i;

		if (model->regions.count > 0 && model->regions.address)
		{
			struct model_region const *reg = (struct model_region const *)model->regions.address + r_i;
			long pm_i;
			geom_idx = NONE;
			for (pm_i = 0; reg->permutations.address && pm_i < reg->permutations.count && geom_idx == NONE; pm_i++)
			{
				struct model_region_permutation const *perm =
					(struct model_region_permutation const *)reg->permutations.address + pm_i;
				int lod;
				for (lod = 4; lod >= 0; lod--)
				{
					short g = perm->geometry_indices[lod];
					if (g >= 0 && g < model->geometries.count)
					{
						geom_idx = g;
						break;
					}
				}
			}
		}
		if (geom_idx < 0 || geom_idx >= model->geometries.count)
			continue;

		geom_ptr = (uint8_t const *)model->geometries.address + (size_t)geom_idx * 48u;
		parts_blk = (struct tag_block const *)(geom_ptr + 36u);
		if (parts_blk->count <= 0 || !parts_blk->address)
			continue;

		for (p_i = 0; p_i < parts_blk->count; p_i++)
		{
			uint8_t const *part = (uint8_t const *)parts_blk->address + (size_t)p_i * 132u /* GBXMODEL_PART_BYTES */;
			short shdr_idx = *(short const *)(part + 0x04u);
			int32_t tri_cnt = *(int32_t const *)(part + 0x48u);
			uint32_t strip_off = *(uint32_t const *)(part + 0x4Cu);
			int32_t vtx_cnt = *(int32_t const *)(part + 0x58u);
			uint32_t vtx_off = *(uint32_t const *)(part + 0x64u);
			int16_t loc_node_cnt = (int16_t)part[0x6Bu];
			uint8_t const *loc_nodes = part + 0x6Cu;
			short mat_idx = -1;

			if (shdr_idx >= 0 && shdr_idx < model->shaders.count &&
				shdr_idx < FORGE_PREVIEW_MAX_SHADERS && model->shaders.address)
			{
				if (shader_to_mat[shdr_idx] == -2)
				{
					uint8_t const *shdr_ref = (uint8_t const *)model->shaders.address + (size_t)shdr_idx * 32u;
					long s_idx = *(long const *)(shdr_ref + 12u);
					uint32_t s_grp = 0;
					uint8_t const *s_def = preview_ce_get_tag(src, s_idx, &s_grp);
					shader_to_mat[shdr_idx] = -1;
					if (s_def)
					{
						long bitm_idx = NONE;
						float su = 1.0f, sv = 1.0f;
						if (s_grp == 'soso')
						{
							su = *(float const *)(s_def + 0x9Cu);
							sv = *(float const *)(s_def + 0xA0u);
							bitm_idx = *(long const *)(s_def + 0xB0u);
						}
						else if (s_grp == 'senv')
						{
							bitm_idx = *(long const *)(s_def + 0x94u);
						}
						if (bitm_idx != NONE)
						{
							uint32_t b_grp = 0;
							struct bitmap_group const *bgrp =
								(struct bitmap_group const *)preview_ce_get_tag(src, bitm_idx, &b_grp);
							if (bgrp && b_grp == 'bitm' && bgrp->bitmaps.count > 0 && bgrp->bitmaps.address)
							{
								struct bitmap_data const *bmp = (struct bitmap_data const *)bgrp->bitmaps.address;
								if (preview_decode_bitmap_texture(
										bmp,
										preview_ce_read_file_cb,
										(void *)src,
										&mesh->textures[mesh->texture_count]))
								{
									short m_slot = (short)mesh->material_count++;
									short t_slot = (short)mesh->texture_count++;
									mesh->materials[m_slot].texture_index = t_slot;
									mesh->materials[m_slot].color_source = 0;
									mesh->materials[m_slot].u_scale = (su > 0.0001f && su < 1000.0f) ? su : 1.0f;
									mesh->materials[m_slot].v_scale = (sv > 0.0001f && sv < 1000.0f) ? sv : 1.0f;
									mesh->materials[m_slot].base_map_alpha_Replicates = 0;
									shader_to_mat[shdr_idx] = m_slot;
								}
							}
						}
					}
				}
				mat_idx = shader_to_mat[shdr_idx];
			}

			if (vtx_cnt > 0 && vtx_cnt <= 16384 && tri_cnt > 0 && tri_cnt <= 65536)
			{
				unsigned long vtx_bytes = (unsigned long)vtx_cnt * 68u;
				unsigned long strip_bytes = ((unsigned long)tri_cnt + 2u) * 2u;
				unsigned long real_vtx_off = src->is_bundle
					? vtx_off
					: (src->map_hdr->model_data_offset + vtx_off);
				unsigned long real_strip_off = src->is_bundle
					? strip_off
					: (src->map_hdr->model_data_offset + src->map_hdr->model_index_data_offset + strip_off);
				uint8_t *verts68 = (uint8_t *)malloc(vtx_bytes);
				uint16_t *strip_idx = (uint16_t *)malloc(strip_bytes);

				if (verts68 && strip_idx &&
					preview_ce_read_file_cb((void *)src, real_vtx_off, vtx_bytes, verts68) &&
					preview_ce_read_file_cb((void *)src, real_strip_off, strip_bytes, strip_idx))
				{
					preview_append_part_vertices_and_strip(
						mesh,
						verts68,
						vtx_cnt,
						strip_idx,
						tri_cnt,
						world_nodes,
						model->nodes.count,
						has_local_nodes,
						loc_node_cnt,
						loc_nodes,
						mat_idx,
						u_scale,
						v_scale);
				}
				free(verts68);
				free(strip_idx);
			}
		}
	}

	free(world_nodes);
	return mesh->triangle_count > 0 ? 1 : 0;
}

int forge_ui_build_tag_preview_mesh(
	int is_installed_mode,
	int category_tab,
	int list_index,
	struct tag_injector_mesh *mesh)
{
	if (!mesh || category_tab < 0 || category_tab >= FORGE_BROWSER_NUM_CATS || list_index < 0)
		return 0;
	memset(mesh, 0, sizeof(*mesh));

	/* Mode 0: ADD TAGS (Map Tag Browser) */
	if (!is_installed_mode)
	{
		int32_t abs_idx;
		if (!s_map_browser.is_open || list_index >= s_map_browser.cat_counts[category_tab])
			return 0;
		abs_idx = s_map_browser.cat_tags[category_tab][list_index];
		if (s_map_browser.is_ce_map && s_map_browser.ce_header)
		{
			struct preview_ce_source src;
			memset(&src, 0, sizeof(src));
			src.is_bundle = 0;
			src.map_hdr = s_map_browser.ce_header;
			custom_edition_cache_bundle_source(&src.map_src);
			return build_preview_mesh_from_ce_source(
				&src,
				(uint8_t const *)s_map_browser.ce_header->instances[abs_idx].base_address,
				(uint32_t)s_map_browser.ce_header->instances[abs_idx].group_tag,
				mesh);
		}
		else if (s_map_browser.is_xbox_map && s_map_browser.xbox_tag_buf)
		{
			return build_preview_mesh_from_xbox_map_data(
				s_map_browser.xbox_fp,
				s_map_browser.xbox_tag_buf,
				s_map_browser.xbox_tag_sz,
				s_map_browser.xbox_tag_base,
				s_map_browser.xbox_inst_off,
				s_map_browser.xbox_tag_count,
				abs_idx,
				mesh);
		}
		return 0;
	}

	/* Mode 1: INSTALLED TAGS (Cache Tag Browser) */
	{
		int cat_idx = get_installed_catalog_index(category_tab, list_index);
		struct forge_catalog_entry const *e;
		long p_cnt, p;
		char fxtg_file[512];

		if (cat_idx < 0 || cat_idx >= g_forge.catalog_count)
			return 0;
		e = &g_forge.catalog[cat_idx];

		/* 1. Check Faber Tag Vault (.fbrtag + faber_vault.bin) */
		p_cnt = port_tag_profiles_count();
		for (p = 0; p < p_cnt; p++)
		{
			long t = port_tag_profiles_tag_find(p, (unsigned long)e->group_tag, e->tag_path);
			if (t >= 0)
			{
				unsigned long bundle_sz = 0;
				long bh = port_tag_profiles_bundle_open(p, t, &bundle_sz);
				if (bh != NONE && bundle_sz >= TAG_BUNDLE_HEADER_BYTES)
				{
					byte hdr[TAG_BUNDLE_HEADER_BYTES];
					if (port_tag_profiles_bundle_read_at(bh, 0, hdr, TAG_BUNDLE_HEADER_BYTES))
					{
						unsigned long tags_sz = tag_bundle_file_data_offset(hdr);
						if (tags_sz >= TAG_BUNDLE_HEADER_BYTES && tags_sz <= bundle_sz)
						{
							byte *b_bytes = (byte *)malloc(tags_sz);
							struct tag_bundle_loaded loaded;
							if (b_bytes && port_tag_profiles_bundle_read_at(bh, 0, b_bytes, tags_sz) &&
								tag_bundle_load_tags(b_bytes, tags_sz, bundle_sz, &loaded) == _tag_bundle_status_ok &&
								loaded.tag_count > 0)
							{
								struct preview_ce_source src;
								int ok;
								memset(&src, 0, sizeof(src));
								src.is_bundle = 1;
								src.bundle_loaded = &loaded;
								src.bundle_read.handle = bh;
								src.bundle_read.file_data_offset = loaded.file_data_offset;
								src.bundle_read.file_data_size = loaded.file_data_size;
								ok = build_preview_mesh_from_ce_source(
									&src,
									loaded.data + *(uint32_t const *)(loaded.tags + TAG_BUNDLE_TAG_ROOT_OFFSET),
									*(uint32_t const *)(loaded.tags + TAG_BUNDLE_TAG_GROUP_OFFSET),
									mesh);
								free(b_bytes);
								port_tag_profiles_bundle_close(bh);
								return ok;
							}
							free(b_bytes);
						}
					}
					port_tag_profiles_bundle_close(bh);
				}
			}
		}

		/* 2. Check compressed .fxtg Xbox tag bundle */
		if (find_fxtg_file_for_tag(e->group_tag, e->tag_path, fxtg_file, sizeof(fxtg_file), NULL))
		{
			FILE *fp = fopen(fxtg_file, "rb");
			if (fp)
			{
				struct fxtg_bundle_header hdr;
				if (fread(&hdr, 1, sizeof(hdr), fp) == sizeof(hdr) &&
					hdr.magic == 0x31545846u &&
					hdr.compressed_size > 0 && hdr.compressed_size <= 64u * 1024u * 1024u &&
					hdr.uncompressed_size >= 2048u && hdr.uncompressed_size <= 128u * 1024u * 1024u)
				{
					uint8_t *comp = (uint8_t *)malloc(hdr.compressed_size);
					uint8_t *uncomp = (uint8_t *)malloc(hdr.uncompressed_size);
					unsigned long d_len = hdr.uncompressed_size;
					if (comp && uncomp &&
						fread(comp, 1, hdr.compressed_size, fp) == hdr.compressed_size &&
						z_uncompress(uncomp, &d_len, comp, hdr.compressed_size) == Z_OK &&
						d_len == hdr.uncompressed_size)
					{
						char donor_path[512];
						FILE *dfp;
						platform_translate_path("z:\\cache_donor.map", donor_path, sizeof(donor_path));
						dfp = fopen(donor_path, "wb+");
						if (dfp)
						{
							uint32_t tag_off = *(uint32_t const *)(uncomp + 0x10);
							uint32_t tag_sz = *(uint32_t const *)(uncomp + 0x14);
							fwrite(uncomp, 1, hdr.uncompressed_size, dfp);
							fflush(dfp);
							if (tag_off >= 2048u && tag_off + tag_sz <= hdr.uncompressed_size)
							{
								uint8_t const *tbuf = uncomp + tag_off;
								uint32_t inst_addr = *(uint32_t const *)(tbuf + 0x00);
								int32_t t_cnt = *(int32_t const *)(tbuf + 0x0C);
								uint32_t t_base = inst_addr - 0x24u;
								int32_t i, root_abs = -1;
								for (i = 0; i < t_cnt && 0x24u + (uint32_t)t_cnt * 32u <= tag_sz; i++)
								{
									uint32_t io = 0x24u + (uint32_t)i * 32u;
									uint32_t grp = *(uint32_t const *)(tbuf + io + 0x00);
									uint32_t n_addr = *(uint32_t const *)(tbuf + io + 0x10);
									if (grp == e->group_tag && n_addr >= t_base && (n_addr - t_base) < tag_sz)
									{
										if (_stricmp((char const *)(tbuf + (n_addr - t_base)), e->tag_path) == 0)
										{
											root_abs = i;
											break;
										}
									}
								}
								if (root_abs >= 0)
								{
									int ok = build_preview_mesh_from_xbox_map_data(
										dfp, tbuf, tag_sz, t_base, 0x24u, t_cnt, root_abs, mesh);
									fclose(dfp);
									remove(donor_path);
									free(comp);
									free(uncomp);
									fclose(fp);
									return ok;
								}
							}
							fclose(dfp);
							remove(donor_path);
						}
					}
					free(comp);
					free(uncomp);
				}
				fclose(fp);
			}
		}

		/* 3. Stock Xbox cached tag: open donor Xbox map (cached across scroll steps) */
		if (open_preview_xbox_donor_for_tag(e->group_tag, e->tag_path))
		{
			int32_t i;
			char const *lookup = (e->group_tag == 'mach' &&
				_stricmp(e->tag_path, "levels\\a30\\devices\\beam emitter\\beam emitter red") == 0)
				? "levels\\a30\\devices\\beam emitter\\beam emitter"
				: e->tag_path;
			for (i = 0; i < s_preview_xbox_donor.tag_count; i++)
			{
				uint32_t io = s_preview_xbox_donor.inst_off + (uint32_t)i * 32u;
				uint32_t grp = *(uint32_t const *)(s_preview_xbox_donor.tag_buf + io + 0x00);
				uint32_t n_addr = *(uint32_t const *)(s_preview_xbox_donor.tag_buf + io + 0x10);
				if (grp == e->group_tag && n_addr >= s_preview_xbox_donor.tag_base &&
					(n_addr - s_preview_xbox_donor.tag_base) < s_preview_xbox_donor.tag_sz)
				{
					char const *tn = (char const *)(s_preview_xbox_donor.tag_buf + (n_addr - s_preview_xbox_donor.tag_base));
					if (_stricmp(tn, lookup) == 0)
					{
						return build_preview_mesh_from_xbox_map_data(
							s_preview_xbox_donor.fp,
							s_preview_xbox_donor.tag_buf,
							s_preview_xbox_donor.tag_sz,
							s_preview_xbox_donor.tag_base,
							s_preview_xbox_donor.inst_off,
							s_preview_xbox_donor.tag_count,
							i,
							mesh);
					}
				}
			}
		}
	}

	return 0;
}
