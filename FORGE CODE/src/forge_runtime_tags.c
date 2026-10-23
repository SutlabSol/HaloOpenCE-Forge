/*
 * OpenCE Forge ("HaloCE Forge.exe")
 * Runtime Tag Catalog, Cached Tag Folder System (OpenCE Forge/cached_tags/),
 * Dynamic Runtime Tag Table Expansion, Multiplayer Asset Guard, Custom Tag Extraction,
 * Standalone Map Baking, Dynamic Network Version & 343 Guilty Spark Hardware Model Renderer.
 * Strictly modular inside FORGE CODE/ with zero modifications to source/ or port/.
 */

#include "cseries.h"
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
#include <xtl.h>
#include "rasterizer/xbox/rasterizer_xbox.h"
#include "rasterizer/xbox/rasterizer_xbox_pixel_shader.h"
#include "../../port/third_party/zlib/zlib_prefixed.h"

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

#ifdef _WIN32
	CreateDirectoryA("z:\\maps", NULL);
	CreateDirectoryA("maps", NULL);
#endif
	platform_translate_path("z:\\maps\\loading.tga", z_tga_host, sizeof(z_tga_host));

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
		FILE *out_m = fopen("maps/loading.tga", "wb");
		FILE *out_z = fopen(z_tga_host, "wb");
		if (out_m)
		{
			fwrite(tga_buf, 1, 230418u, out_m);
			fclose(out_m);
		}
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
int g_forge_session_enabled = 1;

int forge_get_network_version(void)
{
	return g_forge_session_enabled ? 0x461D : 29;
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
	/* Disable bipeds, AI characters, and control devices */
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
	if (prefix && strcmp(prefix, "[c]") == 0)
		snprintf(out, out_sz, "[c] %s", leaf);
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
			if (source_cache_id == 2 && g_forge.catalog[i].source_cache_id != 2)
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

				/* Map group tag directly to the separated WEAPONS vs EQUIPMENT vs VEHICLES vs SCENERY vs DOORS tabs */
				if (grp == 'weap')
					cat_id = _forge_cat_weapons;
				else if (grp == 'eqip')
					cat_id = _forge_cat_equipment;
				else if (grp == 'vehi')
					cat_id = _forge_cat_vehicles;
				else if (is_forge_door_tag(grp, tag_path))
					cat_id = _forge_cat_barriers_devices;
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
							snprintf(prefix_buf, sizeof(prefix_buf), "[c] %s", custom_label);
					}
					else
					{
						snprintf(prefix_buf, sizeof(prefix_buf), "[c]");
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

static void scan_cache_file_into_catalog(char const *map_path, uint8_t source_cache_id)
{
	FILE *f;
	uint8_t hdr[2048];
	uint32_t tag_off, tag_sz;
	uint8_t *tag_buf;
	uint32_t inst_addr, inst_off;
	int32_t tag_count, i;

	f = fopen(map_path, "rb");
	if (!f)
		return;
	if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr))
	{
		fclose(f);
		return;
	}
	tag_off = *(uint32_t const *)(hdr + 0x10);
	tag_sz = *(uint32_t const *)(hdr + 0x14);
	if (tag_off < 2048 || tag_sz < 40 || tag_sz > 64 * 1024 * 1024)
	{
		fclose(f);
		return;
	}
	tag_buf = (uint8_t *)malloc(tag_sz);
	if (!tag_buf)
	{
		fclose(f);
		return;
	}
	fseek(f, (long)tag_off, SEEK_SET);
	if (fread(tag_buf, 1, tag_sz, f) != tag_sz)
	{
		free(tag_buf);
		fclose(f);
		return;
	}
	fclose(f);

	inst_addr = *(uint32_t const *)(tag_buf + 0x00);
	tag_count = *(int32_t const *)(tag_buf + 0x0C);
	if (inst_addr < 0x40440000u || tag_count <= 0 || tag_count > 16384)
	{
		free(tag_buf);
		return;
	}
	inst_off = inst_addr - 0x40440000u;

	for (i = 0; i < tag_count; i++)
	{
		uint32_t io = inst_off + (uint32_t)i * 32u;
		uint32_t grp, name_addr;
		char const *tname;
		char const *prefix = source_cache_id ? "[CUSTOM]" : "";

		if (io + 32u > tag_sz)
			break;
		grp = *(uint32_t const *)(tag_buf + io + 0x00);
		name_addr = *(uint32_t const *)(tag_buf + io + 0x10);
		if (name_addr < 0x40440000u || (name_addr - 0x40440000u) >= tag_sz)
			continue;
		tname = (char const *)(tag_buf + (name_addr - 0x40440000u));

		if (grp == 'weap')
		{
			if (!is_excluded_weapon_or_equipment_tag(tname))
				add_catalog_entry('weap', _forge_cat_weapons, source_cache_id, tname, prefix);
		}
		else if (grp == 'eqip')
		{
			if (!is_excluded_weapon_or_equipment_tag(tname))
				add_catalog_entry('eqip', _forge_cat_equipment, source_cache_id, tname, prefix);
		}
		else if (grp == 'vehi')
		{
			add_catalog_entry('vehi', _forge_cat_vehicles, source_cache_id, tname, prefix);
		}
		else if (grp == 'bipd')
		{
			add_catalog_entry('bipd', _forge_cat_bipeds, source_cache_id, tname, prefix);
		}
		else if (grp == 'scen')
		{
			if (!is_excluded_scenery_or_effect_tag(tname))
				add_catalog_entry('scen', _forge_cat_scenery, source_cache_id, tname, prefix);
		}
		else if (grp == 'actv')
		{
			add_catalog_entry('actv', _forge_cat_ai_squads, source_cache_id, tname, source_cache_id ? "[CUSTOM AI]" : "[AI]");
		}
	}

	free(tag_buf);
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
	scan_cached_tags_manifest("OpenCE Forge/cached_tags/custom_catalog.txt");
	scan_cached_tags_manifest("OpenCE Forge/cached_tags/xbox_catalog.txt");
	scan_cached_tags_manifest("OpenCE Forge/cached_tags/catalog.txt");

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
	scan_ocetags_directory_into_donor_list("OpenCE Forge/ocetags");
	scan_ocetags_directory_into_donor_list("ocetags");
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

static void forge_copy_res_file_if_missing(char const *src_path, char const *dst_path)
{
	FILE *check = fopen(dst_path, "rb");
	FILE *in, *out;
	char buf[65536];
	size_t n;
	if (check)
	{
		fclose(check);
		return;
	}
	in = fopen(src_path, "rb");
	if (!in)
		return;
	out = fopen(dst_path, "wb");
	if (!out)
	{
		fclose(in);
		return;
	}
	while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
		fwrite(buf, 1, n, out);
	fclose(out);
	fclose(in);
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
 * (either in OpenCE's z:\cache000.map..z:\cache005.map or in OpenCE Forge/cached_tags/xbox_cache/<stem>.map).
 * If not, decompresses the Xbox v5 map payload using zlib into OpenCE Forge/cached_tags/xbox_cache/<stem>.map.
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
	char stem[64];
	char const *slash1;
	char const *slash2;
	char const *leaf;
	char const *dot;
	size_t stem_len;
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

	/* 1. Compute permanent Xbox cache path: OpenCE Forge/cached_tags/xbox_cache/<stem>.map */
	slash1 = strrchr(map_path, '\\');
	slash2 = strrchr(map_path, '/');
	leaf = (slash1 > slash2) ? (slash1 + 1) : (slash2 ? (slash2 + 1) : map_path);
	dot = strrchr(leaf, '.');
	stem_len = dot ? (size_t)(dot - leaf) : strlen(leaf);
	if (stem_len == 0 || stem_len >= sizeof(stem))
		stem_len = sizeof(stem) - 1;
	memcpy(stem, leaf, stem_len);
	stem[stem_len] = 0;

#ifdef _WIN32
	CreateDirectoryA("OpenCE Forge", NULL);
	CreateDirectoryA("OpenCE Forge/cached_tags", NULL);
	CreateDirectoryA("OpenCE Forge/cached_tags/xbox_cache", NULL);
#endif
	snprintf(out_cache_path, out_cache_sz, "OpenCE Forge/cached_tags/xbox_cache/%s.map", stem);
	{
		FILE *cf = fopen(out_cache_path, "rb");
		if (cf)
		{
			long csz = 0;
			fseek(cf, 0, SEEK_END);
			csz = ftell(cf);
			fclose(cf);
			if (csz >= (long)(tag_off + tag_sz))
			{
				fclose(in_fp);
				return 1;
			}
		}
	}

	/* 2. Check if OpenCE's Xbox hard-drive cache (z:\cache000.map .. z:\cache005.map) already holds this
	 * decompressed Xbox map; if so, copy it into OpenCE Forge/cached_tags/xbox_cache/<stem>.map so all 23 Xbox
	 * maps are permanently stored in xbox_cache/ rather than lost when z:\cache00X.map rotates! */
	for (slot = 0; slot < 6; slot++)
	{
		char z_xbox[64];
		char z_path[512];
		FILE *zf;
		snprintf(z_xbox, sizeof(z_xbox), "z:\\cache00%d.map", slot);
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
					FILE *dst_fp = fopen(out_cache_path, "wb");
					if (dst_fp)
					{
						size_t const cp_sz = 262144u;
						uint8_t *cp_buf = (uint8_t *)malloc(cp_sz);
						long copied = 0;
						int copy_ok = 1;
						fseek(zf, 0, SEEK_SET);
						if (cp_buf)
						{
							size_t rd;
							while ((rd = fread(cp_buf, 1, cp_sz, zf)) > 0)
							{
								if (fwrite(cp_buf, 1, rd, dst_fp) != rd)
								{
									copy_ok = 0;
									break;
								}
								copied += (long)rd;
								if (s_forge_loading_active && s_forge_loading_map_span > 0.0f && zsz > 0)
								{
									unsigned long now_ms = system_milliseconds();
									if (now_ms - s_forge_loading_last_ms >= 15u)
									{
										float frac = (float)copied / (float)zsz;
										if (frac < 0.0f) frac = 0.0f;
										if (frac > 0.99f) frac = 0.99f;
										forge_xbox_loading_screen_render_frame(
											s_forge_loading_map_base + frac * s_forge_loading_map_span);
									}
								}
							}
							free(cp_buf);
						}
						else
						{
							copy_ok = 0;
						}
						fclose(dst_fp);
						if (copy_ok && copied >= (long)(tag_off + tag_sz))
						{
							fclose(zf);
							fclose(in_fp);
							return 1;
						}
						remove(out_cache_path);
					}
				}
			}
			fclose(zf);
		}
	}

	/* Check if map_path itself is already uncompressed on disk */
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
				FILE *dst_fp = fopen(out_cache_path, "wb");
				if (dst_fp)
				{
					size_t const cp_sz = 262144u;
					uint8_t *cp_buf = (uint8_t *)malloc(cp_sz);
					size_t rd;
					fseek(in_fp, 0, SEEK_SET);
					if (cp_buf)
					{
						while ((rd = fread(cp_buf, 1, cp_sz, in_fp)) > 0)
							fwrite(cp_buf, 1, rd, dst_fp);
						free(cp_buf);
					}
					fclose(dst_fp);
					fclose(in_fp);
					return 1;
				}
				fclose(in_fp);
				snprintf(out_cache_path, out_cache_sz, "%s", map_path);
				return 1;
			}
		}
		fseek(in_fp, 2048L, SEEK_SET);
	}

	/* 3. Stream-decompress Xbox v5 map into OpenCE Forge/cached_tags/xbox_cache/<stem>.map */
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
		else if (grp == 'scen')
			cat_id = _forge_cat_scenery;
		else if (is_forge_door_tag(grp, tname))
			cat_id = _forge_cat_barriers_devices;
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
		if (legacy_manifest_fp)
			fprintf(legacy_manifest_fp, "%s|%d|0|%s\n", grp_str, cat_id, tname);
		if (credits_fp)
			fprintf(credits_fp, "- `%s` (`%s`)\n", tname, grp_str);
		indexed_count++;

		if (grp == 'mach' && _stricmp(tname, "levels\\a30\\devices\\beam emitter\\beam emitter") == 0)
		{
			char const *red_tname = "levels\\a30\\devices\\beam emitter\\beam emitter red";
			if (xbox_manifest_fp)
				fprintf(xbox_manifest_fp, "mach|%d|0|%s\n", cat_id, red_tname);
			if (legacy_manifest_fp)
				fprintf(legacy_manifest_fp, "mach|%d|0|%s\n", cat_id, red_tname);
			if (credits_fp)
				fprintf(credits_fp, "- `%s` (`mach`)\n", red_tname);
			indexed_count++;
		}
	}

	if (credits_fp)
		fprintf(credits_fp, "\n");
	free(tag_buf);
	return indexed_count;
}

static int forge_rip_maps_folder_to_cached_tags(char const *maps_dir)
{
#ifdef _WIN32
	char pattern[512];
	WIN32_FIND_DATAA fd;
	HANDLE h;
	FILE *xbox_manifest_fp;
	FILE *legacy_manifest_fp;
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

	xbox_manifest_fp = fopen("OpenCE Forge/cached_tags/xbox_catalog.txt", "wb");
	legacy_manifest_fp = fopen("OpenCE Forge/cached_tags/catalog.txt", "wb");
	if (!xbox_manifest_fp)
	{
		if (legacy_manifest_fp)
			fclose(legacy_manifest_fp);
		return 0;
	}
	fprintf(xbox_manifest_fp, "# OpenCE Forge - Native Xbox Maps Tag Catalog (Built from Xbox maps in %s)\n", maps_dir);
	if (legacy_manifest_fp)
		fprintf(legacy_manifest_fp, "# OpenCE Forge - Native Xbox Maps Tag Catalog (Built from Xbox maps in %s)\n", maps_dir);

	credits_fp = fopen("OpenCE Forge/credits.md", "wb");
	if (credits_fp)
	{
		fprintf(credits_fp, "# OpenCE Forge - Native Xbox Map Tag Catalog\n\n");
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
			map_files[m], xbox_manifest_fp, legacy_manifest_fp, credits_fp);
		forge_xbox_loading_screen_update((float)(m + 1) / (float)map_file_count);
		if (count_from_map > 0)
		{
			ripped_maps++;
			total_assets += count_from_map;
		}
	}
	forge_xbox_loading_screen_end();

	if (credits_fp)
		fclose(credits_fp);
	if (legacy_manifest_fp)
		fclose(legacy_manifest_fp);
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

int forge_clear_custom_tag_cache(void)
{
	/* Delete all custom .ocetags packages and custom catalog cache */
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

	/* Also clear OpenCE's 6-slot z:\cache000.map..z:\cache005.map ring buffer */
	for (slot = 0; slot < 6; slot++)
	{
		char z_xbox[64];
		char z_path[512];
		snprintf(z_xbox, sizeof(z_xbox), "z:\\cache00%d.map", slot);
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
		snprintf(exe_path, sizeof(exe_path), "HaloCE Forge.exe");

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
			{
#ifdef _WIN32
				WIN32_FIND_DATAA cfd;
				HANDLE ch = FindFirstFileA("OpenCE Forge\\cached_tags\\xbox_cache\\*.map", &cfd);
				if (ch != INVALID_HANDLE_VALUE)
				{
					int cached_map_count = 0;
					do
					{
						if (!(cfd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
							cached_map_count++;
					} while (FindNextFileA(ch, &cfd));
					CloseHandle(ch);
					if (cached_map_count >= 23)
						return;
				}
#else
				return;
#endif
			}
		}
	}

#ifdef _WIN32
	CreateDirectoryA("OpenCE Forge", NULL);
	CreateDirectoryA("OpenCE Forge/custom_tags", NULL);
	CreateDirectoryA("custom_tags", NULL);
	CreateDirectoryA("OpenCE Forge/forge_maps", NULL);
	CreateDirectoryA("OpenCE Forge/cached_tags", NULL);
	CreateDirectoryA("OpenCE Forge/cached_tags/xbox_cache", NULL);
	CreateDirectoryA("OpenCE Forge/cached_tags/markers", NULL);
	CreateDirectoryA("forge_maps", NULL);

	if (ask_before_build)
	{
		int resp = MessageBoxA(
			NULL,
			"OpenCE Forge - Tag Cache Reset\n\n"
			"The Xbox tag cache has been cleared.\n\n"
			"Would you like to build the default Xbox map tag cache now?",
			"OpenCE Forge - Build Xbox Tag Cache",
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
				"OpenCE Forge - Startup Xbox Map Tag Cache\n\n"
				"No pre-existing Xbox map cache was found.\n\n"
				"Click Yes to select your Xbox 'maps/' folder and index the default Xbox Vehicle, Scenery, Weapon & Equipment tags.",
				"OpenCE Forge - Xbox Map Tag Cache",
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
	g_forge.bp_zoom = 1.0f;
	g_forge.bp_cam_z = 12.0f;
	snprintf(g_forge.save_map_name, sizeof(g_forge.save_map_name), "forge_custom_arena");
	g_forge.save_map_name_len = (int)strlen(g_forge.save_map_name);

#ifdef _WIN32
	CreateDirectoryA("OpenCE Forge", NULL);
	CreateDirectoryA("OpenCE Forge/custom_tags", NULL);
	CreateDirectoryA("custom_tags", NULL);
	CreateDirectoryA("forge_maps", NULL);
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
#define FORGE_MAX_DYNAMIC_TAGS   256
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

void forge_runtime_on_map_unload(void)
{
	int i;
	for (i = 0; i < s_forge_phys_allocs.count; i++)
	{
		if (s_forge_phys_allocs.ptrs[i])
			XPhysicalFree(s_forge_phys_allocs.ptrs[i]);
	}
	s_forge_phys_allocs.count = 0;
	s_dyn_sound_perms.count = 0;

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
	if (grp != 'scen' && grp != 'mach' && grp != 'lifi')
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
		else if (group_tag == 'scen' || group_tag == 'mach' || group_tag == 'lifi')
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

static void runtime_clone_schema_blocks(
	struct tag_schema_definition const *def,
	uint8_t *base_ptr,
	int32_t elem_count,
	int depth)
{
	int32_t elem_idx;
	struct tag_schema_field const *field;
	uint32_t elem_size;

	if (!def || !def->fields || !base_ptr || elem_count <= 0 || def->size <= 0 || depth > 8)
		return;

	elem_size = (uint32_t)def->size;
	for (elem_idx = 0; elem_idx < elem_count; elem_idx++)
	{
		uint8_t *elem_ptr = base_ptr + (size_t)elem_idx * elem_size;
		for (field = def->fields; field->type != _tag_schema_terminator; field++)
		{
			int16_t k;
			int16_t fcount = field->count > 0 ? field->count : 1;
			uint32_t fstride = field->size > 0 ? (uint32_t)field->size : 0;

			for (k = 0; k < fcount; k++)
			{
				uint32_t f_off = (uint32_t)field->offset + (uint32_t)k * fstride;

				if (field->type == _tag_schema_struct)
				{
					struct tag_schema_definition const *sub_def =
						(struct tag_schema_definition const *)field->definition;
					if (sub_def && f_off + (uint32_t)sub_def->size <= elem_size)
						runtime_clone_schema_blocks(sub_def, elem_ptr + f_off, 1, depth + 1);
				}
				else if (field->type == _tag_schema_block)
				{
					struct tag_schema_definition const *child_def =
						(struct tag_schema_definition const *)field->definition;
					if (child_def && child_def->size > 0 && f_off + 12u <= elem_size)
					{
						struct tag_block *blk = (struct tag_block *)(elem_ptr + f_off);
						uint32_t sub_elem_sz = (uint32_t)child_def->size;
						int32_t max_cnt = (field->maximum > 0) ? (int32_t)field->maximum : 32768;
						if (blk->count > 0 && blk->count <= max_cnt && blk->address != NULL)
						{
							size_t copy_bytes = (size_t)blk->count * sub_elem_sz;
							uint8_t *new_sub = (uint8_t *)malloc(copy_bytes);
							if (new_sub)
							{
								memcpy(new_sub, blk->address, copy_bytes);
								blk->address = new_sub;
								runtime_clone_schema_blocks(child_def, new_sub, blk->count, depth + 1);
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
					if (f_off + 20u <= elem_size)
					{
						struct tag_data *td = (struct tag_data *)(elem_ptr + f_off);
						int32_t max_sz = (field->maximum > 0) ? (int32_t)field->maximum : (16 * 1024 * 1024);
						if (td->size > 0 && td->size <= max_sz && td->address != NULL)
						{
							void *new_td = malloc((size_t)td->size);
							if (new_td)
							{
								memcpy(new_td, td->address, (size_t)td->size);
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
			}
		}
	}
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

	/* Strictly keep Xbox tags using Xbox bitmaps, shaders, models, and sounds from their source Xbox .map!
	 * Only reuse non-visual/non-audio shared dependencies if we are on a stock Xbox map (never on a CE map). */
	if (cl->order_count > 0 && !custom_edition_cache_tags_loaded() && !cl->is_red_beam_variant &&
		grp != 'bitm' && grp != 'mode' && grp != 'snd!' && grp != 'lsnd' &&
		grp != 'shdr' && grp != 'senv' && grp != 'soso' && grp != 'sotr' &&
		grp != 'schi' && grp != 'scex' && grp != 'swat' && grp != 'sgla' &&
		grp != 'smet' && grp != 'spla')
	{
		long ex = tag_loaded((long)grp, tname);
		if (ex != NONE)
		{
			cl->runtime_handles[abs_idx] = ex;
			return;
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
	FILE *f;
	uint8_t hdr[2048];
	uint32_t tag_off, tag_sz, inst_addr;
	int32_t tag_count, i, root_abs_idx = -1;
	uint8_t *tag_buf;
	struct xbox_runtime_closure cl;
	long root_handle = NONE;
	int is_red_beam_variant = (req_group == 'mach' &&
		_stricmp(req_path, "levels\\a30\\devices\\beam emitter\\beam emitter red") == 0) ? 1 : 0;
	char const *lookup_path = is_red_beam_variant
		? "levels\\a30\\devices\\beam emitter\\beam emitter"
		: req_path;

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
		free(tag_buf);
		fclose(f);
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
		free(tag_buf);
		fclose(f);
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
		free(tag_buf);
		fclose(f);
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
		else if (req_group == 'scen' || req_group == 'mach' || req_group == 'lifi')
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
	free(tag_buf);
	fclose(f);
	return root_handle;
}

static long load_native_xbox_tag_runtime(
	uint32_t req_group,
	char const *req_path,
	uint32_t *out_actual_group)
{
#ifdef _WIN32
	static char const *const s_xbox_map_dirs[] = {
		"OpenCE Forge/cached_tags/xbox_cache",
		"maps",
		"FORGE CODE/GAME MAPS/XBOX MAPS",
		"../FORGE CODE/GAME MAPS/XBOX MAPS"
	};
	int slot, d;

	if (!req_path || !req_path[0])
		return NONE;

	/* 1. Check OpenCE's Xbox hard-drive cache (z:\cache000.map .. z:\cache005.map) */
	for (slot = 0; slot < 6; slot++)
	{
		char z_path[64];
		long hdl;
		snprintf(z_path, sizeof(z_path), "z:\\cache00%d.map", slot);
		hdl = load_native_xbox_tag_from_decompressed_map(z_path, req_group, req_path, out_actual_group);
		if (hdl != NONE)
			return hdl;
	}

	/* 2. Check decompressed Xbox cache in OpenCE Forge/cached_tags/xbox_cache/ and Xbox maps in maps/ */
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
				if (d == 0)
				{
					long hdl = load_native_xbox_tag_from_decompressed_map(full_path, req_group, req_path, out_actual_group);
					if (hdl != NONE)
					{
						CloseHandle(h);
						return hdl;
					}
				}
				else if (is_xbox_version5_map_file(full_path) &&
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

static long register_dynamic_cached_tag(uint32_t group_tag, char const *tag_path, uint32_t *out_actual_group)
{
	int i;
	long native_idx;

	/* Check if already registered for the current map */
	for (i = 0; i < s_dyn_tags.count; i++)
	{
		if (s_dyn_tags.entries[i].group_tag == group_tag &&
			_stricmp(s_dyn_tags.entries[i].tag_path, tag_path) == 0)
		{
			if (out_actual_group)
				*out_actual_group = (uint32_t)tag_get_group_tag(s_dyn_tags.entries[i].definition_index);
			return s_dyn_tags.entries[i].definition_index;
		}
	}

	if (s_dyn_tags.count >= FORGE_MAX_DYNAMIC_TAGS)
		return NONE;

	/* 1. Check if an exact .ocetags package exists in custom_tags/ and load its complete tag tree natively! */
	native_idx = load_native_ocetags_package_runtime(group_tag, tag_path, out_actual_group);
	if (native_idx != NONE)
		return native_idx;

	/* 2. Load default Xbox tag natively from Xbox map cache using OpenCE's exact tag schema & hardware format! */
	native_idx = load_native_xbox_tag_runtime(group_tag, tag_path, out_actual_group);
	if (native_idx != NONE)
		return native_idx;

	return NONE;
}

long forge_resolve_engine_tag(uint32_t group_tag, char const *tag_path, uint32_t *out_actual_group)
{
	long idx;
	int i;

	if (out_actual_group)
		*out_actual_group = group_tag;
	if (!tag_path || !tag_path[0])
		return NONE;

	/* 0. If already dynamically registered for this map (e.g. from custom_tags/*.ocetags), return it directly */
	for (i = 0; i < s_dyn_tags.count; i++)
	{
		if (s_dyn_tags.entries[i].group_tag == group_tag &&
			_stricmp(s_dyn_tags.entries[i].tag_path, tag_path) == 0)
		{
			if (out_actual_group)
				*out_actual_group = (uint32_t)tag_get_group_tag(s_dyn_tags.entries[i].definition_index);
			return s_dyn_tags.entries[i].definition_index;
		}
	}

	/* 1. Check if the exact tag (or its native _mp / SP counterpart) is already in the map */
	idx = find_native_or_mp_variant_tag(group_tag, tag_path);
	if (idx != NONE)
	{
		if (group_tag == 'vehi')
			patch_vehicle_tag_for_player_driving(idx, tag_path);
		return idx;
	}

	/* 2. Dynamically load from custom_tags/*.ocetags or Xbox map cache into global_tag_instances! */
	idx = register_dynamic_cached_tag(group_tag, tag_path, out_actual_group);
	if (idx != NONE)
		return idx;

	return NONE;
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

static int ocetags_find_tag_chunk(struct ocetags_chunk_entry const *chunks, int count, char const *group_str, char const *path_hint)
{
	int i;
	for (i = 0; i < count; i++)
	{
		if (chunks[i].kind[0] != 0)
			continue;
		if (group_str && group_str[0] && _stricmp(chunks[i].group, group_str) != 0)
			continue;
		if (!path_hint || !path_hint[0] || _stricmp(chunks[i].path, path_hint) == 0)
			return i;
	}
	for (i = 0; i < count; i++)
	{
		if (chunks[i].kind[0] != 0)
			continue;
		if (group_str && group_str[0] && _stricmp(chunks[i].group, group_str) == 0)
			return i;
	}
	return -1;
}

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
		"OpenCE Forge/cached_tags/catalog.txt"
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

	cat_id = _forge_cat_scenery;
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

	cat_id = _forge_cat_scenery;
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
		"custom_tags",
		"OpenCE Forge/ocetags",
		"ocetags"
	};
	char found_files[256][512];
	int found_count = 0;
	int d, f_idx;
	FILE *manifest_fp = NULL;
	FILE *credits_fp = NULL;

	CreateDirectoryA("OpenCE Forge", NULL);
	CreateDirectoryA("OpenCE Forge/custom_tags", NULL);
	CreateDirectoryA("custom_tags", NULL);
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
	if (forge_is_multiplayer_session())
	{
		forge_set_status("Multiplayer Asset Guard: Custom tag rehash is Single-Player only!");
		return 0;
	}

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

	forge_set_status("Place custom .ocetags files in 'OpenCE Forge/custom_tags/' and click REHASH CUSTOM TAGS!");
	return 0;
}

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
	if (!stem || !stem[0])
		return 0;
	snprintf(candidate, sizeof(candidate), "OpenCE Forge/forge_maps/%s.map", stem);
	if (is_ce_v609_map_file(candidate)) { snprintf(out_path, out_sz, "%s", candidate); return 1; }

	snprintf(candidate, sizeof(candidate), "../FORGE CODE/GAME MAPS/CUSTOM EDITION DEFAULT MAPS/%s.map", stem);
	if (is_ce_v609_map_file(candidate)) { snprintf(out_path, out_sz, "%s", candidate); return 1; }

	snprintf(candidate, sizeof(candidate), "FORGE CODE/GAME MAPS/CUSTOM EDITION DEFAULT MAPS/%s.map", stem);
	if (is_ce_v609_map_file(candidate)) { snprintf(out_path, out_sz, "%s", candidate); return 1; }

	snprintf(candidate, sizeof(candidate), "custom_maps/%s.map", stem);
	if (is_ce_v609_map_file(candidate)) { snprintf(out_path, out_sz, "%s", candidate); return 1; }

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
		if (is_ce_v609_map_file(candidate))
		{
			if (extract_embedded_forge_base_map_stem(candidate, base_stem, sizeof(base_stem)) &&
				_stricmp(base_stem, map_leaf) != 0 &&
				try_resolve_clean_ce_map_by_stem(base_stem, out_path, out_sz))
			{
				return;
			}
			snprintf(out_path, out_sz, "%s", candidate);
			return;
		}

		snprintf(candidate, sizeof(candidate), "OpenCE Forge/forge_maps/%s.map", map_leaf);
		if (is_ce_v609_map_file(candidate)) { snprintf(out_path, out_sz, "%s", candidate); return; }

		snprintf(candidate, sizeof(candidate), "../FORGE CODE/GAME MAPS/CUSTOM EDITION DEFAULT MAPS/%s.map", map_leaf);
		if (is_ce_v609_map_file(candidate)) { snprintf(out_path, out_sz, "%s", candidate); return; }

		snprintf(candidate, sizeof(candidate), "FORGE CODE/GAME MAPS/CUSTOM EDITION DEFAULT MAPS/%s.map", map_leaf);
		if (is_ce_v609_map_file(candidate)) { snprintf(out_path, out_sz, "%s", candidate); return; }

		snprintf(candidate, sizeof(candidate), "../FORGE CODE/GAME MAPS/PC MAPS/%s.map", map_leaf);
		if ((f = fopen(candidate, "rb")) != NULL) { fclose(f); snprintf(out_path, out_sz, "%s", candidate); return; }

		snprintf(candidate, sizeof(candidate), "FORGE CODE/GAME MAPS/PC MAPS/%s.map", map_leaf);
		if ((f = fopen(candidate, "rb")) != NULL) { fclose(f); snprintf(out_path, out_sz, "%s", candidate); return; }

		snprintf(candidate, sizeof(candidate), "maps/%s.map", map_leaf);
		if ((f = fopen(candidate, "rb")) != NULL) { fclose(f); snprintf(out_path, out_sz, "%s", candidate); return; }
	}

	if ((f = fopen("OpenCE Forge/forge_maps/bloodgulch.map", "rb")) != NULL)
	{
		fclose(f);
		snprintf(out_path, out_sz, "OpenCE Forge/forge_maps/bloodgulch.map");
		return;
	}
	if ((f = fopen("../FORGE CODE/GAME MAPS/CUSTOM EDITION DEFAULT MAPS/bloodgulch.map", "rb")) != NULL)
	{
		fclose(f);
		snprintf(out_path, out_sz, "../FORGE CODE/GAME MAPS/CUSTOM EDITION DEFAULT MAPS/bloodgulch.map");
		return;
	}
	snprintf(out_path, out_sz, "FORGE CODE/GAME MAPS/CUSTOM EDITION DEFAULT MAPS/bloodgulch.map");
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
							if (_stricmp(oname, tpath) != 0 && _strnicmp(oname, tpath, tlen) != 0)
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
				memset(de, 0, sizeof(*de));
				de->active = 1;
				de->group_tag = (uint32_t)grp;
				de->deleted = (uint8_t)deleted;
				de->orig_x = ox; de->orig_y = oy; de->orig_z = oz;
				de->new_x = nx; de->new_y = ny; de->new_z = nz;
				de->new_yaw = nyaw; de->new_pitch = npitch; de->new_roll = nroll;
				snprintf(de->tag_path, sizeof(de->tag_path), "%s", tpath);
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

	/* Check saved custom_maps\<name> and forge_maps\<name>\<name> maps */
	if (_strnicmp(map_name, "custom_maps\\", 12) != 0 &&
		_strnicmp(map_name, "custom_maps/", 12) != 0 &&
		_strnicmp(map_name, "forge_maps\\", 11) != 0 &&
		_strnicmp(map_name, "forge_maps/", 11) != 0)
		return;

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
	struct render_lighting studio_lighting;
	struct render_lighting const *use_lighting;
	union real_rgb_color change_colors[4];
	real function_values[4];
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
		pos,
		radius,
		NULL,
		unique_id,
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

	if (s_marker_meshes.loaded)
		return;
	s_marker_meshes.loaded = 1;

	for (k = 0; k < NUMBER_OF_MARKER_MESH_KINDS; k++)
	{
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
	union real_rgb_color change_colors[4];
	real function_values[4];
	struct real_matrix4x3 node_matrix;
	long z_shdr_idx, col_shdr_idx;
	struct shader *z_shdr_def;
	struct shader *col_shdr_def;
	int p;

	if (!parts || part_count <= 0)
		return;

	z_shdr_idx = get_or_create_translucent_marker_shader(color_preset, is_highlighted, 1);
	col_shdr_idx = get_or_create_translucent_marker_shader(color_preset, is_highlighted, 0);
	if (col_shdr_idx == NONE)
		return;
	z_shdr_def = (z_shdr_idx != NONE) ? shader_definition_get(z_shdr_idx) : NULL;
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

	obj_def_idx = forge_resolve_engine_tag(entry->group_tag, entry->tag_path, &actual_grp);
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
	render_world_objective_and_spawn_markers();
	render_spawn_palette_3d_preview(NONE, lighting_ptr);
}

int forge_monitor_render_player_unit(long object_index, void const *lighting_ptr, unsigned long flags)
{
	static long s_monitor_bipd_handle = NONE;
	static int s_monitor_resolved = 0;
	struct unit_datum *unit;

	if (!g_forge_session_enabled || object_index == NONE)
		return 0;

	/* Check if this object is a player biped in Forge Monitor mode */
	unit = (struct unit_datum *)object_try_and_get_and_verify_type(object_index, _object_mask_biped);
	if (!unit || unit->object.type != _object_type_biped || unit->unit.player_index == NONE)
		return 0;

	if (g_forge.mode == _forge_mode_play)
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

	{
		union real_point3d pos = unit->object.position;
		float t = (float)(system_milliseconds() % 4000u) * (6.2831853f / 4000.0f);
		pos.z += 0.06f * (t < 3.1415926f ? (t / 3.1415926f - 0.5f) : (1.5f - t / 3.1415926f));

		return draw_native_object_model_at(
			s_monitor_bipd_handle,
			object_index,
			lighting_ptr,
			&pos,
			&unit->object.forward,
			&unit->object.up,
			1,
			0,
			flags);
	}
}


